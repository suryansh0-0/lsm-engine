#include "sstable.hpp"

#include <algorithm>
#include <filesystem>

#include "coding.hpp"
#include "fileutil.hpp"

namespace fs = std::filesystem;

namespace lsm {
namespace {

constexpr uint32_t kMaxField = 1u << 28;  // sanity cap so corrupt lengths can't trigger huge allocations

void encode_record(std::string& dst, const std::string& key, const Entry& e) {
    put_bytes(dst, key);
    dst.push_back(e.deleted ? 1 : 0);
    put_bytes(dst, e.value);
}

bool read_record(std::istream& in, std::string& key, Entry& e) {
    char b[4];
    if (!in.read(b, 4)) return false;
    uint32_t klen = decode_u32(b);
    if (klen > kMaxField) return false;
    key.resize(klen);
    if (klen && !in.read(&key[0], klen)) return false;
    char t;
    if (!in.get(t)) return false;
    e.deleted = (t != 0);
    if (!in.read(b, 4)) return false;
    uint32_t vlen = decode_u32(b);
    if (vlen > kMaxField) return false;
    e.value.resize(vlen);
    if (vlen && !in.read(&e.value[0], vlen)) return false;
    return true;
}

uint64_t record_size(const std::string& key, const Entry& e) { return 4 + key.size() + 1 + 4 + e.value.size(); }

}  // namespace

// ---------------- builder ----------------

SSTableBuilder::SSTableBuilder(std::string final_path, size_t expected_keys, int bits, bool sync)
    : final_path_(std::move(final_path)), tmp_path_(final_path_ + ".tmp"), sync_(sync), bloom_(expected_keys, bits) {
    out_.open(tmp_path_, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + tmp_path_);
}

SSTableBuilder::~SSTableBuilder() {
    if (!finished_) {
        out_.close();
        std::error_code ec;
        fs::remove(tmp_path_, ec);
    }
}

void SSTableBuilder::add(const std::string& key, const Entry& e) {
    if (count_ > 0 && key <= last_key_) throw std::logic_error("SSTable keys must be strictly ascending");
    if (count_ % kIndexInterval == 0) index_.emplace_back(key, offset_);
    std::string rec;
    encode_record(rec, key, e);
    out_.write(rec.data(), static_cast<std::streamsize>(rec.size()));
    offset_ += rec.size();
    bloom_.add(key);
    last_key_ = key;
    ++count_;
}

uint64_t SSTableBuilder::finish() {
    finished_ = true;
    if (count_ == 0) {
        out_.close();
        std::error_code ec;
        fs::remove(tmp_path_, ec);
        return 0;
    }
    uint64_t index_off = offset_;
    std::string index;
    for (const auto& [k, off] : index_) { put_bytes(index, k); put_u64(index, off); }
    std::string bloom = bloom_.serialize();

    std::string footer;
    put_u64(footer, index_off);
    put_u64(footer, index.size());
    put_u64(footer, index_off + index.size());
    put_u64(footer, bloom.size());
    put_u64(footer, count_);
    put_u64(footer, kSSTableMagic);

    out_.write(index.data(), static_cast<std::streamsize>(index.size()));
    out_.write(bloom.data(), static_cast<std::streamsize>(bloom.size()));
    out_.write(footer.data(), static_cast<std::streamsize>(footer.size()));
    out_.flush();
    out_.close();
    if (!out_) throw std::runtime_error("write failed for " + tmp_path_);

    if (sync_) fsync_path(tmp_path_);
    fs::rename(tmp_path_, final_path_);  // atomic: file appears complete or not at all
    if (sync_) fsync_path(fs::path(final_path_).parent_path().string());
    return count_;
}

// ---------------- reader ----------------

std::shared_ptr<SSTable> SSTable::open(const std::string& path) {
    std::shared_ptr<SSTable> t(new SSTable());
    t->path_ = path;
    t->file_.open(path, std::ios::binary);
    if (!t->file_) throw std::runtime_error("cannot open " + path);

    t->file_.seekg(0, std::ios::end);
    uint64_t size = static_cast<uint64_t>(t->file_.tellg());
    if (size < kFooterSize) throw std::runtime_error("sstable too small: " + path);
    t->file_size_ = size;

    char fbuf[kFooterSize];
    t->file_.seekg(static_cast<std::streamoff>(size - kFooterSize));
    if (!t->file_.read(fbuf, kFooterSize)) throw std::runtime_error("cannot read footer: " + path);
    BufReader fr(fbuf, kFooterSize);
    uint64_t index_off = fr.u64(), index_size = fr.u64(), bloom_off = fr.u64(), bloom_size = fr.u64();
    t->entry_count_ = fr.u64();
    if (fr.u64() != kSSTableMagic) throw std::runtime_error("bad magic: " + path);
    if (index_off + index_size > size - kFooterSize || bloom_off + bloom_size > size - kFooterSize)
        throw std::runtime_error("bad footer offsets: " + path);
    t->data_end_ = index_off;

    std::string ibuf(index_size, '\0');
    t->file_.seekg(static_cast<std::streamoff>(index_off));
    if (index_size && !t->file_.read(&ibuf[0], static_cast<std::streamsize>(index_size)))
        throw std::runtime_error("cannot read index: " + path);
    BufReader ir(ibuf.data(), ibuf.size());
    while (ir.remaining() > 0) {
        std::string k = ir.bytes();
        uint64_t off = ir.u64();
        t->index_.emplace_back(std::move(k), off);
    }

    std::string bbuf(bloom_size, '\0');
    t->file_.seekg(static_cast<std::streamoff>(bloom_off));
    if (bloom_size && !t->file_.read(&bbuf[0], static_cast<std::streamsize>(bloom_size)))
        throw std::runtime_error("cannot read bloom: " + path);
    t->bloom_ = Bloom::deserialize(bbuf);
    return t;
}

bool SSTable::get(const std::string& key, Entry& out) {
    // Last index entry whose key <= target (binary search).
    auto it = std::upper_bound(index_.begin(), index_.end(), key,
                               [](const std::string& k, const std::pair<std::string, uint64_t>& p) { return k < p.first; });
    if (it == index_.begin()) return false;  // target sorts before the first key in this file
    --it;

    uint64_t pos = it->second;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pos));
    std::string k;
    Entry e;
    while (pos < data_end_) {  // scan at most kIndexInterval records
        if (!read_record(file_, k, e)) throw std::runtime_error("corrupt sstable record: " + path_);
        pos += record_size(k, e);
        if (k == key) { out = std::move(e); return true; }
        if (k > key) return false;  // sorted: we've gone past it
    }
    return false;
}

SSTable::Iterator::Iterator(const SSTable& t) : in_(t.path_, std::ios::binary), end_(t.data_end_) {
    if (!in_) throw std::runtime_error("cannot open " + t.path_);
    next();
}

void SSTable::Iterator::next() {
    if (pos_ >= end_) { valid_ = false; return; }
    if (!read_record(in_, key_, entry_)) throw std::runtime_error("corrupt sstable during scan");
    pos_ += record_size(key_, entry_);
    valid_ = true;
}

}  // namespace lsm
