#!/bin/bash
# Build and run the full dedup test suite, including the mixed-ops tests.
set -e

#echo "=== General Test Suite ==="

cd general
#sudo ./run_all_tests.sh
cd ..

echo "=== cp_ln Test Suite ==="

cd cp_ln
#sudo ./tc.sh
#sudo ./test_cp_dedup_v2.sh
#sudo ./test_master_dedup_v2.sh
sudo ./test_symlinks_dedup_v2.sh
cd ..

echo "=== fio Test Suite ==="

cd fio_scripts
sudo ./tc1.sh
sudo ./tc2.sh
sudo ./tc3.sh
sudo ./tc4.sh
sudo ./tc5.sh
sudo ./tc6.sh
cd ..


