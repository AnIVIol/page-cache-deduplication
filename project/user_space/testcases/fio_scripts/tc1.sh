#!/bin/bash

../setup_env.sh || exit 1


# --- Configuration ---
FILE_A="$PWD/survivor.dat"
FILE_B="$PWD/victim.dat"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"

echo "=== 1. Creating 500MB Test Files ==="
# Write random data to the survivor, then strictly copy it to the victim
dd if=/dev/urandom of="$FILE_A" bs=1M count=500 status=none
cp "$FILE_A" "$FILE_B"
echo "[+] Files created successfully."

echo "=== 2. Feeding Files to Kernel Scanner ==="
# Send the absolute paths to your kernel thread
if [ -w "$SCANNER_SYSFS" ]; then
    echo "$FILE_A" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] Survivor scanned."
    
    # Give the background thread a brief moment to process the Unstable Tree
    sleep 2 
    
    echo "$FILE_B" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] Victim scanned."
    
    # Wait for the merges to complete
    sleep 5
else
    echo "[-] ERROR: Scanner sysfs interface not found at $SCANNER_SYSFS"
    exit 1
fi

echo "=== 3. Verifying Memory Consolidation ==="
# Optional: If you have a way to read how many pages were merged, you could print it here.
echo "[*] Deduplication should be active. Proceeding to stress test."

echo "=== 4. Launching FIO Read Stress Test ==="
# 8 concurrent threads hammering the deduplicated victim file with 4KB reads
sudo fio --name=read_stress \
         --filename="$FILE_B" \
         --rw=randread \
         --bs=4k \
         --numjobs=8 \
         --time_based --runtime=20 \
         --group_reporting

echo "=== 5. Cleaning Up ==="
rm "$FILE_A" "$FILE_B"
echo "[+] Test Complete."
