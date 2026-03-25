/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * ida_uapi.h - IDA Driver User/Kernel shared interface
 *
 * This file is included by both the kernel driver and the userspace
 * application. It defines:
 *   - Status Area memory layout (written by FPGA, read by Host)
 *   - ioctl commands
 *   - ioctl parameter structures
 *
 * FPGA data flow (Method A):
 *   Host prepares IOVA table → writes table address to BAR0
 *   FPGA reads table → DMA writes data into Host Swath Buffers
 *   FPGA writes Status Area → triggers Swath End MSI-X interrupt
 *   Host reads Swath Buffer and Status Area directly (zero-copy)
 */

#ifndef _IDA_UAPI_H
#define _IDA_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
/*
 * In userspace, __s64/__u64/__s32/__u32 come from <linux/types.h>
 * which is part of the kernel UAPI headers installed at
 * /usr/include/linux/types.h on Rocky Linux / RHEL systems.
 * This is the standard way to share kernel UAPI structs with userspace.
 */
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

/* ============================================================
 * Status Area layout
 *
 * Allocated in userspace under /dev/shm (THP hugepage).
 * Pinned and DMA-mapped by kernel driver (DMA_BIDIRECTIONAL).
 * FPGA writes this area via PCIe DMA using the IOVA provided
 * by the kernel driver through BAR0 registers.
 * Userspace reads it directly after Swath End interrupt.
 *
 * Total size: 4096 bytes (one page)
 * ============================================================ */
struct ida_status_area {
	/*
	 * Number of lines transferred in this Swath.
	 * Written by FPGA when Swath End interrupt fires.
	 * actual_bytes = dmaed_line_count * BYTES_PER_LINE
	 */
	__s64  dmaed_line_count;

	/*
	 * CRC error count detected by FPGA during this Swath.
	 * 0 means no errors.
	 */
	__s32  crc_error;

	/*
	 * FPGA state code.
	 *   -2 : DMA end (normal completion)
	 *   -3 : DMA end (early termination)
	 *   Other values: TBD with EE
	 */
	__s32  state;

	/* Reserved / pad to 4096 bytes */
	__u8   _pad[4096 - 16];
} __attribute__((packed));

/* ============================================================
 * ioctl parameter structures
 * ============================================================ */

/*
 * Per-channel buffer descriptor passed in CMD_INIT.
 * Userspace fills this with the virtual addresses of the
 * /dev/shm buffers it has already mmap'd.
 */
struct ida_buf_info {
	__u64  swath_uaddr;    /* userspace vaddr of Swath Buffer (10GB)  */
	__u64  swath_size;     /* size of Swath Buffer in bytes           */
	__u64  status_uaddr;   /* userspace vaddr of Status Area (4096 B) */
};

/*
 * CMD_INIT argument.
 * One ida_buf_info entry per IDA channel.
 * ida_count must match the number of channels the driver
 * was loaded with (kernel validates this).
 */
struct ida_init_param {
	__u32           ida_count;        /* number of valid entries in bufs[] */
	__u32           _pad;
	struct ida_buf_info bufs[4];      /* max IDA_MAX_CHANNELS = 4          */
};

/*
 * CMD_WAIT_DONE return value.
 * Kernel fills this after Swath End interrupt fires.
 * actual_bytes is computed by the kernel:
 *   actual_bytes = status_area.dmaed_line_count * bytes_per_line
 * bytes_per_line is passed in CMD_START so the kernel can compute it.
 */
struct ida_wait_result {
	__s64  dmaed_line_count;  /* copied from Status Area               */
	__u64  actual_bytes;      /* dmaed_line_count * bytes_per_line     */
	__s32  crc_error;         /* copied from Status Area               */
	__s32  state;             /* copied from Status Area               */
	__s32  error;             /* DMA/driver error code, 0 = success    */
	__u32  _pad;
};

/*
 * CMD_START argument.
 * Tells the kernel which IDA channel to start and provides
 * bytes_per_line so the kernel can compute actual_bytes.
 */
struct ida_start_param {
	__u32  ida_index;         /* channel index (0 ~ ida_count-1)       */
	__u32  bytes_per_line;    /* e.g. 3072 = 2048 * 1.5                */
};

/* ============================================================
 * ioctl commands
 *
 * CMD_INIT:
 *   Pin userspace hugepage buffers, build sg_tables, create
 *   IOMMU mappings, allocate coherent address table.
 *   Must be called once before any CMD_START.
 *   arg: struct ida_init_param *
 *
 * CMD_START:
 *   Write address table IOVA to BAR0, trigger FPGA to begin
 *   DMA transfer for the specified channel.
 *   arg: struct ida_start_param *
 *
 * CMD_WAIT_DONE:
 *   Block until Swath End MSI-X interrupt fires for the
 *   channel started by the most recent CMD_START.
 *   arg: struct ida_wait_result *  (filled by kernel on return)
 *
 * CMD_CLEANUP:
 *   Unpin pages, unmap IOMMU, free coherent buffers.
 *   Call before unloading the driver or on application exit.
 *   arg: none (pass 0)
 * ============================================================ */
#define IDA_IOC_MAGIC   'I'

#define CMD_INIT        _IOW(IDA_IOC_MAGIC,  1, struct ida_init_param)
#define CMD_START       _IOW(IDA_IOC_MAGIC,  2, struct ida_start_param)
#define CMD_WAIT_DONE   _IOR(IDA_IOC_MAGIC,  3, struct ida_wait_result)
#define CMD_CLEANUP     _IO(IDA_IOC_MAGIC,   4)

#endif /* _IDA_UAPI_H */
