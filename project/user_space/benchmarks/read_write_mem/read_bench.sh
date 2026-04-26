#!/bin/bash
set -e

GEN_BIN="./gen_files"
REPORT="read_report.csv"

SHARES=(10 20 30 40 50 60 70 80 90 100)
SIZES=(64 128 256 512 1024)
RUNS=5

echo "size,file_type,%shared,avg_ms" > "$REPORT"

drop_caches() {
    echo "[*] Dropping caches"
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

run_seq_read() {
    local file=$1
    local size=$2

    fio --name=seqread \
        --filename="$file" \
        --rw=read \
        --bs=4k \
        --size="${size}M" \
        --ioengine=sync \
        --direct=0 \
        --numjobs=8 \
        --group_reporting \
        > /dev/null 2>&1
}

run_rand_read() {
    local file=$1
    local size=$2

    fio --name=randread \
        --filename="$file" \
        --rw=randread \
        --bs=4k \
        --size="${size}M" \
        --ioengine=sync \
        --direct=0 \
        --numjobs=8 \
        --group_reporting \
        > /dev/null 2>&1
}

time_diff_ms() {
    start=$1
    end=$2
    echo "$(echo "$end - $start" | bc)"
}

for size in ${SIZES[@]}; do
for share in ${SHARES[@]}; do

echo "======================================"
echo "[*] SIZE ${size} MB | SHARE ${share}%"

DIR="/test_files/read_$size"
rm -rf "$DIR"
mkdir -p "$DIR"

echo "[*] Generating files"
$GEN_BIN 1 $share $size "$DIR" >/dev/null

F="$DIR/file_0.bin"

# -------- NORMAL SEQUENTIAL --------
echo "[*] NORMAL SEQUENTIAL"

drop_caches
run_seq_read "$F" "$size"   # warm cache

total=0
for i in $(seq 1 $RUNS); do
    echo "  run $i"

    start=$(date +%s.%N)
    run_seq_read "$F" "$size"
    end=$(date +%s.%N)

    t=$(time_diff_ms "$start" "$end")
    total=$(echo "$total + $t" | bc)
done

avg=$(echo "$total / $RUNS" | bc -l)
echo "[NORMAL SEQ] size=$size avg=${avg} ms"
echo "$size,normal,NA,$avg" >> "$REPORT"

# -------- NORMAL RANDOM --------
echo "[*] NORMAL RANDOM"

drop_caches
run_rand_read "$F" "$size"

total=0
for i in $(seq 1 $RUNS); do
    echo "  run $i"

    start=$(date +%s.%N)
    run_rand_read "$F" "$size"
    end=$(date +%s.%N)

    t=$(time_diff_ms "$start" "$end")
    total=$(echo "$total + $t" | bc)
done

avg=$(echo "$total / $RUNS" | bc -l)
echo "[NORMAL RAND] size=$size avg=${avg} ms"
echo "$size,normal_rand,NA,$avg" >> "$REPORT"

# -------- SHARED --------
echo "[*] SHARED"

sudo insmod ../../../modules/scanner.ko 2>/dev/null || true
echo "1" | sudo tee /sys/kernel/dedup_scanner/run > /dev/null

drop_caches
run_seq_read "$F" "$size"

echo "[*] Sending to scanner"
echo "$F" | sudo tee /sys/kernel/dedup_scanner/scan_file >/dev/null

[ "$size" -ge 512 ] && sleep 3 || sleep 1

# -------- SHARED SEQ --------
echo "[*] SHARED SEQ"

total=0
for i in $(seq 1 $RUNS); do
    echo "  run $i"

    start=$(date +%s.%N)
    run_seq_read "$F" "$size"
    end=$(date +%s.%N)

    t=$(time_diff_ms "$start" "$end")
    total=$(echo "$total + $t" | bc)
done

avg=$(echo "$total / $RUNS" | bc -l)
echo "[SHARED SEQ] size=$size avg=${avg} ms"
echo "$size,shared,$share,$avg" >> "$REPORT"

# -------- SHARED RANDOM --------
echo "[*] SHARED RANDOM"

drop_caches
run_rand_read "$F" "$size"

total=0
for i in $(seq 1 $RUNS); do
    echo "  run $i"

    start=$(date +%s.%N)
    run_rand_read "$F" "$size"
    end=$(date +%s.%N)

    t=$(time_diff_ms "$start" "$end")
    total=$(echo "$total + $t" | bc)
done

avg=$(echo "$total / $RUNS" | bc -l)
echo "[SHARED RAND] size=$size avg=${avg} ms"
echo "$size,shared_rand,$share,$avg" >> "$REPORT"

sudo rmmod scanner 2>/dev/null || true
rm -rf "$DIR"

done
done

echo "======================================"
echo "[+] DONE. Results saved in $REPORT"
