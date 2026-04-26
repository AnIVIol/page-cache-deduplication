#!/bin/bash

../setup_env.sh || exit 1

# --- Configuration ---
TEST_DIR="$PWD/dedup_test_dir"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

# Helper: Check for kernel panics or bugs
check_kernel_health() {
    if dmesg | tail -n 50 | grep -qEi "bug:|oops:|panic:|general protection fault"; then
        echo "[-] FATAL: Kernel panic or BUG detected in dmesg!"
        exit 1
    fi
}

# --- 1. SETUP ---
echo "[*] Setting up baseline..."
dd if=/dev/urandom of=f1 bs=4K count=1000 status=none
cp f1 f2 

if [ -e "$SCANNER_SYSFS" ]; then
    echo 1 | sudo tee "$SCANNER_START" > /dev/null
    echo "$TEST_DIR/f1" > "$SCANNER_SYSFS"
    echo "$TEST_DIR/f2" > "$SCANNER_SYSFS"
    sleep 2 
else
    echo "[-] WARNING: Scanner sysfs not found. Assuming manual merge."
fi

# --- 2. SYMLINK TESTS ---
echo "[+] Test 2A: Symlink Read Verification"
ln -s f1 sym_f1

if ! cmp -s f1 sym_f1; then
    echo "[-] FAIL: Symlink read returned different data than target!"
    exit 1
fi
echo "    [PASS] Symlink read matched target data perfectly."


echo "[+] Test 2B: Symlink CoW Write Verification"
# Write 4K of zeros to the symlink
dd if=/dev/zero of=sym_f1 bs=4K count=1 conv=notrunc status=none 2>/dev/null

# Verify f1 was actually modified by the write to sym_f1
zeros=$(dd if=f1 bs=4K count=1 status=none | od -v -An -t x1 | tr -d ' \n')
if [[ "$zeros" != $(printf '00%.0s' {1..4096}) ]]; then
    echo "[-] FAIL: Write to symlink did not modify the target file!"
    exit 1
fi
echo "    [PASS] Write successfully routed through symlink to target file."
check_kernel_health


# --- 3. HARDLINK TESTS ---
echo "[+] Test 3: Hardlink Bypass Verification"
# Get the original inode number of f2
INODE_F2=$(stat -c %i f2)

ln f2 hard_f2
INODE_HARD=$(stat -c %i hard_f2)

if [ "$INODE_F2" != "$INODE_HARD" ]; then
    echo "[-] FAIL: Hardlink created a different inode!"
    exit 1
fi

# Write to hard_f2 and ensure f2 reflects it instantly
dd if=/dev/urandom of=hard_f2 bs=4K count=1 conv=notrunc status=none 2>/dev/null
if ! cmp -s f2 hard_f2; then
    echo "[-] FAIL: Hardlink data diverged! Your CoW hook triggered when it shouldn't have."
    exit 1
fi
echo "    [PASS] Hardlink modified safely without triggering invalid CoW splits."
check_kernel_health


# --- 4. COPY WATERFALL TESTS ---
echo "[+] Test 4A: Reflink Rejection Verification"
# We expect this to fail, so temporarily disable bash 'set -e' behavior if you use it
cp --reflink=always f2 clone_f2 2>/dev/null
if [ $? -eq 0 ]; then
    echo "[-] FAIL: Ext4 unexpectedly allowed a block-level reflink!"
    exit 1
fi
echo "    [PASS] Kernel correctly rejected unsupported FICLONE ioctl."


echo "[+] Test 4B: Standard Copy (copy_file_range) Verification"
cp f2 copy_f2
if ! cmp -s f2 copy_f2; then
    echo "[-] FAIL: Copied file data does not match original!"
    exit 1
fi
echo "    [PASS] Standard copy succeeded with perfect data integrity."
check_kernel_health


# --- 5. POISON PILL TEST ---
echo "[+] Test 5: Scanner Symlink Poison Pill"
ln -s /dev/null poison_link
if [ -e "$SCANNER_SYSFS" ]; then
    echo "$TEST_DIR/poison_link" > "$SCANNER_SYSFS" 2>/dev/null
    sleep 1
    
    # Check if the kernel died trying to deduplicate a symlink
    if dmesg | tail -n 20 | grep -qEi "bad page cache|null pointer deref"; then
        echo "[-] FAIL: Scanner crashed when fed a symlink!"
        exit 1
    fi
    echo "    [PASS] Scanner gracefully ignored the symlink."
fi

# --- CLEANUP ---
cd ..
rm -rf "$TEST_DIR"
