#pragma once
// Write-Ahead Log. Every mutation is appended here BEFORE it is applied to the memtable,
// so a crash can be repaired by replaying the log.
//
// Record: [u32 crc][u32 payload_len][payload]
// Payload: [u8 type (1=put, 2=delete)][u32 klen][key][u32 vlen][value]
// The CRC covers the payload. A torn (half-written) last record fails the check
// and replay stops there.
#include <functional>
#include <string>

namespace lsm {

class Wal {
public:
    Wal(const std::string& path, bool sync_every_write);
    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    void append(bool deleted, const std::string& key, const std::string& value);
    const std::string& path() const { return path_; }

    struct ReplayResult {
        size_t records = 0;  // valid records applied
        bool clean = true;   // false if replay stopped early on a bad/truncated record
    };
    using Visitor = std::function<void(bool deleted, std::string key, std::string value)>;
    static ReplayResult replay(const std::string& path, const Visitor& fn);

private:
    std::string path_;
    int fd_ = -1;
    bool sync_;
};

}  // namespace lsm
