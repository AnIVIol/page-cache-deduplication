#!/bin/bash
#
# bench_truncate.sh
#
# Measure truncate() latency and stability with and without page-cache
# deduplication, across a matrix of file sizes, share %, and groups.
#
# Phase 1 — baseline truncate (dedup OFF)
# Phase 2 — post-dedup truncate
# Phase 3 — concurrent fio + truncate stress
#
# Inspired by the existing read benchmark.

set -e

GEN_BIN="${GEN_BIN:-./gen_files}"
SCANNER_KO="${SCANNER_KO:-../modules/scanner.ko}"
REPORT="truncate_report.csv"
WORK_DIR="${WORK_DIR:-/tmp/bench_truncate}"

SIZES=(512)
SHARES=(0 30 60 90)
GROUPS_LIST=(1 3)
RUNS=1
NUM_FILES=4
FIO_RUNTIME=10           # seconds (Phase 3 background fio)

echo "size_MB,share_pct,groups,phase,run,latency_ms,errors,unevict_kB" > "$REPORT"

# ----------------------- helpers -----------------------
drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

scanner_start() {
    sudo insmod "$SCANNER_KO" 2>/dev/null || true
    echo "200" | sudo tee /sys/kernel/dedup_scanner/interval >/dev/null
    echo "1"   | sudo tee /sys/kernel/dedup_scanner/run      >/dev/null
}

scanner_stop() {
    echo "0" | sudo tee /sys/kernel/dedup_scanner/run >/dev/null 2>&1 || true
    sleep 1
    sudo rmmod scanner 2>/dev/null || true
}

scanner_register() {
    echo "$1" | sudo tee /sys/kernel/dedup_scanner/scan_file >/dev/null
}

unevict_kb() {
    awk '/^Unevictable:/ {print $2}' /proc/meminfo
}

# Warm cache + register all files in $DIR with the scanner.
warm_and_register() {
    local d=$1 n=$2
    for i in $(seq 0 $((n-1))); do
        local f="$d/file_$i.bin"
        cat "$f" > /dev/null
        scanner_register "$(realpath "$f")"
    done
}

# Replicate file_0.bin -> file_1..file_(n-1) so cross-file dedup can occur.
replicate_master() {
    local d=$1 n=$2
    for i in $(seq 1 $((n-1))); do
        cp "$d/file_0.bin" "$d/file_$i.bin"
    done
}

# Time a truncate to <size_bytes>. Echoes latency in ms or "NA" on failure.
time_truncate() {
    local file=$1
    local newsize=$2
    local start end rc
    start=$(date +%s.%N)
    truncate -s "$newsize" "$file" 2>/dev/null
    rc=$?
    end=$(date +%s.%N)
    if [ $rc -ne 0 ]; then echo "NA"; return 1; fi
    echo "$(echo "($end - $start) * 1000" | bc -l)"
    return 0
}

# Start fio in the background. Echoes its PID.
run_fio_bg() {
    local file=$1 sizeMB=$2 runtime=$3
    fio --name=stress \
        --filename="$file" \
        --rw=randread \
        --bs=4k \
        --size="${sizeMB}M" \
        --ioengine=sync \
        --direct=0 \
        --numjobs=4 \
        --time_based \
        --runtime="$runtime" \
        --group_reporting \
        > /dev/null 2>&1 &
    echo $!
}

# ----------------------- sanity -----------------------
if [ ! -x "$GEN_BIN" ]; then
    echo "ERROR: '$GEN_BIN' not found or not executable. Build with 'make gen_files'."
    exit 1
fi
if [ ! -f "$SCANNER_KO" ]; then
    echo "ERROR: '$SCANNER_KO' not found. Set SCANNER_KO=/path/to/scanner.ko if needed."
    exit 1
fi
if ! command -v fio >/dev/null; then
    echo "ERROR: fio is not installed."
    exit 1
fi

mkdir -p "$WORK_DIR"
trap 'scanner_stop; rm -rf "$WORK_DIR"' EXIT

# ----------------------- main loop -----------------------
for size in "${SIZES[@]}"; do
for share in "${SHARES[@]}"; do
for groups in "${GROUPS_LIST[@]}"; do

# Skip illegal combinations.
if [ "$((share * groups))" -gt 100 ]; then continue; fi

echo "==================================================="
echo "[*] SIZE=${size}MB  SHARE=${share}%  GROUPS=${groups}"
echo "==================================================="

DIR="$WORK_DIR/${size}_${share}_${groups}"
rm -rf "$DIR"; mkdir -p "$DIR"

halfsize=$((size * 1024 * 1024 / 2))

# =============== PHASE 1 — baseline ===============
echo "[*] Phase 1 (baseline truncate, dedup OFF)"
errs=0
for r in $(seq 1 $RUNS); do
    "$GEN_BIN" 1 $share $groups $size "$DIR" >/dev/null
    drop_caches
    cat "$DIR/file_0.bin" > /dev/null

    if t=$(time_truncate "$DIR/file_0.bin" "$halfsize"); then
        echo "$size,$share,$groups,baseline,$r,$t,0,$(unevict_kb)" >> "$REPORT"
    else
        echo "$size,$share,$groups,baseline,$r,NA,1,$(unevict_kb)" >> "$REPORT"
        errs=$((errs+1))
    fi
done
echo "  Phase 1 errors: $errs"

# =============== PHASE 2 — post-dedup ===============
echo "[*] Phase 2 (truncate after dedup)"
"$GEN_BIN" 1 $share $groups $size "$DIR" >/dev/null
replicate_master "$DIR" $NUM_FILES

scanner_start
drop_caches
warm_and_register "$DIR" $NUM_FILES
sleep 1                         # give dedup time to settle

ue_before=$(unevict_kb)
echo "  Unevictable after dedup: ${ue_before} kB"
echo "$DIR/file_0.bin" | sudo tee /sys/kernel/pagecache_inspector/filename_print
errs=0
for r in $(seq 1 $RUNS); do
    F="$DIR/file_$((r-1)).bin"
    if t=$(time_truncate "$F" "$halfsize"); then
        echo "$size,$share,$groups,dedup,$r,$t,0,$(unevict_kb)" >> "$REPORT"
    else
        echo "$size,$share,$groups,dedup,$r,NA,1,$(unevict_kb)" >> "$REPORT"
        errs=$((errs+1))
    fi
done
echo "  Phase 2 errors: $errs"

scanner_stop

# =============== PHASE 3 — fio + truncate stress ===============
echo "[*] Phase 3 (truncate under concurrent fio read load)"
"$GEN_BIN" 1 $share $groups $size "$DIR" >/dev/null
replicate_master "$DIR" $NUM_FILES

scanner_start
drop_caches
warm_and_register "$DIR" $NUM_FILES
sleep 1

ue_before=$(unevict_kb)
echo "  Unevictable after dedup: ${ue_before} kB"

# Background fio reads on file_0
F0="$DIR/file_0.bin"
FIO_PID=$(run_fio_bg "$F0" "$size" "$FIO_RUNTIME")

errs=0
for r in $(seq 1 $RUNS); do
    target=$(( ((r-1) % (NUM_FILES-1)) + 1 ))   # cycles 1..NUM_FILES-1
    F="$DIR/file_$target.bin"
    if t=$(time_truncate "$F" "$halfsize"); then
        echo "$size,$share,$groups,stress,$r,$t,0,$(unevict_kb)" >> "$REPORT"
    else
        echo "$size,$share,$groups,stress,$r,NA,1,$(unevict_kb)" >> "$REPORT"
        errs=$((errs+1))
    fi
    sleep 1
done

# Wait for fio to finish (up to FIO_RUNTIME).
wait "$FIO_PID" 2>/dev/null || true
echo "  Phase 3 errors: $errs"

scanner_stop
rm -rf "$DIR"

done
done
done

trap - EXIT
scanner_stop
rmdir "$WORK_DIR" 2>/dev/null || true

echo "==================================================="
echo "[+] DONE. Results saved in $REPORT"
echo "==================================================="
echo "    Tail of dmesg (check for warnings/oops):"
echo "---------------------------------------------------"
sudo dmesg | tail -n 20
