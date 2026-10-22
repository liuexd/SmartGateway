
obj-m += sgw_ap3216.o

KDIR ?= $(HOME)/Desktop/Linux-4.9.88

ARCH := arm
CROSS_COMPILE := arm-linux-gnueabihf-

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		clean

.PHONY: all clean
