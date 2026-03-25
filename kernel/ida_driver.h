/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ida_driver.h - IDA kernel driver internal data structures
 *
 * Kernel-only header. Not included by userspace.
 *
 * Data ownership model:
 *
 *   Swath Buffer:
 *     - Allocated in userspace under /dev/shm (THP 2MB hugepage)
 *     - Pinned by kernel: pin_user_pages()
 *     - IOMMU-mapped: dma_map_sg(DMA_FROM_DEVICE)
 *       FPGA writes into it, Host reads it (zero-copy)
 *
 *   Status Area:
 *     - Allocated in userspace under /dev/shm (single page)
 *     - Pinned by kernel: pin_user_pages()
 *     - IOMMU-mapped: dma_map_sg(DMA_BIDIRECTIONAL)
 *       FPGA writes dmaed_line_count / crc_error / state
 *       Host reads it directly via userspace pointer (zero-copy)
 *
 *   Coherent buffer (address table):
 *     - Allocated by kernel: dma_alloc_coherent()
 *     - CPU writes IOVAs into it (coherent_cpu)
 *     - FPGA reads it via coherent_iova to know where to DMA
 *     - Format: array of ida_iova_entry, one per sg segment
 */

#ifndef _IDA_DRIVER_H
#define _IDA_DRIVER_H

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/wait.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/mm_types.h>

#include "ida_hw.h"
#include "../include/ida_uapi.h"

/* ============================================================
 * Coherent buffer entry format
 *
 * This is the "User defined structure" from the design doc.
 * The kernel fills an array of these and hands the array's
 * IOVA to the FPGA via BAR0 registers.
 * FPGA reads each entry and DMA-writes data to dma_addr.
 * ============================================================ */
struct ida_iova_entry {
	__u64  dma_addr;    /* IOVA of one sg segment of Swath Buffer     */
	__u64  size;        /* length of that segment in bytes            */
};

/* ============================================================
 * Per-channel pinned buffer state
 * Shared structure used for both Swath Buffer and Status Area.
 * ============================================================ */
struct ida_pinned_buf {
	/* userspace address passed in from App */
	unsigned long        uaddr;

	/* size in bytes */
	size_t               size;

	/* pinned pages */
	struct page        **pages;
	long                 npages;

	/* Linux sg_table (PA segments, then IOVA after dma_map_sg) */
	struct sg_table      sgt;
	bool                 sgt_valid;

	/* DMA direction used when mapping */
	enum dma_data_direction dir;

	/* true after dma_map_sg succeeds */
	bool                 mapped;
};

/* ============================================================
 * Per-IDA-channel state
 * ============================================================ */
struct ida_channel {
	/* ---- userspace buffers (pinned and mapped) ---- */
	struct ida_pinned_buf  swath;     /* Swath Buffer (10 GB)           */
	struct ida_pinned_buf  status;    /* Status Area (4096 B)           */

	/*
	 * Status Area IOVA.
	 * Status Area is typically a single page so sg has 1 entry.
	 * Cached here for fast access during CMD_START.
	 */
	dma_addr_t             status_iova;

	/* ---- coherent address table (kernel-allocated) ---- */
	/*
	 * Array of ida_iova_entry written by CPU, read by FPGA.
	 * coherent_cpu  : kernel virtual address (for CPU writes)
	 * coherent_iova : bus/IOVA address       (written to BAR0)
	 * coherent_size : allocation size in bytes
	 * coherent_nents: number of valid entries
	 */
	struct ida_iova_entry *coherent_cpu;
	dma_addr_t             coherent_iova;
	size_t                 coherent_size;
	unsigned int           coherent_nents;

	/* ---- transfer runtime state ---- */
	/*
	 * Set to true by Swath End ISR, cleared at CMD_START.
	 * App waits on wait_q until this becomes true.
	 */
	bool                   transfer_done;

	/*
	 * bytes_per_line supplied by App in CMD_START.
	 * Used by kernel to compute actual_bytes in CMD_WAIT_DONE.
	 */
	u32                    bytes_per_line;

	/*
	 * Error code from ISR or DMA fault.
	 * 0 = no error.
	 */
	int                    last_error;

	/* ---- synchronization ---- */
	wait_queue_head_t      wait_q;

	/*
	 * Protects: transfer_done, last_error
	 * Taken by ISR (irqsave) and ioctl (irqsave).
	 */
	spinlock_t             lock;

	/* ---- lifecycle flag ---- */
	bool                   initialized;
};

/* ============================================================
 * Driver global state
 * One instance per PCI function (PF).
 * ============================================================ */
struct ida_driver {
	struct pci_dev        *pdev;

	/* BAR0 mapped address (control registers) */
	void __iomem          *bar0;

	/*
	 * MSI-X: managed via pci_alloc_irq_vectors / pci_irq_vector.
	 * msix_nvecs: number of vectors actually allocated.
	 */
	int                    msix_nvecs;

	/* Per-channel state */
	struct ida_channel     channels[IDA_MAX_CHANNELS];

	/*
	 * Number of channels in use.
	 * Set from ida_init_param.ida_count in CMD_INIT.
	 */
	int                    ida_count;

	/*
	 * Index of the channel most recently started via CMD_START.
	 * Protected by current_lock.
	 */
	int                    current_ida;
	spinlock_t             current_lock;

	/* misc device: /dev/ida_dma */
	struct miscdevice      misc_dev;
};

/* ============================================================
 * Function declarations
 * ============================================================ */

/* ida_dma.c */
int  ida_init_channel(struct ida_driver *drv, int idx,
		      struct ida_buf_info *info);
void ida_cleanup_channel(struct ida_driver *drv, int idx);
void ida_cleanup_all_channels(struct ida_driver *drv);
int  ida_start_channel(struct ida_driver *drv, int idx,
		       u32 bytes_per_line);

#endif /* _IDA_DRIVER_H */
