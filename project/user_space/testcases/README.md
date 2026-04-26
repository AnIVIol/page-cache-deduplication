# Page Cache Deduplication — Test Suite

This directory contains the correctness test suite for the page-cache
deduplication subsystem. Every test creates its own temporary files,
exercises a specific scenario through the `/sys/kernel/dedup_scanner`
sysfs interface, and verifies byte-level data integrity using on a per byte basis.

---

## Test Description

Each directory has its own README describing the testing feature of the suite

## Quick Start

```bash
# Make sure the dedup modules are loaded:
ls /sys/kernel/dedup_scanner /sys/kernel/page_dedup

# Build and run the entire suite:
chmod +x run_all_tests.sh
sudo ./run_all_tests.sh
```

A summary of `PASS` / `FAIL` lines is printed at the end. Full per-test
output is captured in `/tmp/dedup_test.log`.


---

## Cleanup / Recovery

Each test self-cleans on exit:
- stops the scanner (which triggers `unmerge_all_dedup_pages()`),
- closes its fds,
- removes its temporary directory under `/tmp/dedup_t*/`.

If a test is interrupted (Ctrl-C, oops), recover with:

```bash
sudo sh -c 'echo 0 > /sys/kernel/dedup_scanner/run'
sudo sh -c 'echo 1 > /sys/kernel/page_dedup/flush'
sudo rm -rf /tmp/dedup_*
```

before re-running the suite.

---

## Requirements

- Linux with the modified `mm/filemap.c`, `mm/truncate.c`,
  `fs/drop_caches.c`, and the `dedup_scanner` / `page_dedup` modules.
- Root privileges (the suite writes to sysfs and `/proc/sys/vm/drop_caches`).
- `gcc` with pthread support; tests build cleanly with `-Wall -Wextra`.
- ~2 GB free memory and sufficient free space in `/tmp` for the largest tests
  (`test_thousand_files`, `test_chaos_all`).
```
