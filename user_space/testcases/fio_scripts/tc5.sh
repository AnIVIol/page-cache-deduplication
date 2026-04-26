#!/bin/bash

# --- 0. Setup Environment ---
../setup_env.sh || exit 1

# --- Defaults ---
FILE_SIZE_MB=500
SHARE_PERCENT=40
NUM_GROUPS=2
BLOCK_SIZE="4k"

FILE="$PWD/output.dat"
SCANNER_SYSFS="/sys/kernel/dedup_scanner/scan_file"
SCANNER_START="/sys/kernel/dedup_scanner/run"

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

echo "=== Parameters ==="
echo "Size: ${FILE_SIZE_MB}MB | Share: ${SHARE_PERCENT}% | Groups: ${NUM_GROUPS} | BS: ${BLOCK_SIZE}"
echo "=================="

# --- 1. Generate file ---
echo "=== 1. Generating File ==="
./gen_dedup "$FILE_SIZE_MB" "$SHARE_PERCENT" "$NUM_GROUPS" || exit 1
echo "[+] File generated: $FILE"

sync

# --- 2. Feed to scanner ---
echo "=== 2. Feeding to Scanner ==="
if [ -e "$SCANNER_SYSFS" ]; then
    echo 1 | sudo tee "$SCANNER_START" > /dev/null
    echo "$FILE" | sudo tee "$SCANNER_SYSFS" > /dev/null
    echo "[+] Initial scan submitted"
else
    echo "[-] Scanner sysfs not found"
    exit 1
fi

        
# --- 4. Read stress ---
echo "=== 4. Read Stress ==="
sudo fio --name=read \
         --filename="$FILE" \
         --rw=randread \
         --bs="$BLOCK_SIZE" \
         --numjobs=8 \
	 --invalidate=0 \
         --time_based --runtime=5 \
         --group_reporting

sleep 5
echo "=== 6. Write Stress ==="
sudo fio --name=write \
         --filename="$FILE" \
         --rw=randwrite \
         --bs="$BLOCK_SIZE" \
         --invalidate=0 \
         --numjobs=1 \
         --time_based --runtime=15 \
         --group_reporting

# --- 5. Mixed stress ---
echo "=== 5. Mixed Stress ==="
sudo fio --name=mixed \
         --filename="$FILE" \
         --rw=randrw \
         --rwmixread=70 \
         --bs="$BLOCK_SIZE" \
          --invalidate=0 \
         --numjobs=16 \
         --time_based --runtime=15 \
         --group_reporting

# --- 6. Write stress ---
# --- Cleanup ---
echo "=== Cleaning Up ==="
rm -f "$FILE"

echo "[+] Test complete"
