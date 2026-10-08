#pragma once
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). Detects torn/corrupt records.
#include <array>
#include <cstddef>
#include <cstdint>

namespace lsm {

inline uint32_t crc32(const char* data, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace lsm
