# Page Cache Deduplication Test Suite

This suite provides a comprehensive set of tests to validate correctness, stability, performance, and real-world impact of the page-cache deduplication module.

1.  **Load Modules:**
    ```bash
    sudo ./load_modules.sh load
    ```

---

## Detailed Test Module Descriptions

### 01_profiling_latency.sh
**Purpose:** Micro-level performance analysis across varying dataset scales.
- **Workflow:** Iterates through multiple file sizes (1MB to 1.5GB).
- **Data Source:** Uses the kernel's high-precision `ktime_get_ns()` to calculate the exact duration of each operation (hashing, comparing, merging) as well as the `total_scan_time_ns`.
- **Output:** Generates a detailed CSV breakdown in `tests/results/profilin`g_results.csv` showing how each component scales with data size.
- **Usage:** `sudo ./04_profiling_latency.sh [SIZE_MB1] [SIZE_MB2] ...`

### 02_benchmark_filebench.sh
**Purpose:** Advanced application-level benchmarking under controlled deduplication states.
- **Mechanism:** Uses `filebench` with three distinct profiles: `read_heavy`, `write_heavy`, and `web_mixed`.
- **Pre-conditioning:** Forcefully overwrites Filebench-generated files with specific percentages of shared data (25%, 50%, 100%) to ensure deduplication is active during the test.
- **Comparison:** Measures IOPS and throughput drop between a baseline (scanner off) and an active deduplicated state.
- **Test Directory:** All operations occur in `/tmp/page-cache-deduplication/fb_test` to ensure guest page cache utilization.
- **Usage:** `sudo ./05_benchmark_filebench.sh [WORKLOAD_NAME]`

---

## Usage Summary Table

| Script | Default Behavior | Parameters |
| :--- | :--- | :--- |
| `01_profiling_latency.sh` | 1MB to 1.5GB sweep | `[Size1] [Size2] ...` |
| `02_benchmark_filebench.sh` | All 3 workloads | `[Workload_Name]` |
| `load_modules.sh` | load/unload modules | `load` or `unload` |

---

## Data Visualization
After running the profiling or benchmark scripts, you can generate report-ready graphs using:
```bash
python3 plot/generate_plots.py
```
This produces `profiling_detailed.png` in the `results/` folder, visualizing the time components and throughput scalability.

## Prerequisites
- **Build Tools:** `gcc`, `make`, `build-essential`.
- **Filebench:** Must be installed (Stable version 1.4.9.1).
- **Python:** `pandas` and `matplotlib` libraries.
