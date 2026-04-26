obj-m += scanner.o
obj-m += pagecache_inspector.o

ccflags-y := -std=gnu11 -Wno-declaration-after-statement -Wno-error 
KVERSION = $(shell uname -r)

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
