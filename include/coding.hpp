#pragma once
// Serialization helpers: fixed-width little-endian integers and
// length-prefixed byte strings. Every on-disk format here is built from these.
#include <cstdint>
#include <stdexcept>
#include <string>

namespace lsm {

inline void put_u32(std::string& dst, uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    dst.append(b, 4);
}

inline void put_u64(std::string& dst, uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    dst.append(b, 8);
}

// [u32 length][bytes]
inline void put_bytes(std::string& dst, const std::string& s) {
    put_u32(dst, static_cast<uint32_t>(s.size()));
    dst.append(s);
}

inline uint32_t decode_u32(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    return v;
}

inline uint64_t decode_u64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    return v;
}

// Bounds-checked cursor over a byte buffer. Throws on truncated/corrupt input
// instead of reading past the end.
class BufReader {
public:
    BufReader(const char* data, size_t size) : p_(data), end_(data + size) {}
    uint8_t u8() { need(1); return static_cast<uint8_t>(*p_++); }
    uint32_t u32() { need(4); uint32_t v = decode_u32(p_); p_ += 4; return v; }
    uint64_t u64() { need(8); uint64_t v = decode_u64(p_); p_ += 8; return v; }
    std::string bytes() {
        uint32_t n = u32();
        need(n);
        std::string s(p_, n);
        p_ += n;
        return s;
    }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

private:
    void need(size_t n) const {
        if (remaining() < n) throw std::runtime_error("corrupt data: truncated buffer");
    }
    const char* p_;
    const char* end_;
};

}  // namespace lsm
