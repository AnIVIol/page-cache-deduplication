# Truncate Benchmark — Page Cache Deduplication

This directory contains a micro-benchmark that measures the latency of
`truncate(2)` on files whose pages have been deduplicated by the
page-cache dedup subsystem, plus a Python plotter that turns the raw CSV
into a set of human-readable graphs.

| File | Role |
|------|------|
| `gen_files.c`               | Test-file generator (random pages, controllable % of duplicates per group). |
| `bench_truncate.c`          | The benchmark driver. Runs three phases per configuration and writes a CSV. |
| `plot_truncate_bench.py`    | Reads the CSV and produces several plots + a text summary. |

`gen_files` and `bench_truncate` are external dependencies of each other.
The benchmark also requires the `dedup_scanner` kernel module
(`scanner.ko`) and the `fio` tool.

---

## 1. Prerequisites

- A kernel built with the page-cache deduplication patches (modified
  `mm/filemap.c`, `mm/truncate.c`, `fs/drop_caches.c` and the
  `dedup_scanner` / `page_dedup` modules).
- `fio` installed (`sudo apt install fio` on Debian/Ubuntu).
- Python 3 with `pandas`, `matplotlib`, `numpy`:
  ```bash
  pip install pandas matplotlib numpy
  ```
- Root privileges (the benchmark writes to `/sys/kernel/dedup_scanner/*`,
  loads/unloads the scanner module, and drops caches).

---

## 2. Build

```bash
gcc -Wall -Wextra -O2 -o gen_files       gen_files.c
gcc -Wall -Wextra -O2 -o bench_truncate  bench_truncate.c
```

The default scanner module path is `../modules/scanner.ko`. Override
with the `SCANNER_KO` env var if it lives elsewhere.

---

## 3. Run the benchmark

```bash
sudo ./bench_truncate                 # writes truncate_report.csv
sudo ./bench_truncate myreport.csv    # custom CSV name
```

Or with environment overrides:

```bash
sudo GEN_BIN=./gen_files \
     SCANNER_KO=../modules/scanner.ko \
     WORK_DIR=/tmp/bench_truncate \
     ./bench_truncate
```

### What it does

For every combination in the matrix
`SIZES × SHARES × GROUPS_LIST` (skipping `share × groups > 100`):

| | |
|---|---|
| `SIZES`        | 64, 128, 256 (MB) |
| `SHARES`       | 30, 60, 90 (% of pages duplicated within a group) |
| `GROUPS_LIST`  | 1, 3 (number of distinct shared-content groups in the file) |

it runs three phases and times a single `truncate(file, file_size/2)`
call in each one:

1. **`baseline`** — scanner is **off**; this is the unmerged,
   conventional truncate latency on the same file.
2. **`dedup`**    — scanner is **on**; the file (plus 3 replicas) has
   been registered and given 3 s to deduplicate.
   The truncate now triggers `unmerge_shared_pages_in_range`, which is
   the path we want to characterise.
3. **`stress`**   — same as `dedup`, but with a background `fio`
   `randread numjobs=4` running on `file_0` for 10 s while we
   truncate the *other* files. Catches contention bugs / lock
   amplification under read load.

Each row of the CSV records a single sample. After every sample the
benchmark also reads `Unevictable:` from `/proc/meminfo` (a proxy for
"how many shared survivor pages exist right now").

### Output CSV format

```
size_MB,share_pct,groups,phase,run,latency_ms,errors,unevict_kB
64,30,1,baseline,1,4.131836,0,16
64,30,1,dedup,1,15.85,0,12384
64,30,1,stress,1,98.42,0,12380
...
```

`latency_ms` is `NA` for any run where `truncate` returned an error.

The matrix is small by default (`RUNS=1`, 3 sizes × 4 shares × 2 groups
≈ 18 configs), so a full sweep typically completes in a few minutes.
You can edit the constants at the top of `bench_truncate.c` to expand
or shrink it.

### Cleanup

The benchmark cleans up its working directory on success and on Ctrl-C
(via `SIGINT`/`SIGTERM` handlers). If something goes wrong, manually
recover with:

```bash
sudo sh -c 'echo 0 > /sys/kernel/dedup_scanner/run' 2>/dev/null
sudo rmmod scanner 2>/dev/null
sudo rm -rf /tmp/bench_truncate
```

---

## 4. Plot the results

```bash
python3 plot_truncate_bench.py truncate_report.csv truncate_plots
```

Output files go into `truncate_plots/` (created if missing):

| File | What it shows |
|------|---------------|
| `phase_comparison_g{N}.png` | Bar chart per `groups=N`: mean latency by phase, grouped by share %, faceted by file size. |
| `latency_vs_share_g{N}.png` | Line chart: latency vs share %, one curve per file size, one panel per phase. Shows how truncate cost grows as more pages are deduplicated. |
| `latency_vs_size_g{N}.png`  | Line chart: latency vs file size (log–log), one curve per share %, one panel per phase. Shows whether unmerge cost scales linearly or worse. |
| `heatmap_g{N}.png`          | log10(latency) heatmap over (size × share) for each phase — quick visual of where the worst cases are. |
| `slowdown.png`              | `dedup / baseline` and `stress / baseline` ratios — the headline "how much does dedup cost on truncate?" plot. |
| `distribution.png`          | Violin + box plot of every sample, pooled by phase. Outliers (long-tail truncates) become visible here. |
| `unevictable.png`           | Peak `Unevictable:` (kB) during the dedup phase — proxy for the number of survivor pages actually pinned. |
| `errors_g{N}.png`           | Heatmap of error counts (only emitted if any run produced an `NA`). |
| `summary.txt`               | Plain-text table: mean ± std / median / min / max / runs / errors per `(size, share, groups, phase)`, plus the overall best and worst single samples. |

The plotter is strictly a CSV consumer — if you append more runs to the
same CSV (or merge multiple CSVs), re-running the script regenerates
all plots from scratch.

---

## 5. Interpreting the numbers

- **`baseline` ≈ a few ms** — page-cache truncate without dedup. This is
  your reference point.
- **`dedup` ≫ `baseline`** at high share % — expected, because each
  shared page in the truncated range needs to be split (CoW) before it
  can be released. The slowdown ratio in `slowdown.png` quantifies this.
- **`stress` ≈ `dedup` (or larger)** — concurrent fio reads compete for
  the same per-mapping locks; large excess here would suggest a
  scalability bottleneck.
- **`Unevictable_kB` ≈ 0 in baseline / large in dedup** — confirms that
  deduplication actually happened (survivor pages are pinned with
  `SetPageUnevictable`).

A failure (`errors > 0`) in any cell means `truncate(2)` returned an
error during that run; check the run log and `dmesg` (the benchmark
prints the last 20 lines of dmesg at the end).
```
