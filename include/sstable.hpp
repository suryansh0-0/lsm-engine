#pragma once
// SSTable: an immutable file of key-sorted records.
//
// File layout:
//   [data records ...............]   record = [u32 klen][key][u8 type][u32 vlen][value]
//   [sparse index ................]   every 16th record: [u32 klen][key][u64 file offset]
//   [bloom filter ................]
//   [footer, 48 bytes]  index_off, index_size, bloom_off, bloom_size, entry_count, magic (all u64)
//
// Lookup: bloom check -> binary search the sparse index -> scan <=16 records.
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bloom.hpp"
#include "memtable.hpp"

namespace lsm {

constexpr uint64_t kSSTableMagic = 0x4C534D5353543031ULL;  // "LSMSST01"
constexpr size_t kFooterSize = 48;
constexpr size_t kIndexInterval = 16;

// Writes to "<path>.tmp", then fsync + atomic rename to "<path>". A crash mid-write
// can therefore never leave a half-written file under the real name.
class SSTableBuilder {
public:
    SSTableBuilder(std::string final_path, size_t expected_keys, int bloom_bits_per_key, bool sync);
    ~SSTableBuilder();
    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;

    void add(const std::string& key, const Entry& e);  // keys must be strictly ascending
    uint64_t finish();  // returns entry count; 0 means nothing was written (no file created)

private:
    std::string final_path_, tmp_path_;
    bool sync_;
    std::ofstream out_;
    Bloom bloom_;
    std::vector<std::pair<std::string, uint64_t>> index_;
    std::string last_key_;
    uint64_t offset_ = 0, count_ = 0;
    bool finished_ = false;
};

class SSTable {
public:
    static std::shared_ptr<SSTable> open(const std::string& path);

    bool may_contain(const std::string& key) const { return bloom_.may_contain(key); }
    // true if the key has a record here (the record may be a tombstone).
    bool get(const std::string& key, Entry& out);

    const std::string& path() const { return path_; }
    uint64_t entry_count() const { return entry_count_; }
    uint64_t file_size() const { return file_size_; }

    // Sequential scan used by compaction.
    class Iterator {
    public:
        explicit Iterator(const SSTable& t);
        bool valid() const { return valid_; }
        const std::string& key() const { return key_; }
        const Entry& entry() const { return entry_; }
        void next();

    private:
        std::ifstream in_;
        uint64_t pos_ = 0, end_ = 0;
        std::string key_;
        Entry entry_;
        bool valid_ = false;
    };
    std::unique_ptr<Iterator> new_iterator() const { return std::make_unique<Iterator>(*this); }

private:
    SSTable() = default;

    std::string path_;
    std::ifstream file_;
    std::vector<std::pair<std::string, uint64_t>> index_;
    Bloom bloom_;
    uint64_t data_end_ = 0, entry_count_ = 0, file_size_ = 0;
};

}  // namespace lsm
