#include "db.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <queue>

namespace fs = std::filesystem;

namespace lsm {
namespace {

bool parse_id(const std::string& name, const std::string& prefix, const std::string& suffix, uint64_t& id) {
    if (name.size() <= prefix.size() + suffix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    std::string mid = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (!std::all_of(mid.begin(), mid.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    id = std::stoull(mid);
    return true;
}

}  // namespace

std::string DB::sst_path(uint64_t id) const {
    char b[32];
    std::snprintf(b, sizeof b, "%06llu.sst", static_cast<unsigned long long>(id));
    return dir_ + "/" + b;
}

std::string DB::wal_path(uint64_t id) const {
    char b[32];
    std::snprintf(b, sizeof b, "wal-%06llu.log", static_cast<unsigned long long>(id));
    return dir_ + "/" + b;
}

std::unique_ptr<DB> DB::open(const std::string& dir, const Options& opts) {
    std::unique_ptr<DB> db(new DB(dir, opts));
    fs::create_directories(dir);
    db->recover();
    return db;
}

// Startup: load SSTables, replay any WALs into the memtable, flush that, start a fresh WAL.
void DB::recover() {
    std::vector<uint64_t> sst_ids, wal_ids;
    uint64_t max_id = 0;
    for (const auto& de : fs::directory_iterator(dir_)) {
        std::string name = de.path().filename().string();
        uint64_t id;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            fs::remove(de.path());  // leftover from a crash mid-flush/compaction: never valid
        } else if (parse_id(name, "", ".sst", id)) {
            sst_ids.push_back(id);
            max_id = std::max(max_id, id);
        } else if (parse_id(name, "wal-", ".log", id)) {
            wal_ids.push_back(id);
            max_id = std::max(max_id, id);
        }
    }
    next_id_ = max_id + 1;

    std::sort(sst_ids.begin(), sst_ids.end(), std::greater<uint64_t>());  // newest first
    for (uint64_t id : sst_ids) tables_.push_back(SSTable::open(sst_path(id)));

    // Oldest log first, so newer writes overwrite older ones. More than one log exists
    // only if we crashed in the middle of a flush.
    std::sort(wal_ids.begin(), wal_ids.end());
    for (uint64_t id : wal_ids) {
        Wal::replay(wal_path(id), [&](bool deleted, std::string k, std::string v) {
            if (deleted) mem_.del(k); else mem_.put(k, v);
        });
    }
    if (!mem_.empty()) {
        write_sstable(mem_);
        mem_ = MemTable();
    }
    for (uint64_t id : wal_ids) fs::remove(wal_path(id));  // contents are now safely in an SSTable

    wal_ = std::make_unique<Wal>(wal_path(next_id_++), opts_.sync_wal);
    maybe_compact();
}

void DB::put(const std::string& key, const std::string& value) {
    wal_->append(false, key, value);  // 1. log first (durability)
    mem_.put(key, value);             // 2. then memory
    if (mem_.approx_bytes() >= opts_.memtable_bytes) flush_memtable();
}

void DB::del(const std::string& key) {
    wal_->append(true, key, "");
    mem_.del(key);  // a tombstone, not an erase
    if (mem_.approx_bytes() >= opts_.memtable_bytes) flush_memtable();
}

// Read path: newest data first. The first record found wins (tombstone => "not found").
std::optional<std::string> DB::get(const std::string& key) {
    if (const Entry* e = mem_.find(key)) {
        if (e->deleted) return std::nullopt;
        return e->value;
    }
    if (imm_) {
        if (const Entry* e = imm_->find(key)) {
            if (e->deleted) return std::nullopt;
            return e->value;
        }
    }
    for (auto& t : tables_) {
        if (!t->may_contain(key)) { ++bloom_skips_; continue; }
        ++table_probes_;
        Entry e;
        if (t->get(key, e)) {
            if (e.deleted) return std::nullopt;
            return e.value;
        }
    }
    return std::nullopt;
}

void DB::write_sstable(const MemTable& mem) {
    uint64_t id = next_id_++;
    SSTableBuilder b(sst_path(id), mem.size(), opts_.bloom_bits_per_key, opts_.sync_files);
    for (const auto& [k, e] : mem) b.add(k, e);
    if (b.finish() > 0) tables_.insert(tables_.begin(), SSTable::open(sst_path(id)));
}

void DB::flush() { flush_memtable(); }

void DB::flush_memtable() {
    if (mem_.empty()) return;
    // 1. Freeze the active memtable; start a new memtable AND a new WAL for incoming writes.
    imm_ = std::make_unique<MemTable>(std::move(mem_));
    mem_ = MemTable();
    std::string old_wal = wal_->path();
    wal_ = std::make_unique<Wal>(wal_path(next_id_++), opts_.sync_wal);
    // 2. Write the frozen table to disk as an SSTable.
    write_sstable(*imm_);
    // 3. Only now is the old WAL redundant.
    imm_.reset();
    fs::remove(old_wal);
    ++flushes_;
    maybe_compact();
}

// Size-tiered policy: walk the newest tables; keep extending the run while the next
// table is no bigger than everything newer combined. If the run reaches the trigger
// length, those similar-sized tables get merged. Big old tables are left alone.
size_t DB::pick_compaction() const {
    uint64_t sum = 0;
    size_t run = 0;
    for (size_t i = 0; i < tables_.size(); ++i) {
        uint64_t sz = tables_[i]->file_size();
        if (i > 0 && sz > sum) break;
        sum += sz;
        ++run;
    }
    return run >= opts_.compaction_trigger ? run : 0;
}

void DB::maybe_compact() {
    while (size_t n = pick_compaction()) compact_prefix(n);
}

void DB::compact() {
    flush_memtable();
    if (!tables_.empty()) compact_prefix(tables_.size());
}

// Merge the n NEWEST tables into one. Contiguity matters: the merged table takes their
// place in the newest-first order, so lookups still see the right version of each key.
void DB::compact_prefix(size_t n) {
    // A tombstone may only be dropped if no OLDER table could still hold a value it hides.
    // That is guaranteed only when the oldest table is part of this merge.
    const bool drop_tombstones = (n == tables_.size());

    uint64_t id = next_id_++;
    std::vector<std::shared_ptr<SSTable>> inputs(tables_.begin(), tables_.begin() + static_cast<long>(n));
    uint64_t upper_bound_keys = 0;
    for (auto& t : inputs) upper_bound_keys += t->entry_count();

    uint64_t written = 0;
    {
        std::vector<std::unique_ptr<SSTable::Iterator>> its;
        for (auto& t : inputs) its.push_back(t->new_iterator());  // its[0] = newest

        // K-way merge with a min-heap ordered by (key, source index). For equal keys the
        // newest source (smallest index) pops first and wins; older versions are skipped.
        struct Item { const std::string* key; size_t idx; };
        auto cmp = [](const Item& a, const Item& b) {
            int c = a.key->compare(*b.key);
            return c != 0 ? c > 0 : a.idx > b.idx;
        };
        std::priority_queue<Item, std::vector<Item>, decltype(cmp)> pq(cmp);
        for (size_t i = 0; i < its.size(); ++i)
            if (its[i]->valid()) pq.push({&its[i]->key(), i});

        SSTableBuilder out(sst_path(id), upper_bound_keys, opts_.bloom_bits_per_key, opts_.sync_files);
        auto advance = [&](size_t i) {
            its[i]->next();
            if (its[i]->valid()) pq.push({&its[i]->key(), i});
        };
        while (!pq.empty()) {
            Item top = pq.top();
            pq.pop();
            std::string key = *top.key;
            Entry e = its[top.idx]->entry();
            advance(top.idx);
            while (!pq.empty() && *pq.top().key == key) {  // drop shadowed older versions
                size_t i = pq.top().idx;
                pq.pop();
                advance(i);
            }
            if (e.deleted && drop_tombstones) continue;
            out.add(key, e);
        }
        written = out.finish();
    }

    // Install the result, then delete the inputs. The new file is fully on disk before any
    // input is removed, so a crash in between leaves redundant (still correct) data.
    tables_.erase(tables_.begin(), tables_.begin() + static_cast<long>(n));
    if (written > 0) tables_.insert(tables_.begin(), SSTable::open(sst_path(id)));
    for (auto& t : inputs) fs::remove(t->path());
    ++compactions_;
}

Stats DB::stats() const {
    Stats s;
    s.sstables = tables_.size();
    for (auto& t : tables_) s.sstable_entries += t->entry_count();
    s.bloom_skips = bloom_skips_;
    s.table_probes = table_probes_;
    s.flushes = flushes_;
    s.compactions = compactions_;
    return s;
}

}  // namespace lsm
