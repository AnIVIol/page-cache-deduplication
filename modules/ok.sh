#!/bin/bash

cd "$(dirname "$0")" || exit 1

if [ $# -eq 0 ]; then
    echo "Usage: $0 <module1> <module2> ... [0]"
    exit 1
fi

# Check if last argument is 0
STOP_AFTER_BUILD=0
if [ "${!#}" == "0" ]; then
    STOP_AFTER_BUILD=1
    set -- "${@:1:$(($#-1))}"   # remove last argument
fi

MODULES=("$@")

for ((i=${#MODULES[@]}-1; i>=0; i--)); do
    mod="${MODULES[i]}"
    if lsmod | grep -q "^${mod}\b"; then
        sudo rmmod ${mod}.ko 2>/dev/null || sudo rmmod ${mod}
    fi
done


make clean


if [ $STOP_AFTER_BUILD -eq 1 ]; then
    echo "Stopping after build (flag detected)"
    exit 0
fi

make 
sudo dmesg -c > /dev/null

for mod in "${MODULES[@]}"; do
    if [[ ! -f "${mod}.ko" ]]; then
        echo "Compilation error: ${mod}.ko not found"
        exit 1
    fi
done

for mod in "${MODULES[@]}"; do
    sudo insmod ${mod}.ko
    if [[ $? != 0 ]]; then
        echo "Error loading module: $mod"
        exit 1
    fi
done
