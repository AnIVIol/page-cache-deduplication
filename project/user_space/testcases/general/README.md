
## File Layout

| File | Purpose |
|------|---------|
| `dedup_test_common.h`  | Helpers: sysfs writers, `meminfo` snapshots, deterministic pattern fill, prewarm/verify utilities. |
| `dedup_ops_common.h`   | Operation framework used by the mixed-ops tests: `op_read` / `op_write` / `op_truncate` / `op_cp`, each updating both the file and the shadow under per-file mutexes. |
| `run_all_tests.sh`     | Builds every test, runs them in order, aggregates PASS/FAIL. |
| `test_*.c`             | Individual test programs (one per scenario). |
| `multithread_write_test.c` | Stand-alone heavy concurrent-write stress test. |

---

## Tests at a Glance

The summary printed by `run_all_tests.sh` looks like:

```
===== SUMMARY =====
PASS: 1.1 single-file dedup
PASS: 1.2 cross-file dedup
PASS: 1.3 write CoW
PASS: 1.4 truncate cleanup
PASS: 2.1 concurrent readers
PASS: 3.* multithread writes
PASS: 6.5 refcount stress
PASS: 8.1 1000-file dedup
PASS: 9.3 sparse file
PASS: MIX-RW   readers+writers
PASS: MIX-RWT  readers+writers+truncate
PASS: MIX-RWC  readers+writers+cp
PASS: CHAOS    all-ops together
PASS: PHASED   ordered phase test
PASS: STORM    same-page contention
```

Each line corresponds to one program described below.

---


## Basic correctness (Tests 1.x)

### 1.1 — `test_basic_internal` *single-file dedup*
Creates one file containing many duplicated pages (only two unique page
contents repeated 32 times each). After the scanner runs, verifies that
~62 pages of `Cached:` were freed and that the file still reads back its
original content.

### 1.2 — `test_basic_cross` *cross-file dedup*
Creates 10 identical files. Confirms that exactly nine "copies" worth of
page-cache memory is reclaimed and that every file is bit-identical to the
original on read-back.

### 1.3 — `test_write_cow` *Copy-on-Write on write*
Two identical files are merged. A small write to one file must:
- modify only that file's content,
- leave the sibling unchanged,
- cause `Cached:` to grow back by one page (the new private copy).

### 1.4 — `test_truncate` *truncate cleanup*
Truncates a deduplicated file to half size and back to full. Verifies the
extension region is zero-filled, the sibling file is intact, and that the
kernel reports no warnings (`dmesg`).

---

## Concurrency primitives (Tests 2.x, 3.x)

### 2.1 — `test_concurrent_read` *many parallel readers*
16 reader threads pound 4 deduplicated files with 2000 random reads each.
Each read is verified against the deterministic pattern. Catches torn or
misrouted reads on shared survivor pages.

### 3.* — `multithread_write_test` *concurrent writes stress*
16 writer threads issue ~32000 random writes covering:
- random offsets,
- page-boundary spanning writes,
- multiple threads hitting the **same shared page** simultaneously.

After the storm, every byte of every file must match the shadow buffer.
Empirically confirms that CoW unmerge in `__filemap_get_folio` handles
the worst contention cases.

---

## Refcount and scale (Tests 6.x, 8.x)

### 6.5 — `test_refcount_stress`
50 identical files share a single survivor page (refcount = 50).
Half of them are unlinked while their fds remain open. Tests:
- reads through still-open but unlinked fds return correct data,
- linked siblings remain intact,
- on close, the `shared_cache_node` is destroyed cleanly (no slab leaks).

### 8.1 — `test_thousand_files`
Stress-scale test: 1000 identical 16 KB files are registered and merged.
The page-cache footprint should drop from ~16 MB to <100 KB. Validates
that the scanner's lists handle large reverse-map fan-out without
performance pathologies.

---

## Edge cases (Tests 9.x)

### 9.3 — `test_sparse`
A 16 MB sparse file (all zeros) with a single 1-byte write at offset 4096.
Verifies that:
- all-zero pages dedup as expected,
- the lone write triggers CoW for that single page only,
- surrounding zero pages remain shared.

---

## Mixed concurrent operations (MIX, CHAOS, PHASED, STORM)

These tests exercise multiple operations **simultaneously** on a shared
pool of deduplicated files. They use the operation framework in
`dedup_ops_common.h`, which guarantees that file mutations and the
shadow buffer stay in lock-step under per-file mutexes.

### MIX-RW — `test_mixed_rw` *readers + writers*
12 readers + 6 writers contend for the same 8 deduped files for thousands
of operations. Readers must never observe corrupt data; writers must
unmerge cleanly via the in-tree CoW hook.

### MIX-RWT — `test_mixed_rwt` *readers + writers + truncate*
Adds 2 truncate threads that randomly resize files to page-aligned offsets
in `[0, MAX]`. Stresses the path where `truncate_inode_pages_range` calls
`unmerge_shared_pages_in_range` while reads/writes race in.

### MIX-RWC — `test_mixed_rwc` *readers + writers + cp*
Adds 2 `cp` threads that overwrite random destination files with random
source files. Each `cp` creates new dedup opportunities for the next
sweep, while concurrent writes/reads attempt to disrupt them.

### CHAOS — `test_chaos_all` *every operation simultaneously*
The integration catch-all: 10 readers + 6 writers + 2 truncators + 2 `cp`
threads all running together over 10 files, including page-boundary and
same-page-targeted operations. The strongest test of overall correctness.

### PHASED — `test_ordered_phases` *order-sensitive bugs*
Runs operations in a deliberate sequence and verifies after each phase:
1. writes only
2. cp only
3. reads + writes
4. writes + cp
5. writes + truncate
6. cp + truncate
7. all together

Catches order-dependent bugs that random scheduling might hide.
Between phases the scanner is allowed to re-merge, exercising
re-deduplication after writes, truncates, and copies.

### STORM — `test_samepage_storm` *targeted contention*
20 threads slam the **same page index** across 8 different files with
reads, writes, page-killing truncates, and cross-file `cp`. Validates
per-page lock correctness and survivor-page atomicity under maximum
pressure.
