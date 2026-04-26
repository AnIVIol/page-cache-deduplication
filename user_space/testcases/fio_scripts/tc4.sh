#!/bin/bash
set -e
# --- 0. Prepare Environment ---
# Ensure a completely clean slate before we start generating data
../setup_env.sh || exit 1

# --- Configuration & Defaults ---
TEST_FILE="$PWD/single_dedup.dat"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"
# Default values if you don't specify them in the terminal
FILE_SIZE="512M"
DEDUP_RATIO="80"
BLOCK_SIZE="4k"

# Parse terminal arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -s|--size) FILE_SIZE="$2"; shift ;;
        -r|--ratio) DEDUP_RATIO="$2"; shift ;;
        -b|--bs) BLOCK_SIZE="$2"; shift ;;
        -h|--help) 
            echo "Usage: ./tc3_single_file.sh [-s|--size 100M] [-r|--ratio 50] [-b|--bs 4k]"
            exit 0 
            ;;
        *) 
            echo "[-] Unknown parameter: $1"
            echo "Run './tc3_single_file.sh --help' for usage."
            exit 1 
            ;;
    esac
    shift
done

echo "=== Test Parameters ==="
echo "Size: $FILE_SIZE | Ratio: $DEDUP_RATIO% | Block Size: $BLOCK_SIZE"
echo "======================="
echo "=== 1. Layout: Generating Internally Compressible File ==="
# We use FIO to generate the file so we can precisely control the duplication ratio
sudo fio --name=layout \
         --filename="$TEST_FILE" \
         --size="$FILE_SIZE" \
         --rw=write \
         --bs=4k \
         --dedupe_percentage="$DEDUP_RATIO" \
         --scramble_buffers=0 \
         --numjobs=1 \
         --invalidate=0 \
         --ioengine=psync \
         --direct=0 > /dev/null

echo "[+] $FILE_SIZE file generated with $DEDUP_RATIO% duplicate blocks."
sync # Ensure all dirty pages from creation are flushed

echo "=== 2. Scanner: Feeding File to Kernel ==="
if [ -e "$SCANNER_SYSFS" ]; then
    echo 1 | sudo tee "$SCANNER_START" > /dev/null
    echo "$TEST_FILE" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] File submitted to background scanner."
    
    # Give the scanner thread time to hash 100MB and traverse the Unstable Tree
    echo "[*] Waiting 5 seconds for background merges to complete..."
    sleep 5 
else
    echo "[-] ERROR: Scanner sysfs interface not found."
    exit 1
fi

echo "=== 3. Phase 1: Read Stress Test ==="
# 4 concurrent threads reading randomly to verify the XArray fast-path
sudo fio --name=read_stress \
         --filename="$TEST_FILE" \
         --rw=randread \
         --bs="$BLOCK_SIZE" \
         --numjobs=8 \
         --invalidate=0 \
         --time_based --runtime=5 \
         --group_reporting

echo "=== 4. Phase 2: Write Stress Test (DANGER ZONE) ==="
echo "[!] WARNING: If your Kprobe atomic sleep bug isn't fixed, the VM will panic NOW."
sleep 2

# 4 concurrent threads attempting to overwrite the deduplicated pages
# This WILL trigger your FGP_WRITE kprobe interceptor.
sudo fio --name=write_stress \
         --filename="$TEST_FILE" \
         --rw=randwrite \
         --bs="$BLOCK_SIZE" \
         --numjobs=16 \
         --invalidate=0 \
         --time_based --runtime=5 \
         --group_reporting

echo "=== 3. Phase 3: Read Stress Test ==="
# 4 concurrent threads reading randomly to verify the XArray fast-path
sudo fio --name=read_stress \
         --filename="$TEST_FILE" \
         --rw=randread \
         --bs="$BLOCK_SIZE" \
         --numjobs=32 \
         --invalidate=0 \
         --time_based --runtime=5 \
         --group_reporting


echo "=== 5. Cleaning Up ==="
rm "$TEST_FILE"
echo "[+] Test Complete. You survived!"
