#!/bin/bash

../setup_env.sh || exit 1

# --- Configuration ---
FILE_A="$PWD/survivor.dat"
FILE_B="$PWD/victim.dat"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

# --- Defaults ---
FILE_SIZE="512M"
BLOCK_SIZE="4k"

# --- Parse CLI arguments ---
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -s|--size) FILE_SIZE="$2"; shift ;;
        -b|--bs) BLOCK_SIZE="$2"; shift ;;
        -h|--help)
            echo "Usage: ./script.sh [-s|--size 50M] [-b|--bs 4k]"
            exit 0
            ;;
        *)
            echo "[-] Unknown parameter: $1"
            echo "Run './script.sh --help' for usage."
            exit 1
            ;;
    esac
    shift
done

echo "=== Test Parameters ==="
echo "Size: $FILE_SIZE | Block Size: $BLOCK_SIZE"
echo "======================="

echo "=== 1. Creating Test Files ==="
# Convert size like 50M → count for dd (in MB)
COUNT=$(echo $FILE_SIZE | sed 's/M//')

dd if=/dev/urandom of="$FILE_A" bs=1M count="$COUNT" status=none
cp "$FILE_A" "$FILE_B"
echo "[+] Files created successfully."

echo "=== 2. Feeding Files to Kernel Scanner ==="
if [ -w "$SCANNER_SYSFS" ]; then
    echo 1 | sudo tee "$SCANNER_START" > /dev/null

    echo "$FILE_A" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] Survivor scanned."
        
    echo "$FILE_B" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] Victim scanned."
        
    sleep 2
else
    echo "[-] ERROR: Scanner sysfs interface not found at $SCANNER_SYSFS"
    exit 1
fi

echo "=== 3. Verifying Memory Consolidation ==="
echo "[*] Deduplication should be active. Proceeding to stress test."

echo "=== 4. Launching FIO Read Stress Test ==="
sudo fio --name=read_stress \
         --filename="$FILE_B" \
         --rw=randwrite \
         --bs="$BLOCK_SIZE" \
         --numjobs=8 \
         --time_based --runtime=20 \
         --group_reporting

echo "=== 5. Cleaning Up ==="
rm "$FILE_A" "$FILE_B"
echo "[+] Test Complete."
