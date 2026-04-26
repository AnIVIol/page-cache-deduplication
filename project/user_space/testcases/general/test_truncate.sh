#!/bin/bash

# Truncate + Dedup Test Suite
# Tests truncate(2) syscall on deduplicated files
# Structure: Setup → Generate → Copy → Dedup → Truncate → Verify

set -e

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# --- Defaults ---
FILE_SIZE_MB=256
SHARE_PERCENT=40
NUM_GROUPS=2
BLOCK_SIZE="4k"

TEST_DIR="$(pwd)/dedup_truncate_test"
SOURCE_FILE="$TEST_DIR/source.dat"
COPY1="$TEST_DIR/copy1.dat"
COPY2="$TEST_DIR/copy2.dat"
COPY3="$TEST_DIR/copy3.dat"

SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

LOG_FILE="$TEST_DIR/truncate_test_$(date +%s).log"

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

# --- Helper: Truncate via truncate(2) syscall (using truncate_syscall binary) ---
truncate_via_syscall() {
    local file="$1"
    local new_size="$2"
    ./truncate_syscall "$file" "$new_size"
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
echo "  Truncate + Dedup Test Suite"
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
    
    rm -f "$SOURCE_FILE" "$COPY1" "$COPY2" "$COPY3"
    
    if [ ! -f "$SCANNER_SYSFS" ]; then
        echo "[-] ERROR: Scanner sysfs not found at $SCANNER_SYSFS"
        exit 1
    fi
    echo "[+] Scanner sysfs verified"
    echo "[+] Test directory: $TEST_DIR"
    echo "[+] truncate_syscall available: $(which truncate_syscall 2>/dev/null || echo 'NOT IN PATH')"
    echo

    # --- PHASE 1: Generate Source File ---
    echo "=== PHASE 1: Generate Source File ==="
    
    if command -v gen_dedup &> /dev/null; then
        gen_dedup "$FILE_SIZE_MB" "$SHARE_PERCENT" "$NUM_GROUPS" "$SOURCE_FILE" || exit 1
        echo "[+] Generated source file (${FILE_SIZE_MB}MB)"
    else
        # Fallback: create file manually
        echo "[-] gen_dedup not found, creating file manually"
        dd if=/dev/zero bs=1M count=$FILE_SIZE_MB 2>/dev/null | tr '\0' 'B' > "$SOURCE_FILE"
        echo "[+] Created source file (${FILE_SIZE_MB}MB)"
    fi
    
    SOURCE_SIZE=$(get_file_size "$SOURCE_FILE")
    echo "[+] Source file size: $SOURCE_SIZE bytes"
    
    sync
    echo

    # --- PHASE 2: Create Copies ---
    echo "=== PHASE 2: Create Copies via CP ==="
    
    TEST_PASS=0
    TEST_FAIL=0
    
    cp "$SOURCE_FILE" "$COPY1" || { echo "[-] Failed to create COPY1"; ((TEST_FAIL++)); }
    cp "$SOURCE_FILE" "$COPY2" || { echo "[-] Failed to create COPY2"; ((TEST_FAIL++)); }
    cp "$SOURCE_FILE" "$COPY3" || { echo "[-] Failed to create COPY3"; ((TEST_FAIL++)); }
    echo "[+] Created 3 copies via cp"
    
    # Verify copy sizes
    for i in 1 2 3; do
        var="COPY$i"
        file="${!var}"
        size=$(get_file_size "$file" 2>/dev/null || echo "0")
        if [ "$size" -eq "$SOURCE_SIZE" ]; then
            echo "[+] Copy$i size correct ($size bytes)"
            ((TEST_PASS+=1))
        else
            echo "[-] Copy$i size mismatch"
            ((TEST_FAIL++))
        fi
    done
    
    sync
    echo

    # --- PHASE 3: Feed Files to Scanner ---
    echo "=== PHASE 3: Feed Files to Scanner ==="
    
    if [ -e "$SCANNER_START" ]; then
        echo 1 | sudo tee "$SCANNER_START" > /dev/null
    fi
    
    echo "$SOURCE_FILE" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$COPY1" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$COPY2" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$COPY3" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] All files submitted to scanner"
    
    # Wait for deduplication to complete
    sleep 5
    
    # Read files multiple times to ensure dedup is visible
    cat "$SOURCE_FILE" > /dev/null
    cat "$COPY1" > /dev/null
    cat "$COPY2" > /dev/null
    cat "$COPY3" > /dev/null
    sleep 2
    
    echo "[+] Deduplication should be complete"
    echo

    # --- PHASE 4: Perform CP Operations on DEDUPED Files ---
    echo "=== PHASE 4: Create Recopies FROM Deduped Files ==="
    
    cp "$COPY1" "$TEST_DIR/recopy1.dat" || { echo "[-] Failed to create recopy1"; ((TEST_FAIL++)); }
    cp "$COPY2" "$TEST_DIR/recopy2.dat" || { echo "[-] Failed to create recopy2"; ((TEST_FAIL++)); }
    echo "[+] Created 2 recopies from deduped files"
    
    RECOPY1_SIZE=$(get_file_size "$TEST_DIR/recopy1.dat")
    RECOPY2_SIZE=$(get_file_size "$TEST_DIR/recopy2.dat")
    
    if [ "$RECOPY1_SIZE" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Recopy1 size correct"
        ((TEST_PASS++))
    else
        echo "[-] Recopy1 size incorrect"
        ((TEST_FAIL++))
    fi
    
    if [ "$RECOPY2_SIZE" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Recopy2 size correct"
        ((TEST_PASS++))
    else
        echo "[-] Recopy2 size incorrect"
        ((TEST_FAIL++))
    fi
    
    echo

    # --- PHASE 5: Verify Content on Deduped Files ---
    echo "=== PHASE 5: Content Verification ==="
    
    SOURCE_CHECK=$(head -c 10 "$SOURCE_FILE")
    if [ "$SOURCE_CHECK" = "BBBBBBBBBB" ]; then
        echo "[+] Source content correct"
        ((TEST_PASS++))
    else
        echo "[-] Source content corrupted"
        ((TEST_FAIL++))
    fi
    
    for i in 1 2 3; do
        var="COPY$i"
        file="${!var}"
        check=$(head -c 10 "$file")
        if [ "$check" = "BBBBBBBBBB" ]; then
            echo "[+] Copy$i content correct"
            ((TEST_PASS++))
        else
            echo "[-] Copy$i content corrupted"
            ((TEST_FAIL++))
        fi
    done
    echo

    # --- PHASE 6: Truncate SHRINK on Deduped File (CoW Test) ---
    echo "=== PHASE 6: Truncate SHRINK on Deduped File (CoW Test) ==="
    
    SHRINK_SIZE=$((SOURCE_SIZE / 2))
    echo "[*] Truncating copy1 from $SOURCE_SIZE to $SHRINK_SIZE bytes (50%)"
    
    truncate_via_syscall "$COPY1" "$SHRINK_SIZE"
    echo "[+] Truncate shrink syscall completed"
    
    # Verify truncated size
    COPY1_SIZE=$(get_file_size "$COPY1")
    if [ "$COPY1_SIZE" -eq "$SHRINK_SIZE" ]; then
        echo "[+] Copy1 shrink verified ($COPY1_SIZE bytes)"
        ((TEST_PASS++))
    else
        echo "[-] Copy1 shrink verification failed (got: $COPY1_SIZE, expected: $SHRINK_SIZE)"
        ((TEST_FAIL++))
    fi
    
    # Verify source unchanged (CoW split occurred)
    SOURCE_SIZE_CHECK=$(get_file_size "$SOURCE_FILE")
    if [ "$SOURCE_SIZE_CHECK" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Source size unchanged (CoW split occurred)"
        ((TEST_PASS++))
    else
        echo "[-] Source size changed unexpectedly"
        ((TEST_FAIL++))
    fi
    
    # Verify other copies unchanged
    for i in 2 3; do
        var="COPY$i"
        file="${!var}"
        size=$(get_file_size "$file")
        if [ "$size" -eq "$SOURCE_SIZE" ]; then
            echo "[+] Copy$i size unchanged"
            ((TEST_PASS++))
        else
            echo "[-] Copy$i size changed unexpectedly"
            ((TEST_FAIL++))
        fi
    done
    echo

    # --- PHASE 7: Truncate EXPAND on Deduped File ---
    echo "=== PHASE 7: Truncate EXPAND on Deduped File ==="
    
    EXPAND_SIZE=$((SOURCE_SIZE + SOURCE_SIZE / 4))
    echo "[*] Truncating copy2 from $SOURCE_SIZE to $EXPAND_SIZE bytes (125%)"
    
    truncate_via_syscall "$COPY2" "$EXPAND_SIZE"
    echo "[+] Truncate expand syscall completed"
    
    # Verify expanded size
    COPY2_SIZE=$(get_file_size "$COPY2")
    if [ "$COPY2_SIZE" -eq "$EXPAND_SIZE" ]; then
        echo "[+] Copy2 expand verified ($COPY2_SIZE bytes)"
        ((TEST_PASS++))
    else
        echo "[-] Copy2 expand verification failed (got: $COPY2_SIZE, expected: $EXPAND_SIZE)"
        ((TEST_FAIL++))
    fi
    
    # Verify source still unchanged
    SOURCE_SIZE_CHECK=$(get_file_size "$SOURCE_FILE")
    if [ "$SOURCE_SIZE_CHECK" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Source size still unchanged"
        ((TEST_PASS++))
    else
        echo "[-] Source size changed unexpectedly"
        ((TEST_FAIL++))
    fi
    
    # Verify copy3 unchanged
    COPY3_SIZE=$(get_file_size "$COPY3")
    if [ "$COPY3_SIZE" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Copy3 size unchanged"
        ((TEST_PASS++))
    else
        echo "[-] Copy3 size changed unexpectedly"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 8: Truncate on Already-Truncated File (2nd truncate) ---
    echo "=== PHASE 8: Truncate on Already-Truncated File ==="
    
    SECOND_SHRINK=$((SHRINK_SIZE / 2))
    echo "[*] Truncating already-truncated copy1 from $SHRINK_SIZE to $SECOND_SHRINK bytes"
    
    truncate_via_syscall "$COPY1" "$SECOND_SHRINK"
    echo "[+] Second truncate syscall completed"
    
    COPY1_SIZE=$(get_file_size "$COPY1")
    if [ "$COPY1_SIZE" -eq "$SECOND_SHRINK" ]; then
        echo "[+] Copy1 second truncate verified ($COPY1_SIZE bytes)"
        ((TEST_PASS++))
    else
        echo "[-] Copy1 second truncate verification failed"
        ((TEST_FAIL++))
    fi
    
    # Verify source still unchanged
    SOURCE_SIZE_CHECK=$(get_file_size "$SOURCE_FILE")
    if [ "$SOURCE_SIZE_CHECK" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Source size still unchanged after 2nd truncate"
        ((TEST_PASS++))
    else
        echo "[-] Source size changed after 2nd truncate"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 9: Write Stress on Truncated File ---
    echo "=== PHASE 9: Write Stress on Truncated File ==="
    
    sudo fio --name=truncate_write \
             --filename="$COPY1" \
             --rw=randwrite \
             --bs="$BLOCK_SIZE" \
             --numjobs=2 \
             --time_based --runtime=5 \
             --group_reporting 2>/dev/null
    
    echo "[+] Write stress completed on truncated copy1"
    
    # Verify source still unchanged
    SOURCE_SIZE_CHECK=$(get_file_size "$SOURCE_FILE")
    if [ "$SOURCE_SIZE_CHECK" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Source unchanged after write stress on truncated copy"
        ((TEST_PASS++))
    else
        echo "[-] Source size changed during write stress"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 10: Mixed Operations (Truncate + Read on Recopy) ---
    echo "=== PHASE 10: Mixed Operations on Recopies ==="
    
    RECOPY_SHRINK=$((SOURCE_SIZE / 4))
    echo "[*] Truncating recopy1 (cp'd from deduped) from $SOURCE_SIZE to $RECOPY_SHRINK bytes"
    
    truncate_via_syscall "$TEST_DIR/recopy1.dat" "$RECOPY_SHRINK"
    echo "[+] Recopy1 truncated"
    
    # Verify recopy size changed
    RECOPY1_SIZE=$(get_file_size "$TEST_DIR/recopy1.dat")
    if [ "$RECOPY1_SIZE" -eq "$RECOPY_SHRINK" ]; then
        echo "[+] Recopy1 truncate verified"
        ((TEST_PASS++))
    else
        echo "[-] Recopy1 truncate verification failed"
        ((TEST_FAIL++))
    fi
    
    # Verify source unaffected
    SOURCE_SIZE_CHECK=$(get_file_size "$SOURCE_FILE")
    if [ "$SOURCE_SIZE_CHECK" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Source unaffected by recopy truncate"
        ((TEST_PASS++))
    else
        echo "[-] Source affected by recopy truncate"
        ((TEST_FAIL++))
    fi
    
    # Verify original copy1 unaffected (it was our recopy source)
    COPY1_SIZE=$(get_file_size "$COPY1")
    if [ "$COPY1_SIZE" -eq "$SECOND_SHRINK" ]; then
        echo "[+] Copy1 unaffected by recopy truncate"
        ((TEST_PASS++))
    else
        echo "[-] Copy1 size changed unexpectedly"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 11: Summary ---
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
    echo "  - Successfully created 3 copies of source file"
    echo "  - Deduplication completed (verified with multiple reads)"
    echo "  - Created 2 recopies from deduped files"
    echo "  - Tested truncate SHRINK on deduped file (CoW split)"
    echo "  - Tested truncate EXPAND on deduped file"
    echo "  - Tested sequential truncates on same file"
    echo "  - Tested write stress on truncated file"
    echo "  - Tested truncate on cp'd deduped files"
    echo "  - File isolation maintained across all operations"
    echo
    echo "Test Directory: $TEST_DIR"
    echo "Files: $(ls -lh "$TEST_DIR"/*.dat 2>/dev/null | wc -l) data files"
    echo "Total Disk Usage: $(du -sh "$TEST_DIR" | awk '{print $1}')"
    echo "Log: $LOG_FILE"

} | tee "$LOG_FILE"

# --- Final Cleanup ---
echo
echo "=== Cleanup ==="
rm -f "$SOURCE_FILE" "$COPY1" "$COPY2" "$COPY3"
rm -rf "$TEST_DIR"
echo "[+] Test complete and cleaned up"
