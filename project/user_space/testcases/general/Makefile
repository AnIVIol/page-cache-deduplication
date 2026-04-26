# Page cache deduplication benchmark suite.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-result -std=gnu11
LDFLAGS ?=

BENCHES := bench_dedup_ratio bench_throughput bench_multifile bench_stress

all: generate $(BENCHES)

# The generator is unmodified — built from the file you provided.
generate: generate.c
	$(CC) $(CFLAGS) -o $@ $<

# Each benchmark is a single .c file that pulls in bench_common.h
$(BENCHES): %: %.c bench_common.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f generate $(BENCHES) output.dat /tmp/bench_*.dat

# Quick sanity run (requires root for sysfs + drop_caches)
.PHONY: smoke
smoke: all
	sudo ./bench_dedup_ratio 64 30 2

.PHONY: full
full: all
	sudo ./bench_dedup_ratio  256 25 3
	sudo ./bench_throughput   128 30 2
	sudo ./bench_multifile    128 20 2 4
	sudo ./bench_stress       128 25 3 30