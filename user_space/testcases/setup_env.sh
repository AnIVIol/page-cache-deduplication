#!/bin/bash

cd "$(dirname "$0")" || exit 1

echo "=== [Setup] Initializing Kernel Environment ==="
echo "[*] Flushing existing merges"
echo 0 | sudo tee /sys/kernel/dedup_scanner/run
echo 1 | sudo tee /sys/kernel/page_dedup/flush
# 1. Unload and Reload Modules
echo "[*] Reloading custom kernel modules..."
# We use 'if !' to catch if your ok.sh script fails (e.g., a module is busy)
if ! ../../modules/ok.sh scanner pagecache_inspector ; then
    echo "[-] ERROR: Failed to reload modules. Is a file still open?"
    exit 1
fi

# 2. Clear Caches
echo "[*] Clearing PageCache, dentries, and inodes..."
# ALWAYS run 'sync' before dropping caches to flush dirty data to disk
sync 
# 'echo 3' tells the VM subsystem to drop everything it safely can
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

# 3. Start the Scanner
echo "[*] Activating background scanner..."
SCANNER_RUN="/sys/kernel/dedup_scanner/run"

if [ -e "$SCANNER_RUN" ]; then
    echo 1 | sudo tee "$SCANNER_RUN" > /dev/null
    echo "[+] Scanner activated."
else
    echo "[-] ERROR: Scanner run trigger not found at $SCANNER_RUN"
    exit 1
fi

echo "=== [Setup] Environment Ready ==="
# Exit 0 indicates success to whatever script called this one
exit 0
