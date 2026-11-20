obj-m := gigabyte_waterforce.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

# Build with the same compiler as the kernel. Kernels built with clang
# (e.g. CachyOS) need LLVM=1, otherwise gcc gets clang-only flags.
ifneq ($(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config $(KDIR)/include/config/auto.conf),)
KMAKE_FLAGS += LLVM=1
endif

DKMS_NAME := gigabyte_waterforce
DKMS_VER := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(PWD)/dkms.conf 2>/dev/null)
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VER)
DKMS_FILES := gigabyte_waterforce.c Makefile dkms.conf
SUDO := $(if $(filter 0,$(shell id -u)),,sudo)

# Versions registered with DKMS, from lines like
# "gigabyte_waterforce/1.3.0, 7.3.4-1-cachyos, x86_64: installed"
# (older DKMS prints "gigabyte_waterforce, 1.3.0, ...")
installed_versions = $(shell dkms status $(DKMS_NAME) 2>/dev/null | \
	sed -nE 's/^$(DKMS_NAME)[/,] *([^,:]+).*/\1/p' | sort -u)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KMAKE_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KMAKE_FLAGS) clean

# Swap the stock module for this build until the next reboot (for testing)
load: all
	-$(SUDO) modprobe -r gigabyte_waterforce || $(SUDO) rmmod gigabyte_waterforce
	$(SUDO) insmod ./gigabyte_waterforce.ko

unload:
	-$(SUDO) rmmod gigabyte_waterforce

# Remove any DKMS-installed version, then install this one with DKMS so it is
# rebuilt for every new kernel, and load it right away.
install: remove-installed
	@command -v dkms >/dev/null || { echo "dkms is not installed (sudo pacman -S dkms)"; exit 1; }
	@test -n "$(DKMS_VER)" || { echo "no PACKAGE_VERSION in dkms.conf"; exit 1; }
	@echo "== installing $(DKMS_NAME) $(DKMS_VER)"
	$(SUDO) mkdir -p $(DKMS_SRC)
	$(SUDO) cp $(DKMS_FILES) $(DKMS_SRC)/
	$(SUDO) dkms install $(DKMS_NAME)/$(DKMS_VER)
	@$(MAKE) --no-print-directory reload

# Remove every DKMS-installed version; the stock driver is used again
uninstall: remove-installed
	@$(MAKE) --no-print-directory reload

remove-installed:
	@for v in $(installed_versions); do \
		echo "== removing $(DKMS_NAME) $$v"; \
		$(SUDO) dkms remove $(DKMS_NAME)/$$v --all || exit 1; \
		$(SUDO) rm -rf /usr/src/$(DKMS_NAME)-$$v; \
	done

# Load whichever module modprobe now finds and let CoolerControl pick it up
reload:
	-$(SUDO) modprobe -r gigabyte_waterforce || $(SUDO) rmmod gigabyte_waterforce
	$(SUDO) modprobe gigabyte_waterforce
	@echo "== loaded: $$(cat /sys/module/gigabyte_waterforce/version 2>/dev/null || echo 'stock driver')"
	@if systemctl is-active -q coolercontrold; then \
		$(SUDO) systemctl restart coolercontrold && echo "== coolercontrold restarted"; \
	fi

.PHONY: all clean load unload install uninstall remove-installed reload
