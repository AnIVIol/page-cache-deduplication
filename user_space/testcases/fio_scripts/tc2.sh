#!/bin/bash

# --- Configuration ---
FILE_A="$PWD/survivor1.dat"
FILE_B="$PWD/victim1.dat"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"

echo "=== 1. Creating 50MB Test Files ==="
# Write random data to the survivor, then strictly copy it to the victim
dd if=/dev/urandom of="$FILE_A" bs=1M count=50 status=none
cp "$FILE_A" "$FILE_B"
echo "[+] Files created successfully."


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
