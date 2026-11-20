obj-m := gigabyte_waterforce.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Swap the stock module for this build until the next reboot (for testing)
load: all
	-sudo modprobe -r gigabyte_waterforce
	sudo insmod ./gigabyte_waterforce.ko

unload:
	-sudo rmmod gigabyte_waterforce

.PHONY: all clean load unload
