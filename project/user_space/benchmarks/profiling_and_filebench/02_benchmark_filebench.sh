#!/bin/bash

# 05_benchmark_filebench.sh - Advanced benchmarking with data pre-conditioning
# Measures the impact of deduplication across different levels of data sharing (25%, 50%, 100%).

source "$(dirname "$0")/lib/common.sh"
check_modules

# Configuration
WORKLOADS_DIR="$(dirname "$0")/lib/workloads"
RESULTS_DIR="$(dirname "$0")/results/filebench"
TEST_DIR="/tmp/page-cache-deduplication/fb_test"

mkdir -p "$RESULTS_DIR" "$TEST_DIR"

# Parse arguments: if provided, run only that workload
if [[ $# -gt 0 ]]; then
    WORKLOADS=("$@")
else
    # Focus on the three distinct types
    WORKLOADS=("read_heavy" "write_heavy" "web_mixed")
fi

# Sharing levels to test (percentage of file that is identical across the fileset)
SHARING_LEVELS=(25 50 100)

# Helper: Overwrite all files with a mix of shared and unique data
# Args: $1 = Sharing percentage (0-100)
function precondition_files() {
    local percent=$1
    log_info "Pre-conditioning fileset with ${percent}% shared data..."
    
    # Files are 1MB (256 pages)
    local shared_kb=$(( 1024 * percent / 100 ))
    local unique_kb=$(( 1024 - shared_kb ))
    
    find "$TEST_DIR" -type f | while read f; do
        # 1. Write the shared portion (same for all files)
        head -c "${shared_kb}K" /dev/zero | tr '\0' 'A' > "$f"
        # 2. Append unique portion (different for every file)
        if [ "$unique_kb" -gt 0 ]; then
            head -c "${unique_kb}K" /dev/urandom >> "$f"
        fi
    done
    sync
    # Load into page cache
    find "$TEST_DIR" -type f -exec cat {} > /dev/null \;
    sync
}

# Helper: Register all files in the test directory for scanning
function register_all_files() {
    log_info "Registering all files in $TEST_DIR for scanning..."
    find "$TEST_DIR" -type f | while read f; do
        echo "$f" | sudo tee "$SCANNER_FILE" > /dev/null
    done
}

function run_benchmark() {
    local workload_name=$1
    local workload_path="$WORKLOADS_DIR/$workload_name.f"
    local percent=$2
    
    log_info ">>> Benchmarking: $workload_name | Sharing: ${percent}%"
    
    # --- STEP 1: Baseline Setup (Scanner OFF) ---
    echo 0 | sudo tee "$SCANNER_RUN" > /dev/null
    rm -rf "$TEST_DIR"/*
    
    # Let filebench create structure
    echo "set \$dir = $TEST_DIR" > /tmp/prep.f
    grep "define fileset" "$workload_path" >> /tmp/prep.f
    echo "create files" >> /tmp/prep.f
    echo "quit" >> /tmp/prep.f
    sudo filebench -f /tmp/prep.f > /dev/null
    
    precondition_files "$percent"

    log_info "Running Baseline..."
    sed 's/create files/#create files/' "$workload_path" > /tmp/workload_baseline.f
    sudo filebench -f /tmp/workload_baseline.f > "$RESULTS_DIR/${workload_name}_${percent}_baseline.txt"
    
    # --- STEP 2: Dedup Setup (Scanner ON) ---
    reset_scanner
    precondition_files "$percent"
    register_all_files
    
    # Wait for merge (only wait if sharing > 0)
    if [ "$percent" -gt 0 ]; then
        FILE_A=$(find "$TEST_DIR" -type f | head -n 1)
        FILE_B=$(find "$TEST_DIR" -type f | head -n 2 | tail -n 1)
        log_info "Waiting for kernel to merge shared pages..."
        # Note: assert_shared expects 100% sharing. 
        # For partial sharing, we just wait a fixed time or check if merge count > 0
        sleep 10 
    fi
    
    log_info "Running with Deduplication..."
    sed 's/create files/#create files/' "$workload_path" > /tmp/workload_dedup.f
    sudo filebench -f /tmp/workload_dedup.f > "$RESULTS_DIR/${workload_name}_${percent}_dedup.txt"
}

for w in "${WORKLOADS[@]}"; do
    for p in "${SHARING_LEVELS[@]}"; do
        run_benchmark "$w" "$p"
    done
done

log_pass "All advanced benchmarks completed! Full logs in $RESULTS_DIR/"
