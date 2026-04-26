#!/bin/bash

# MASTER Test Suite - Symlinks & CP with Dedup
# Structure: ../setup_env.sh -> Create files -> Feed to scanner -> Run all tests

set -e

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# --- Defaults ---
FILE_SIZE_MB=256
SHARE_PERCENT=100
NUM_GROUPS=1
BLOCK_SIZE="4k"

TEST_DIR="$(pwd)/dedup_master_test"
LOG_FILE="$TEST_DIR/master_test_$(date +%s).log"

SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

# --- Parse CLI args ---
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -s|--size) FILE_SIZE_MB="$2"; shift ;;
        -p|--percent) SHARE_PERCENT="$2"; shift ;;
        -g|--groups) NUM_GROUPS="$2"; shift ;;
        -b|--bs) BLOCK_SIZE="$2"; shift ;;
        -h|--help)
            echo "Usage: $0 [-s sizeMB] [-p percent] [-g groups] [-b blocksize]"
            exit 0
            ;;
        *)
            echo "[-] Unknown parameter: $1"
            exit 1
            ;;
    esac
    shift
done

# --- Sanity check ---
if (( SHARE_PERCENT * NUM_GROUPS > 100 )); then
    echo "[-] ERROR: percent * groups must be <= 100"
    exit 1
fi

echo "==========================================================="
echo "  MASTER Test Suite: Symlinks & CP with Dedup"
echo "==========================================================="
echo "=== Parameters ==="
echo "Size: ${FILE_SIZE_MB}MB | Share: ${SHARE_PERCENT}% | Groups: ${NUM_GROUPS} | BS: ${BLOCK_SIZE}"
echo "==========================================================="
echo

{
    # Initialize counters
    TOTAL_TESTS=0
    TOTAL_PASSED=0
    TOTAL_FAILED=0

    # --- PHASE 0: Setup ---
    echo "=== PHASE 0: Setup Environment ==="
    
    mkdir -p "$TEST_DIR"
    
    # Create subdirectories for each test group
    mkdir -p "$TEST_DIR/symlinks"
    mkdir -p "$TEST_DIR/cp_test"
    mkdir -p "$TEST_DIR/combined"
    
    if [ ! -f "$SCANNER_SYSFS" ]; then
        echo "[-] ERROR: Scanner sysfs not found"
        exit 1
    fi  
    echo "[+] Scanner verified"
    echo "[+] Test directories created"
    echo

    # --- PHASE 1: Symlink Operations ---
    echo "=== PHASE 1: Symlink Operations ==="
    
    SYMLINK_DIR="$TEST_DIR/symlinks"
    
    # Generate files
    if command -v gen_dedup &> /dev/null; then
        gen_dedup "$FILE_SIZE_MB" "$SHARE_PERCENT" "$NUM_GROUPS" "$SYMLINK_DIR/orig.dat" || exit 1
    else
        dd if=/dev/zero bs=1M count=$FILE_SIZE_MB 2>/dev/null | tr '\0' 'A' > "$SYMLINK_DIR/orig.dat"
    fi  
    
    cp "$SYMLINK_DIR/orig.dat" "$SYMLINK_DIR/dup1.dat"
    cp "$SYMLINK_DIR/orig.dat" "$SYMLINK_DIR/dup2.dat"
    echo "[+] Created 3 identical files for symlink test"
    ((TOTAL_TESTS+=3))
    ((TOTAL_PASSED+=3))
    
    sync
    
    # Feed to scanner
    echo "$SYMLINK_DIR/orig.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$SYMLINK_DIR/dup1.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$SYMLINK_DIR/dup2.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    sleep 2
    echo "[+] Files fed to scanner"
    
    # Create symlinks
    ln -s "$SYMLINK_DIR/orig.dat" "$SYMLINK_DIR/link_orig.dat"
    ln -s "$SYMLINK_DIR/dup1.dat" "$SYMLINK_DIR/link_dup.dat"
    ln -s "$SYMLINK_DIR/link_orig.dat" "$SYMLINK_DIR/link_chain.dat"
    echo "[+] Symlinks created"
    
    # Test reads
    if [ "$(head -c 1 "$SYMLINK_DIR/link_orig.dat")" = "A" ]; then
        echo "[+] Symlink to original readable"
        ((TOTAL_PASSED++))
    else
        echo "[-] Symlink to original failed"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    if [ "$(head -c 1 "$SYMLINK_DIR/link_dup.dat")" = "A" ]; then
        echo "[+] Symlink to dedup'd file readable"
        ((TOTAL_PASSED++))
    else
        echo "[-] Symlink to dedup'd file failed"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    # Test modification
    ./write_offset "$SYMLINK_DIR/link_orig.dat" 0 "MODIFIED"
    if [ "$(head -c 8 "$SYMLINK_DIR/orig.dat")" = "MODIFIED" ]; then
        echo "[+] Symlink write successful"
        ((TOTAL_PASSED++))
    else
        echo "[-] Symlink write failed"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    # Check CoW
    if [ "$(head -c 1 "$SYMLINK_DIR/dup1.dat")" = "A" ]; then
        echo "[+] CoW split worked"
        ((TOTAL_PASSED++))
    else
        echo "[-] CoW split failed"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    echo "Symlink phase: $TOTAL_PASSED/$((TOTAL_PASSED + TOTAL_FAILED)) passed"
    echo

    # --- PHASE 2: CP Operations ---
    echo "=== PHASE 2: CP Operations ==="
    
    CP_DIR="$TEST_DIR/cp_test"
    
    # Generate source
    if command -v gen_dedup &> /dev/null; then
        gen_dedup "$FILE_SIZE_MB" "$SHARE_PERCENT" "$NUM_GROUPS" "$CP_DIR/source.dat" || exit 1
    else
        dd if=/dev/zero bs=1M count=$FILE_SIZE_MB 2>/dev/null | tr '\0' 'B' > "$CP_DIR/source.dat"
    fi  
    echo "[+] Source file created"
    ((TOTAL_TESTS++))
    ((TOTAL_PASSED++))
    
    # Create copies
    for i in {1..4}; do
        cp "$CP_DIR/source.dat" "$CP_DIR/copy_$i.dat"
    done
    echo "[+] 4 copies created"
    ((TOTAL_TESTS++))
    ((TOTAL_PASSED++))
    
    sync
    
    # Feed to scanner
    echo "$CP_DIR/source.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    for i in {1..4}; do
        echo "$CP_DIR/copy_$i.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    done
    sleep 2
    echo "[+] Files fed to scanner"
    
    # Verify copies
    COPIES_OK=1
    for i in {1..4}; do
        if [ "$(head -c 1 "$CP_DIR/copy_$i.dat")" != "B" ]; then
            COPIES_OK=0
        fi
    done
    if [ "$COPIES_OK" = "1" ]; then
        echo "[+] All copies readable"
        ((TOTAL_PASSED++))
    else
        echo "[-] Some copies unreadable"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    # Modify first copy
    ./write_offset "$CP_DIR/copy_1.dat" 0 "MODIFIED_COPY"
    if [ "$(head -c 1 "$CP_DIR/source.dat")" = "B" ]; then
        echo "[+] Source unchanged after copy modification"
        ((TOTAL_PASSED++))
    else
        echo "[-] Source was modified (data corruption)"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    # Partial modification
    ./write_offset "$CP_DIR/copy_2.dat" 0 "X"
    if [ "$(head -c 1 "$CP_DIR/source.dat")" = "B" ]; then
        echo "[+] Source unchanged after partial modify"
        ((TOTAL_PASSED++))
    else
        echo "[-] Source corrupted"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    echo "CP phase: $TOTAL_PASSED/$((TOTAL_PASSED + TOTAL_FAILED)) passed"
    echo

    # --- PHASE 3: Combined Operations ---
    echo "=== PHASE 3: Combined Symlink + CP Operations ==="
    
    COMBINED_DIR="$TEST_DIR/combined"
    
    # Create, copy, and symlink
    echo "COMBINED_DATA" > "$COMBINED_DIR/orig.dat"
    cp "$COMBINED_DIR/orig.dat" "$COMBINED_DIR/copy.dat"
    
    ln -s "$COMBINED_DIR/orig.dat" "$COMBINED_DIR/link_orig.dat"
    ln -s "$COMBINED_DIR/copy.dat" "$COMBINED_DIR/link_copy.dat"
    
    echo "$COMBINED_DIR/orig.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$COMBINED_DIR/copy.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    sleep 2
    
    # Test combined reads
    if [ "$(cat "$COMBINED_DIR/link_orig.dat")" = "COMBINED_DATA" ]; then
        echo "[+] Symlink to original works"
        ((TOTAL_PASSED++))
    else
        echo "[-] Symlink to original failed"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    if [ "$(cat "$COMBINED_DIR/link_copy.dat")" = "COMBINED_DATA" ]; then
        echo "[+] Symlink to copy works"
        ((TOTAL_PASSED++))
    else
        echo "[-] Symlink to copy failed"
        ((TOTAL_FAILED++))
    fi  
    ((TOTAL_TESTS++))
    
    echo "Combined phase: $TOTAL_PASSED/$((TOTAL_PASSED + TOTAL_FAILED)) passed"
    echo

    # --- PHASE 4: Summary ---
    echo "==========================================================="
    echo "  MASTER TEST SUMMARY"
    echo "==========================================================="
    echo "Total Tests: $TOTAL_TESTS"
    echo "Passed: $TOTAL_PASSED"
    echo "Failed: $TOTAL_FAILED"
    echo "Success Rate: $((TOTAL_PASSED * 100 / TOTAL_TESTS))%"
    
    if [ $TOTAL_FAILED -eq 0 ]; then
        echo "Status: [+] ALL TESTS PASSED"
    else
        echo "Status: [-] FAILURES DETECTED"
    fi  
    
    echo "==========================================================="
    echo "Test Directory: $TEST_DIR"
    echo "Log File: $LOG_FILE"

} | tee "$LOG_FILE"

# --- Cleanup ---
echo
echo "=== Cleanup ==="
rm -rf "$TEST_DIR"
echo "[+] Test complete and cleaned up"
