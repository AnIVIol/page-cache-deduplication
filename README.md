# Page Cache Deduplication for Linux Kernel 6.1.4

This artifact implements a page-cache deduplication mechanism in the Linux Kernel. It identifies identical pages in the page cache across different files and merges them to a single physical page, significantly reducing memory footprint for redundant data.

---

## Artifact Directory Structure

- **`final.zip`**: The primary kernel patch for Linux 6.1.4 implementing the core deduplication logic.
- **`project/modules/`**: Source code for out-of-tree kernel modules.
    - `scanner.c`: Background kthread that hashes and merges identical pages.
    - `pagecache_inspector.c`: Sysfs interface for inspecting page states (PFNs, refcounts).
    - `ok.sh`: Automation script to build and reload modules.
- **`project/user_space/`**: Testing and benchmarking suite.
    - **`manual_testing/`**: Utilities for step-by-step verification (`generate_files`, `write_offset`, `write_test`).
    - **`testcases/`**: Automated functional scripts (`cp_ln`, `fio_scripts`). There is a [README](project/user_space/testcases/README.md) in this directory
    - **`benchmarks/`**: Performance measurement suites. There is a README in each benchmark suite folder inside this directory.
        - `profiling_and_filebench/`: Benchmarks using Filebench and CPU profiling.
        - `read_write_mem/`: Targeted memory savings and I/O latency benchmarks.
        - `truncate/`: Benchmarks for file truncation performance.

---

## Setup Instructions

### Hardware Requirements
- **CPU**: Quad-core or higher
- **Memory**: Minimum 4GB RAM (8GB+ recommended)
- **Storage**: Atleast 10GB free space
- **Extra Hardware**: None required

### Software Requirements
- **OS**: Ubuntu 20.04 LTS or higher
- **Base Kernel**: Linux Kernel 6.1.4.
- **Dependencies**: 
  ```bash
  sudo apt update
  sudo apt install build-essential libncurses-dev bison flex libssl-dev libelf-dev fio iostat
  ```
  *   **`fio`**: Critical for stress-testing deduplicated pages.
  *   **`filebench 1.4.9`**: Required for the application workload simulations in `benchmarks/`.

### Linux Kernel Compilation
1. Download Linux 6.1.4 source and prepare the environment:
   ```bash
   tar -xf linux-6.1.4.tar.xz
   mv linux-6.1.4 linux_base
   ```
2. Apply the provided patch:
   ```bash
   unzip final.zip
   patch -p0 < final.patch
   ```
   Agree with ```y``` to all prompts
3. Add your own config file or copy ours
   ```
   cp .config linux_base/.
   ```
4. Configure and build (Agree to the prompt that asks if dedup module has to be added):
   ```bash
   make oldconfig
   make -j2
   sudo make modules_install && sudo make install
   sudo reboot
   ```

---

## Features & Functionalities

- read, write, truncate, cp, symlink, echo 3 > drop_caches on deduplicated pages.
- Editors like vi also work on deduplicated pages but while exiting some delay will be observed.

---

## Assumptions and Unsupported Features
- Discussed in the report

---

## Getting Started (Manual Testing)

Verify basic functionality within 30 minutes using these manual steps:

1. **Build and Load Modules**:
   ```bash
   cd project/modules
   ./ok.sh scanner pagecache_inspector
   ```
2. **Setup Test Files**:
   ```bash
   cd ../user_space/manual_testing
   make
   # Create 2 files with 1024 identical "AAAAAAA\n" pages each
   ./generate_files 2 1024 
   ```
3. **Trigger Deduplication**:
   ```bash
   # Start the scanner
   echo "1" | sudo tee /sys/kernel/dedup_scanner/run
   # Submit files for scanning
   echo "$file_path1" | sudo tee /sys/kernel/dedup_scanner/scan_file
   echo "$file_path2" | sudo tee /sys/kernel/dedup_scanner/scan_file
   ```
4. **Inspect Results**:
   ```bash
   # View PFNs and refcounts (should show shared PFNs)
   echo "$file_path" | sudo tee /sys/kernel/pagecache_inspector/filename_print
   ```
5. **Verify CoW**:
   ```bash
   # Modify a shared page and check isolation
   ./write_offset "$(pwd)/file_0.dat" 0 "MODIFIED"
   ./write_test "$(pwd)/file_0.dat" "A\n"
   ```
   You may use other commands like cat or editors like vi to view the updated contents in place of write_test.
   
 6. **Verify Other Functionalities**:
- truncate, symlink, cp, echo 3 can also be directly verified.
---
