#!/bin/bash

# Shared library for Page Cache Dedup Tests

SCANNER_RUN="/sys/kernel/dedup_scanner/run"
SCANNER_FILE="/sys/kernel/dedup_scanner/scan_file"
INSPECTOR="/sys/kernel/pagecache_inspector/filename_print"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

function log_pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
function log_fail() { echo -e "${RED}[FAIL]${NC} $1"; }
function log_info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

# Extract PFNs from dmesg for a specific file
function get_pfns() {
    local filepath=$(realpath "$1")
    # Clear dmesg to ensure we only get the latest inspector output
    sudo dmesg -c > /dev/null
    echo "$filepath" | sudo tee "$INSPECTOR" > /dev/null
    # Small sleep to allow kernel to print to dmesg
    sleep 0.2
    # Field $4 is the actual PFN value in your log format
    dmesg | grep "pagecache_inspector: ---> PFN:" | awk '{print $4}'
}

# Verify if two files share the same physical pages (PFNs)
function assert_shared() {
    local file1=$(realpath "$1")
    local file2=$(realpath "$2")

    log_info "Checking shared state for $file1 and $file2 ..."

    # Poll until success
    while true; do
        sudo dmesg -c > /dev/null # Clear dmesg
        echo "$file1 $file2" | sudo tee /sys/kernel/pagecache_inspector/verify_shared > /dev/null
        
        # Give kernel time to process and print, then check for success keyword
        sleep 0.1 # Small sleep for kernel printk buffer
        if dmesg | grep -q "VERIFY_SUCCESS"; then
            return 0 # Success
        fi

	sleep 1
        
    done
}

# Clear page cache
function clear_cache() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
}

# Trigger scanner for a list of files
function trigger_dedup() {
    echo 1 | sudo tee "$SCANNER_RUN" > /dev/null
    for f in "$@"; do
        echo "$(realpath "$f")" | sudo tee "$SCANNER_FILE" > /dev/null
    done
}

# Reset scanner state (clear files and stats)
function reset_scanner() {
    # Stopping the daemon triggers cleanup_scanner_state()
    echo 0 | sudo tee /sys/kernel/dedup_scanner/run > /dev/null
    sleep 0.2
    # Starting the daemon triggers the automatic stats reset in scanner.c
    echo 1 | sudo tee /sys/kernel/dedup_scanner/run > /dev/null
    sleep 0.2
    log_info "Scanner daemon restarted and statistics reset."
}

# Check if modules are loaded
function check_modules() {
    if ! ls /sys/kernel/dedup_scanner > /dev/null 2>&1; then
        log_fail "Deduplication modules are not loaded!"
        exit 1
    fi
}

export -f log_pass log_fail log_info get_pfns assert_shared clear_cache trigger_dedup reset_scanner check_modules
