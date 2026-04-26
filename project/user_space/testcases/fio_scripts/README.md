# `fio_scripts` — FIO Stress & CoW Tests for Page Cache Deduplication

This directory exercises the page-cache dedup subsystem under heavy I/O
load using [`fio`](https://fio.readthedocs.io/). Each script creates one
or more deduplicable files, registers them with the scanner via sysfs,
then pounds them with `randread` / `randwrite` / mixed workloads.

The goal is **not** to exhaustively check every CoW edge case (the
`cp_ln/` and main test suites already do that) — it's to verify that the
dedup hooks survive sustained high-concurrency I/O without deadlocks,
data corruption, or kernel oopses.

---
## Files

| File | Purpose |
|------|---------|
| `gen_dedup.c` | Deduplicable Test File Generator |
| `tc1.sh`     | Two-file 500 MB dedup → 8-thread `randread` stress (20 s). Sanity baseline. |
| `tc2.sh`     | **Control / negative test.** Same as tc1 but does **not** feed the scanner — establishes the no-dedup latency baseline. |
| `tc3.sh`     | Two-file dedup → 8-thread `randwrite` stress (configurable size/bs). |
| `tc4.sh`     | Single-file internal-dedup (uses fio's `--dedupe_percentage` to plant duplicates) → read + write + read stress phases. |
| `tc5.sh`     | Uses `gen_dedup` to control share % and number of duplicate-content groups → read, write, and mixed stress. |
| `tc6.sh`     | **Master test.** 13-phase end-to-end correctness suite with Python helpers: dedup verification, CoW verification at multiple offsets, and FIO concurrency progression (1 → 16 → 32 jobs). |

`gen_dedup` (built from `../cp_ln/gen_dedup.c`) must be on `PATH` or in
the current directory for `tc5.sh` to work. The other scripts use `dd`
and/or fio's built-in generators.

---
## `gen_dedup` — Deduplicable Test File Generator

A small utility that produces a single output file with a precisely
controlled amount of page-level redundancy. It is the primary input
generator for the dedup stress and benchmark tests in this repository
(`fio_scripts/tc5.sh`, the `cp_ln/*_v2.sh` suite, etc.).

### Build

```bash
gcc -Wall -O2 -o gen_dedup gen_dedup.c
```

No external dependencies — just libc.

### Usage

```bash
./gen_dedup <file_size_MB> <percentage> <groups>
```

| Argument        | Meaning |
|-----------------|---------|
| `file_size_MB`  | Total file size in MiB. The file size is rounded down to a multiple of the 4 KiB page size. |
| `percentage`    | Percentage of the total file pages that are shared in each group |
| `groups`        | Number of shared distinct duplicate-content groups. Each group has its own random 4 KiB pattern. Each group size is 'percentage' of the total file size |

**Constraint:** `percentage × groups ≤ 100` — otherwise the duplicate
slots would exceed 100 % of the file. The program rejects the run with
an error message if violated.

### Output

The generated file is **always** written to `./output.dat` in the
current working directory (filename is hardcoded). Any previous
`output.dat` is overwritten.

On stdout you'll see something like:

```
Total pages: 12800
Shared pages per group: 5120
[+] File generated: output.dat
```

> **Note:** because the output path is fixed, scripts that pass an
> additional path argument (e.g. `gen_dedup 256 100 1 some/path.dat`)
> will silently still produce `output.dat`. Move/rename it after the
> run, or modify the source to honour `argv[4]`.

### How redundancy is constructed

Given `N = file_size_MB × 256` pages (4 KiB each):

1. **Generate `groups` random 4 KiB patterns** — one per group.
2. **Shuffle** the integer page indices `[0, N)` once.
3. **Walk the shuffled list** and, for each group `g ∈ [0, groups)`,
   tag the next `(percentage × N) / 100` page indices as belonging to
   group `g`.
4. **Write the file**:
   - tagged pages are written with their group's pre-computed pattern
     (so they will deduplicate against any other tagged page from the
     same group);
   - untagged pages are filled with fresh random bytes (so they almost
     surely **won't** deduplicate against anything).

Because step 2 is a uniform random shuffle, duplicate pages are
**scattered throughout the file** rather than clustered at the start.
This better simulates real-world workloads (e.g. shared library
mappings, common file headers, copy-on-write descendants) where
identical pages appear at unrelated offsets.

### Examples

| Command | Result |
|---------|--------|
| `./gen_dedup 256 100 1`  | 256 MB; 100 % of pages share **one** common pattern. After dedup, virtually the whole file collapses to a single survivor page. |
| `./gen_dedup 256 50 2`   | 256 MB; two groups, each covering 50 % of pages. Two survivor pages remain after dedup. |
| `./gen_dedup 500 40 2`   | 500 MB; 80 % of pages are duplicate (40 % × 2 groups), 20 % are unique random pages. |
| `./gen_dedup 64 0 1`     | 64 MB of fully unique random data — useful as a "dedup yields nothing" control. |

---

## Prerequisites

1. A kernel with the page-cache deduplication subsystem (modules
   loaded via `../setup_env.sh` — every script except `tc2.sh` calls
   it first).
2. **`fio`** installed:
   ```bash
   sudo apt install fio
   ```
3. **`iostat`** (used by `tc6.sh`):
   ```bash
   sudo apt install sysstat
   ```
4. **`python3`** (`tc6.sh` uses it for byte-level file generation and
   verification).
5. Root privileges (the scripts write to sysfs and run fio with `sudo`).
6. Enough free disk in `$PWD` to host the test files
   (defaults: 50 MB – 1 GB; some scripts let you reduce via flags).

---

## How to run

Each script is self-contained; just execute from this directory:

```bash
sudo ./tc1.sh
sudo ./tc2.sh                 # control: dedup OFF
sudo ./tc3.sh -s 256M -b 4k
sudo ./tc4.sh -s 256M -r 80   # 80% internal duplicate ratio
sudo ./tc5.sh -s 500 -p 40 -g 2
sudo ./tc6.sh                 # full correctness + stress suite (~5 min)
```

### Common flags (where supported)

| Flag | Used by | Meaning |
|------|---------|---------|
| `-s` / `--size`     | tc3, tc4, tc5         | File size (`tc4`: e.g. `256M`; `tc5`: integer MB) |
| `-b` / `--bs`       | tc3, tc4, tc5         | Block size for fio (default `4k`) |
| `-r` / `--ratio`    | tc4                   | `--dedupe_percentage` for fio's file generator |
| `-p` / `--percent`  | tc5                   | % of pages duplicated within each group |
| `-g` / `--groups`   | tc5                   | Number of distinct shared-content groups |
| `-h` / `--help`     | tc3, tc4, tc5         | Show usage |

Sanity rule on tc5: `percent × groups ≤ 100`.

---

## What each script tests

### `tc1.sh` — minimum viable dedup smoke test
1. `dd` two 500 MB files with the same `/dev/urandom` content (`survivor.dat` is the original; `victim.dat` is `cp`'d from it).
2. Register both with the scanner.
3. Run 8-thread `randread` fio against the **victim** for 20 s.

Expected behaviour: scanner converts the victim's pages to point at the
survivor; subsequent reads come from the (single) shared page cache and
fio reports normal IOPS without errors / oopses.

### `tc2.sh` — control / no-dedup baseline
Identical to `tc1.sh` **but**:
- does *not* call `../setup_env.sh`,
- does *not* register files with the scanner.

Use this to compare fio numbers against tc1 and verify the dedup path
isn't introducing read regressions.

### `tc3.sh` — write-side stress
Same setup as tc1 (two files, dedup on) but the fio job is
`randwrite`. Each write to a deduplicated page should trigger the
in-tree CoW hook in `__filemap_get_folio`. The script exercises that
hook continuously for 20 s with 8 concurrent workers.

### `tc4.sh` — single-file internal dedup + write storm
1. Use `fio --dedupe_percentage=$DEDUP_RATIO` to plant a configurable
   fraction of duplicate 4 k blocks inside a single file (default 80 %).
2. Register that single file with the scanner.
3. Run three back-to-back fio phases on the same file:
   - **Phase 1**: 8-thread `randread`, 5 s.
   - **Phase 2**: 16-thread `randwrite`, 5 s — the "danger zone" that
     hammers the CoW path.
   - **Phase 3**: 32-thread `randread`, 5 s — verifies the page cache
     is still healthy and not deadlocked after the write storm.

### `tc5.sh` — controllable share/groups + read+write+mixed stress
1. Generate one file via `gen_dedup` with a precise mix of `groups`
   distinct duplicate-content patterns (default: 40 % shared per group,
   2 groups).
2. Register with the scanner.
3. Three sequential 5–15 s fio phases:
   - 8-thread `randread`,
   - 1-thread `randwrite`,
   - 16-thread `randrw 70/30` mixed.

### `tc6.sh` — master correctness + concurrency progression
The most thorough script in this directory. Uses inline Python helpers
to do byte-level file authoring and verification rather than relying on
`dd`/`head -c`. Roughly 13 phases:

| Phase | What happens |
|-------|--------------|
| 1     | Create three 256 MB files filled entirely with byte `'A'` |
| 2     | Load all three into the page cache (`cat … > /dev/null`) |
| 3     | Register with the scanner; wait for dedup |
| 4     | SHA-256 before/after — content must be unchanged |
| 5     | Create three 128 MB files filled with byte `'X'` (CoW victims) |
| 6     | Load + dedup those too |
| 7     | Use `pwrite(2)` to write byte `'Y'` at offsets 0, 1 MB, and 100 MB of one shared file |
| 8     | Read those exact ranges back — must read `'Y'`; an unwritten range must still read `'X'` (CoW must split only the touched pages) |
| 8B    | Re-read all files, sample `iostat` — disk utilisation should stay near 0 % (proves page cache is serving) |
| 9     | fio `write`, 1 job, `psync`, 30 s — single-threaded baseline |
| 10    | fio `randwrite`, 16 jobs, 30 s — moderate concurrency |
| 11    | fio `randwrite`, 32 jobs, 30 s — maximum concurrency |
| 12    | Read every file back to confirm none were corrupted by the storm |
| 13    | Final pure-read sample — page cache should still be hot |

The script `set -e`s at the top, so any failed verification aborts the
run with a clear `[✗] CoW FAILED!` / `[✗] Mismatches found` message.

Throughout, two helpers print:
- `show_cache_stats` — `/proc/meminfo`'s `Cached:` and `Dirty:` lines.
- `show_disk_stats` — last line of `iostat -d 1 1`.

So you can see live evidence that operations are hitting the page cache
and not the underlying disk.

---

## What constitutes "pass"

Per script:

- **`tc1` / `tc2` / `tc3`** — fio prints aggregate IOPS / bandwidth and
  exits cleanly. **No oopses or warnings in `dmesg`.** Compare tc1 vs
  tc2 numbers to estimate the dedup overhead.
- **`tc4` / `tc5`** — same; the script ends with a clean
  `[+] Test complete` / `[+] Test Complete. You survived!` line.
- **`tc6`** — ends with the banner:
  ```
  =======================================================
  ✓ ALL TESTS PASSED
  =======================================================
  ```
  Any phase failure aborts the script with a non-zero exit and a
  `[✗]` diagnostic.

After every run, **always** check `dmesg | tail -50` for warnings or
BUGs that might be silently logged without affecting fio's exit code.

---

## Recommended run order

For a fresh kernel build / first sanity check:
```bash
sudo ./tc6.sh        # most comprehensive; will catch most issues
```

For investigating performance regressions:
```bash
sudo ./tc2.sh        # baseline (no dedup)
sudo ./tc1.sh        # same workload, dedup on
sudo ./tc3.sh        # write-side under dedup
```

For exploring share-pattern scaling:
```bash
sudo ./tc5.sh -p 25 -g 2     # 50% deduplicable, two patterns
sudo ./tc5.sh -p 50 -g 2     # 100% deduplicable, two patterns
sudo ./tc5.sh -p 100 -g 1    # all-shared single pattern
```

---

## Cleanup / recovery

Every script `rm`s its data files on exit. If a script is killed
mid-run you may have stale `survivor.dat`, `victim.dat`,
`single_dedup.dat`, `output.dat`, `dedup_*.dat`, or `cow_test_*.dat`
sitting in `$PWD`. Remove manually:

```bash
rm -f survivor*.dat victim*.dat single_dedup.dat output.dat dedup_*.dat cow_test_*.dat
sudo sh -c 'echo 0 > /sys/kernel/dedup_scanner/run' 2>/dev/null
sudo sh -c 'echo 1 > /sys/kernel/page_dedup/flush'  2>/dev/null
```

before re-running so the next test starts from a clean cache.

---

## Notes on a few quirks

- `tc2.sh` deliberately omits the scanner setup — it's the **negative
  control**, not a typo.
- The comments inside `tc3.sh` say "Read Stress Test" but the actual
  fio job is `randwrite`. The label is wrong; the workload is right.
- `tc4.sh` originally targeted the kprobe-era CoW implementation
  ("If your Kprobe atomic sleep bug isn't fixed, the VM will panic
  NOW"). The current in-tree CoW path no longer has that problem, so
  the warning is historical — but the test itself is still useful for
  the write storm it generates.
- `tc6.sh` uses 4 k page-aligned write offsets (0, 1 MB, 100 MB) — these
  are intentionally on full-page boundaries so CoW splits exactly one
  page per write, making the verification step unambiguous.
```
