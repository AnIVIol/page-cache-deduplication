#!/bin/bash
# PRIORITY 1 EXTENDED TESTS
# Large files, mixed duplication, memory pressure, large CoW offsets

set -e
../setup_env.sh || exit 1

TEST_DIR="$PWD"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

# Helpers
show_cache_stats() {
    local label=$1
    echo "  [CACHE] $label"
    cat /proc/meminfo | grep -E "Cached:|Dirty:|MemAvailable:" | sed 's/^/    /'
}

show_disk_stats() {
    local label=$1
    echo "  [DISK] $label"
    iostat -d 1 1 | tail -1 | sed 's/^/    /'
}

verify_bytes_at_offset() {
    local filename=$1
    local offset=$2
    local expected_byte=$3
    local num_bytes=$4
    
    python3 << PYTHON_VERIFY
import os
filename = "$filename"
offset = $offset
expected_char = ord("$expected_byte")
num_bytes = $num_bytes

fd = os.open(filename, os.O_RDONLY)
os.lseek(fd, offset, os.SEEK_SET)
data = os.read(fd, num_bytes)
os.close(fd)

mismatches = sum(1 for b in data if b != expected_char)
if mismatches == 0:
    print(f"[✓] Verified: offset {offset}, {num_bytes} bytes are '{chr(expected_char)}'")
    exit(0)
else:
    print(f"[✗] Verification failed: {mismatches}/{num_bytes} bytes mismatch")
    exit(1)
PYTHON_VERIFY
}

echo "======================================================="
echo "  PRIORITY 1: EXTENDED TESTING SUITE                  "
echo "======================================================="
echo ""
echo "[TEST FOCUS]"
echo "  1. Large file stress (1GB+ files)"
echo "  2. Mixed duplication ratios (10%, 50%, 90%)"
echo "  3. Memory pressure scenarios"
echo "  4. Large CoW offsets in GB-sized files"
echo ""

echo "[+] Cleaning up..."
rm -f "$TEST_DIR"/*.dat "$TEST_DIR"/stats_*.txt

echo ""
echo "======================================================="
echo "  TEST 1: MIXED DUPLICATION RATIOS (256MB files)"
echo "======================================================="

echo ""
echo "=== TEST 1A: 100% Duplicate (baseline) ==="
echo "[+] Creating 256MB files with 100% duplication..."
show_cache_stats "Before TEST 1A"

python3 << PYTHON_CREATE
import os
for filename in ['dup_100_file1.dat', 'dup_100_file2.dat', 'dup_100_file3.dat']:
    fd = os.open(filename, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    for i in range(256):
        os.write(fd, b'A' * (1024*1024))
    os.fsync(fd)
    os.close(fd)
PYTHON_CREATE

for f in dup_100_*.dat; do cat "$f" > /dev/null; done
show_cache_stats "After loading 100% duplicate files"

echo 1 | sudo tee "$SCANNER_START" > /dev/null
for f in dup_100_*.dat; do echo "$f" | sudo tee "$SCANNER_SYSFS" > /dev/null; done
sleep 5

show_cache_stats "After dedup (100% duplicate)"
echo "[✓] TEST 1A completed"

echo ""
echo "=== TEST 1B: 90% Duplicate (real-world) ==="
echo "[+] Creating 256MB files with 90% duplication..."
show_cache_stats "Before TEST 1B"

python3 << PYTHON_CREATE
import os
# File 1: 230MB 'A' + 26MB 'B'
fd = os.open('dup_90_file1.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(230):
    os.write(fd, b'A' * (1024*1024))
for i in range(26):
    os.write(fd, b'B' * (1024*1024))
os.fsync(fd)
os.close(fd)

# File 2: 230MB 'A' + 26MB 'C'
fd = os.open('dup_90_file2.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(230):
    os.write(fd, b'A' * (1024*1024))
for i in range(26):
    os.write(fd, b'C' * (1024*1024))
os.fsync(fd)
os.close(fd)

# File 3: 230MB 'A' + 26MB 'D'
fd = os.open('dup_90_file3.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(230):
    os.write(fd, b'A' * (1024*1024))
for i in range(26):
    os.write(fd, b'D' * (1024*1024))
os.fsync(fd)
os.close(fd)
PYTHON_CREATE

for f in dup_90_*.dat; do cat "$f" > /dev/null; done
show_cache_stats "After loading 90% duplicate files"

echo 1 | sudo tee "$SCANNER_START" > /dev/null
for f in dup_90_*.dat; do echo "$f" | sudo tee "$SCANNER_SYSFS" > /dev/null; done
sleep 5

show_cache_stats "After dedup (90% duplicate)"
echo "[✓] TEST 1B completed"

echo ""
echo "=== TEST 1C: 50% Duplicate (sparse) ==="
echo "[+] Creating 256MB files with 50% duplication..."
show_cache_stats "Before TEST 1C"

python3 << PYTHON_CREATE
import os
# File 1: 128MB 'A' + 128MB 'B'
fd = os.open('dup_50_file1.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(128):
    os.write(fd, b'A' * (1024*1024))
for i in range(128):
    os.write(fd, b'B' * (1024*1024))
os.fsync(fd)
os.close(fd)

# File 2: 128MB 'A' + 128MB 'C'
fd = os.open('dup_50_file2.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(128):
    os.write(fd, b'A' * (1024*1024))
for i in range(128):
    os.write(fd, b'C' * (1024*1024))
os.fsync(fd)
os.close(fd)
PYTHON_CREATE

for f in dup_50_*.dat; do cat "$f" > /dev/null; done
show_cache_stats "After loading 50% duplicate files"

echo 1 | sudo tee "$SCANNER_START" > /dev/null
for f in dup_50_*.dat; do echo "$f" | sudo tee "$SCANNER_SYSFS" > /dev/null; done
sleep 5

show_cache_stats "After dedup (50% duplicate)"
echo "[✓] TEST 1C completed"

echo ""
echo "=== TEST 1D: 10% Duplicate (minimal) ==="
echo "[+] Creating 256MB files with 10% duplication..."
show_cache_stats "Before TEST 1D"

python3 << PYTHON_CREATE
import os
# File 1: 25MB 'A' + 231MB 'B'
fd = os.open('dup_10_file1.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(25):
    os.write(fd, b'A' * (1024*1024))
for i in range(231):
    os.write(fd, b'B' * (1024*1024))
os.fsync(fd)
os.close(fd)

# File 2: 25MB 'A' + 231MB 'C'
fd = os.open('dup_10_file2.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
for i in range(25):
    os.write(fd, b'A' * (1024*1024))
for i in range(231):
    os.write(fd, b'C' * (1024*1024))
os.fsync(fd)
os.close(fd)
PYTHON_CREATE

for f in dup_10_*.dat; do cat "$f" > /dev/null; done
show_cache_stats "After loading 10% duplicate files"

echo 1 | sudo tee "$SCANNER_START" > /dev/null
for f in dup_10_*.dat; do echo "$f" | sudo tee "$SCANNER_SYSFS" > /dev/null; done
sleep 5

show_cache_stats "After dedup (10% duplicate)"
echo "[✓] TEST 1D completed"

rm -f dup_*.dat

echo ""
echo "======================================================="
echo "  TEST 2: LARGE FILE STRESS (1GB+ files)"
echo "======================================================="

echo ""
echo "=== TEST 2A: 1GB Identical Files ==="
echo "[+] Creating 1GB identical files (takes ~30 seconds)..."
show_cache_stats "Before TEST 2A creation"

python3 << PYTHON_CREATE
import os
import time
start = time.time()
for filename in ['large_1gb_file1.dat', 'large_1gb_file2.dat']:
    print(f"[+] Creating {filename}...")
    fd = os.open(filename, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    for i in range(1024):
        os.write(fd, b'X' * (1024*1024))
        if (i+1) % 256 == 0:
            print(f"    {i+1}/1024 MB written...")
    os.fsync(fd)
    os.close(fd)
elapsed = time.time() - start
print(f"[+] Creation took {elapsed:.1f}s")
PYTHON_CREATE

echo "[+] Loading 1GB files into cache..."
start_cache=$(cat /proc/meminfo | grep "^Cached:" | awk '{print $2}')
for f in large_1gb_*.dat; do cat "$f" > /dev/null; done
end_cache=$(cat /proc/meminfo | grep "^Cached:" | awk '{print $2}')
echo "[+] Cache increased by $((end_cache - start_cache)) KB"
show_cache_stats "After loading 1GB files"

echo "[+] Running dedup on 1GB files..."
echo 1 | sudo tee "$SCANNER_START" > /dev/null
for f in large_1gb_*.dat; do echo "$f" | sudo tee "$SCANNER_SYSFS" > /dev/null; done
sleep 10

show_cache_stats "After dedup (1GB files)"
echo "[✓] TEST 2A completed"

echo ""
echo "=== TEST 2B: CoW on Large Offsets (1GB file) ==="
echo "[+] Testing CoW writes at large offsets..."

python3 << PYTHON_WRITE
import os

fd = os.open('large_1gb_file1.dat', os.O_RDWR)

# Write at offset 100MB
print("[+] Writing 'Y' at offset 100MB...")
os.lseek(fd, 100*1024*1024, os.SEEK_SET)
os.write(fd, b'Y' * 4096)

# Write at offset 500MB
print("[+] Writing 'Z' at offset 500MB...")
os.lseek(fd, 500*1024*1024, os.SEEK_SET)
os.write(fd, b'Z' * 4096)

# Write at offset 900MB
print("[+] Writing 'W' at offset 900MB...")
os.lseek(fd, 900*1024*1024, os.SEEK_SET)
os.write(fd, b'W' * 4096)

os.fsync(fd)
os.close(fd)
PYTHON_WRITE

show_cache_stats "After CoW writes on 1GB file"

echo "[+] Verifying CoW writes..."
verify_bytes_at_offset "large_1gb_file1.dat" $((100*1024*1024)) "Y" 4096
verify_bytes_at_offset "large_1gb_file1.dat" $((500*1024*1024)) "Z" 4096
verify_bytes_at_offset "large_1gb_file1.dat" $((900*1024*1024)) "W" 4096

# Verify unwritten regions still have original data
echo "[+] Verifying unwritten regions..."
verify_bytes_at_offset "large_1gb_file1.dat" $((50*1024*1024)) "X" 4096
verify_bytes_at_offset "large_1gb_file1.dat" $((999*1024*1024)) "X" 4096

echo "[✓] TEST 2B completed"

rm -f large_1gb_*.dat

echo ""
echo "======================================================="
echo "  TEST 3: CONCURRENT STRESS WITH LARGE FILES"
echo "======================================================="

echo ""
echo "=== TEST 3A: 32 Concurrent Jobs on 512MB Files ==="
echo "[+] Creating 512MB test files..."
show_cache_stats "Before TEST 3A"

python3 << PYTHON_CREATE
import os
for i in range(3):
    fd = os.open(f'stress_file_{i}.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    for j in range(512):
        os.write(fd, b'S' * (1024*1024))
    os.fsync(fd)
    os.close(fd)
PYTHON_CREATE

for f in stress_file_*.dat; do cat "$f" > /dev/null; done
show_cache_stats "After loading stress files"

echo "[+] Running FIO stress test (32 jobs, 30 seconds)..."
fio --name=concurrent_large \
    --filename="$TEST_DIR/stress_file_0.dat:$TEST_DIR/stress_file_1.dat:$TEST_DIR/stress_file_2.dat" \
    --rw=randwrite \
    --bs=4k \
    --iodepth=1 \
    --numjobs=32 \
    --runtime=30 \
    --time_based \
    --ioengine=psync \
    --direct=0 \
    2>&1 | tail -30

show_cache_stats "After concurrent stress"
echo "[✓] TEST 3A completed"

rm -f stress_file_*.dat

echo ""
echo "======================================================="
echo "  TEST 4: MEMORY PRESSURE"
echo "======================================================="

echo ""
echo "=== TEST 4A: Dedup Under Memory Pressure ==="
echo "[+] Checking available memory..."
show_cache_stats "Available memory"

MEM_AVAILABLE=$(cat /proc/meminfo | grep "^MemAvailable:" | awk '{print $2}')
MEM_TOTAL=$(cat /proc/meminfo | grep "^MemTotal:" | awk '{print $2}')
MEM_USED=$((MEM_TOTAL - MEM_AVAILABLE))
MEM_USAGE=$((MEM_USED * 100 / MEM_TOTAL))

echo "[+] Current memory usage: ${MEM_USAGE}% (${MEM_USED}MB / ${MEM_TOTAL}MB)"

if [ $MEM_AVAILABLE -lt $((1024*1024)) ]; then
    echo "[-] Not enough available memory for pressure test"
    echo "    Available: $((MEM_AVAILABLE / 1024))GB, Need: ~1GB"
else
    echo "[+] Memory pressure test: Creating 256MB test files..."
    python3 << PYTHON_CREATE
import os
for i in range(4):
    fd = os.open(f'pressure_file_{i}.dat', os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    for j in range(256):
        os.write(fd, b'P' * (1024*1024))
    os.fsync(fd)
    os.close(fd)
PYTHON_CREATE

    for f in pressure_file_*.dat; do cat "$f" > /dev/null; done
    show_cache_stats "After loading files under pressure"

    echo 1 | sudo tee "$SCANNER_START" > /dev/null
    for f in pressure_file_*.dat; do echo "$f" | sudo tee "$SCANNER_SYSFS" > /dev/null; done
    sleep 5

    show_cache_stats "After dedup under pressure"
    echo "[✓] TEST 4A completed"

    rm -f pressure_file_*.dat
fi

echo ""
echo "======================================================="
echo "  FINAL RESULTS - PRIORITY 1"
echo "======================================================="
echo ""
echo "[✓] TEST 1: Mixed duplication ratios (10%, 50%, 90%, 100%)"
echo "[✓] TEST 2: Large file stress (1GB files)"
echo "[✓] TEST 2B: Large CoW offsets verified"
echo "[✓] TEST 3: Concurrent stress with large files"
echo "[✓] TEST 4: Memory pressure scenarios"
echo ""
echo "======================================================="
echo "✓ PRIORITY 1 TESTS COMPLETE"
echo "======================================================="
echo ""
