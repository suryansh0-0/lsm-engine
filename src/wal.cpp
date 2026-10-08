#include "wal.hpp"

#include <fstream>
#include <iterator>

#include "coding.hpp"
#include "crc32.hpp"
#include "fileutil.hpp"

namespace lsm {
namespace {
constexpr uint8_t kPut = 1;
constexpr uint8_t kDelete = 2;
}  // namespace

Wal::Wal(const std::string& path, bool sync) : path_(path), sync_(sync) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) throw_errno("open wal " + path);
}

Wal::~Wal() {
    if (fd_ >= 0) ::close(fd_);
}

void Wal::append(bool deleted, const std::string& key, const std::string& value) {
    std::string payload;
    payload.push_back(static_cast<char>(deleted ? kDelete : kPut));
    put_bytes(payload, key);
    put_bytes(payload, value);

    std::string rec;
    put_u32(rec, crc32(payload.data(), payload.size()));
    put_u32(rec, static_cast<uint32_t>(payload.size()));
    rec += payload;

    write_all(fd_, rec.data(), rec.size());  // one write() per record
    // write() reaches the OS page cache: survives a process crash (kill -9).
    // fsync() reaches the disk: survives power loss. It is slow, so it is optional.
    if (sync_ && ::fsync(fd_) != 0) throw_errno("fsync wal");
}

Wal::ReplayResult Wal::replay(const std::string& path, const Visitor& fn) {
    ReplayResult res;
    std::ifstream in(path, std::ios::binary);
    if (!in) return res;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    size_t pos = 0;
    while (pos < data.size()) {
        if (data.size() - pos < 8) { res.clean = false; break; }  // torn header
        uint32_t crc = decode_u32(data.data() + pos);
        uint32_t len = decode_u32(data.data() + pos + 4);
        if (len > data.size() - pos - 8) { res.clean = false; break; }  // torn payload
        const char* payload = data.data() + pos + 8;
        if (crc32(payload, len) != crc) { res.clean = false; break; }   // corruption
        try {
            BufReader r(payload, len);
            uint8_t type = r.u8();
            if (type != kPut && type != kDelete) throw std::runtime_error("bad type");
            std::string key = r.bytes();
            std::string value = r.bytes();
            fn(type == kDelete, std::move(key), std::move(value));
        } catch (const std::exception&) {
            res.clean = false;
            break;
        }
        pos += 8 + len;
        ++res.records;
    }
    return res;
}

}  // namespace lsm
