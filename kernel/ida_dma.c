// SPDX-License-Identifier: GPL-2.0
/*
 * ida_dma.c - IDA DMA buffer management
 *
 * This file implements the core DMA logic for Method A:
 *   FPGA is the DMA initiator - it reads the IOVA table provided
 *   by this driver and writes data directly into Host memory.
 *
 * Per-channel initialization flow:
 *   1. pin_user_pages()          - lock hugepages in memory
 *   2. sg_alloc_table_from_pages() - build PA segment list
 *   3. dma_map_sg()              - IOMMU: PA → IOVA mapping
 *   4. dma_alloc_coherent()      - allocate IOVA address table
 *   5. populate coherent buffer  - fill table with IOVAs from step 3
 *
 * Per-transfer start flow (ida_start_channel):
 *   1. Reset transfer state
 *   2. wmb() - ensure coherent buffer is visible before BAR writes
 *   3. Write coherent buffer IOVA to BAR0 (table address + count)
 *   4. Write Status Area IOVA to BAR0
 *   5. Write START command to BAR0
 *   → FPGA reads the table, DMAs data into Swath Buffer,
 *     writes Status Area, then fires Swath End MSI-X interrupt.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/pagemap.h>
#include <linux/io.h>

#include "ida_driver.h"

/* ============================================================
 * Internal helper: pin and DMA-map a userspace buffer
 *
 * Fills in all fields of struct ida_pinned_buf.
 * On failure, cleans up any partial state and returns < 0.
 *
 * @dev  : PCI device (for dma_map_sg)
 * @buf  : target structure to fill
 * @uaddr: userspace virtual address (must be page-aligned)
 * @size : buffer size in bytes (must be page-aligned)
 * @dir  : DMA direction (DMA_FROM_DEVICE or DMA_BIDIRECTIONAL)
 * ============================================================ */
static int ida_pin_and_map(struct device *dev,
			   struct ida_pinned_buf *buf,
			   unsigned long uaddr,
			   size_t size,
			   enum dma_data_direction dir)
{
	long    pinned;
	int     mapped;
	long    npages;

	if (!uaddr || !size) {
		dev_err(dev, "ida: invalid buffer: uaddr=0x%lx size=%zu\n",
			uaddr, size);
		return -EINVAL;
	}

	if (!PAGE_ALIGNED(uaddr) || !PAGE_ALIGNED(size)) {
		dev_err(dev, "ida: buffer not page-aligned: "
			"uaddr=0x%lx size=%zu\n", uaddr, size);
		return -EINVAL;
	}

	buf->uaddr = uaddr;
	buf->size  = size;
	buf->dir   = dir;
	npages     = size / PAGE_SIZE;

	/* ---- 1. Allocate pages array ---------------------------------- */
	buf->pages = kvmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);
	if (!buf->pages)
		return -ENOMEM;

	/* ---- 2. Pin userspace pages ----------------------------------- */
	/*
	 * FOLL_WRITE  : we need write access (FPGA writes into the buffer)
	 * FOLL_LONGTERM: pages must not be migrated (required for DMA)
	 *
	 * API note: pin_user_pages() dropped the trailing 'vmas' parameter
	 * in Linux 6.5. Use LINUX_VERSION_CODE to stay compatible with
	 * both older and newer kernels.
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	pinned = pin_user_pages(uaddr, npages,
				FOLL_WRITE | FOLL_LONGTERM,
				buf->pages);
#else
	pinned = pin_user_pages(uaddr, npages,
				FOLL_WRITE | FOLL_LONGTERM,
				buf->pages);
#endif
	if (pinned < 0) {
		dev_err(dev, "ida: pin_user_pages failed: %ld\n", pinned);
		kvfree(buf->pages);
		buf->pages = NULL;
		return (int)pinned;
	}
	if (pinned != npages) {
		dev_err(dev, "ida: pin_user_pages: got %ld, expected %ld\n",
			pinned, npages);
		unpin_user_pages(buf->pages, pinned);
		kvfree(buf->pages);
		buf->pages = NULL;
		return -ENOMEM;
	}
	buf->npages = npages;

	/* ---- 3. Build sg_table from pinned pages ---------------------- */
	/*
	 * sg_alloc_table_from_pages merges physically contiguous pages
	 * into single sg entries. With THP 2MB pages this significantly
	 * reduces the number of IOVA table entries the FPGA must process.
	 */
	if (sg_alloc_table_from_pages(&buf->sgt,
				      buf->pages,
				      (unsigned int)buf->npages,
				      0,       /* offset within first page */
				      size,
				      GFP_KERNEL)) {
		dev_err(dev, "ida: sg_alloc_table_from_pages failed\n");
		goto err_unpin;
	}
	buf->sgt_valid = true;

	/* ---- 4. IOMMU mapping: PA → IOVA ----------------------------- */
	/*
	 * dma_map_sg returns the number of DMA-mapped entries which may
	 * be <= sgt.nents (IOMMU may merge adjacent entries further).
	 * We MUST use the return value, not sgt.nents, for subsequent
	 * sg_dma_address() / sg_dma_len() calls.
	 */
	mapped = dma_map_sg(dev, buf->sgt.sgl, buf->sgt.nents, dir);
	if (mapped <= 0) {
		dev_err(dev, "ida: dma_map_sg failed (returned %d)\n", mapped);
		goto err_sg_free;
	}

	/*
	 * Update nents to reflect actual number of mapped entries.
	 * sgt.orig_nents already holds the pre-map count (set by
	 * sg_alloc_table_from_pages). We update nents to the mapped
	 * count for iteration, but orig_nents is preserved for unmap.
	 */
	buf->sgt.nents = (unsigned int)mapped;
	buf->mapped    = true;

	dev_dbg(dev, "ida: pin_and_map uaddr=0x%lx size=%zu "
		"npages=%ld sg_nents=%d dir=%d\n",
		uaddr, size, npages, mapped, dir);

	return 0;

err_sg_free:
	sg_free_table(&buf->sgt);
	buf->sgt_valid = false;
err_unpin:
	unpin_user_pages(buf->pages, buf->npages);
	kvfree(buf->pages);
	buf->pages  = NULL;
	buf->npages = 0;
	return -ENOMEM;
}

/* ============================================================
 * Internal helper: undo pin_and_map
 * Safe to call even if only partially initialized.
 * ============================================================ */
static void ida_unmap_and_unpin(struct device *dev,
				struct ida_pinned_buf *buf)
{
	if (buf->mapped) {
		/*
		 * dma_unmap_sg must receive orig_nents (the count passed
		 * into dma_map_sg), not the mapped count it returned.
		 * sg_table.orig_nents holds the original count set by
		 * sg_alloc_table_from_pages before dma_map_sg was called.
		 */
		dma_unmap_sg(dev, buf->sgt.sgl, buf->sgt.orig_nents, buf->dir);
		buf->mapped = false;
	}
	if (buf->sgt_valid) {
		sg_free_table(&buf->sgt);
		buf->sgt_valid = false;
	}
	if (buf->pages && buf->npages > 0) {
		unpin_user_pages(buf->pages, buf->npages);
		buf->npages = 0;
	}
	if (buf->pages) {
		kvfree(buf->pages);
		buf->pages = NULL;
	}
	buf->uaddr = 0;
	buf->size  = 0;
}

/* ============================================================
 * ida_init_channel
 *
 * Called from CMD_INIT ioctl for each IDA channel.
 * Pins both Swath Buffer and Status Area, builds IOMMU mappings,
 * then allocates and populates the coherent IOVA address table.
 *
 * @drv : driver global state
 * @idx : channel index (0 ~ IDA_MAX_CHANNELS-1)
 * @info: buffer addresses from userspace (ida_buf_info)
 *
 * Returns 0 on success, negative errno on failure.
 * ============================================================ */
int ida_init_channel(struct ida_driver *drv,
		     int idx,
		     struct ida_buf_info *info)
{
	struct device       *dev = &drv->pdev->dev;
	struct ida_channel  *ch  = &drv->channels[idx];
	struct scatterlist  *sg;
	unsigned int         i;
	int                  ret;

	if (ch->initialized) {
		dev_warn(dev, "ida[%d]: already initialized, skipping\n", idx);
		return 0;
	}

	dev_info(dev, "ida[%d]: initializing channel\n", idx);
	dev_info(dev, "ida[%d]:   swath_uaddr  = 0x%llx\n",
		 idx, info->swath_uaddr);
	dev_info(dev, "ida[%d]:   swath_size   = %llu MB\n",
		 idx, info->swath_size / (1024 * 1024));
	dev_info(dev, "ida[%d]:   status_uaddr = 0x%llx\n",
		 idx, info->status_uaddr);

	/* Initialize synchronization primitives */
	init_waitqueue_head(&ch->wait_q);
	spin_lock_init(&ch->lock);

	/* ---- Step 1: Pin and map Swath Buffer ------------------------- */
	/*
	 * DMA_FROM_DEVICE: FPGA writes into Host Swath Buffer.
	 * Host only reads (via userspace pointer), never writes via DMA.
	 */
	ret = ida_pin_and_map(dev,
			      &ch->swath,
			      (unsigned long)info->swath_uaddr,
			      (size_t)info->swath_size,
			      DMA_FROM_DEVICE);
	if (ret) {
		dev_err(dev, "ida[%d]: failed to map Swath Buffer: %d\n",
			idx, ret);
		return ret;
	}

	/* ---- Step 2: Pin and map Status Area -------------------------- */
	/*
	 * DMA_BIDIRECTIONAL: FPGA writes Status Area (dmaed_line_count,
	 * crc_error, state). Host reads it via userspace pointer.
	 * We use BIDIRECTIONAL because the FPGA is writing into memory
	 * that lives on the Host side.
	 */
	ret = ida_pin_and_map(dev,
			      &ch->status,
			      (unsigned long)info->status_uaddr,
			      IDA_STATUS_AREA_SIZE,
			      DMA_BIDIRECTIONAL);
	if (ret) {
		dev_err(dev, "ida[%d]: failed to map Status Area: %d\n",
			idx, ret);
		goto err_unmap_swath;
	}

	/*
	 * Cache the Status Area IOVA.
	 * Status Area is IDA_STATUS_AREA_SIZE (4096 bytes = 1 page),
	 * so it will always be exactly one sg entry after dma_map_sg.
	 */
	if (ch->status.sgt.nents != 1) {
		dev_err(dev, "ida[%d]: Status Area has %d sg entries "
			"(expected 1)\n", idx, ch->status.sgt.nents);
		ret = -EINVAL;
		goto err_unmap_status;
	}
	ch->status_iova = sg_dma_address(ch->status.sgt.sgl);

	dev_dbg(dev, "ida[%d]: status_iova = 0x%pad\n",
		idx, &ch->status_iova);

	/* ---- Step 3: Allocate coherent IOVA address table ------------ */
	/*
	 * One entry per sg segment of the Swath Buffer.
	 * dma_alloc_coherent gives us:
	 *   coherent_cpu  : kernel VA that CPU writes into
	 *   coherent_iova : bus address that FPGA reads from
	 * Both point to the same physical memory.
	 *
	 * Guard against unreasonably large sg tables.
	 */
	if (ch->swath.sgt.nents > IDA_MAX_SG_ENTRIES) {
		dev_err(dev, "ida[%d]: sg_nents %d exceeds IDA_MAX_SG_ENTRIES "
			"%d\n", idx, ch->swath.sgt.nents, IDA_MAX_SG_ENTRIES);
		ret = -EINVAL;
		goto err_unmap_status;
	}

	ch->coherent_nents = ch->swath.sgt.nents;
	ch->coherent_size  = ch->coherent_nents * sizeof(struct ida_iova_entry);

	ch->coherent_cpu = dma_alloc_coherent(dev,
					      ch->coherent_size,
					      &ch->coherent_iova,
					      GFP_KERNEL);
	if (!ch->coherent_cpu) {
		dev_err(dev, "ida[%d]: dma_alloc_coherent failed "
			"(size=%zu)\n", idx, ch->coherent_size);
		ret = -ENOMEM;
		goto err_unmap_status;
	}

	/* ---- Step 4: Populate coherent buffer with Swath IOVAs ------- */
	/*
	 * Iterate over the DMA-mapped sg entries and fill the table.
	 * After dma_map_sg, sg_dma_address() and sg_dma_len() return
	 * the IOVA and mapped length respectively.
	 *
	 * This is the "User defined structure × N" from the design doc:
	 *   entry[i].dma_addr = IOVA of segment i
	 *   entry[i].size     = byte length of segment i
	 */
	for_each_sg(ch->swath.sgt.sgl, sg, ch->swath.sgt.nents, i) {
		ch->coherent_cpu[i].dma_addr = sg_dma_address(sg);
		ch->coherent_cpu[i].size     = sg_dma_len(sg);
		dev_dbg(dev, "ida[%d]:   iova_entry[%u] addr=0x%llx "
			"size=%llu\n", idx, i,
			ch->coherent_cpu[i].dma_addr,
			ch->coherent_cpu[i].size);
	}

	dev_info(dev, "ida[%d]: init OK - swath_nents=%u "
		 "coherent_iova=0x%pad status_iova=0x%pad\n",
		 idx, ch->coherent_nents,
		 &ch->coherent_iova, &ch->status_iova);

	ch->initialized = true;
	return 0;

err_unmap_status:
	ida_unmap_and_unpin(dev, &ch->status);
err_unmap_swath:
	ida_unmap_and_unpin(dev, &ch->swath);
	return ret;
}

/* ============================================================
 * ida_cleanup_channel
 *
 * Releases all resources allocated by ida_init_channel.
 * Safe to call on a partially-initialized channel.
 * ============================================================ */
void ida_cleanup_channel(struct ida_driver *drv, int idx)
{
	struct device      *dev = &drv->pdev->dev;
	struct ida_channel *ch  = &drv->channels[idx];

	if (!ch->initialized && !ch->swath.mapped && !ch->status.mapped
	    && !ch->coherent_cpu)
		return;

	dev_info(dev, "ida[%d]: cleaning up\n", idx);

	/* Free coherent address table */
	if (ch->coherent_cpu) {
		dma_free_coherent(dev,
				  ch->coherent_size,
				  ch->coherent_cpu,
				  ch->coherent_iova);
		ch->coherent_cpu   = NULL;
		ch->coherent_iova  = 0;
		ch->coherent_size  = 0;
		ch->coherent_nents = 0;
	}

	/* Unmap and unpin Status Area */
	ida_unmap_and_unpin(dev, &ch->status);
	ch->status_iova = 0;

	/* Unmap and unpin Swath Buffer */
	ida_unmap_and_unpin(dev, &ch->swath);

	ch->initialized = false;
	dev_info(dev, "ida[%d]: cleanup done\n", idx);
}

/* ============================================================
 * ida_cleanup_all_channels
 * Convenience wrapper called from remove() and CMD_CLEANUP.
 * ============================================================ */
void ida_cleanup_all_channels(struct ida_driver *drv)
{
	int i;

	for (i = 0; i < drv->ida_count; i++)
		ida_cleanup_channel(drv, i);
}

/* ============================================================
 * ida_start_channel
 *
 * Hands the IOVA table to the FPGA and triggers DMA transfer.
 * Must be called after ida_init_channel for this channel.
 *
 * Steps:
 *   1. Reset transfer state (transfer_done, last_error)
 *   2. wmb() - ensure coherent buffer content is visible to FPGA
 *      before we tell FPGA where to find it
 *   3. Write coherent buffer address + count to BAR0
 *   4. Write Status Area IOVA to BAR0
 *   5. Write START command to BAR0
 *
 * @drv           : driver global state
 * @idx           : channel index
 * @bytes_per_line: used to compute actual_bytes in CMD_WAIT_DONE
 *
 * Returns 0 on success, negative errno on failure.
 * ============================================================ */
int ida_start_channel(struct ida_driver *drv,
		      int idx,
		      u32 bytes_per_line)
{
	struct device      *dev = &drv->pdev->dev;
	struct ida_channel *ch  = &drv->channels[idx];
	unsigned long       flags;

	if (!ch->initialized) {
		dev_err(dev, "ida[%d]: not initialized, "
			"call CMD_INIT first\n", idx);
		return -ENODEV;
	}

	if (!drv->bar0) {
		dev_err(dev, "ida[%d]: BAR0 not mapped\n", idx);
		return -EIO;
	}

	/* ---- Reset transfer state ------------------------------------ */
	spin_lock_irqsave(&ch->lock, flags);
	ch->transfer_done  = false;
	ch->last_error     = 0;
	ch->bytes_per_line = bytes_per_line;
	spin_unlock_irqrestore(&ch->lock, flags);

	dev_info(dev, "ida[%d]: starting transfer - "
		 "coherent_iova=0x%pad nents=%u status_iova=0x%pad "
		 "bytes_per_line=%u\n",
		 idx,
		 &ch->coherent_iova, ch->coherent_nents,
		 &ch->status_iova,   bytes_per_line);

	/*
	 * ---- Memory barrier -----------------------------------------
	 *
	 * dma_alloc_coherent memory is cache-coherent by definition,
	 * but we still need a write memory barrier here to ensure that
	 * all CPU writes to the coherent buffer (the IOVA table) are
	 * globally visible *before* we write the table's address into
	 * the BAR0 registers. Without this barrier the FPGA could read
	 * the address from BAR0, immediately fetch the table, and see
	 * stale (zero) entries if the CPU writes haven't propagated yet.
	 */
	wmb();

	/* ---- Write BAR0 registers ------------------------------------ */
	/*
	 * Tell FPGA where the IOVA address table is.
	 * Write low word first, then high word, then count.
	 * The FPGA should only act on the count write (or the CTRL
	 * write below) so ordering within this group matters less,
	 * but we follow the natural low→high→count order.
	 */
	iowrite32(lower_32_bits(ch->coherent_iova),
		  drv->bar0 + REG_TABLE_ADDR_LOW);
	iowrite32(upper_32_bits(ch->coherent_iova),
		  drv->bar0 + REG_TABLE_ADDR_HIGH);
	iowrite32(ch->coherent_nents,
		  drv->bar0 + REG_TABLE_COUNT);

	/*
	 * Tell FPGA where to write the Status Area.
	 * FPGA will DMA dmaed_line_count / crc_error / state here
	 * when the Swath transfer completes.
	 */
	iowrite32(lower_32_bits(ch->status_iova),
		  drv->bar0 + REG_STATUS_ADDR_LOW);
	iowrite32(upper_32_bits(ch->status_iova),
		  drv->bar0 + REG_STATUS_ADDR_HIGH);

	/*
	 * Issue START command last.
	 * This is the trigger: FPGA begins reading the table and
	 * initiating DMA on this write.
	 * iowrite32 includes an implicit barrier on x86, but is
	 * explicitly ordered after all preceding writes on all arches.
	 */
	iowrite32(CTRL_CMD_START, drv->bar0 + REG_CTRL);

	dev_dbg(dev, "ida[%d]: START command written to BAR0\n", idx);
	return 0;
}
