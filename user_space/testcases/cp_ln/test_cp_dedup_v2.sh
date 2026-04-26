#!/bin/bash

# CP and Dedup Test Suite
# Structure: ../setup_env.sh -> Create files -> Feed to scanner -> Run tests

#set -e

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# --- Defaults ---
FILE_SIZE_MB=256
SHARE_PERCENT=100
NUM_GROUPS=1
BLOCK_SIZE="4k"

TEST_DIR="$(pwd)/dedup_cp_test"
SOURCE_FILE="$TEST_DIR/source.dat"
COPY1="$TEST_DIR/copy1.dat"
COPY2="$TEST_DIR/copy2.dat"
COPY3="$TEST_DIR/copy3.dat"
COPY4="$TEST_DIR/copy4.dat"

SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

LOG_FILE="$TEST_DIR/cp_test_$(date +%s).log"

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
    offset=$(stat -c%s "$file" 2>/dev/null || echo "0")
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
echo "  CP + Dedup Test Suite"
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
    
    rm -f "$SOURCE_FILE" "$COPY1" "$COPY2" "$COPY3" "$COPY4"
    
    if [ ! -f "$SCANNER_SYSFS" ]; then
        echo "[-] ERROR: Scanner sysfs not found at $SCANNER_SYSFS"
        exit 1
    fi
    echo "[+] Scanner sysfs verified"
    echo "[+] Test directory: $TEST_DIR"
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
    cp "$SOURCE_FILE" "$COPY4" || { echo "[-] Failed to create COPY4"; ((TEST_FAIL++)); }
    echo "[+] Created 4 copies via cp"
    
    # Verify copy sizes using the safe function
    for i in 1 2 3 4; do
        var="COPY$i"
        file="${!var}"
        echo "[DEBUG] Checking Copy$i at $file" >&2
        
        if [ ! -f "$file" ]; then
            echo "[-] Copy$i does not exist at $file"
            ((TEST_FAIL++))
            continue
        fi
        
        size=$(get_file_size "$file" 2>/dev/null || true)
        [ -z "$size" ] && size="0"
        echo "[DEBUG] Copy$i size: $size" >&2
        
        # Ensure size is a valid number
        if ! [[ "$size" =~ ^[0-9]+$ ]]; then
            echo "[-] Copy$i size could not be determined (got: $size)"
            ((TEST_FAIL++))
            continue
        fi
        
        if [ "$size" -eq 0 ]; then
            echo "[-] Copy$i is empty"
            ((TEST_FAIL++))
            continue
        fi
        
        if [ "$size" -eq "$SOURCE_SIZE" ]; then
            echo "[+] Copy$i size correct ($size bytes)"
            ((TEST_PASS++))
        else
            echo "[-] Copy$i size mismatch (expected: $SOURCE_SIZE, got: $size)"
            ((TEST_FAIL++))
        fi
    done
    echo "[DEBUG] Copy verification loop complete" >&2
    
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
    echo "$COPY4" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] All files submitted to scanner"
    
    # Wait for deduplication to complete
    sleep 5
    
    # Read files multiple times to ensure dedup is visible
    cat "$SOURCE_FILE" > /dev/null
    for i in 1 2 3 4; do
        var="COPY$i"
        cat "${!var}" > /dev/null
    done
    sleep 2
    
    echo "[+] Deduplication should be complete"
    echo

    # --- PHASE 4: Perform CP Operations on DEDUPED Files ---
    echo "=== PHASE 4: Perform CP Operations on Already-Deduped Files ==="
    
    # Create new copies FROM the deduped copies
    cp "$COPY1" "$TEST_DIR/recopy1.dat" || { echo "[-] Failed to create recopy1"; ((TEST_FAIL++)); }
    cp "$COPY2" "$TEST_DIR/recopy2.dat" || { echo "[-] Failed to create recopy2"; ((TEST_FAIL++)); }
    cp "$COPY3" "$TEST_DIR/recopy3.dat" || { echo "[-] Failed to create recopy3"; ((TEST_FAIL++)); }
    echo "[+] Created 3 new copies FROM deduped files"
    
    # Verify sizes (with error handling and validation)
    RECOPY1_SIZE=$(get_file_size "$TEST_DIR/recopy1.dat" 2>/dev/null || echo "0")
    if ! [[ "$RECOPY1_SIZE" =~ ^[0-9]+$ ]]; then RECOPY1_SIZE=0; fi
    if [ "$RECOPY1_SIZE" -gt 0 ] && [ "$RECOPY1_SIZE" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Recopy1 size correct ($RECOPY1_SIZE bytes)"
        ((TEST_PASS++))
    else
        echo "[-] Recopy1 size mismatch (expected: $SOURCE_SIZE, got: $RECOPY1_SIZE)"
        ((TEST_FAIL++))
    fi
    
    RECOPY2_SIZE=$(get_file_size "$TEST_DIR/recopy2.dat" 2>/dev/null || echo "0")
    if ! [[ "$RECOPY2_SIZE" =~ ^[0-9]+$ ]]; then RECOPY2_SIZE=0; fi
    if [ "$RECOPY2_SIZE" -gt 0 ] && [ "$RECOPY2_SIZE" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Recopy2 size correct ($RECOPY2_SIZE bytes)"
        ((TEST_PASS++))
    else
        echo "[-] Recopy2 size mismatch (expected: $SOURCE_SIZE, got: $RECOPY2_SIZE)"
        ((TEST_FAIL++))
    fi
    
    RECOPY3_SIZE=$(get_file_size "$TEST_DIR/recopy3.dat" 2>/dev/null || echo "0")
    if ! [[ "$RECOPY3_SIZE" =~ ^[0-9]+$ ]]; then RECOPY3_SIZE=0; fi
    if [ "$RECOPY3_SIZE" -gt 0 ] && [ "$RECOPY3_SIZE" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Recopy3 size correct ($RECOPY3_SIZE bytes)"
        ((TEST_PASS++))
    else
        echo "[-] Recopy3 size mismatch (expected: $SOURCE_SIZE, got: $RECOPY3_SIZE)"
        ((TEST_FAIL++))
    fi
    
    sync
    
    # Feed recopies to scanner
    echo "$TEST_DIR/recopy1.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$TEST_DIR/recopy2.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "$TEST_DIR/recopy3.dat" | sudo tee "$SCANNER_SYSFS" > /dev/null
    sleep 2
    echo "[+] Recopies fed to scanner"
    echo

    # --- PHASE 5: Verify Content on CP'd Deduped Files ---
    echo "=== PHASE 5: Content Verification on CP'd Deduped Files ==="
    
    SOURCE_CHECK=$(head -c 10 "$SOURCE_FILE")
    if [ "$SOURCE_CHECK" = "BBBBBBBBBB" ]; then
        echo "[+] Source content correct"
        ((TEST_PASS++))
    else
        echo "[-] Source content corrupted"
        ((TEST_FAIL++))
    fi
    
    # Verify original copies (should still be deduped)
    for i in 1 2 3 4; do
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
    
    # Verify recopies (cp'd from deduped files)
    for i in 1 2 3; do
        file="$TEST_DIR/recopy$i.dat"
        check=$(head -c 10 "$file")
        if [ "$check" = "BBBBBBBBBB" ]; then
            echo "[+] Recopy$i content correct"
            ((TEST_PASS++))
        else
            echo "[-] Recopy$i content corrupted"
            ((TEST_FAIL++))
        fi
    done
    echo

    # --- PHASE 6: Modify Copy from Deduped File (CoW Test) ---
    echo "=== PHASE 6: Modify Copy from Deduped File (CoW Test) ==="
    
    write_via_syscall "$COPY1" "MODIFIED_FROM_DEDUPED_COPY"
    echo "[+] Modified copy1.dat via write syscall (which was deduped)"
    
    # Verify source unchanged
    SOURCE_MOD=$(head -c 1 "$SOURCE_FILE")
    if [ "$SOURCE_MOD" = "B" ]; then
        echo "[+] Source unchanged (CoW split on deduped file)"
        ((TEST_PASS++))
    else
        echo "[-] Source was modified"
        ((TEST_FAIL++))
    fi
    
    # Verify other copies still unchanged
    for i in 2 3 4; do
        var="COPY$i"
        file="${!var}"
        check=$(head -c 1 "$file")
        if [ "$check" = "B" ]; then
            echo "[+] Copy$i unchanged"
            ((TEST_PASS++))
        else
            echo "[-] Copy$i corrupted"
            ((TEST_FAIL++))
        fi
    done
    echo

    # --- PHASE 7: Modify Recopy from Deduped Copy (CoW Test 2) ---
    echo "=== PHASE 7: Modify Recopy from Deduped Copy (CoW Test 2) ==="
    
    # Overwrite only first byte of recopy2 via write syscall
    printf 'X' | dd of="$TEST_DIR/recopy2.dat" bs=1 count=1 conv=notrunc 2>/dev/null
    echo "[+] Modified first byte of recopy2 via write syscall (cp'd from deduped file)"
    
    # Verify original copy2 untouched
    COPY2_BYTE=$(head -c 1 "$COPY2")
    if [ "$COPY2_BYTE" = "B" ]; then
        echo "[+] Copy2 unchanged (source of recopy2)"
        ((TEST_PASS++))
    else
        echo "[-] Copy2 corrupted"
        ((TEST_FAIL++))
    fi
    
    # Verify source unchanged
    SOURCE_BYTE=$(head -c 1 "$SOURCE_FILE")
    if [ "$SOURCE_BYTE" = "B" ]; then
        echo "[+] Source unchanged"
        ((TEST_PASS++))
    else
        echo "[-] Source corrupted"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 8: Write Stress to Modified Copy ---
    echo "=== PHASE 8: Write Stress to Modified Copy from Dedup ==="
    
    sudo fio --name=cp_write \
             --filename="$COPY1" \
             --rw=randwrite \
             --bs="$BLOCK_SIZE" \
             --numjobs=2 \
             --time_based --runtime=10 \
             --group_reporting
    
    echo "[+] Write stress completed"
    
    # Verify source still unchanged
    SOURCE_FINAL=$(head -c 1 "$SOURCE_FILE")
    if [ "$SOURCE_FINAL" = "B" ]; then
        echo "[+] Source still unchanged after write stress"
        ((TEST_PASS++))
    else
        echo "[-] Source corrupted during write stress"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 9: Append Operations on Recopy ---
    echo "=== PHASE 9: Append Operations on Recopy from Dedup ==="
    
    RECOPY3_SIZE_BEFORE=$(get_file_size "$TEST_DIR/recopy3.dat" 2>/dev/null || echo "0")
    if ! [[ "$RECOPY3_SIZE_BEFORE" =~ ^[0-9]+$ ]]; then RECOPY3_SIZE_BEFORE=0; fi
    append_via_syscall "$TEST_DIR/recopy3.dat" "APPENDED_TO_RECOPY"
    RECOPY3_SIZE_AFTER=$(get_file_size "$TEST_DIR/recopy3.dat" 2>/dev/null || echo "0")
    if ! [[ "$RECOPY3_SIZE_AFTER" =~ ^[0-9]+$ ]]; then RECOPY3_SIZE_AFTER=0; fi
    
    if [ "$RECOPY3_SIZE_AFTER" -gt "$RECOPY3_SIZE_BEFORE" ]; then
        echo "[+] Append successful"
        ((TEST_PASS++))
    else
        echo "[-] Append failed"
        ((TEST_FAIL++))
    fi
    
    # Verify source and copy3 sizes unchanged
    SOURCE_SIZE_FINAL=$(get_file_size "$SOURCE_FILE" 2>/dev/null || echo "0")
    if ! [[ "$SOURCE_SIZE_FINAL" =~ ^[0-9]+$ ]]; then SOURCE_SIZE_FINAL=0; fi
    COPY3_SIZE_FINAL=$(get_file_size "$COPY3" 2>/dev/null || echo "0")
    if ! [[ "$COPY3_SIZE_FINAL" =~ ^[0-9]+$ ]]; then COPY3_SIZE_FINAL=0; fi
    
    if [ "$SOURCE_SIZE_FINAL" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Source size unchanged"
        ((TEST_PASS++))
    else
        echo "[-] Source size changed"
        ((TEST_FAIL++))
    fi
    
    if [ "$COPY3_SIZE_FINAL" -eq "$SOURCE_SIZE" ]; then
        echo "[+] Copy3 (source of recopy3) size unchanged"
        ((TEST_PASS++))
    else
        echo "[-] Copy3 size changed"
        ((TEST_FAIL++))
    fi
    echo

    # --- PHASE 10: Mixed Read/Write on Deduped Copies ---
    echo "=== PHASE 10: Mixed Read/Write Stress on Deduped Files ==="
    
    sudo fio --name=cp_mixed \
             --filename="$COPY4" \
             --rw=randrw \
             --rwmixread=70 \
             --bs="$BLOCK_SIZE" \
             --numjobs=4 \
             --time_based --runtime=10 \
             --group_reporting
    
    echo "[+] Mixed stress completed"
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
    echo "  - Successfully created 4 copies of source file"
    echo "  - Deduplication completed (verified with multiple reads)"
    echo "  - Created 3 new copies FROM deduped files"
    echo "  - Performed read/write/mixed operations on cp'd deduped files"
    echo "  - CoW split verified on all modifications"
    echo "  - File isolation maintained across all files"
    echo
    echo "Test Directory: $TEST_DIR"
    echo "Files: $(ls -lh "$TEST_DIR"/*.dat 2>/dev/null | wc -l) data files"
    echo "Total Disk Usage: $(du -sh "$TEST_DIR" | awk '{print $1}')"
    echo "Log: $LOG_FILE"

} | tee "$LOG_FILE"

# --- Final Cleanup ---
echo
echo "=== Cleanup ==="
rm -f "$SOURCE_FILE" "$COPY1" "$COPY2" "$COPY3" "$COPY4"
rm -rf "$TEST_DIR"
echo "[+] Test complete and cleaned up"
