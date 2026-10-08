#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "memtable.hpp"
#include "sstable.hpp"
#include "wal.hpp"

namespace lsm {

struct Options {
    size_t memtable_bytes = 1 << 20;   // flush the memtable to an SSTable at this size
    size_t compaction_trigger = 4;     // merge when this many similar-sized tables pile up
    int bloom_bits_per_key = 10;       // 0 disables bloom filters
    bool sync_wal = false;             // fsync the WAL on every write (durable vs power loss, slower)
    bool sync_files = true;            // fsync SSTables and directory on flush/compaction
};

struct Stats {
    size_t sstables = 0;
    uint64_t sstable_entries = 0;
    uint64_t bloom_skips = 0;   // table lookups avoided by a bloom filter
    uint64_t table_probes = 0;  // tables actually searched on disk
    uint64_t flushes = 0;
    uint64_t compactions = 0;
};

class DB {
public:
    static std::unique_ptr<DB> open(const std::string& dir, const Options& opts = Options());

    void put(const std::string& key, const std::string& value);
    void del(const std::string& key);
    std::optional<std::string> get(const std::string& key);

    void flush();    // force memtable -> SSTable
    void compact();  // flush, then merge ALL tables into one (drops tombstones)
    Stats stats() const;

private:
    DB(std::string dir, Options opts) : dir_(std::move(dir)), opts_(opts) {}

    void recover();
    void flush_memtable();
    void write_sstable(const MemTable& mem);
    void maybe_compact();
    size_t pick_compaction() const;
    void compact_prefix(size_t n);
    std::string sst_path(uint64_t id) const;
    std::string wal_path(uint64_t id) const;

    std::string dir_;
    Options opts_;
    MemTable mem_;                      // active: receives writes
    std::unique_ptr<MemTable> imm_;     // immutable: being flushed, still readable
    std::unique_ptr<Wal> wal_;
    std::vector<std::shared_ptr<SSTable>> tables_;  // newest first
    uint64_t next_id_ = 1;
    uint64_t bloom_skips_ = 0, table_probes_ = 0, flushes_ = 0, compactions_ = 0;
};

}  // namespace lsm
