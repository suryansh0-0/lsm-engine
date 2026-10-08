# Study Guide: understand this project in one week

Rule: for each day, read the concept first (60 min), then read the code, then run the tests and
answer the "can you explain" questions out loud WITHOUT looking. If you can't, reread.

## Reading order
| # | File | Concepts it uses |
|---|------|------------------|
| 1 | `coding.hpp` | Fixed-width ints, little-endian, length-prefixed strings, bounds-checked reading |
| 2 | `crc32.hpp` | Checksums: detecting corruption |
| 3 | `memtable.hpp` | Sorted in-memory buffer, tombstones |
| 4 | `wal.cpp` | Write-ahead log, durability, `write` vs `fsync`, torn writes |
| 5 | `sstable.cpp` | Immutable sorted files, sparse index, binary search, atomic rename |
| 6 | `bloom.hpp` | Bloom filters, double hashing, false positives |
| 7 | `db.cpp` | Read path, flush, recovery, compaction, K-way merge |
| 8 | `tests/test_lsm.cpp` | Oracle testing, crash testing |

## 7-day plan
**Day 1: the big picture + building blocks.**
Read: *Designing Data-Intensive Applications* ch. 3 (the LSM / SSTable part) and LevelDB `doc/impl.md`.
Code: files 1-3. Run `./build/lsm_cli` and try put/get/del.
Can you explain: why is writing sequentially faster than random updates? What is a tombstone and why can't a delete just erase the key?

**Day 2: WAL and durability.**
Learn: page cache vs disk, `write()` vs `fsync()`, what a torn write is, CRC.
Code: `wal.cpp`, then `test_wal` and `test_crash_recovery`.
Can you explain: what survives `kill -9`? What survives power loss and what option controls it? Why is the CRC checked on replay?

**Day 3: SSTables.**
Learn: sparse index vs dense index, binary search, why write-to-temp-then-rename is atomic.
Code: `sstable.cpp` (builder, then `SSTable::get`), `test_sstable`.
Can you explain: how does `get` find a key using the index? Why scan at most 16 records? Why is the footer at the END of the file?

**Day 4: Bloom filters and the read path.**
Learn: Bloom filter math: false-positive rate `(1 - e^(-kn/m))^k`, optimal `k = (m/n) ln 2`.
Code: `bloom.hpp`, `DB::get`, `test_bloom`. Run `./build/lsm_bench` and read the bloom section.
Can you explain: why no false negatives? why is `get` newest-first? what does the bloom filter save?

**Day 5: flush, recovery and compaction.**
Learn: K-way merge with a min-heap, size-tiered vs leveled compaction, write amplification.
Code: `flush_memtable`, `recover`, `pick_compaction`, `compact_prefix`.
Can you explain: why is the new WAL created BEFORE the SSTable is written? When is it safe to drop a tombstone? Why must compaction merge a *contiguous newest* run?

**Day 6: testing.**
Learn: oracle (model-based) testing, fault injection.
Code: `test_random_oracle`, `test_crash_recovery`, `test_tombstone_shadowing_and_drop`. Build with `-fsanitize=address,undefined` and run.
Can you explain: why compare against `std::map`? what does the crash test prove, and what does it not?

**Day 7: interview prep and one extension.**
Do the Q&A below aloud. Then change something yourself, for example:
- add `scan(start, end)` using a merge iterator over memtable + tables
- make the compaction trigger configurable from the CLI
- add a CRC to each SSTable record and verify it on read
Commit it with a clear message. One feature you built yourself is worth more than five you read.

## Interview Q&A (answers point to code)
1. **What happens on a crash between the WAL write and the flush?** On restart `DB::recover` replays all WAL files into a memtable and flushes it. The CRC stops replay at any torn tail record.
2. **How does `get` find the newest value?** `DB::get` checks memtable, immutable memtable, then SSTables newest to oldest. The first record found wins; a tombstone means "not found".
3. **Why tombstones?** SSTables are immutable, so a delete must be recorded as data that shadows older values. It is dropped only when the compaction includes the oldest table (`drop_tombstones` in `compact_prefix`).
4. **Why can a Bloom filter give false positives but never false negatives?** Adding a key sets its k bits; bits are never cleared. A different key can happen to hit only already-set bits (false positive), but an added key's bits are always set.
5. **How are keys found inside an SSTable?** Binary search the sparse index (every 16th key) for the last entry <= target, seek to its offset, scan forward until the key or a larger key appears.
6. **What does compaction cost and save?** It rewrites data (write amplification), but it bounds the number of files a read must check and reclaims space from overwritten and deleted keys.
7. **Why is the SSTable written to `.tmp` then renamed?** `rename` is atomic, so a crash leaves either no file or a complete one. Leftover `.tmp` files are deleted on startup.
8. **Is it thread-safe?** No. It is single-threaded by design. Adding a mutex around the public API is the first extension; background compaction would be the second.
9. **What is the difference between `write` and `fsync` here?** `write` reaches the OS page cache (survives process death); `fsync` reaches the disk (survives power loss). `Options::sync_wal` toggles it.
10. **Is the immutable memtable real?** Yes, but flush is synchronous, so it exists only while a flush is running. Say so honestly; it is the correct place to introduce a background flush thread.

## Resume bullets (fill the numbers from YOUR `lsm_bench` run)
- Built an LSM-tree key-value store in C++17: memtable, immutable memtable, WAL with CRC32 recovery, SSTables with sparse index and Bloom filters, size-tiered compaction via K-way merge.
- Verified with a 30k-operation randomized test against a `std::map` oracle and `kill -9` crash-recovery tests; clean under AddressSanitizer/UBSan.
- Bloom filters cut disk-table probes on missing-key lookups by ~99% (250,000 -> ~2,000 per 50,000 lookups); sustained ~X writes/s (WAL buffered) on <your machine>.
