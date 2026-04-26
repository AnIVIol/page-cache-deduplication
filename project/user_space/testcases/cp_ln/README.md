# `cp_ln` — Copy & Link Test Suite for Page Cache Deduplication

This directory contains correctness tests focused on how the page-cache
deduplication subsystem interacts with **`cp`**, **symbolic links**,
**hard links**, and various write paths. Where the main test suite
checks "does dedup do the right thing on a single file?", these tests
verify "does dedup do the right thing when files relate to each other
through filesystem-level mechanisms?".

---

## Files

| File | Purpose |
|------|---------|
| `tc.sh`                       | Quick smoke test (symlink read/write, hard-link bypass, `cp --reflink` rejection, scanner poison-pill). |
| `test_cp_dedup_v2.sh`         | 10-phase deep test of `cp` interacting with dedup: copy, re-copy, CoW, append, fio stress. |
| `test_symlinks_dedup_v2.sh`   | 10-phase deep test of symlinks on already-deduplicated files: read, CoW write, fio stress. |
| `test_master_dedup_v2.sh`     | Combined symlink + cp suite — quick roll-up of the previous two with cleaner pass/fail counts. |
| `gen_dedup.c`                 | Helper: generates a file with controllable page-level redundancy (see below). |
| `../setup_env.sh`             | Common environment setup (loads scanner module, mounts sysfs, etc.) sourced by every test. |
| `../write_offset` *(binary)*  | Helper: performs a `pwrite(2)` at a given offset. Exercises the dedup CoW path through the real `write(2)` syscall. |

---

## Prerequisites

1. A kernel with the page-cache deduplication patches and the
   `dedup_scanner` / `page_dedup` modules.
2. The `setup_env.sh` script in the parent directory (handles module
   loading and sysfs verification — every test calls it first).
3. The `write_offset` helper binary in the parent directory or on
   `PATH`. The tests use it to issue raw `write(2)` syscalls, ensuring
   they hit the modified `__filemap_get_folio` CoW path.
4. **`fio`** installed (used by the `_v2` tests for read/write stress
   phases).
5. Root privileges (the tests write to `/sys/kernel/dedup_scanner/*`
   via `sudo tee`).
6. Compiled `gen_dedup`:
   ```bash
   gcc -Wall -O2 -o gen_dedup gen_dedup.c
   ```

> **Note about `gen_dedup`**: the source as committed always writes the
> generated content to `./output.dat`, ignoring any path argument. The
> `_v2` test scripts pass an output path expecting it to be respected,
> and fall back to `dd if=/dev/zero | tr '\0' 'A'` (or `'B'`) if
> `gen_dedup` is not on `PATH`. If you want true random-content
> deduplicable files, modify `gen_dedup.c` so it writes to `argv[4]`,
> or rely on the `dd` fallback (which produces uniform-byte files
> that still deduplicate trivially).

---

## How to run

Each script is self-contained; just execute it from this directory:

```bash
# Quick smoke test (~10 seconds):
sudo ./tc.sh

# In-depth tests; default 256 MB files:
sudo ./test_cp_dedup_v2.sh
sudo ./test_symlinks_dedup_v2.sh
sudo ./test_master_dedup_v2.sh
```

The `_v2` scripts accept the same CLI flags:

| Flag | Default | Meaning |
|------|---------|---------|
| `-s` / `--size`     | `256` | Test file size in MB |
| `-p` / `--percent`  | `100` | % of pages duplicated within each group |
| `-g` / `--groups`   | `1`   | Number of distinct shared-content groups |
| `-b` / `--bs`       | `4k`  | Block size used by the `fio` stress phases |

Sanity rule enforced by all `_v2` scripts: `percent × groups ≤ 100`.

Example — small fast run:
```bash
sudo ./test_cp_dedup_v2.sh -s 64 -p 50 -g 2 -b 4k
```

Each `_v2` script writes a timestamped log into its temporary working
directory (e.g. `dedup_cp_test/cp_test_<epoch>.log`) **and** to stdout
via `tee`. The temporary directory is removed on success.

---

## What each test verifies

### `tc.sh` — quick correctness smoke test (~5 phases)
Creates two random 4 MB files, registers them with the scanner, and
then runs four targeted checks:

- **Symlink read** — `sym_f1 → f1` returns identical bytes (`cmp -s`).
- **Symlink write CoW** — writing zeros through the symlink reaches the
  underlying inode (`f1`); the survivor page is correctly split and
  the change is observable on the target file.
- **Hard-link bypass** — `hard_f2` shares the inode of `f2`; modifying
  one is immediately visible in the other (no spurious CoW).
- **`cp --reflink=always` rejection** — confirms ext4 still rejects
  block-level reflinks (we don't support them; the dedup engine
  must never silently fall through to one).
- **Standard `cp`** — bit-for-bit equality of source and copy.
- **Scanner poison-pill** — feeding a symlink to `/dev/null` to the
  scanner's `scan_file` sysfs entry must not crash the kernel
  (`dmesg` is grepped for "bad page cache" / "null pointer deref").

After every check the script tails `dmesg` for `BUG:` / `oops:` /
`panic:` / general protection faults; any hit aborts with `FATAL`.

### `test_cp_dedup_v2.sh` — `cp` interaction (10 phases)

| Phase | What happens | What's verified |
|-------|--------------|-----------------|
| 0 | Setup, scanner sanity-check | sysfs path exists |
| 1 | Generate `source.dat` (size/share configurable) | size matches |
| 2 | `cp` source → `copy{1..4}.dat` | all four copy sizes match |
| 3 | Feed all five files to the scanner; multiple cache-warming reads | dedup gets time to settle |
| 4 | `cp` deduped copy → `recopy{1..3}.dat` | sizes match again, exercising `copy_file_range` on already-shared pages |
| 5 | Verify `head -c 10` of every file is the expected pattern | content unchanged across dedup |
| 6 | `write_offset(copy1, 0, "MODIFIED…")` | source + sibling copies remain `B…` (CoW splits only `copy1`) |
| 7 | 1-byte `pwrite` to `recopy2` | source/copy2 unchanged (CoW from a previously-deduped→copied page) |
| 8 | 10-second `fio randwrite` on `copy1` | source still `B`-prefixed afterwards (no shared-page corruption) |
| 9 | `pwrite` append to `recopy3` | source/copy3 sizes unchanged, only `recopy3` grew |
| 10 | 10-second `fio randrw 70/30` on `copy4` | passes if no failures |

Pass/fail is counted across phases and printed in a final summary.

### `test_symlinks_dedup_v2.sh` — symlinks on deduped files (10 phases)

| Phase | What happens | What's verified |
|-------|--------------|-----------------|
| 0–3 | Generate three identical files, feed to scanner, wait for dedup | dedup occurs |
| 4 | Create `symlink1 → file1`, `symlink2 → file2`, and a chained symlink `symlink_chain → symlink1` | all symlinks created |
| 5 | `head -c 10` through each symlink | bytes equal to the expected pattern |
| 6 | `fio randread` through `symlink1` for 5 s | no I/O errors, dedup survives reads |
| 7 | Append via `write_offset` through `symlink1` | target file grows; siblings (`file2`, `file3`) stay byte-for-byte identical to original (CoW worked correctly) |
| 8 | `fio randwrite` directly on the now-modified `file1` | symlink remains valid afterwards |
| 9 | `fio randrw` on `symlink2` for 10 s | passes if no failures |
| 10 | Summary | counts pass/fail |

The key invariant under test: **modifying a deduplicated file via a
symlink must split only that file's pages, leaving every sibling
sharer pristine.**

### `test_master_dedup_v2.sh` — combined roll-up
Runs three lightweight phases sequentially:

1. Symlink phase — same as `test_symlinks_dedup_v2.sh` but with fewer
   tests (read, CoW, isolation).
2. CP phase — same as `test_cp_dedup_v2.sh` but condensed.
3. Combined phase — small file, multi-step `cp` + symlink chain,
   verifying both routes still resolve to the same content after dedup.

It's the fastest of the three `_v2` scripts and a good sanity check
before running the deeper variants.

---

## Output

Each `_v2` script ends with a summary block like:

```
===========================================================
  TEST SUMMARY
===========================================================
Total Tests: 22
Passed: 22
Failed: 0
Status: [+] ALL TESTS PASSED
===========================================================
```

A non-zero `Failed` count, or any "[-]" line in the body, indicates a
real bug in the dedup behaviour for that scenario. The full per-phase
log is preserved in `*_test_<epoch>.log` inside the test directory
(printed at the end of the script before cleanup).

`tc.sh` is more terse: it `exit 1`s on the first failure with a
descriptive `[-] FAIL:` message, otherwise prints `[PASS]` for each
sub-test and falls through to a clean exit.

---

## Cleanup / recovery

All scripts `rm -rf` their working directories on success. If a test
is killed mid-run you may need to:

```bash
sudo sh -c 'echo 0 > /sys/kernel/dedup_scanner/run' 2>/dev/null
sudo sh -c 'echo 1 > /sys/kernel/page_dedup/flush' 2>/dev/null
rm -rf ./dedup_test_dir ./dedup_cp_test ./dedup_symlink_test ./dedup_master_test
```

before re-running, otherwise the next `gen_dedup`/`cp` may collide with
leftover files and confuse the scanner's bookkeeping.

---

```
