#pragma once
// Bloom filter: "definitely not present" or "maybe present". Never a false negative.
// Uses double hashing: bit_i = (h1 + i*h2) mod m, so k hash functions cost 2 real hashes.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "coding.hpp"

namespace lsm {

class Bloom {
public:
    Bloom() = default;  // empty filter: may_contain() is always true (filter disabled)

    Bloom(size_t expected_keys, int bits_per_key) {
        if (bits_per_key <= 0) return;
        size_t nbits = std::max<size_t>(64, expected_keys * static_cast<size_t>(bits_per_key));
        bits_.assign((nbits + 7) / 8, 0);
        nbits_ = bits_.size() * 8;
        // Optimal k = (m/n) * ln2 ~= bits_per_key * 0.69
        k_ = static_cast<uint32_t>(std::clamp(static_cast<int>(bits_per_key * 0.69), 1, 30));
    }

    void add(const std::string& key) {
        if (bits_.empty()) return;
        uint64_t h1, h2;
        hashes(key, h1, h2);
        for (uint32_t i = 0; i < k_; ++i) {
            uint64_t bit = (h1 + i * h2) % nbits_;
            bits_[bit >> 3] = static_cast<uint8_t>(bits_[bit >> 3] | (1u << (bit & 7)));
        }
    }

    bool may_contain(const std::string& key) const {
        if (bits_.empty()) return true;
        uint64_t h1, h2;
        hashes(key, h1, h2);
        for (uint32_t i = 0; i < k_; ++i) {
            uint64_t bit = (h1 + i * h2) % nbits_;
            if (!(bits_[bit >> 3] & (1u << (bit & 7)))) return false;
        }
        return true;
    }

    // Layout: [u32 k][u64 nbits][bit bytes]. Empty string = disabled filter.
    std::string serialize() const {
        std::string s;
        if (bits_.empty()) return s;
        put_u32(s, k_);
        put_u64(s, nbits_);
        s.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
        return s;
    }

    static Bloom deserialize(const std::string& s) {
        Bloom b;
        if (s.empty()) return b;
        BufReader r(s.data(), s.size());
        b.k_ = r.u32();
        b.nbits_ = r.u64();
        if (b.nbits_ == 0 || b.nbits_ % 8 != 0 || b.nbits_ / 8 != r.remaining() || b.k_ == 0)
            throw std::runtime_error("corrupt bloom filter");
        b.bits_.assign(s.begin() + 12, s.end());
        return b;
    }

private:
    static uint64_t fnv1a(const std::string& s, uint64_t seed) {
        uint64_t h = 1469598103934665603ULL ^ seed;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }
    static uint64_t mix(uint64_t x) {  // murmur3 finalizer: spreads bits well
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }
    static void hashes(const std::string& key, uint64_t& h1, uint64_t& h2) {
        h1 = mix(fnv1a(key, 0));
        h2 = mix(fnv1a(key, 0x9e3779b97f4a7c15ULL)) | 1;  // odd, so the probe sequence cycles well
    }

    std::vector<uint8_t> bits_;
    uint64_t nbits_ = 0;
    uint32_t k_ = 0;
};

}  // namespace lsm
