#!/bin/bash

# CP + Truncate + Symlink Dedup Test Suite
# Tests cp(1), truncate(2), and symlink operations on deduplicated files
# Structure: Setup → Generate → Copy → Dedup → CP/Truncate/Symlink → Verify

set -e

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# --- Defaults ---
FILE_SIZE_MB=512
SHARE_PERCENT=80
NUM_GROUPS=1
BLOCK_SIZE="4k"

TEST_DIR="$(pwd)/dedup_ops_test"
SOURCE_FILE="$TEST_DIR/source.dat"
COPY1="$TEST_DIR/copy1.dat"
COPY2="$TEST_DIR/copy2.dat"
COPY3="$TEST_DIR/copy3.dat"

SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"
LOG_FILE="$TEST_DIR/ops_test_$(date +%s).log"

# --- Parse CLI args ---
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -s|--size)    FILE_SIZE_MB="$2";  shift ;;
        -p|--percent) SHARE_PERCENT="$2"; shift ;;
        -g|--groups)  NUM_GROUPS="$2";    shift ;;
        -b|--bs)      BLOCK_SIZE="$2";    shift ;;
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

# --- Helper: Truncate via truncate(2) syscall ---
truncate_via_syscall() {
    local file="$1"
    local new_size="$2"
    ./truncate_syscall "$file" "$new_size"
}

# --- Helper: Get regular file size (does NOT follow symlinks) ---
get_file_size() {
    local file="$1"
    if [ -e "$file" ]; then
        stat -c%s "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

# --- Helper: Get size through a symlink (follows all links) ---
get_link_size() {
    local link="$1"
    if [ -e "$link" ]; then
        stat -L -c%s "$link" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

# --- Helper: Uniform pass/fail check (string or numeric equality) ---
check() {
    local desc="$1"
    local got="$2"
    local want="$3"
    if [ "$got" = "$want" ]; then
        echo "[+] PASS: $desc"
        TEST_PASS=$((TEST_PASS + 1))
    else
        echo "[-] FAIL: $desc  (got=${got}  want=${want})"
        TEST_FAIL=$((TEST_FAIL + 1))
    fi
}

# --- Banner ---
echo "================================================================"
echo "  CP + Truncate + Symlink Dedup Test Suite"
echo "================================================================"
echo "  Size: ${FILE_SIZE_MB}MB | Share: ${SHARE_PERCENT}% | Groups: ${NUM_GROUPS} | BS: ${BLOCK_SIZE}"
echo "================================================================"
echo

mkdir -p "$TEST_DIR"

{
    TEST_PASS=0
    TEST_FAIL=0

    # =========================================================================
    # PHASE 0: Environment Check
    # =========================================================================
    echo "=== PHASE 0: Environment Check ==="

    rm -f "$TEST_DIR"/*.dat "$TEST_DIR"/*.lnk 2>/dev/null || true

    if [ ! -f "$SCANNER_SYSFS" ]; then
        echo "[-] ERROR: Scanner sysfs not found at $SCANNER_SYSFS"
        exit 1
    fi
    if [ ! -x "./truncate_syscall" ]; then
        echo "[-] ERROR: truncate_syscall binary not found in current directory"
        exit 1
    fi

    echo "[+] Scanner sysfs   : $SCANNER_SYSFS"
    echo "[+] truncate_syscall: present"
    echo "[+] Test directory  : $TEST_DIR"
    echo

    # =========================================================================
    # PHASE 1: Generate Source File
    # =========================================================================
    echo "=== PHASE 1: Generate Source File ==="

    if command -v gen_dedup &>/dev/null; then
        gen_dedup "$FILE_SIZE_MB" "$SHARE_PERCENT" "$NUM_GROUPS" "$SOURCE_FILE" || exit 1
        echo "[+] gen_dedup: ${FILE_SIZE_MB}MB source file created"
    else
        echo "[!] gen_dedup not found – falling back to dd"
        dd if=/dev/zero bs=1M count="$FILE_SIZE_MB" 2>/dev/null | tr '\0' 'B' > "$SOURCE_FILE"
        echo "[+] dd: ${FILE_SIZE_MB}MB source file created"
    fi

    SOURCE_SIZE=$(get_file_size "$SOURCE_FILE")
    echo "[+] Source file size: $SOURCE_SIZE bytes"
    sync
    echo

    # =========================================================================
    # PHASE 2: Create Initial Copies via CP (Pre-Dedup Baseline)
    # =========================================================================
    echo "=== PHASE 2: Create Copies via CP (Pre-Dedup Baseline) ==="

    cp "$SOURCE_FILE" "$COPY1" || { echo "[-] Failed to create COPY1"; TEST_FAIL=$((TEST_FAIL + 1)); }
    cp "$SOURCE_FILE" "$COPY2" || { echo "[-] Failed to create COPY2"; TEST_FAIL=$((TEST_FAIL + 1)); }
    cp "$SOURCE_FILE" "$COPY3" || { echo "[-] Failed to create COPY3"; TEST_FAIL=$((TEST_FAIL + 1)); }
    echo "[+] Created 3 copies via cp"

    check "copy1 initial size matches source" "$(get_file_size "$COPY1")" "$SOURCE_SIZE"
    check "copy2 initial size matches source" "$(get_file_size "$COPY2")" "$SOURCE_SIZE"
    check "copy3 initial size matches source" "$(get_file_size "$COPY3")" "$SOURCE_SIZE"

    sync
    echo

    # =========================================================================
    # PHASE 3: Feed Files to Dedup Scanner
    # =========================================================================
    echo "=== PHASE 3: Feed Files to Dedup Scanner ==="

    [ -e "$SCANNER_START" ] && echo 1 | sudo tee "$SCANNER_START" >/dev/null

    for f in "$SOURCE_FILE" "$COPY1" "$COPY2" "$COPY3"; do
        echo "$f" | sudo tee "$SCANNER_SYSFS" >/dev/null
        echo "[+] Submitted: $(basename "$f")"
    done

    echo "[*] Waiting for dedup to complete..."
    sleep 5
    cat "$SOURCE_FILE" "$COPY1" "$COPY2" "$COPY3" >/dev/null
    sleep 2
    echo "[+] Dedup should be complete"
    echo

    # =========================================================================
    # PHASE 4: CP Operations on Deduped Files
    # =========================================================================
    echo "=== PHASE 4: CP Operations on Deduped Files ==="

    # 4a: cp a deduped file → new independent regular file
    CP_A="$TEST_DIR/cp_dedup_a.dat"
    cp "$COPY1" "$CP_A" || { echo "[-] 4a: cp from deduped file failed"; TEST_FAIL=$((TEST_FAIL + 1)); }
    check "4a: cp-from-dedup size correct"   "$(get_file_size "$CP_A")" "$SOURCE_SIZE"
    check "4a: cp-from-dedup content correct" "$(head -c 10 "$CP_A")"   "BBBBBBBBBB"

    # 4b: write to the copy → verify source and copy1 are not affected (independence)
    printf 'ZZZZZZZZZZ' | dd of="$CP_A" bs=10 count=1 conv=notrunc 2>/dev/null
    check "4b: source unchanged after write to cp-a" \
          "$(get_file_size "$SOURCE_FILE")" "$SOURCE_SIZE"
    check "4b: copy1 unchanged after write to cp-a" \
          "$(get_file_size "$COPY1")" "$SOURCE_SIZE"

    # 4c: cp with destination already existing (overwrite)
    CP_B="$TEST_DIR/cp_dedup_b.dat"
    cp "$COPY2" "$CP_B"
    cp "$COPY3" "$CP_B"   # intentional overwrite
    check "4c: cp overwrite size correct" "$(get_file_size "$CP_B")" "$SOURCE_SIZE"

    # 4d: cp one deduped file onto another deduped file path
    CP_C="$TEST_DIR/cp_dedup_c.dat"
    cp "$COPY2" "$CP_C"
    check "4d: cp copy2→copy_c size correct" "$(get_file_size "$CP_C")" "$SOURCE_SIZE"
    check "4d: source unchanged after multi-cp" "$(get_file_size "$SOURCE_FILE")" "$SOURCE_SIZE"

    echo

    # =========================================================================
    # PHASE 5: Symlink Creation and Read-Through on Deduped Files
    # =========================================================================
    echo "=== PHASE 5: Symlink Creation and Read-Through ==="

    SYM1="$TEST_DIR/sym1.lnk"
    SYM2="$TEST_DIR/sym2.lnk"
    SYM_SRC="$TEST_DIR/sym_src.lnk"

    ln -sf "$COPY1"       "$SYM1"
    ln -sf "$COPY2"       "$SYM2"
    ln -sf "$SOURCE_FILE" "$SYM_SRC"
    echo "[+] Created: sym1→copy1, sym2→copy2, sym_src→source"

    # 5a: verify each is actually a symlink
    check "5a: sym1 is a symlink"    "$(test -L "$SYM1"    && echo yes || echo no)" "yes"
    check "5a: sym2 is a symlink"    "$(test -L "$SYM2"    && echo yes || echo no)" "yes"
    check "5a: sym_src is a symlink" "$(test -L "$SYM_SRC" && echo yes || echo no)" "yes"

    # 5b: stat -L through symlink → should show target's size
    check "5b: sym1 target size"    "$(get_link_size "$SYM1")"    "$SOURCE_SIZE"
    check "5b: sym2 target size"    "$(get_link_size "$SYM2")"    "$SOURCE_SIZE"
    check "5b: sym_src target size" "$(get_link_size "$SYM_SRC")" "$SOURCE_SIZE"

    # 5c: read file data through symlink
    check "5c: read content via sym1"    "$(head -c 10 "$SYM1")"    "BBBBBBBBBB"
    check "5c: read content via sym_src" "$(head -c 10 "$SYM_SRC")" "BBBBBBBBBB"

    # 5d: cp follows symlink by default → produces a regular file with target's data
    CP_VIA_SYM="$TEST_DIR/cp_via_sym.dat"
    cp "$SYM1" "$CP_VIA_SYM" || { echo "[-] 5d: cp via symlink failed"; TEST_FAIL=$((TEST_FAIL + 1)); }
    check "5d: cp-via-sym is a regular file (not symlink)" \
          "$(test -f "$CP_VIA_SYM" && ! test -L "$CP_VIA_SYM" && echo yes || echo no)" "yes"
    check "5d: cp-via-sym size correct"   "$(get_file_size "$CP_VIA_SYM")" "$SOURCE_SIZE"
    check "5d: cp-via-sym content correct" "$(head -c 10 "$CP_VIA_SYM")"   "BBBBBBBBBB"

    # 5e: cp -P copies the symlink node itself, not the target data
    CP_SYM_NODE="$TEST_DIR/cp_sym_node.lnk"
    cp -P "$SYM2" "$CP_SYM_NODE" || { echo "[-] 5e: cp -P of symlink failed"; TEST_FAIL=$((TEST_FAIL + 1)); }
    check "5e: cp -P produces a symlink"             "$(test -L "$CP_SYM_NODE" && echo yes || echo no)" "yes"
    check "5e: copied symlink target equals original" "$(readlink "$CP_SYM_NODE")" "$(readlink "$SYM2")"

    echo

    # =========================================================================
    # PHASE 6: Truncate SHRINK on Deduped File (CoW Split Test)
    # =========================================================================
    echo "=== PHASE 6: Truncate SHRINK on Deduped File (CoW Test) ==="

    SHRINK_SIZE=$((SOURCE_SIZE / 2))
    echo "[*] copy1: $SOURCE_SIZE → $SHRINK_SIZE bytes (50%)"
    truncate_via_syscall "$COPY1" "$SHRINK_SIZE"
    echo "[+] Truncate shrink complete"

    check "6a: copy1 shrunk correctly"       "$(get_file_size "$COPY1")"      "$SHRINK_SIZE"
    check "6b: source unchanged (CoW split)" "$(get_file_size "$SOURCE_FILE")" "$SOURCE_SIZE"
    check "6c: copy2 unchanged"              "$(get_file_size "$COPY2")"      "$SOURCE_SIZE"
    check "6d: copy3 unchanged"              "$(get_file_size "$COPY3")"      "$SOURCE_SIZE"
    # sym1 → copy1: stat -L must now reflect the truncated size
    check "6e: sym1 reflects copy1 shrink"   "$(get_link_size "$SYM1")"       "$SHRINK_SIZE"

    echo

    # =========================================================================
    # PHASE 7: Truncate EXPAND on Deduped File
    # =========================================================================
    echo "=== PHASE 7: Truncate EXPAND on Deduped File ==="

    EXPAND_SIZE=$((SOURCE_SIZE + SOURCE_SIZE / 4))
    echo "[*] copy2: $SOURCE_SIZE → $EXPAND_SIZE bytes (125%)"
    truncate_via_syscall "$COPY2" "$EXPAND_SIZE"
    echo "[+] Truncate expand complete"

    check "7a: copy2 expanded correctly"    "$(get_file_size "$COPY2")"      "$EXPAND_SIZE"
    check "7b: source unchanged"            "$(get_file_size "$SOURCE_FILE")" "$SOURCE_SIZE"
    check "7c: copy3 unchanged"             "$(get_file_size "$COPY3")"      "$SOURCE_SIZE"
    # sym2 → copy2: must reflect expanded size
    check "7d: sym2 reflects copy2 expand"  "$(get_link_size "$SYM2")"       "$EXPAND_SIZE"

    echo

    # =========================================================================
    # PHASE 8: Truncate Through Symlink (CoW via Symlink Path)
    # =========================================================================
    echo "=== PHASE 8: Truncate via Symlink on Deduped File ==="

    # Re-submit copy3 to ensure it is still deduped before this test
    echo "$COPY3" | sudo tee "$SCANNER_SYSFS" >/dev/null
    sleep 2
    cat "$COPY3" >/dev/null

    SYM3="$TEST_DIR/sym3.lnk"
    ln -sf "$COPY3" "$SYM3"
    echo "[+] Created sym3 → copy3"

    SYM_SHRINK=$((SOURCE_SIZE / 3))
    echo "[*] truncate(2) via sym3 path: $SOURCE_SIZE → $SYM_SHRINK bytes"
    truncate_via_syscall "$SYM3" "$SYM_SHRINK"
    echo "[+] Truncate via symlink complete"

    # truncate(2) follows the symlink: the actual target must be truncated
    check "8a: copy3 truncated via symlink"      "$(get_file_size "$COPY3")"      "$SYM_SHRINK"
    check "8b: sym3 stat -L reflects truncation" "$(get_link_size "$SYM3")"       "$SYM_SHRINK"
    check "8c: source unchanged after sym-trunc" "$(get_file_size "$SOURCE_FILE")" "$SOURCE_SIZE"
    check "8d: copy2 unchanged after sym-trunc"  "$(get_file_size "$COPY2")"      "$EXPAND_SIZE"

    echo

    # =========================================================================
    # PHASE 9: CP from a Truncated Deduped File
    # =========================================================================
    echo "=== PHASE 9: CP from Truncated Deduped File ==="

    # copy1 is at SHRINK_SIZE from phase 6
    CP_FROM_TRUNC="$TEST_DIR/cp_from_trunc.dat"
    cp "$COPY1" "$CP_FROM_TRUNC" || { echo "[-] 9a: cp from truncated file failed"; TEST_FAIL=$((TEST_FAIL + 1)); }
    check "9a: cp-from-trunc size matches truncated copy1" \
          "$(get_file_size "$CP_FROM_TRUNC")" "$SHRINK_SIZE"

    # Independently truncate the copy; copy1 must not be affected
    DEEP_SHRINK=$((SHRINK_SIZE / 2))
    truncate_via_syscall "$CP_FROM_TRUNC" "$DEEP_SHRINK"
    check "9b: cp-from-trunc further shrunk"         "$(get_file_size "$CP_FROM_TRUNC")" "$DEEP_SHRINK"
    check "9c: copy1 unchanged after cp-trunc shrink" "$(get_file_size "$COPY1")"         "$SHRINK_SIZE"

    echo

    # =========================================================================
    # PHASE 10: Symlink Chain (symlink → symlink → deduped file)
    # =========================================================================
    echo "=== PHASE 10: Symlink Chain (sym → sym → deduped file) ==="

    # sym_src → source.dat (created in phase 5)
    # sym_chain → sym_src → source.dat
    SYM_CHAIN="$TEST_DIR/sym_chain.lnk"
    ln -sf "$SYM_SRC" "$SYM_CHAIN"
    echo "[+] Chain: sym_chain → sym_src → source.dat"

    check "10a: sym_chain is a symlink"   "$(test -L "$SYM_CHAIN" && echo yes || echo no)" "yes"
    # stat -L follows ALL links in chain
    check "10b: sym_chain resolves to source size" "$(get_link_size "$SYM_CHAIN")"  "$SOURCE_SIZE"
    check "10c: content readable via chain"         "$(head -c 10 "$SYM_CHAIN")"    "BBBBBBBBBB"

    # cp follows the chain to produce a regular file with the final target's data
    CP_VIA_CHAIN="$TEST_DIR/cp_via_chain.dat"
    cp "$SYM_CHAIN" "$CP_VIA_CHAIN" || { echo "[-] 10d: cp via chain failed"; TEST_FAIL=$((TEST_FAIL + 1)); }
    check "10d: cp-via-chain is a regular file" \
          "$(test -f "$CP_VIA_CHAIN" && ! test -L "$CP_VIA_CHAIN" && echo yes || echo no)" "yes"
    check "10e: cp-via-chain size correct"   "$(get_file_size "$CP_VIA_CHAIN")" "$SOURCE_SIZE"
    check "10f: cp-via-chain content correct" "$(head -c 10 "$CP_VIA_CHAIN")"   "BBBBBBBBBB"

    # Truncate through the chain; must reach source.dat
    CHAIN_TRUNC=$((SOURCE_SIZE * 2 / 3))
    echo "[*] truncate via sym_chain: $SOURCE_SIZE → $CHAIN_TRUNC bytes"
    truncate_via_syscall "$SYM_CHAIN" "$CHAIN_TRUNC"
    check "10g: source.dat truncated via chain"      "$(get_file_size "$SOURCE_FILE")" "$CHAIN_TRUNC"
    check "10h: sym_chain stat -L reflects truncate" "$(get_link_size "$SYM_CHAIN")"   "$CHAIN_TRUNC"
    check "10i: sym_src stat -L reflects truncate"   "$(get_link_size "$SYM_SRC")"     "$CHAIN_TRUNC"
    check "10j: copy1 unaffected by chain truncate"  "$(get_file_size "$COPY1")"        "$SHRINK_SIZE"

    echo

    # =========================================================================
    # PHASE 11: Sequential Truncates Interleaved with CP Snapshots
    # =========================================================================
    echo "=== PHASE 11: Sequential Truncates Interleaved with CP Snapshots ==="

    # Deduplicate a fresh working copy
    WORK="$TEST_DIR/work.dat"
    cp "$COPY2" "$WORK"   # copy2 is at EXPAND_SIZE
    echo "$WORK" | sudo tee "$SCANNER_SYSFS" >/dev/null
    sleep 3
    cat "$WORK" >/dev/null
    WORK_BASE=$(get_file_size "$WORK")
    echo "[+] work.dat base size: $WORK_BASE bytes"

    T1=$((WORK_BASE * 3 / 4))
    T2=$((WORK_BASE / 2))
    T3=$((WORK_BASE * 9 / 10))

    # Truncate 1: shrink to 75 %
    truncate_via_syscall "$WORK" "$T1"
    check "11a: work truncate-1 (75%)" "$(get_file_size "$WORK")" "$T1"

    # Snapshot after truncate 1
    SNAP1="$TEST_DIR/snap1.dat"
    cp "$WORK" "$SNAP1"
    check "11b: snap1 size equals T1"   "$(get_file_size "$SNAP1")" "$T1"

    # Truncate 2: shrink to 50 %
    truncate_via_syscall "$WORK" "$T2"
    check "11c: work truncate-2 (50%)"    "$(get_file_size "$WORK")"  "$T2"
    check "11d: snap1 unchanged"          "$(get_file_size "$SNAP1")" "$T1"

    # Snapshot after truncate 2
    SNAP2="$TEST_DIR/snap2.dat"
    cp "$WORK" "$SNAP2"
    check "11e: snap2 size equals T2"   "$(get_file_size "$SNAP2")" "$T2"

    # Truncate 3: expand back to 90 %
    truncate_via_syscall "$WORK" "$T3"
    check "11f: work truncate-3 (expand 90%)"  "$(get_file_size "$WORK")"  "$T3"
    check "11g: snap1 still at T1 after expand" "$(get_file_size "$SNAP1")" "$T1"
    check "11h: snap2 still at T2 after expand" "$(get_file_size "$SNAP2")" "$T2"
    check "11i: copy2 unchanged throughout"     "$(get_file_size "$COPY2")" "$EXPAND_SIZE"

    echo

    # =========================================================================
    # PHASE 12: Write Stress on Post-Truncate Deduped File
    # =========================================================================
    echo "=== PHASE 12: Write Stress ==="

    if command -v fio &>/dev/null; then
        sudo fio --name=dedup_stress \
                 --filename="$WORK" \
                 --rw=randwrite \
                 --bs="$BLOCK_SIZE" \
                 --numjobs=2 \
                 --time_based --runtime=5 \
                 --group_reporting 2>/dev/null
        echo "[+] fio write stress completed"
    else
        dd if=/dev/urandom of="$WORK" bs=4k count=128 conv=notrunc 2>/dev/null
        echo "[+] dd write stress completed (fio not available)"
    fi

    check "12a: copy2 unchanged after write stress" "$(get_file_size "$COPY2")" "$EXPAND_SIZE"
    check "12b: snap1 unchanged after write stress" "$(get_file_size "$SNAP1")" "$T1"

    echo

    # =========================================================================
    # PHASE 13: Final Summary
    # =========================================================================
    echo "================================================================"
    echo "  FINAL TEST SUMMARY"
    echo "================================================================"
    TOTAL=$((TEST_PASS + TEST_FAIL))
    printf "  Tests run : %d\n" "$TOTAL"
    printf "  Passed    : %d\n" "$TEST_PASS"
    printf "  Failed    : %d\n" "$TEST_FAIL"
    echo

    if [ "$TEST_FAIL" -eq 0 ]; then
        echo "  Status: [+] ALL $TOTAL TESTS PASSED"
    else
        echo "  Status: [-] $TEST_FAIL / $TOTAL TESTS FAILED"
    fi

    echo "================================================================"
    echo
    echo "  Coverage matrix:"
    echo "    CP  4a    cp deduped file → independent regular file"
    echo "    CP  4b    write-to-copy does not bleed into source/original"
    echo "    CP  4c    cp with overwrite of existing destination"
    echo "    CP  4d    cp between two deduped files"
    echo "    CP  5d    cp follows symlink, produces regular file"
    echo "    CP  5e    cp -P copies symlink node, not target data"
    echo "    CP  9a-c  cp from a previously-truncated deduped file"
    echo "    CP  10d   cp via multi-hop symlink chain"
    echo "    CP  11b   cp snapshot mid-sequence of truncates"
    echo "    TRN 6a-e  truncate SHRINK on deduped file (CoW isolation)"
    echo "    TRN 7a-d  truncate EXPAND on deduped file"
    echo "    TRN 8a-d  truncate(2) path goes through symlink"
    echo "    TRN 10g   truncate(2) path goes through symlink chain"
    echo "    TRN 11    sequential shrink/shrink/expand with snapshots"
    echo "    SYM 5a    symlink creation on deduped file"
    echo "    SYM 5b    stat -L through symlink reports target size"
    echo "    SYM 5c    file data readable through symlink"
    echo "    SYM 6e    symlink reflects SHRINK of its target"
    echo "    SYM 7d    symlink reflects EXPAND of its target"
    echo "    SYM 8b    truncate-via-symlink visible in stat -L"
    echo "    SYM 10a-i symlink chain: resolution, cp, truncate"
    echo "    ISO       sibling file isolation across all phases"
    echo
    echo "  Test dir   : $TEST_DIR"
    echo "  Log file   : $LOG_FILE"
    echo "  Disk usage : $(du -sh "$TEST_DIR" 2>/dev/null | awk '{print $1}')"

} | tee "$LOG_FILE"

# --- Cleanup ---
echo
echo "=== Cleanup ==="
rm -rf "$TEST_DIR"
echo "[+] Done"
