#!/bin/bash
# Build and run the full dedup test suite, including the mixed-ops tests.
set -u
LOG=/tmp/dedup_test.log
: > "$LOG"

CC=${CC:-gcc}
CFLAGS="-Wall -Wextra -O2"
PCFLAGS="$CFLAGS -pthread"

build() {
    local out="$1"; local flags="$2"; local src="$3"
    echo "  CC  $out"
    $CC $flags -o "$out" "$src" || { echo "BUILD FAIL: $out"; exit 1; }
}

../setup_env.sh

echo "=== Building tests ==="
build test_basic_internal      "$CFLAGS"  test_basic_internal.c
build test_basic_cross         "$CFLAGS"  test_basic_cross.c
build test_write_cow           "$CFLAGS"  test_write_cow.c
build test_truncate            "$CFLAGS"  test_truncate.c
build test_concurrent_read     "$PCFLAGS" test_concurrent_read.c
build test_refcount_stress     "$CFLAGS"  test_refcount_stress.c
build test_thousand_files      "$CFLAGS"  test_thousand_files.c
build test_sparse              "$CFLAGS"  test_sparse.c
build multithread_write_test   "$PCFLAGS" multithread_write_test.c
build test_mixed_rw            "$PCFLAGS" test_mixed_rw.c
build test_mixed_rwt           "$PCFLAGS" test_mixed_rwt.c
build test_mixed_rwc           "$PCFLAGS" test_mixed_rwc.c
build test_chaos_all           "$PCFLAGS" test_chaos_all.c
build test_ordered_phases      "$PCFLAGS" test_ordered_phases.c
build test_samepage_storm      "$PCFLAGS" test_samepage_storm.c

run() {
    local name="$1"; shift
    echo "=== $name ===" | tee -a "$LOG"
    if "$@" >>"$LOG" 2>&1; then
        echo "PASS: $name" | tee -a "$LOG"
    else
        echo "FAIL: $name (see $LOG)" | tee -a "$LOG"
    fi
}

if [ ! -d /sys/kernel/dedup_scanner ] || [ ! -d /sys/kernel/page_dedup ]; then
    echo "ERROR: dedup modules not loaded."
    exit 1
fi

echo "=== Running tests ==="
# Basic
run "1.1 single-file dedup"      ./test_basic_internal
run "1.2 cross-file dedup"       ./test_basic_cross
run "1.3 write CoW"              ./test_write_cow
run "1.4 truncate cleanup"       ./test_truncate
# Concurrency primitives
run "2.1 concurrent readers"     ./test_concurrent_read
run "3.* multithread writes"     ./multithread_write_test -t 16 -f 8 -p 32 -o 2000
# Refcount + scale
run "6.5 refcount stress"        ./test_refcount_stress
run "8.1 1000-file dedup"        ./test_thousand_files
# Edge
run "9.3 sparse file"            ./test_sparse
# Mixed concurrent operations (NEW)
run "MIX-RW   readers+writers"             ./test_mixed_rw
run "MIX-RWT  readers+writers+truncate"    ./test_mixed_rwt
run "MIX-RWC  readers+writers+cp"          ./test_mixed_rwc
run "CHAOS    all-ops together"            ./test_chaos_all
run "PHASED   ordered phase test"          ./test_ordered_phases
run "STORM    same-page contention"        ./test_samepage_storm

echo ""
echo "===== SUMMARY ====="
grep -E '^(PASS|FAIL):' "$LOG"
echo ""
echo "Full log: $LOG"
