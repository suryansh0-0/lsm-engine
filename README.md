# LSM-Tree Key-Value Storage Engine (C++17)

A small LevelDB-style storage engine: memtable, immutable memtable, write-ahead log,
SSTables with sparse index + Bloom filter, tombstones, and size-tiered compaction (K-way merge).

## Build and run
```
cmake -S . -B build && cmake --build build
./build/lsm_tests      # unit tests + random-vs-std::map oracle + kill -9 crash recovery
./build/lsm_bench      # throughput and Bloom filter effect
./build/lsm_cli        # interactive shell (try: put a 1, get a, flush, stats, compact)
```
No CMake? `g++ -std=c++17 -Iinclude src/*.cpp tests/test_lsm.cpp -o lsm_tests`

## Architecture
```
 put/del ─► WAL (append, CRC per record) ─► MemTable (std::map, tombstones for deletes)
                                              │ full (>= memtable_bytes)
                                              ▼
                                  immutable MemTable ─► SSTable file (000007.sst)
                                              │
 get ─► MemTable ─► immutable ─► SSTables newest→oldest  (Bloom filter skips most files)
                                              │ too many similar-sized tables
                                              ▼
                            compaction: K-way merge, newest version wins, tombstones dropped when safe
```

## On-disk formats
**WAL record:** `[u32 crc][u32 len][u8 type][u32 klen][key][u32 vlen][value]` (CRC covers everything after `len`)

**SSTable:** `data records | sparse index (every 16th key + offset) | bloom filter | 48-byte footer`
(record = `[u32 klen][key][u8 type][u32 vlen][value]`; footer = index/bloom offsets+sizes, entry count, magic)

## Key design decisions
- **Write path:** WAL first, then memtable, so an acknowledged write survives a process crash.
- **Flush order:** new WAL + new memtable are created *before* the old one is written out; the old WAL is deleted only after the SSTable is safely renamed into place. A crash at any point leaves recoverable state.
- **Atomic SSTables:** written to `.tmp`, fsynced, then renamed. Stray `.tmp` files are deleted on startup.
- **Recovery:** replay every WAL (oldest first), flush the result to an SSTable, start a fresh WAL. A torn or corrupt WAL tail is detected by CRC and ignored.
- **Tombstones** hide older values. They are dropped only when the merge includes the oldest table.
- **Compaction** merges the newest run of similar-sized tables (contiguous, to preserve newest-first ordering).

## Limitations (deliberate, to keep scope small)
- Single-threaded: one caller at a time. Flush and compaction run synchronously, so the immutable
  memtable exists only while a flush is in progress (reads still consult it).
- No MANIFEST: tables are discovered by scanning the directory for `NNNNNN.sst`, ordered by number.
- No range scans or iterators in the public API, no compression, no block cache.
- `sync_wal=false` (default) survives process crashes, not power loss. Set `sync_wal=true` for the latter.
