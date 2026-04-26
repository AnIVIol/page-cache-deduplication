#!/bin/bash
set -euo pipefail

SCRIPT_DIR="/"
PROJECT_ROOT="$(cd "../../.." && pwd)"

GEN_BIN="./gen_files"
REPORT_FILE="mem_report.csv"

TEST_FILES_DIR="test_files"
MODULES_DIR="$PROJECT_ROOT/modules"

SCANNER_MOD="scanner"
SCANNER_KO="$MODULES_DIR/scanner.ko"
SCANNER_START_SYSFS="/sys/kernel/dedup_scanner/run"
SCANNER_ADD_SYSFS="/sys/kernel/dedup_scanner/scan_file"




# -------- SIZE CONFIG --------
SIZES_SINGLE=(64 128 256 512 1024)
SIZES_MULTI=(64 128 256)   # kept small to avoid crash

SHARES=(10 20 30 40 50 60 70 80 90 100)
NUM_FILES_LIST=(1 5)




# -------- CACHE METRIC --------
get_cache_kb() {
    awk '
    /^Active\(file\):/ {a=$2}
    /^Inactive\(file\):/ {b=$2}
    END {print a+b}
    ' /proc/meminfo
}

drop_caches() {
    echo "[*] Dropping caches..."
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

load_scanner() {
    if ! lsmod | awk '{print $1}' | grep -qx "$SCANNER_MOD"; then
        sudo insmod "$SCANNER_KO"
    fi
    echo 1 | sudo tee "$SCANNER_START_SYSFS" >/dev/null
}

unload_scanner() {
    sudo rmmod "$SCANNER_MOD" 2>/dev/null || true
}

register_files() {
    for f in "$@"; do
        echo "$f" | sudo tee "$SCANNER_ADD_SYSFS" >/dev/null
    done
}

run_case() {
    local num_files="$1"
    local size_mb="$2"
    local share_percent="$3"
    local test_type="$4"

    local case_dir="$TEST_FILES_DIR/${test_type}_n${num_files}_s${size_mb}_p${share_percent}"
    rm -rf "$case_dir"
    mkdir -p "$case_dir"

    echo "[*] Generating files"
    "$GEN_BIN" "$num_files" "$share_percent" "$size_mb" "$case_dir" >/dev/null

    local files=("$case_dir"/*.bin)

    drop_caches

    echo "[*] Warming page cache"
    for f in "${files[@]}"; do
        cat "$f" > /dev/null
    done
    sync
    sleep 1

    local before_kb="$(get_cache_kb)"

    echo "[*] Running dedup scanner"
    register_files "${files[@]}"

    # wait for dedup
    [ "$size_mb" -ge 512 ] && sleep 3 || sleep 2

    local after_kb="$(get_cache_kb)"

    local saving_kb=$((before_kb - after_kb))
    local saving_pct=0

    if [ "$before_kb" -gt 0 ]; then
        saving_pct=$((saving_kb * 100 / before_kb))
    fi

    echo "[RESULT] $test_type files=$num_files size=${size_mb}MB share=${share_percent}% → saved=${saving_kb} KB"

    echo "$test_type,$num_files,$size_mb,$share_percent,$before_kb,$after_kb,$saving_kb,$saving_pct" >> "$REPORT_FILE"

    unload_scanner
    drop_caches

    rm -rf "$case_dir"
}

# -------- MAIN --------
mkdir -p "$TEST_FILES_DIR"
echo "test_type,num_files,size_mb,share_percent,before_kb,after_kb,saving_kb,saving_pct" > "$REPORT_FILE"

for num_files in "${NUM_FILES_LIST[@]}"; do

    if [ "$num_files" -eq 1 ]; then
        sizes=("${SIZES_SINGLE[@]}")
        test_type="single"
    else
        sizes=("${SIZES_MULTI[@]}")
        test_type="multi"
    fi

    for size_mb in "${sizes[@]}"; do
        for share_percent in "${SHARES[@]}"; do

            echo "======================================"
            echo "[*] $test_type files=$num_files size=${size_mb}MB shared=${share_percent}%"

            load_scanner
            run_case "$num_files" "$size_mb" "$share_percent" "$test_type"

        done
    done
done

echo "======================================"
echo "[+] Done. Report saved to $REPORT_FILE"
