KDIR := $(HOME)/Desktop/Linux-4.9.88

TOOLCHAIN := $(HOME)/Desktop/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin

ARCH := arm

CROSS_COMPILE := $(TOOLCHAIN)/arm-linux-gnueabihf-

obj-m := bsp_hello.o

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(CURDIR) clean