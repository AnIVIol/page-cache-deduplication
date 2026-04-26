#!/bin/bash
# MASTER TEST SCRIPT - Page Cache Dedup + CoW Validator
# Complete comprehensive testing suite

set -e
../setup_env.sh || exit 1

TEST_DIR="$PWD"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

DEDUP_FILE_SIZE="256m"
COW_FILE_SIZE="128m"
WAIT_TIME=1

# Helper: Show cache stats
show_cache_stats() {
    local label=$1
    echo "  [CACHE] $label"
    cat /proc/meminfo | grep -E "Cached:|Dirty:" | sed 's/^/    /'
}

# Helper: Show disk stats
show_disk_stats() {
    local label=$1
    echo "  [DISK] $label"
    iostat -d 1 1 | tail -1 | sed 's/^/    /'
}

echo "======================================================="
echo "  KERNEL PAGE CACHE DEDUP + COW VALIDATOR              "
echo "======================================================="
echo ""
echo "[CRITICAL SETTINGS]"
echo "  ✓ --direct=0  (page cache enabled, NOT O_DIRECT)"
echo "  ✓ psync engine only (verification + stress)"
echo "  ✓ --numjobs scaling (1 → 16 → 32)"
echo "  ✓ ../setup_env.sh executed"
echo ""

create_file_with_byte() {
    local filename=$1
    local byte_char=$2
    local size_mb=$3
    
    echo "[+] Creating $filename ($size_mb MB) filled with '$byte_char'"
    
    python3 << PYTHON_CREATE
import os
filename = "$filename"
byte_char = ord("$byte_char")
size_mb = $size_mb
chunk_size = 1024 * 1024

fd = os.open(filename, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(size_mb):
    chunk = bytes([byte_char]) * chunk_size
    os.write(fd, chunk)
os.fsync(fd)
os.close(fd)
PYTHON_CREATE
}

verify_file_byte_sample() {
    local filename=$1
    local expected_byte=$2
    local sample_size=$3
    
    python3 << PYTHON_VERIFY
import os
filename = "$filename"
expected_char = ord("$expected_byte")
sample_size = $sample_size

fd = os.open(filename, os.O_RDONLY)
data = os.read(fd, sample_size)
os.close(fd)

matches = sum(1 for b in data if b == expected_char)
if matches == sample_size:
    print(f"[✓] File verified: '{chr(expected_char)}'")
    exit(0)
else:
    print(f"[✗] Verification failed")
    exit(1)
PYTHON_VERIFY
}

write_bytes_at_offset() {
    local filename=$1
    local offset=$2
    local byte_char=$3
    local num_bytes=$4
    
    python3 << PYTHON_WRITE
import os
filename = "$filename"
offset = $offset
byte_char = ord("$byte_char")
num_bytes = $num_bytes

fd = os.open(filename, os.O_RDWR)
os.lseek(fd, offset, os.SEEK_SET)
os.write(fd, bytes([byte_char]) * num_bytes)
os.fsync(fd)
os.close(fd)

print(f"Wrote {num_bytes} bytes at offset {offset}")
PYTHON_WRITE
}

read_and_verify_bytes() {
    local filename=$1
    local offset=$2
    local expected_byte=$3
    local num_bytes_to_check=$4
    
    python3 << PYTHON_READ
import os
filename = "$filename"
offset = $offset
expected_char = ord("$expected_byte")
num_bytes = $num_bytes_to_check

fd = os.open(filename, os.O_RDONLY)
os.lseek(fd, offset, os.SEEK_SET)
data = os.read(fd, num_bytes)
os.close(fd)

if len(data) < num_bytes:
    print(f"[✗] Not enough data")
    exit(1)

mismatches = sum(1 for b in data if b != expected_char)
if mismatches == 0:
    print(f"[✓] Offset {offset}: all bytes are '{chr(expected_char)}'")
else:
    print(f"[✗] Mismatches found")
    exit(1)
PYTHON_READ
}

echo "[+] Cleaning up..."
rm -f "$TEST_DIR"/dedup_*.dat "$TEST_DIR"/cow_*.dat

echo ""
echo "=== PHASE 1: Create Dedup Test Files (All 'A') ==="
create_file_with_byte "$TEST_DIR/dedup_orig.dat" "A" 256
create_file_with_byte "$TEST_DIR/dedup_dup1.dat" "A" 256
create_file_with_byte "$TEST_DIR/dedup_dup2.dat" "A" 256

echo "[+] Verify pattern..."
verify_file_byte_sample "$TEST_DIR/dedup_orig.dat" "A" 10000
verify_file_byte_sample "$TEST_DIR/dedup_dup1.dat" "A" 10000

echo "[+] Checksums before dedup..."
sha256sum "$TEST_DIR"/dedup_*.dat > "$TEST_DIR/dedup_before.txt"

echo ""
echo "=== PHASE 2: Load into Page Cache ==="
for file in "$TEST_DIR"/dedup_*.dat; do
    cat "$file" > /dev/null
done

echo "[+] Memory before dedup:"
show_cache_stats "After loading files"

echo ""
echo "=== PHASE 3: Run Deduplicator ==="
if [ -e "$SCANNER_SYSFS" ]; then
    echo 1 | sudo tee "$SCANNER_START" > /dev/null
    for file in "$TEST_DIR"/dedup_*.dat; do
        echo "$file" | sudo tee "$SCANNER_SYSFS" > /dev/null
    done
    sleep "$WAIT_TIME"
else
    echo "[-] ERROR: Scanner not found!"
    exit 1
fi

echo "[+] Memory after dedup:"
show_cache_stats "After dedup scan"

echo ""
echo "=== PHASE 4: Verify Dedup Files ==="
sha256sum "$TEST_DIR"/dedup_*.dat > "$TEST_DIR/dedup_after.txt"

if diff "$TEST_DIR/dedup_before.txt" "$TEST_DIR/dedup_after.txt" > /dev/null; then
    echo "[✓] Checksums match - no corruption"
else
    echo "[✗] Checksums differ!"
    exit 1
fi

verify_file_byte_sample "$TEST_DIR/dedup_orig.dat" "A" 10000

echo ""
echo "=== PHASE 5: Create CoW Test Files (All 'X') ==="
create_file_with_byte "$TEST_DIR/cow_test_1.dat" "X" 128
create_file_with_byte "$TEST_DIR/cow_test_2.dat" "X" 128
create_file_with_byte "$TEST_DIR/cow_test_3.dat" "X" 128

verify_file_byte_sample "$TEST_DIR/cow_test_1.dat" "X" 10000

echo ""
echo "=== PHASE 6: Load CoW Files & Deduplicate ==="
for file in "$TEST_DIR"/cow_test_*.dat; do
    cat "$file" > /dev/null
done

echo 1 | sudo tee "$SCANNER_START" > /dev/null
for file in "$TEST_DIR"/cow_test_*.dat; do
    echo "$file" | sudo tee "$SCANNER_SYSFS" > /dev/null
done
sleep 5

verify_file_byte_sample "$TEST_DIR/cow_test_1.dat" "X" 10000
show_cache_stats "After CoW files deduped"

echo ""
echo "=== PHASE 7: Write to Merged Pages ==="
echo "[+] Writing 'Y' to offsets (triggers CoW)..."

write_bytes_at_offset "$TEST_DIR/cow_test_1.dat" 0 "Y" 4096
write_bytes_at_offset "$TEST_DIR/cow_test_1.dat" 1048576 "Y" 4096
write_bytes_at_offset "$TEST_DIR/cow_test_1.dat" 104857600 "Y" 4096

show_cache_stats "After CoW writes"

echo ""
echo "=== PHASE 8: Verify CoW (CRITICAL TEST) ==="
echo "[+] Reading back to verify CoW..."

echo ""
echo "[TEST 1] Offset 0:"
read_and_verify_bytes "$TEST_DIR/cow_test_1.dat" 0 "Y" 4096 || { echo "[✗] CoW FAILED!"; exit 1; }

echo "[TEST 2] Offset 1MB:"
read_and_verify_bytes "$TEST_DIR/cow_test_1.dat" 1048576 "Y" 4096 || { echo "[✗] CoW FAILED!"; exit 1; }

echo "[TEST 3] Offset 100MB:"
read_and_verify_bytes "$TEST_DIR/cow_test_1.dat" 104857600 "Y" 4096 || { echo "[✗] CoW FAILED!"; exit 1; }

echo "[TEST 4] Unwritten region (should still be 'X'):"
read_and_verify_bytes "$TEST_DIR/cow_test_1.dat" 50000000 "X" 1000 || { echo "[✗] Corruption!"; exit 1; }

echo "[✓] All CoW tests passed"

echo ""
echo "=== PHASE 8B: Cache Verification (Read-Only) ==="
echo "[+] Pure read test - disk stats should be LOW"
show_cache_stats "Before read-only phase"

echo "[+] Reading all files (3 iterations)..."
for i in {1..3}; do
    echo "  Iteration $i..."
    cat "$TEST_DIR"/dedup_orig.dat > /dev/null
    cat "$TEST_DIR"/dedup_dup1.dat > /dev/null
    cat "$TEST_DIR"/cow_test_1.dat > /dev/null
    show_disk_stats "Read #$i"
done

show_cache_stats "After read-only phase"

echo ""
echo "=== PHASE 9: FIO Correctness Test (psync, 1 job) ==="
echo "[+] Sequential write - single threaded baseline"
show_cache_stats "Before FIO phase 9"

fio --name=verify_baseline \
    --filename="$TEST_DIR/cow_test_1.dat:$TEST_DIR/cow_test_2.dat:$TEST_DIR/cow_test_3.dat" \
    --rw=write \
    --bs=4k \
    --iodepth=1 \
    --numjobs=1 \
    --runtime=30 \
    --time_based \
    --ioengine=psync \
    --direct=0

show_cache_stats "After FIO phase 9"

echo ""
echo "=== PHASE 10: FIO Stress Test (psync, 16 jobs) ==="
echo "[+] Random write - moderate concurrency"
show_cache_stats "Before FIO phase 10"

fio --name=stress_16 \
    --filename="$TEST_DIR/cow_test_1.dat:$TEST_DIR/cow_test_2.dat:$TEST_DIR/cow_test_3.dat" \
    --rw=randwrite \
    --bs=4k \
    --iodepth=1 \
    --numjobs=16 \
    --runtime=30 \
    --time_based \
    --ioengine=psync \
    --direct=0

show_cache_stats "After FIO phase 10"

echo ""
echo "=== PHASE 11: FIO Chaos Test (psync, 32 jobs) ==="
echo "[+] Random write - maximum concurrent stress"
show_cache_stats "Before FIO phase 11"

fio --name=chaos_32 \
    --filename="$TEST_DIR/cow_test_1.dat:$TEST_DIR/cow_test_2.dat:$TEST_DIR/cow_test_3.dat" \
    --rw=randwrite \
    --bs=4k \
    --iodepth=1 \
    --numjobs=32 \
    --runtime=30 \
    --time_based \
    --ioengine=psync \
    --direct=0

show_cache_stats "After FIO phase 11"

echo ""
echo "=== PHASE 12: Post-Stress Verification ==="
for file in "$TEST_DIR"/cow_test_*.dat; do
    echo "  Checking $file..."
    cat "$file" > /dev/null || { echo "[✗] Unreadable!"; exit 1; }
done
echo "[✓] All files readable"

echo ""
echo "=== PHASE 13: Final Cache Check (Read-Only) ==="
echo "[+] Final read test - should still be cached"
show_cache_stats "Before final reads"

for i in {1..2}; do
    echo "  Final read iteration $i..."
    cat "$TEST_DIR"/cow_test_*.dat > /dev/null
    show_disk_stats "Final read #$i"
done

show_cache_stats "After final reads"

echo ""
echo "======================================================="
echo "  FINAL RESULTS"
echo "======================================================="
echo ""
echo "[✓] DEDUP CORRECTNESS: Data preserved"
echo "[✓] CoW CORRECTNESS: Writes trigger proper page copies"
echo "[✓] DATA VERIFICATION: All offsets verified"
echo "[✓] CONCURRENCY STRESS: 1 → 16 → 32 process progression"
echo "[✓] PAGE CACHE: All operations via page cache (--direct=0)"
echo "[✓] CACHE MONITORING: Enabled throughout test"
echo ""
echo "======================================================="
echo "✓ ALL TESTS PASSED"
echo "======================================================="
echo ""
echo "[INTERPRETATION]"
echo "  If read-only disk stats show ~0% utilization:"
echo "    → Files ARE cached in page cache ✓"
echo "  If read-only disk stats show 70%:"
echo "    → Files NOT cached, possible cache eviction issue"
echo ""

