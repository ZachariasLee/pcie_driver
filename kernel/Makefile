# SPDX-License-Identifier: GPL-2.0
#
# Makefile for IDA PCIe DMA kernel driver
#
# Directory layout (inside Xilinx dma_ip_drivers repo):
#
#   dma_ip_drivers/
#   └── QDMA/
#       └── linux-kernel/
#           └── ida/
#               ├── include/
#               │   └── ida_uapi.h
#               └── kernel/          ← this Makefile lives here
#                   ├── Makefile
#                   ├── ida_hw.h
#                   ├── ida_driver.h
#                   ├── ida_driver.c
#                   └── ida_dma.c
#
# The driver does NOT depend on libqdma or any Xilinx-specific
# kernel library. It only uses standard Linux kernel APIs:
#   - PCI / DMA API (pci_*, dma_*)
#   - IOMMU (dma_map_sg, pin_user_pages)
#   - MSI-X (pci_alloc_irq_vectors, request_irq)
#   - misc device (misc_register)
#
# Usage:
#   make              - build against running kernel
#   make KDIR=<path>  - build against specific kernel source tree
#   make clean        - remove build artifacts
#   make help         - show this message
#
# Example (from this directory):
#   make
#   sudo insmod ida.ko
#   sudo dmesg | tail -20

# ---------------------------------------------------------------
# Kernel build directory - default to running kernel
# ---------------------------------------------------------------
KDIR ?= /lib/modules/$(shell uname -r)/build

# ---------------------------------------------------------------
# Module definition
# ---------------------------------------------------------------
MODULE_NAME := ida

obj-m += $(MODULE_NAME).o

# Source files compiled into the module
# ida_dma.c  : DMA buffer management (pin, map, coherent table)
# ida_driver.c: probe/remove/ioctl/ISR
$(MODULE_NAME)-objs := \
	ida_driver.o \
	ida_dma.o

# ---------------------------------------------------------------
# Include path for ida_uapi.h (shared with userspace app)
# ---------------------------------------------------------------
# $(src) is set by the kernel build system to the directory
# containing this Makefile (absolute path, safe for out-of-tree)
ccflags-y += -I$(src)/../include

# ---------------------------------------------------------------
# Optional: enable pr_debug() output
# Uncomment for verbose debug logging via dmesg
# ---------------------------------------------------------------
# ccflags-y += -DDEBUG

# ---------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------
.PHONY: all clean help

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

help:
	@echo "Targets:"
	@echo "  all     - build ida.ko (default)"
	@echo "  clean   - remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR    - kernel build dir (default: running kernel)"
	@echo ""
	@echo "Example:"
	@echo "  make KDIR=/lib/modules/\$$(uname -r)/build"
	@echo "  sudo insmod ida.ko"
