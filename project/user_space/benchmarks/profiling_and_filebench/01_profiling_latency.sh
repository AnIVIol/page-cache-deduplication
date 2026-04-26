#!/bin/bash

# 04_profiling_latency.sh - Measures operational overhead across various dataset sizes

source "$(dirname "$0")/lib/common.sh"
check_modules

# Configuration
RESULTS_DIR="$(dirname "$0")/results"
RESULTS_FILE="$RESULTS_DIR/profiling_results.csv"
mkdir -p "$RESULTS_DIR"

# Size list in MB
DEFAULT_SIZES=(1 2 4 8 16 32 64 128 256 512 1024 1536)

if [[ $# -gt 0 ]]; then
    SIZES=("$@")
else
    SIZES=("${DEFAULT_SIZES[@]}")
fi

# New CSV header with detailed timing components
echo "size_mb,total_active_ms,hash_ms,comp_ms,merge_ms,throughput_mbs" > "$RESULTS_FILE"

function run_profile() {
    local size=$1
    local test_file="/tmp/latency_test"
    
    log_info ">>> Testing Size: ${size}MB"
    
    # 1. Prepare data
    head -c "${size}M" /dev/zero | tr '\0' 'D' > "${test_file}_1"
    cp "${test_file}_1" "${test_file}_2"

    # 2. Clean State
    reset_scanner
    clear_cache
    
    # 3. Load into cache
    cat "${test_file}_1" "${test_file}_2" > /dev/null
    
    # 4. Trigger and wait for completion
    trigger_dedup "${test_file}_1" "${test_file}_2"
    assert_shared "${test_file}_1" "${test_file}_2"

    # 5. Capture Kernel Metrics
    STATS=$(cat /sys/kernel/dedup_scanner/stats)
    
    # Extract values in nanoseconds
    HASH_NS=$(echo "$STATS" | grep "hash_time_ns" | awk '{print $2}')
    COMP_NS=$(echo "$STATS" | grep "compare_time_ns" | awk '{print $2}')
    MERGE_NS=$(echo "$STATS" | grep "merge_time_ns" | awk '{print $2}')
    SCAN_NS=$(echo "$STATS" | grep "total_scan_time_ns" | awk '{print $2}')
    
    # Convert to milliseconds for display and CSV
    HASH_MS=$(echo "scale=3; $HASH_NS / 1000000.0" | bc -l)
    COMP_MS=$(echo "scale=3; $COMP_NS / 1000000.0" | bc -l)
    MERGE_MS=$(echo "scale=3; $MERGE_NS / 1000000.0" | bc -l)
    SCAN_MS=$(echo "scale=3; $SCAN_NS / 1000000.0" | bc -l)
    
    # Calculate throughput (MB / (ms/1000))
    THROUGHPUT=$(echo "scale=2; $size / ($SCAN_MS / 1000.0)" | bc -l)
    
    # Save to CSV
    echo "${size},${SCAN_MS},${HASH_MS},${COMP_MS},${MERGE_MS},${THROUGHPUT}" >> "$RESULTS_FILE"
    
    log_pass "Done. Active: ${SCAN_MS}ms (Hash: ${HASH_MS}, Comp: ${COMP_MS}, Merge: ${MERGE_MS}) | Throughput: ${THROUGHPUT} MB/s"
    
    # Cleanup
    rm -f "${test_file}_1" "${test_file}_2"
    echo "-----------------------------------"
}

log_info "Starting multi-size profiling. Results will be saved to $RESULTS_FILE"

for s in "${SIZES[@]}"; do
    run_profile "$s"
done

log_pass "Full profiling completed!"

echo ""
echo "--- Final Profiling Summary Table ---"
column -t -s, "$RESULTS_FILE"
echo ""
echo "--- Raw CSV Data ---"
cat "$RESULTS_FILE"
echo "-------------------------------------"
echo "Detailed CSV saved to: $RESULTS_FILE"
