#pragma once
// In-memory sorted write buffer. A delete is stored as a TOMBSTONE entry, not erased,
// so it can shadow older values that already live in SSTables on disk.
#include <map>
#include <string>

namespace lsm {

struct Entry {
    bool deleted = false;  // true = tombstone
    std::string value;
};

class MemTable {
public:
    void put(const std::string& key, const std::string& value) { set(key, Entry{false, value}); }
    void del(const std::string& key) { set(key, Entry{true, {}}); }

    // nullptr = key never seen here. A non-null Entry may still be a tombstone.
    const Entry* find(const std::string& key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }

    size_t approx_bytes() const { return bytes_; }
    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
    std::map<std::string, Entry>::const_iterator begin() const { return map_.begin(); }
    std::map<std::string, Entry>::const_iterator end() const { return map_.end(); }

private:
    static constexpr size_t kOverhead = 32;  // rough per-node bookkeeping cost

    void set(const std::string& key, Entry e) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            bytes_ = bytes_ - it->second.value.size() + e.value.size();
            it->second = std::move(e);
        } else {
            bytes_ += key.size() + e.value.size() + kOverhead;
            map_.emplace(key, std::move(e));
        }
    }

    std::map<std::string, Entry> map_;
    size_t bytes_ = 0;
};

}  // namespace lsm
