#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <tuple>

#include "bloom.hpp"
#include "coding.hpp"
#include "crc32.hpp"
#include "db.hpp"
#include "memtable.hpp"
#include "sstable.hpp"
#include "wal.hpp"

using namespace lsm;
namespace fs = std::filesystem;

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #c "\n"; \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)

struct TempDir {
    std::string path;
    TempDir() {
        char t[] = "/tmp/lsmtestXXXXXX";
        if (!mkdtemp(t)) std::abort();
        path = t;
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

static std::string key_of(int i) {
    char b[32];
    std::snprintf(b, sizeof b, "key%08d", i);
    return b;
}
static std::string val_of(int i) { return "value-" + std::to_string(i) + std::string(40, 'x'); }

static void test_coding_and_crc() {
    std::string s;
    put_u32(s, 0xDEADBEEF);
    put_u64(s, 0x0123456789ABCDEFULL);
    put_bytes(s, "hello");
    BufReader r(s.data(), s.size());
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0123456789ABCDEFULL);
    CHECK(r.bytes() == "hello");
    CHECK(r.remaining() == 0);
    bool threw = false;
    try { r.u32(); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(crc32("123456789", 9) == 0xCBF43926u);  // standard CRC-32 check value
}

static void test_bloom() {
    Bloom b(10000, 10);
    for (int i = 0; i < 10000; ++i) b.add("present-" + std::to_string(i));
    for (int i = 0; i < 10000; ++i) CHECK(b.may_contain("present-" + std::to_string(i)));  // no false negatives
    int fp = 0;
    for (int i = 0; i < 10000; ++i) fp += b.may_contain("absent-" + std::to_string(i));
    CHECK(fp < 300);  // theory: ~1% at 10 bits/key
    Bloom b2 = Bloom::deserialize(b.serialize());  // round-trips
    for (int i = 0; i < 10000; ++i) CHECK(b2.may_contain("present-" + std::to_string(i)));
    CHECK(Bloom().may_contain("anything"));  // disabled filter never excludes
}

static void test_memtable() {
    MemTable m;
    m.put("a", "1");
    m.put("b", "2");
    m.put("a", "3");
    CHECK(m.find("a")->value == "3");
    CHECK(m.size() == 2);
    m.del("a");
    CHECK(m.find("a") != nullptr && m.find("a")->deleted);  // tombstone, not erased
    CHECK(m.find("zzz") == nullptr);
}

using Rec = std::tuple<bool, std::string, std::string>;

static std::pair<std::vector<Rec>, Wal::ReplayResult> replay_all(const std::string& p) {
    std::vector<Rec> got;
    auto r = Wal::replay(p, [&](bool d, std::string k, std::string v) { got.emplace_back(d, k, v); });
    return {got, r};
}

static void test_wal() {
    TempDir d;
    std::string p = d.path + "/w.log";
    std::vector<Rec> exp;
    auto write_all_records = [&] {
        Wal w(p, false);
        for (int i = 0; i < 10; ++i) {
            bool del = (i % 4 == 3);
            Rec r{del, "key" + std::to_string(i), del ? "" : "value" + std::to_string(i)};
            w.append(std::get<0>(r), std::get<1>(r), std::get<2>(r));
            exp.push_back(r);
        }
    };
    write_all_records();
    auto full = replay_all(p);
    CHECK(full.first == exp);
    CHECK(full.second.clean && full.second.records == 10);

    // Torn tail: last record half-written.
    fs::resize_file(p, fs::file_size(p) - 3);
    auto torn = replay_all(p);
    CHECK(torn.first.size() == 9);
    CHECK(!torn.second.clean);
    CHECK(std::equal(torn.first.begin(), torn.first.end(), exp.begin()));

    // Corruption in the middle: replay stops, and everything it returned is a correct prefix.
    fs::remove(p);
    exp.clear();
    write_all_records();
    {
        std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(fs::file_size(p) / 2));
        char c = 0x7f;
        f.write(&c, 1);
    }
    auto bad = replay_all(p);
    CHECK(!bad.second.clean);
    CHECK(bad.first.size() < 10);
    CHECK(std::equal(bad.first.begin(), bad.first.end(), exp.begin()));
}

static void test_sstable() {
    TempDir d;
    std::string p = d.path + "/000001.sst";
    {
        SSTableBuilder b(p, 1000, 10, true);
        for (int i = 0; i < 1000; ++i) {
            Entry e;
            e.deleted = (i % 7 == 0);
            e.value = e.deleted ? "" : val_of(i);
            b.add(key_of(i), e);
        }
        CHECK(b.finish() == 1000);
    }
    CHECK(!fs::exists(p + ".tmp"));  // temp file was renamed away
    auto t = SSTable::open(p);
    CHECK(t->entry_count() == 1000);
    for (int i = 0; i < 1000; ++i) {
        Entry e;
        CHECK(t->may_contain(key_of(i)));
        CHECK(t->get(key_of(i), e));
        CHECK(e.deleted == (i % 7 == 0));
        if (!e.deleted) CHECK(e.value == val_of(i));
    }
    Entry e;
    CHECK(!t->get("a", e));                 // before first key
    CHECK(!t->get("zzz", e));               // after last key
    CHECK(!t->get("key00000005x", e));      // between keys
    auto it = t->new_iterator();
    int n = 0;
    for (; it->valid(); it->next(), ++n) CHECK(it->key() == key_of(n));
    CHECK(n == 1000);

    // Out-of-order keys are rejected.
    SSTableBuilder b2(d.path + "/000002.sst", 2, 10, false);
    b2.add("b", Entry{false, "1"});
    bool threw = false;
    try { b2.add("a", Entry{false, "2"}); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
}

static Options small_opts() {
    Options o;
    o.memtable_bytes = 4096;
    o.sync_files = false;
    return o;
}

static void test_db_basic_and_reopen() {
    TempDir d;
    {
        auto db = DB::open(d.path, small_opts());
        db->put("a", "1");
        db->put("b", "2");
        db->put("a", "3");
        db->del("b");
        CHECK(db->get("a") == "3");
        CHECK(!db->get("b"));
        CHECK(!db->get("never"));
    }  // no flush: durability comes from the WAL alone
    {
        auto db = DB::open(d.path, small_opts());
        CHECK(db->get("a") == "3");
        CHECK(!db->get("b"));
        db->flush();
        db->put("c", "9");
    }
    {
        auto db = DB::open(d.path, small_opts());
        CHECK(db->get("a") == "3");
        CHECK(db->get("c") == "9");
    }
}

static void test_tombstone_shadowing_and_drop() {
    TempDir d;
    auto db = DB::open(d.path, small_opts());
    db->put("k", "v1"); db->flush();
    db->put("k", "v2"); db->flush();
    CHECK(db->get("k") == "v2");  // newest table wins
    db->del("k"); db->flush();
    CHECK(!db->get("k"));         // tombstone in newest table hides older values
    CHECK(db->stats().sstables == 3);
    db->compact();
    CHECK(!db->get("k"));
    CHECK(db->stats().sstables == 0);  // full merge: tombstone and the values it hid are all gone
}

static void test_leftover_tmp_ignored() {
    TempDir d;
    { auto db = DB::open(d.path, small_opts()); db->put("a", "1"); db->flush(); }
    { std::ofstream f(d.path + "/000099.sst.tmp"); f << "garbage from a crashed flush"; }
    auto db = DB::open(d.path, small_opts());
    CHECK(db->get("a") == "1");
    CHECK(!fs::exists(d.path + "/000099.sst.tmp"));
}

static void test_compaction_bounds_tables() {
    TempDir d;
    Options o = small_opts();
    auto db = DB::open(d.path, o);
    for (int i = 0; i < 5000; ++i) db->put(key_of(i), val_of(i));
    Stats s = db->stats();
    CHECK(s.flushes > 20);
    CHECK(s.compactions > 0);
    CHECK(s.sstables < 15);  // without compaction there would be 100+
    for (int i = 0; i < 5000; ++i) CHECK(db->get(key_of(i)) == val_of(i));
}

// The key test: random operations checked against std::map, with restarts and compactions.
static void test_random_oracle() {
    TempDir d;
    Options o = small_opts();
    auto db = DB::open(d.path, o);
    std::map<std::string, std::string> oracle;
    std::mt19937_64 rng(12345);
    for (int op = 0; op < 30000; ++op) {
        std::string k = "k" + std::to_string(rng() % 400);
        int r = static_cast<int>(rng() % 100);
        if (r < 55) {
            std::string v(rng() % 100, static_cast<char>('a' + rng() % 26));
            db->put(k, v);
            oracle[k] = v;
        } else if (r < 75) {
            db->del(k);
            oracle.erase(k);
        } else {
            auto got = db->get(k);
            auto it = oracle.find(k);
            if (it == oracle.end()) CHECK(!got); else CHECK(got && *got == it->second);
        }
        if (op % 5000 == 4999) { db.reset(); db = DB::open(d.path, o); }  // simulated restart
        if (op == 15000) db->compact();
    }
    auto verify = [&] {
        for (int i = 0; i < 400; ++i) {
            std::string k = "k" + std::to_string(i);
            auto got = db->get(k);
            auto it = oracle.find(k);
            if (it == oracle.end()) CHECK(!got); else CHECK(got && *got == it->second);
        }
    };
    verify();
    db->compact();
    verify();
    CHECK(db->stats().sstable_entries == oracle.size());  // after full compaction: exactly the live keys
}

// Kill -9 a writer mid-run; every write it acknowledged must survive.
static void test_crash_recovery() {
    for (int round = 0; round < 8; ++round) {
        TempDir d;
        int fds[2];
        CHECK(pipe(fds) == 0);
        Options o = small_opts();
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            auto db = DB::open(d.path, o);
            for (int32_t i = 0;; ++i) {
                db->put(key_of(i), val_of(i));
                if (write(fds[1], &i, 4) != 4) _exit(1);  // "acknowledge" after put returns
            }
        }
        close(fds[1]);
        fcntl(fds[0], F_SETFL, O_NONBLOCK);
        int32_t last = -1, v;
        auto drain = [&] { while (read(fds[0], &v, 4) == 4) last = v; };
        // Keep draining so the writer never blocks on a full pipe; kill it at a varying moment.
        auto start = std::chrono::steady_clock::now();
        auto run_for = std::chrono::milliseconds(150 + round * 53);
        while (std::chrono::steady_clock::now() - start < run_for) { drain(); usleep(500); }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        drain();
        close(fds[0]);
        CHECK(last >= 0);
        auto db = DB::open(d.path, o);
        for (int i = 0; i <= last; ++i) CHECK(db->get(key_of(i)) == val_of(i));
        std::cout << "    round " << round << ": " << (last + 1) << " acknowledged writes recovered\n";
    }
}

int main() {
    struct T { const char* name; void (*fn)(); };
    T tests[] = {
        {"coding + crc32", test_coding_and_crc},
        {"bloom filter", test_bloom},
        {"memtable", test_memtable},
        {"wal (torn tail, corruption)", test_wal},
        {"sstable (build/get/iterate)", test_sstable},
        {"db basic + reopen", test_db_basic_and_reopen},
        {"tombstones + compaction drop", test_tombstone_shadowing_and_drop},
        {"leftover .tmp ignored", test_leftover_tmp_ignored},
        {"compaction bounds table count", test_compaction_bounds_tables},
        {"random ops vs std::map oracle", test_random_oracle},
        {"crash recovery (kill -9)", test_crash_recovery},
    };
    for (auto& t : tests) {
        std::cout << "[ RUN  ] " << t.name << std::endl;
        t.fn();
        std::cout << "[  OK  ] " << t.name << std::endl;
    }
    std::cout << "all tests passed\n";
}
