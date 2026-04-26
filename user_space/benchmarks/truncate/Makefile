CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11

all: gen_files

gen_files: gen_files.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f gen_files truncate_report.csv
	rm -rf /tmp/bench_truncate

.PHONY: run
run: gen_files
	chmod +x bench_truncate.sh
	./bench_truncate.sh

.PHONY: smoke
smoke: gen_files
	# Quick run with one combination for sanity.
	@SIZES="64" SHARES="30" GROUPS_LIST="2" RUNS=1 ./bench_truncate.sh