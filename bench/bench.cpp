#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>

#include "db.hpp"

using namespace lsm;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static std::string key_of(uint64_t i) {
    char b[32];
    std::snprintf(b, sizeof b, "key%012llu", static_cast<unsigned long long>(i));
    return b;
}
static double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }
static void report(const char* name, size_t ops, double s) {
    std::cout << std::left << std::setw(44) << name << std::right << std::setw(10) << static_cast<long>(ops / s)
              << " ops/s   (" << std::fixed << std::setprecision(2) << s << " s)\n";
}

static std::unique_ptr<DB> build(const std::string& dir, int bloom_bits, size_t n) {
    fs::remove_all(dir);
    Options o;
    o.bloom_bits_per_key = bloom_bits;
    auto db = DB::open(dir, o);
    std::mt19937_64 rng(42);
    std::string val(100, 'v');
    for (size_t i = 0; i < n; ++i) db->put(key_of(rng() % 2000000), val);  // random keys, even-numbered range
    return db;
}

int main() {
    const std::string dir = "/tmp/lsm_bench";
    const size_t N = 200000;
    std::string val(100, 'v');
    std::cout << "key=15B value=100B  memtable=1MiB  compaction_trigger=4\n\n";

    {
        fs::remove_all(dir);
        auto db = DB::open(dir);
        auto t = Clock::now();
        for (size_t i = 0; i < N; ++i) db->put(key_of(i), val);
        report("sequential writes (WAL not fsynced)", N, secs(t));
    }
    {
        fs::remove_all(dir);
        Options o;
        o.sync_wal = true;
        auto db = DB::open(dir, o);
        const size_t M = 2000;
        auto t = Clock::now();
        for (size_t i = 0; i < M; ++i) db->put(key_of(i), val);
        report("sequential writes (fsync every write)", M, secs(t));
    }
    {
        auto t = Clock::now();
        auto db = build(dir, 10, N);
        report("random writes (WAL not fsynced)", N, secs(t));
        Stats s0 = db->stats();
        std::cout << "    -> " << s0.flushes << " flushes, " << s0.compactions << " compactions, "
                  << s0.sstables << " sstables remain\n";
        std::mt19937_64 rng(7);
        t = Clock::now();
        size_t hits = 0;
        for (size_t i = 0; i < 50000; ++i) hits += db->get(key_of(rng() % 2000000)).has_value();
        report("random reads (mix of hit and miss)", 50000, secs(t));
        std::cout << "    -> " << hits << " hits\n";
    }
    std::cout << "\nBloom filter effect: 50,000 lookups of keys that do NOT exist\n";
    for (int bits : {0, 10}) {
        auto db = build(dir, bits, N);
        Stats before = db->stats();
        auto t = Clock::now();
        for (size_t i = 0; i < 50000; ++i) db->get(key_of(5000000 + i));  // outside the written range
        double s = secs(t);
        Stats after = db->stats();
        std::cout << "  bloom bits/key=" << std::setw(2) << bits << ": "
                  << std::setw(8) << static_cast<long>(50000 / s) << " ops/s, table probes="
                  << (after.table_probes - before.table_probes) << ", skipped by bloom="
                  << (after.bloom_skips - before.bloom_skips) << "  (" << after.sstables << " sstables)\n";
    }
    fs::remove_all(dir);
}
