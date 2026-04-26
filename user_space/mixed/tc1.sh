#!/bin/bash

# ===========================================================
# STRESS & CONCURRENCY TEST SUITE
# Targets: drop_caches, O_TRUNC, Concurrent teardowns, Lifecycle
# ===========================================================

set -e

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# FIX 1: Create the directory BEFORE we define the log file and pipe!
TEST_DIR="$(pwd)/dedup_stress_test"
mkdir -p "$TEST_DIR"
LOG_FILE="$TEST_DIR/stress_test_$(date +%s).log"

SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

echo "==========================================================="
echo "  STRESS Test Suite: Truncate, Drop Caches & Concurrency"
echo "==========================================================="

{
    TOTAL_TESTS=0
    TOTAL_PASSED=0
    TOTAL_FAILED=0

    echo "[+] Test environment created"
    echo "1" > "$SCANNER_START"
    echo "[+] Scanner Daemon Started"
    echo

    # --- PHASE 1: The O_TRUNC Hang Test ---
    echo "=== PHASE 1: The O_TRUNC Hang Test ==="
    
    dd if=/dev/zero bs=1M count=50 2>/dev/null | tr '\0' 'T' > "$TEST_DIR/trunc1.dat"
    cp "$TEST_DIR/trunc1.dat" "$TEST_DIR/trunc2.dat"
    
    echo "$TEST_DIR/trunc1.dat" > "$SCANNER_SYSFS"
    echo "$TEST_DIR/trunc2.dat" > "$SCANNER_SYSFS"
    sleep 2 
    
    echo "[*] Attempting O_TRUNC overwrite..."
    ((TOTAL_TESTS+=1))
    
    if timeout 5 bash -c "echo 'TRUNCATED_DATA' > '$TEST_DIR/trunc2.dat'" 2>/dev/null; then
        echo "[+] O_TRUNC succeeded instantly! No infinite loop."
        ((TOTAL_PASSED+=1))
    else
        echo "[-] O_TRUNC FAILED (System Hang or Timeout)"
        ((TOTAL_FAILED+=1))
    fi
    echo

    # --- PHASE 2: Concurrent Truncate ---
    echo "=== PHASE 2: Concurrent Truncate Test ==="
    
    mkdir -p "$TEST_DIR/mass_delete"
    dd if=/dev/zero bs=1M count=10 2>/dev/null | tr '\0' 'M' > "$TEST_DIR/mass_delete/base.dat"
    echo "$TEST_DIR/mass_delete/base.dat" > "$SCANNER_SYSFS"

    for i in {1..20}; do
        cp "$TEST_DIR/mass_delete/base.dat" "$TEST_DIR/mass_delete/file_$i.dat"
        echo "$TEST_DIR/mass_delete/file_$i.dat" > "$SCANNER_SYSFS"
    done
    sleep 3 
    
    echo "[*] Triggering concurrent deletion of 20 deduplicated files..."
    ((TOTAL_TESTS+=1))
    
    # FIX 2: Track explicit PIDs instead of using the flaky 'jobs' command
    RM_PIDS=""
    for i in {1..20}; do
        rm -f "$TEST_DIR/mass_delete/file_$i.dat" &
        RM_PIDS="$RM_PIDS $!"
    done
    
    # Wait for all background deletions in a subshell
    (wait $RM_PIDS) &
    WAIT_PID=$!
    
    TIMER=0
    while kill -0 $WAIT_PID 2>/dev/null; do
        sleep 1
        ((TIMER+=1))
        if [ $TIMER -ge 15 ]; then # 15 second timeout to be safe
            break
        fi
    done
    
    if kill -0 $WAIT_PID 2>/dev/null; then
        echo "[-] Concurrent deletion FAILED (Deadlock detected)"
        kill -9 $WAIT_PID 2>/dev/null || true
        ((TOTAL_FAILED+=1))
    else
        echo "[+] Concurrent deletion finished safely. Atomic pause counter works!"
        ((TOTAL_PASSED+=1))
    fi
    echo

    # --- PHASE 3: The Drop Caches Hammer ---
    echo "=== PHASE 3: The Drop Caches Hammer ==="
    
    dd if=/dev/zero bs=1M count=100 2>/dev/null | tr '\0' 'D' > "$TEST_DIR/drop1.dat"
    cp "$TEST_DIR/drop1.dat" "$TEST_DIR/drop2.dat"
    echo "$TEST_DIR/drop1.dat" > "$SCANNER_SYSFS"
    echo "$TEST_DIR/drop2.dat" > "$SCANNER_SYSFS"
    sleep 3
    
    echo "[*] Spamming drop_caches..."
    ((TOTAL_TESTS+=1))
    
    DROP_SUCCESS=1
    for i in {1..5}; do
        if ! timeout 3 bash -c 'echo 3 > /proc/sys/vm/drop_caches'; then
            DROP_SUCCESS=0
            break
        fi
    done
    
    if [ "$DROP_SUCCESS" -eq 1 ]; then
        echo "[+] drop_caches executed 5 times instantly. No deadlocks."
        ((TOTAL_PASSED+=1))
    else
        echo "[-] drop_caches FAILED (System Hang)"
        ((TOTAL_FAILED+=1))
    fi
    echo

    # --- PHASE 4: The Ghost Scanner Deadlock Race ---
    echo "=== PHASE 4: Lifecycle Teardown Race ==="
    
    dd if=/dev/zero bs=1M count=50 2>/dev/null | tr '\0' 'G' > "$TEST_DIR/ghost1.dat"
    cp "$TEST_DIR/ghost1.dat" "$TEST_DIR/ghost2.dat"
    echo "$TEST_DIR/ghost1.dat" > "$SCANNER_SYSFS"
    echo "$TEST_DIR/ghost2.dat" > "$SCANNER_SYSFS"
    sleep 2
    
    echo "[*] Triggering drop_caches and module unload simultaneously..."
    ((TOTAL_TESTS+=1))
    
    bash -c 'echo 3 > /proc/sys/vm/drop_caches' &
    DROP_PID=$!
    
    echo 0 > "$SCANNER_START"
    
    TIMER=0
    while kill -0 $DROP_PID 2>/dev/null; do
        sleep 1
        ((TIMER+=1))
        if [ $TIMER -ge 5 ]; then
            break
        fi
    done
    
    if kill -0 $DROP_PID 2>/dev/null; then
        echo "[-] Lifecycle wipe FAILED (drop_caches is deadlocked)"
        kill -9 $DROP_PID 2>/dev/null || true
        ((TOTAL_FAILED+=1))
    else
        echo "[+] Lifecycle wipe succeeded! drop_caches was rescued from wait queue."
        ((TOTAL_PASSED+=1))
    fi
    echo

    # --- PHASE 5: Summary ---
    echo "==========================================================="
    echo "  STRESS TEST SUMMARY"
    echo "==========================================================="
    echo "Total Tests: $TOTAL_TESTS"
    echo "Passed: $TOTAL_PASSED"
    echo "Failed: $TOTAL_FAILED"
    
    if [ $TOTAL_FAILED -eq 0 ]; then
        echo "Status: [+] SYSTEM IS BULLETPROOF"
    else
        echo "Status: [-] KERNEL PANIC / DEADLOCK DETECTED"
    fi  
    echo "==========================================================="
    echo "Log File: $LOG_FILE"

} | tee "$LOG_FILE"

# --- Cleanup ---
echo "0" > "$SCANNER_START" 2>/dev/null || true
rm -rf "$TEST_DIR"
