#!/bin/bash

# load_modules.sh - Loads the remaining external modules


MODULES_DIR="../../../modules" 

# Only these two remain as external modules
MODS=(
    "pagecache_inspector.ko"
    "scanner.ko"
)

function load_mods() {
    for mod in "${MODS[@]}"; do
        if [ -f "$MODULES_DIR/$mod" ]; then
            echo "Loading $mod..."
            sudo insmod "$MODULES_DIR/$mod" || echo "Failed to load $mod (might be already loaded)"
        else
            echo "Warning: $mod not found in $MODULES_DIR"
        fi
    done
}

function unload_mods() {
    # Unload in reverse order
    for (( i=${#MODS[@]}-1; i>=0; i-- )); do
        mod_name=$(basename "${MODS[$i]}" .ko)
        echo "Unloading $mod_name..."
        sudo rmmod "$mod_name" 2>/dev/null
    done
}

case "$1" in
    load)
        load_mods
        # Start the scanner daemon immediately after loading modules
        echo 1 | sudo tee /sys/kernel/dedup_scanner/run > /dev/null
        ;;
    unload)
        # Stop the scanner daemon before unloading modules
        echo 0 | sudo tee /sys/kernel/dedup_scanner/run > /dev/null
        unload_mods
        ;;
    *)
        echo "Usage: $0 {load|unload}"
        exit 1
esac
