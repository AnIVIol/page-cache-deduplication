#!/bin/bash
set -e

GEN_BIN="./gen_files"
REPORT="write_report.csv"

SHARES=(10 20 30 40 50 60 70 80 90 100)
SIZES=(64 128 256 512 1024)
RUNS=5

echo "size,mode,%shared,avg_ms" > "$REPORT"

drop_caches() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
}

run_seq_write() {
    local file=$1
    local size=$2

    fio --name=seqwrite \
        --filename="$file" \
        --rw=write \
        --bs=4k \
        --size="${size}M" \
        --ioengine=sync \
        --direct=0 \
        --numjobs=8 \
        --group_reporting \
        > /dev/null 2>&1
}

run_rand_write() {
    local file=$1
    local size=$2

    fio --name=randwrite \
        --filename="$file" \
        --rw=randwrite \
        --bs=4k \
        --size="${size}M" \
        --ioengine=sync \
        --direct=0 \
        --numjobs=8 \
        --group_reporting \
        > /dev/null 2>&1
}

time_diff_ms() {
    echo "$(echo "$2 - $1" | bc)"
}

for size in ${SIZES[@]}; do

    echo "======================================"
    echo "[*] SIZE ${size} MB"

    # ---------- NORMAL ----------
    echo "[*] NORMAL"

    DIR="/test_files/write_normal_$size"
    rm -rf "$DIR"
    mkdir -p "$DIR"

    $GEN_BIN 1 0 $size "$DIR" >/dev/null
    F="$DIR/file_0.bin"

    drop_caches
    cat $F > /dev/null

    # NORMAL SEQ
    total=0
    for i in $(seq 1 $RUNS); do
        start=$(date +%s.%N)
        run_seq_write "$F" "$size"
        end=$(date +%s.%N)
        t=$(time_diff_ms "$start" "$end")
        total=$(echo "$total + $t" | bc)
    done
    avg=$(echo "$total / $RUNS" | bc -l)
    echo "[NORMAL SEQ] $avg ms"
    echo "$size,normal_seq,NA,$avg" >> "$REPORT"

    # NORMAL RAND
    total=0
    for i in $(seq 1 $RUNS); do
        start=$(date +%s.%N)
        run_rand_write "$F" "$size"
        end=$(date +%s.%N)
        t=$(time_diff_ms "$start" "$end")
        total=$(echo "$total + $t" | bc)
    done
    avg=$(echo "$total / $RUNS" | bc -l)
    echo "[NORMAL RAND] $avg ms"
    echo "$size,normal_rand,NA,$avg" >> "$REPORT"

    rm -rf "$DIR"

    # ---------- SHARED ----------
    for share in ${SHARES[@]}; do

        echo "--------------------------------------"
        echo "[*] SHARED ${share}%"

        DIR="/test_files/write_shared_${size}_${share}"
        rm -rf "$DIR"
        mkdir -p "$DIR"

        $GEN_BIN 1 $share $size "$DIR" >/dev/null
        F="$DIR/file_0.bin"

        sudo insmod ../../../modules/scanner.ko 2>/dev/null || true
        echo 1 | sudo tee /sys/kernel/dedup_scanner/run >/dev/null

        drop_caches

        # load + dedup
        cat "$F" > /dev/null
        echo "$F" | sudo tee /sys/kernel/dedup_scanner/scan_file >/dev/null
        [ "$size" -ge 512 ] && sleep 3 || sleep 1

        # ---- SHARED SEQ ----
        total=0
        for i in $(seq 1 $RUNS); do
            start=$(date +%s.%N)
            run_seq_write "$F" "$size"
            end=$(date +%s.%N)
            t=$(time_diff_ms "$start" "$end")
            total=$(echo "$total + $t" | bc)
        done
        avg=$(echo "$total / $RUNS" | bc -l)
        echo "[SHARED SEQ ${share}%] $avg ms"
        echo "$size,shared_seq,$share,$avg" >> "$REPORT"

        sudo rmmod scanner 2>/dev/null || true
        rm -rf "$DIR"

        # ---- SHARED RAND (fresh setup again) ----
        DIR="/test_files/write_shared_${size}_${share}"
        mkdir -p "$DIR"

        $GEN_BIN 1 $share $size "$DIR" >/dev/null
        F="$DIR/file_0.bin"

        sudo insmod ../../../modules/scanner.ko 2>/dev/null || true
        echo 1 | sudo tee /sys/kernel/dedup_scanner/run >/dev/null

        drop_caches

        cat "$F" > /dev/null
        echo "$F" | sudo tee /sys/kernel/dedup_scanner/scan_file >/dev/null
        [ "$size" -ge 512 ] && sleep 3 || sleep 1

        total=0
        for i in $(seq 1 $RUNS); do
            start=$(date +%s.%N)
            run_rand_write "$F" "$size"
            end=$(date +%s.%N)
            t=$(time_diff_ms "$start" "$end")
            total=$(echo "$total + $t" | bc)
        done
        avg=$(echo "$total / $RUNS" | bc -l)
        echo "[SHARED RAND ${share}%] $avg ms"
        echo "$size,shared_rand,$share,$avg" >> "$REPORT"

        sudo rmmod scanner 2>/dev/null || true
        rm -rf "$DIR"

    done

done

echo "======================================"
echo "[+] DONE. Results saved in $REPORT"
