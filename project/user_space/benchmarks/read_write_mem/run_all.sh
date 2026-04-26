#!/bin/bash

set -e

echo "memory performance testing started"
./mem_bench.sh
echo "Done!, look into mem_reports.csv for data points"

echo "read performance testing started"
./read_bench.sh
echo "finished!, llok into read_report.csv"


echo "write performance testing started"
./write_bench.sh
echo "finished!, look into write_report.csv"

echo "plotting the results obtained"
python3 plot.py
echo "plots generated, look into the plots folder for the generated plots"
