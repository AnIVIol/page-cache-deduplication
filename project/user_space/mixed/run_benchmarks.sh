#!/bin/bash
# Build and run all benchmarks. Each emits a CSV in $OUTDIR.
set -u

OUTDIR=${OUTDIR:-./bench_results}
mkdir -p "$OUTDIR"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-Wall -Wextra -O2"}

build() {
    local out="$1"; local src="$2"
    echo "  CC  $out"
    $CC $CFLAGS -o "$out" "$src" || exit 1
}

echo "=== Building benchmarks ==="
build bench_savings           bench_savings.c
build bench_op_latency        bench_op_latency.c
build bench_large_pressure    bench_large_pressure.c
build bench_scanner_profile   bench_scanner_profile.c

if [ ! -d /sys/kernel/dedup_scanner ]; then
    echo "ERROR: dedup_scanner module not loaded"; exit 1
fi

echo "=== 1/4 bench_savings (memory savings matrix) ==="
sudo ./bench_savings           "$OUTDIR/savings.csv"

echo "=== 2/4 bench_op_latency (read/write/trunc/cp before vs after) ==="
sudo ./bench_op_latency        "$OUTDIR/op_latency.csv"

echo "=== 3/4 bench_scanner_profile (CPU/mem during sweep) ==="
sudo ./bench_scanner_profile   "$OUTDIR/scanner_profile.csv"

echo "=== 4/4 bench_large_pressure (1GB files, pre-register, pressure) ==="
sudo ./bench_large_pressure    "$OUTDIR/pressure.csv"

echo
echo "All CSVs written under: $OUTDIR/"
ls -la "$OUTDIR"
