obj-m = pfns.o
obj-m += pfn_tracker.o
obj-m += scanner.o
obj-m += pfn_checker.o
obj-m += pagecache_inspector.o
obj-m += check_pfn.o

ccflags-y := -std=gnu11 -Wno-declaration-after-statement -Wno-error 
KVERSION = $(shell uname -r)

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
