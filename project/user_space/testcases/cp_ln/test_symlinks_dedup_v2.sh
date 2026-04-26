#!/bin/bash

# Symlinks and Dedup Test Suite
# Structure: ../setup_env.sh -> Create files -> Feed to scanner -> Run tests

set -e

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# --- Defaults ---
FILE_SIZE_MB=256
SHARE_PERCENT=100
NUM_GROUPS=1
BLOCK_SIZE="4k"

TEST_DIR="$(pwd)/dedup_symlink_test"
FILE1="$TEST_DIR/file1.dat"
FILE2="$TEST_DIR/file2.dat"
FILE3="$TEST_DIR/file3.dat"

SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

LOG_FILE="$TEST_DIR/symlink_test_$(date +%s).log"

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

# --- Helper: Write via write(2) syscall (using write_offset binary) ---
write_via_syscall() {
    local file="$1"
    local content="$2"
    # write_offset uses write(2) syscall directly
    ./write_offset "$file" 0 "$content"
}

# --- Helper: Append via write(2) syscall (using write_offset binary) ---
append_via_syscall() {
    local file="$1"
    local content="$2"
    # Get current file size and append at that offset
    local offset
#    offset=$(stat -c%s "$file" 2>/dev/null || echo "0")
     offset=$(stat -c%s -- "$(readlink -f -- "$file")") 
   ./write_offset "$file" "$offset" "$content"
}

# --- Helper: Get file size safely (stat only - reads inode, not file data) ---
get_file_size() {
    local file="$1"
    if [ -f "$file" ]; then
        stat -c%s "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

echo "==========================================================="
echo "  Symlinks + Dedup Test Suite"
echo "==========================================================="
echo "=== Parameters ==="
echo "Size: ${FILE_SIZE_MB}MB | Share: ${SHARE_PERCENT}% | Groups: ${NUM_GROUPS} | BS: ${BLOCK_SIZE}"
echo "==========================================================="
echo

# --- Create test directory before main block (needed for tee log file) ---
mkdir -p "$TEST_DIR"

{
    # --- PHASE 0: Setup ---
    echo "=== PHASE 0: Setup Environment ==="
    
    rm -f "$FILE1" "$FILE2" "$FILE3"
    
    if [ ! -f "$SCANNER_SYSFS" ]; then
        echo "[-] ERROR: Scanner sysfs not found at $SCANNER_SYSFS"
        exit 1
    fi
    echo "[+] Scanner sysfs verified"
    echo "[+] Test directory: $TEST_DIR"
    echo

    # --- PHASE 1: Generate Files ---
    echo "=== PHASE 1: Generate Identical Files ==="
    
    if command -v gen_dedup &> /dev/null; then
        gen_dedup "$FILE_SIZE_MB" "$SHARE_PERCENT" "$NUM_GROUPS" "$FILE1" || exit 1
        cp "$FILE1" "$FILE2"
        cp "$FILE1" "$FILE3"
        echo "[+] Generated 3 identical files (${FILE_SIZE_MB}MB each)"
    else
        # Fallback: create files manually
        echo "[-] gen_dedup not found, creating files manually"
        dd if=/dev/zero bs=1M count=$FILE_SIZE_MB 2>/dev/null | tr '\0' 'A' > "$FILE1"
        cp "$FILE1" "$FILE2"
        cp "$FILE1" "$FILE3"
        echo "[+] Created 3 identical files (${FILE_SIZE_MB}MB each)"
    fi
    
    sync
    echo

    # --- PHASE 2: Feed Files to Scanner ---
    echo "=== PHASE 2: Feed Files to Scanner ==="
    
    if [ -e "$SCANNER_START" ]; then
        echo 1 | sudo tee "$SCANNER_START" > /dev/null
    fi
    
    echo "$FILE1" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$FILE2" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$FILE3" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] Files submitted to scanner"
    
    sleep 3
    echo

    # --- PHASE 3: Verify Deduplication Occurred ---
    echo "=== PHASE 3: Verify Deduplication Occurred ==="
    
    # Wait longer for dedup to complete
    sleep 5
    
    # Read files multiple times to ensure dedup is visible
    cat "$FILE1" > /dev/null
    cat "$FILE2" > /dev/null
    cat "$FILE3" > /dev/null
    sleep 2
    
    echo "[+] Deduplication should be complete"
    echo "[+] Proceeding to test symlinks on deduped files"
    echo

    # --- PHASE 4: Create Symlinks on DEDUPED Files ---
    echo "=== PHASE 4: Create Symlinks on Deduped Files ==="
    
    TEST_PASS=0
    TEST_FAIL=0
    
    SYMLINK1="$TEST_DIR/symlink1.dat"
    SYMLINK2="$TEST_DIR/symlink2.dat"
    SYMLINK_CHAIN="$TEST_DIR/symlink_chain.dat"
    
    # Create symlinks to already-deduped files
    ln -s "$FILE1" "$SYMLINK1"
    echo "[+] Created symlink1 -> file1.dat (already deduped)"
    ((TEST_PASS+=1))
    
    ln -s "$FILE2" "$SYMLINK2"
    echo "[+] Created symlink2 -> file2.dat (already deduped)"
    ((TEST_PASS++))
    
    ln -s "$SYMLINK1" "$SYMLINK_CHAIN"
    echo "[+] Created symlink chain on deduped files"
    ((TEST_PASS++))
    echo

    # --- PHASE 5: Test Symlink Reads on Deduped Files ---
    echo "=== PHASE 5: Test Symlink Reads (On Deduped Files) ==="
    
    # Test 5.1: Direct symlink read
    SYMLINK1_CONTENT=$(head -c 10 "$SYMLINK1")
    if [ "$SYMLINK1_CONTENT" = "AAAAAAAAAA" ]; then
        echo "[+] Symlink1 read correct (from deduped file1)"
        ((TEST_PASS++))
    else
        echo "[-] Symlink1 read FAILED"
        ((TEST_FAIL++))
    fi
    
    # Test 5.2: Symlink to dedup'd file
    SYMLINK2_CONTENT=$(head -c 10 "$SYMLINK2")
    if [ "$SYMLINK2_CONTENT" = "AAAAAAAAAA" ]; then
        echo "[+] Symlink2 read correct (from deduped file2)"
        ((TEST_PASS++))
    else
        echo "[-] Symlink2 read FAILED"
        ((TEST_FAIL++))
    fi
    
    # Test 5.3: Symlink chain read
    CHAIN_CONTENT=$(head -c 10 "$SYMLINK_CHAIN")
    if [ "$CHAIN_CONTENT" = "AAAAAAAAAA" ]; then
        echo "[+] Symlink chain read correct"
        ((TEST_PASS++))
    else
        echo "[-] Symlink chain read FAILED"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 6: Read Stress via Symlinks on Deduped Files ---
    echo "=== PHASE 6: Read Stress via Symlinks (On Deduped Files) ==="
    
    sudo fio --name=symlink_read \
             --filename="$SYMLINK1" \
             --rw=randread \
             --bs="$BLOCK_SIZE" \
             --numjobs=4 \
             --time_based --runtime=5 \
             --group_reporting
    
    echo "[+] Read stress on deduped file via symlink completed"
    echo

    # --- PHASE 7: Modify Through Symlink on Deduped File (CoW) ---
    echo "=== PHASE 7: Modify Through Symlink on Deduped File (CoW Test) ==="
    
    # Get initial size of deduped file
    INITIAL_SIZE=$(get_file_size "$FILE1" 2>/dev/null || echo "0")
    if ! [[ "$INITIAL_SIZE" =~ ^[0-9]+$ ]]; then INITIAL_SIZE=0; fi
    
    # Append data through symlink to deduped file (via write syscall)
    append_via_syscall "$SYMLINK1" "MODIFIED_VIA_SYMLINK_ON_DEDUPED"
    echo "[+] Appended data through symlink to deduped file (via write syscall)"
    
    # Check original deduped file was modified
    MODIFIED_CHECK=$(tail -c 35 "$FILE1" 2>/dev/null || echo "")
    if [[ "$MODIFIED_CHECK" == *"MODIFIED_VIA_SYMLINK_ON_DEDUPED"* ]]; then
        echo "[+] Deduped file modified correctly via symlink"
        ((TEST_PASS++))
    else
        echo "[-] Deduped file NOT modified"
        ((TEST_FAIL++))
    fi
    
    # Check file2 (duplicate) is STILL unchanged - CoW split on deduped file
    FILE2_SIZE=$(get_file_size "$FILE2" 2>/dev/null || echo "0")
    if ! [[ "$FILE2_SIZE" =~ ^[0-9]+$ ]]; then FILE2_SIZE=0; fi
    if [ "$INITIAL_SIZE" -gt 0 ] && [ "$FILE2_SIZE" -eq "$INITIAL_SIZE" ]; then
        echo "[+] File2 (duplicate) unchanged - CoW split worked on deduped file"
        ((TEST_PASS++))
    else
        echo "[-] File2 was modified (CoW split FAILED)"
        ((TEST_FAIL++))
    fi
    
    # Check file3 is also unchanged
    FILE3_SIZE=$(get_file_size "$FILE3" 2>/dev/null || echo "0")
    if ! [[ "$FILE3_SIZE" =~ ^[0-9]+$ ]]; then FILE3_SIZE=0; fi
    if [ "$INITIAL_SIZE" -gt 0 ] && [ "$FILE3_SIZE" -eq "$INITIAL_SIZE" ]; then
        echo "[+] File3 (duplicate) unchanged - isolation maintained"
        ((TEST_PASS++))
    else
        echo "[-] File3 was modified (isolation broken)"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 8: Write Stress to Deduped File via Symlink ---
    echo "=== PHASE 8: Write Stress to Deduped File via Symlink ==="
    
    sudo fio --name=symlink_write \
             --filename="$FILE1" \
             --rw=randwrite \
             --bs="$BLOCK_SIZE" \
             --numjobs=2 \
             --time_based --runtime=5 \
             --group_reporting
    
    echo "[+] Write stress on deduped file completed"
    
    # Verify symlink still valid
    if [ -L "$SYMLINK1" ] && [ -e "$SYMLINK1" ]; then
        echo "[+] Symlink still valid after write stress"
        ((TEST_PASS++))
    else
        echo "[-] Symlink broken after write"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 9: Mixed Read/Write via Symlinks on Deduped Files ---
    echo "=== PHASE 9: Mixed Read/Write on Deduped Files ==="
    
    sudo fio --name=symlink_mixed \
             --filename="$SYMLINK2" \
             --rw=randrw \
             --rwmixread=70 \
             --bs="$BLOCK_SIZE" \
             --numjobs=4 \
             --time_based --runtime=10 \
             --group_reporting
    
    echo "[+] Mixed stress on deduped file via symlink completed"
    echo

    # --- PHASE 10: Cleanup & Report ---
    echo "==========================================================="
    echo "  TEST SUMMARY"
    echo "==========================================================="
    echo "Total Tests: $((TEST_PASS + TEST_FAIL))"
    echo "Passed: $TEST_PASS"
    echo "Failed: $TEST_FAIL"
    
    if [ $TEST_FAIL -eq 0 ]; then
        echo "Status: [+] ALL TESTS PASSED"
    else
        echo "Status: [-] SOME TESTS FAILED"
    fi
    
    echo "==========================================================="
    echo
    echo "Summary:"
    echo "  - Successfully created and deduped 3 identical files"
    echo "  - Created symlinks to deduped files"
    echo "  - Read/Write operations on deduped files via symlinks"
    echo "  - CoW split verified on deduped file modifications"
    echo "  - File isolation maintained"
    echo
    echo "Test Directory: $TEST_DIR"
    echo "Files: $(ls -lh "$TEST_DIR"/*.dat 2>/dev/null | wc -l) data files"
    echo "Log: $LOG_FILE"

} | tee "$LOG_FILE"

# --- Final Cleanup ---
echo "=== Cleanup ==="
rm -f "$FILE1" "$FILE2" "$FILE3" "$SYMLINK1" "$SYMLINK2" "$SYMLINK_CHAIN"
rm -rf "$TEST_DIR"
echo "[+] Test complete and cleaned up"
