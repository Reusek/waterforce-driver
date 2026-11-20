obj-m := gigabyte_waterforce.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

# Build with the same compiler as the kernel. Kernels built with clang
# (e.g. CachyOS) need LLVM=1, otherwise gcc gets clang-only flags.
ifneq ($(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config $(KDIR)/include/config/auto.conf),)
KMAKE_FLAGS += LLVM=1
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KMAKE_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KMAKE_FLAGS) clean

# Swap the stock module for this build until the next reboot (for testing)
load: all
	-sudo modprobe -r gigabyte_waterforce
	sudo insmod ./gigabyte_waterforce.ko

unload:
	-sudo rmmod gigabyte_waterforce

.PHONY: all clean load unload
