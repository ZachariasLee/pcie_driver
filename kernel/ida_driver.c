// SPDX-License-Identifier: GPL-2.0
/*
 * ida_driver.c - IDA PCIe DMA driver main module
 *
 * Handles:
 *   - PCI probe / remove
 *   - MSI-X interrupt setup and Swath End ISR
 *   - ioctl dispatch (CMD_INIT / CMD_START / CMD_WAIT_DONE / CMD_CLEANUP)
 *   - misc device registration (/dev/ida)
 *
 * Design: Method A - FPGA is the DMA initiator.
 *   The driver only prepares the IOVA table and hands it to the FPGA
 *   via BAR0 registers. The FPGA DMAs data into Host memory autonomously
 *   and signals completion via a Swath End MSI-X interrupt.
 *
 * ioctl flow:
 *   CMD_INIT      → ida_dma.c: pin + map + build coherent table
 *   CMD_START     → ida_dma.c: write BAR0, trigger FPGA
 *   CMD_WAIT_DONE → block on wait_q, read Status Area, return result
 *   CMD_CLEANUP   → ida_dma.c: unmap + unpin + free
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/highmem.h>   /* kmap_local_page / kunmap_local */
#include <linux/io.h>

#include "ida_driver.h"

/* ============================================================
 * Forward declarations
 * ============================================================ */
static long ida_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int  ida_open(struct inode *inode, struct file *filp);
static int  ida_release(struct inode *inode, struct file *filp);

static const struct file_operations ida_fops = {
	.owner          = THIS_MODULE,
	.open           = ida_open,
	.release        = ida_release,
	.unlocked_ioctl = ida_ioctl,
};

/* ============================================================
 * Helper: read Status Area from pinned kernel page
 *
 * Status Area lives in userspace (/dev/shm) and is pinned.
 * After Swath End interrupt fires, the kernel needs to read
 * dmaed_line_count / crc_error / state to fill ida_wait_result.
 *
 * We use kmap_local_page() to get a temporary kernel mapping
 * of the first pinned page, copy the status fields out, then
 * unmap. This avoids any data copy into kernel memory - we only
 * read what we need for the ioctl return value.
 *
 * Note: the FPGA may still be writing the Status Area when the
 * interrupt fires (race window). We add a read memory barrier
 * to ensure we see the FPGA's writes after the interrupt.
 * ============================================================ */
static void ida_read_status(struct ida_channel *ch,
			    struct ida_wait_result *result)
{
	struct ida_status_area *sa;
	struct page            *page;

	if (!ch->status.pages || ch->status.npages < 1) {
		pr_warn_once("ida: ida_read_status: status pages not available\n");
		return;
	}

	page = ch->status.pages[0];

	/*
	 * Read barrier: ensure all FPGA DMA writes to the Status Area
	 * are visible to the CPU before we read them. The MSI-X interrupt
	 * acts as a synchronization point but we add rmb() for safety.
	 */
	rmb();

	/*
	 * kmap_local_page: efficient per-CPU temporary mapping.
	 * Must not sleep or schedule between kmap and kunmap.
	 */
	sa = kmap_local_page(page);

	result->dmaed_line_count = sa->dmaed_line_count;
	result->crc_error        = sa->crc_error;
	result->state            = sa->state;

	kunmap_local(sa);
}

/* ============================================================
 * Swath End MSI-X interrupt service routine
 *
 * Fired by FPGA when a Swath transfer is complete.
 * At this point:
 *   - All Swath Buffer segments have been DMA-written by FPGA
 *   - Status Area has been DMA-written by FPGA
 *     (dmaed_line_count, crc_error, state are valid)
 *
 * We only set transfer_done and wake up the App.
 * The App reads Swath Buffer and Status Area directly (zero-copy).
 * The kernel reads Status Area once in CMD_WAIT_DONE to fill result.
 * ============================================================ */
static irqreturn_t ida_swath_end_isr(int irq, void *data)
{
	struct ida_driver  *drv = (struct ida_driver *)data;
	struct ida_channel *ch;
	unsigned long       flags;
	int                 idx;

	spin_lock_irqsave(&drv->current_lock, flags);
	idx = drv->current_ida;
	spin_unlock_irqrestore(&drv->current_lock, flags);

	if (idx < 0 || idx >= drv->ida_count) {
		dev_warn(&drv->pdev->dev,
			 "ida: swath_end_isr with invalid current_ida=%d\n",
			 idx);
		return IRQ_HANDLED;
	}

	ch = &drv->channels[idx];

	spin_lock_irqsave(&ch->lock, flags);
	ch->transfer_done = true;
	spin_unlock_irqrestore(&ch->lock, flags);

	wake_up(&ch->wait_q);

	dev_dbg(&drv->pdev->dev,
		"ida[%d]: Swath End interrupt received\n", idx);

	return IRQ_HANDLED;
}

/* ============================================================
 * ioctl: CMD_INIT
 *
 * Pin and DMA-map all channel buffers. Builds IOVA table.
 * Must be called once before any CMD_START.
 * ============================================================ */
static long ida_ioctl_init(struct ida_driver *drv, unsigned long arg)
{
	struct ida_init_param  param;
	int                    i, ret;

	if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
		return -EFAULT;

	if (param.ida_count == 0 || param.ida_count > IDA_MAX_CHANNELS) {
		dev_err(&drv->pdev->dev,
			"ida: CMD_INIT invalid ida_count=%u\n",
			param.ida_count);
		return -EINVAL;
	}

	/* Store channel count - used by cleanup and iteration */
	drv->ida_count = (int)param.ida_count;

	for (i = 0; i < drv->ida_count; i++) {
		ret = ida_init_channel(drv, i, &param.bufs[i]);
		if (ret) {
			dev_err(&drv->pdev->dev,
				"ida: CMD_INIT failed at channel %d: %d\n",
				i, ret);
			/* Clean up already-initialized channels */
			while (--i >= 0)
				ida_cleanup_channel(drv, i);
			drv->ida_count = 0;
			return ret;
		}
	}

	dev_info(&drv->pdev->dev,
		 "ida: CMD_INIT done, %d channel(s) ready\n",
		 drv->ida_count);
	return 0;
}

/* ============================================================
 * ioctl: CMD_START
 *
 * Write IOVA table address to BAR0, trigger FPGA DMA.
 * current_ida is protected by current_lock (spinlock).
 * This driver assumes a single-threaded App (one CMD_START
 * active at a time). Concurrent CMD_START from multiple threads
 * is not supported and will corrupt current_ida.
 * ============================================================ */
static long ida_ioctl_start(struct ida_driver *drv, unsigned long arg)
{
	struct ida_start_param param;
	unsigned long          flags;
	int                    ret;

	if (copy_from_user(&param, (void __user *)arg, sizeof(param)))
		return -EFAULT;

	if ((int)param.ida_index >= drv->ida_count) {
		dev_err(&drv->pdev->dev,
			"ida: CMD_START invalid ida_index=%u (count=%d)\n",
			param.ida_index, drv->ida_count);
		return -EINVAL;
	}

	if (param.bytes_per_line == 0) {
		dev_err(&drv->pdev->dev,
			"ida: CMD_START bytes_per_line must be > 0\n");
		return -EINVAL;
	}

	/* Record which channel is active before triggering FPGA */
	spin_lock_irqsave(&drv->current_lock, flags);
	drv->current_ida = (int)param.ida_index;
	spin_unlock_irqrestore(&drv->current_lock, flags);

	ret = ida_start_channel(drv, (int)param.ida_index,
				param.bytes_per_line);
	if (ret) {
		/* Reset current_ida if start failed */
		spin_lock_irqsave(&drv->current_lock, flags);
		drv->current_ida = -1;
		spin_unlock_irqrestore(&drv->current_lock, flags);
	}

	return ret;
}

/* ============================================================
 * ioctl: CMD_WAIT_DONE
 *
 * Block until Swath End interrupt fires for the active channel.
 * Then read Status Area and fill ida_wait_result for the App.
 *
 * The App also has direct (zero-copy) access to the Status Area
 * via its /dev/shm mapping, but CMD_WAIT_DONE returns the key
 * fields so the App doesn't need to parse the struct itself.
 * ============================================================ */
static long ida_ioctl_wait_done(struct ida_driver *drv, unsigned long arg)
{
	struct ida_wait_result result;
	struct ida_channel    *ch;
	unsigned long          flags;
	int                    idx, ret;

	spin_lock_irqsave(&drv->current_lock, flags);
	idx = drv->current_ida;
	spin_unlock_irqrestore(&drv->current_lock, flags);

	if (idx < 0 || idx >= drv->ida_count) {
		dev_err(&drv->pdev->dev,
			"ida: CMD_WAIT_DONE: no active channel "
			"(call CMD_START first)\n");
		return -ENODEV;
	}

	ch = &drv->channels[idx];

	/*
	 * Block until Swath End ISR sets transfer_done.
	 * Interruptible: allows SIGINT / SIGTERM to unblock.
	 */
	ret = wait_event_interruptible(ch->wait_q, ch->transfer_done);
	if (ret) {
		/* Interrupted by signal - App can retry */
		dev_dbg(&drv->pdev->dev,
			"ida[%d]: CMD_WAIT_DONE interrupted: %d\n", idx, ret);
		return ret;
	}

	/* Build result for userspace */
	memset(&result, 0, sizeof(result));

	/*
	 * Read Status Area fields via temporary kernel mapping.
	 * rmb() inside ida_read_status() ensures we see FPGA writes.
	 */
	ida_read_status(ch, &result);

	/*
	 * Compute actual_bytes from line count and bytes_per_line.
	 * bytes_per_line was supplied by App in CMD_START.
	 */
	if (result.dmaed_line_count > 0 && ch->bytes_per_line > 0) {
		result.actual_bytes = (u64)result.dmaed_line_count
				      * ch->bytes_per_line;
	}

	result.error = ch->last_error;

	if (copy_to_user((void __user *)arg, &result, sizeof(result)))
		return -EFAULT;

	dev_info(&drv->pdev->dev,
		 "ida[%d]: CMD_WAIT_DONE - lines=%lld bytes=%llu "
		 "crc=%d state=%d err=%d\n",
		 idx,
		 result.dmaed_line_count,
		 result.actual_bytes,
		 result.crc_error,
		 result.state,
		 result.error);

	return 0;
}

/* ============================================================
 * ioctl: CMD_CLEANUP
 *
 * Unpin pages, unmap IOMMU, free coherent buffers.
 * Should be called by App on exit or before re-initializing.
 * ============================================================ */
static long ida_ioctl_cleanup(struct ida_driver *drv)
{
	unsigned long flags;

	ida_cleanup_all_channels(drv);

	spin_lock_irqsave(&drv->current_lock, flags);
	drv->current_ida = -1;
	spin_unlock_irqrestore(&drv->current_lock, flags);

	drv->ida_count = 0;

	dev_info(&drv->pdev->dev, "ida: CMD_CLEANUP done\n");
	return 0;
}

/* ============================================================
 * ioctl dispatcher
 * ============================================================ */
static long ida_ioctl(struct file *filp,
		      unsigned int cmd,
		      unsigned long arg)
{
	struct ida_driver *drv = container_of(filp->private_data,
					      struct ida_driver,
					      misc_dev);

	switch (cmd) {
	case CMD_INIT:
		return ida_ioctl_init(drv, arg);
	case CMD_START:
		return ida_ioctl_start(drv, arg);
	case CMD_WAIT_DONE:
		return ida_ioctl_wait_done(drv, arg);
	case CMD_CLEANUP:
		return ida_ioctl_cleanup(drv);
	default:
		return -ENOTTY;
	}
}

static int ida_open(struct inode *inode, struct file *filp)
{
	/* private_data set by misc framework to point to miscdevice */
	return 0;
}

static int ida_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* ============================================================
 * MSI-X setup
 *
 * Uses pci_alloc_irq_vectors (modern API) to request MSI-X
 * vectors and pci_irq_vector to obtain the Linux IRQ number.
 * ============================================================ */
static int ida_setup_msix(struct ida_driver *drv)
{
	struct pci_dev *pdev = drv->pdev;
	int             nvecs, irq, ret;

	nvecs = pci_alloc_irq_vectors(pdev,
				      IDA_MSIX_VECTORS_MIN,
				      IDA_MSIX_VECTORS_MAX,
				      PCI_IRQ_MSIX);
	if (nvecs < 0) {
		dev_err(&pdev->dev,
			"ida: pci_alloc_irq_vectors failed: %d\n", nvecs);
		return nvecs;
	}

	drv->msix_nvecs = nvecs;
	dev_info(&pdev->dev, "ida: allocated %d MSI-X vector(s)\n", nvecs);

	/* Register Swath End ISR on vector 0 */
	irq = pci_irq_vector(pdev, IDA_MSIX_SWATH_END_VECTOR);
	ret = request_irq(irq, ida_swath_end_isr, 0, "ida_swath_end", drv);
	if (ret) {
		dev_err(&pdev->dev,
			"ida: request_irq failed for vector %d: %d\n",
			IDA_MSIX_SWATH_END_VECTOR, ret);
		pci_free_irq_vectors(pdev);
		drv->msix_nvecs = 0;
		return ret;
	}

	dev_info(&pdev->dev,
		 "ida: Swath End ISR registered on IRQ %d\n", irq);
	return 0;
}

static void ida_teardown_msix(struct ida_driver *drv)
{
	struct pci_dev *pdev = drv->pdev;
	int             irq;

	if (drv->msix_nvecs <= 0)
		return;

	irq = pci_irq_vector(pdev, IDA_MSIX_SWATH_END_VECTOR);
	free_irq(irq, drv);
	pci_free_irq_vectors(pdev);
	drv->msix_nvecs = 0;
}

/* ============================================================
 * PCI probe
 * ============================================================ */
static int ida_probe(struct pci_dev *pdev,
		     const struct pci_device_id *id)
{
	struct ida_driver *drv;
	int                ret;

	dev_info(&pdev->dev, "ida: probing %s\n", pci_name(pdev));

	/* Allocate driver state */
	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->pdev        = pdev;
	drv->current_ida = -1;
	spin_lock_init(&drv->current_lock);
	pci_set_drvdata(pdev, drv);

	/* Enable PCI device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "ida: pci_enable_device failed: %d\n",
			ret);
		return ret;
	}

	pci_set_master(pdev);

	/* Set 64-bit DMA mask */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_warn(&pdev->dev,
			 "ida: 64-bit DMA mask failed, trying 32-bit\n");
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev,
				"ida: no usable DMA mask\n");
			goto err_disable;
		}
	}

	/* Request and map BAR0 (control registers) */
	ret = pci_request_region(pdev, IDA_BAR_CTRL, "ida");
	if (ret) {
		dev_err(&pdev->dev,
			"ida: pci_request_region BAR%d failed: %d\n",
			IDA_BAR_CTRL, ret);
		goto err_disable;
	}

	drv->bar0 = pci_iomap(pdev, IDA_BAR_CTRL, 0);
	if (!drv->bar0) {
		dev_err(&pdev->dev,
			"ida: pci_iomap BAR%d failed\n", IDA_BAR_CTRL);
		ret = -ENOMEM;
		goto err_release_region;
	}

	/* Setup MSI-X interrupts */
	ret = ida_setup_msix(drv);
	if (ret)
		goto err_iounmap;

	/* Register misc device /dev/ida */
	drv->misc_dev.minor = MISC_DYNAMIC_MINOR;
	drv->misc_dev.name  = "ida_dma";   /* /dev/ida_dma */
	drv->misc_dev.fops  = &ida_fops;

	ret = misc_register(&drv->misc_dev);
	if (ret) {
		dev_err(&pdev->dev,
			"ida: misc_register failed: %d\n", ret);
		goto err_msix;
	}

	dev_info(&pdev->dev,
		 "ida: probe complete - /dev/%s ready\n",
		 drv->misc_dev.name);
	return 0;

err_msix:
	ida_teardown_msix(drv);
err_iounmap:
	pci_iounmap(pdev, drv->bar0);
err_release_region:
	pci_release_region(pdev, IDA_BAR_CTRL);
err_disable:
	pci_disable_device(pdev);
	return ret;
}

/* ============================================================
 * PCI remove
 * ============================================================ */
static void ida_remove(struct pci_dev *pdev)
{
	struct ida_driver *drv = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "ida: removing\n");

	misc_deregister(&drv->misc_dev);

	/* Clean up any channels that were initialized */
	if (drv->ida_count > 0)
		ida_cleanup_all_channels(drv);

	ida_teardown_msix(drv);

	pci_iounmap(pdev, drv->bar0);
	pci_release_region(pdev, IDA_BAR_CTRL);
	pci_disable_device(pdev);

	/* drv itself is devm-allocated, released automatically */
	dev_info(&pdev->dev, "ida: removed\n");
}

/* ============================================================
 * PCI device table
 * ============================================================ */
static const struct pci_device_id ida_pci_ids[] = {
	{ PCI_DEVICE(IDA_PCI_VENDOR_ID, IDA_PCI_DEVICE_ID_ZU7EG) },
	{ PCI_DEVICE(IDA_PCI_VENDOR_ID, IDA_PCI_DEVICE_ID_KU5P)  },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, ida_pci_ids);

static struct pci_driver ida_pci_driver = {
	.name     = "ida",
	.id_table = ida_pci_ids,
	.probe    = ida_probe,
	.remove   = ida_remove,
};

/* ============================================================
 * Module init / exit
 * ============================================================ */
static int __init ida_driver_init(void)
{
	pr_info("ida: loading IDA PCIe DMA driver\n");
	return pci_register_driver(&ida_pci_driver);
}

static void __exit ida_driver_exit(void)
{
	pci_unregister_driver(&ida_pci_driver);
	pr_info("ida: unloaded\n");
}

module_init(ida_driver_init);
module_exit(ida_driver_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("IDA Team");
MODULE_DESCRIPTION("IDA PCIe DMA driver - FPGA to Host via IOMMU IOVA table");
MODULE_VERSION("2.0.0");
