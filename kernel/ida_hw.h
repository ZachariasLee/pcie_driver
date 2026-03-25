/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ida_hw.h - IDA hardware definitions (kernel only)
 *
 * Contains:
 *   - PCI Vendor/Device IDs
 *   - BAR definitions
 *   - BAR0 register offsets (provided by EE)
 *   - Hardware command values
 *
 * All offsets marked "TODO: confirm with EE" must be verified
 * against the FPGA register map before use.
 */

#ifndef _IDA_HW_H
#define _IDA_HW_H

/* ============================================================
 * PCI identifiers
 * TODO: confirm Device IDs with EE / board schematics
 * ============================================================ */
#define IDA_PCI_VENDOR_ID           0x10EE  /* Xilinx */
#define IDA_PCI_DEVICE_ID_ZU7EG     0x9038  /* xczu7eg dev board  - TODO */
#define IDA_PCI_DEVICE_ID_KU5P      0x9028  /* xcku5p production  - TODO */

/* ============================================================
 * BAR assignments
 * ============================================================ */
#define IDA_BAR_CTRL                0       /* BAR0: control registers    */
/* BAR2 (Status Area) is no longer used - Status Area is now in Host      */
/* memory (userspace /dev/shm), mapped via IOMMU and written by FPGA DMA  */

/* ============================================================
 * BAR0 register offsets
 *
 * The kernel driver writes these registers once per CMD_START
 * to hand the IOVA addresses to the FPGA so it can DMA data
 * into Host memory autonomously.
 *
 * Register map (each register is 32-bit):
 *
 *  Offset  Name                    Direction   Description
 *  ------  ----------------------  ----------  --------------------------
 *  0x00    REG_TABLE_ADDR_LOW      Host→FPGA   coherent buffer IOVA[31:0]
 *  0x04    REG_TABLE_ADDR_HIGH     Host→FPGA   coherent buffer IOVA[63:32]
 *  0x08    REG_TABLE_COUNT         Host→FPGA   number of IOVA table entries
 *  0x0C    REG_STATUS_ADDR_LOW     Host→FPGA   Status Area IOVA[31:0]
 *  0x10    REG_STATUS_ADDR_HIGH    Host→FPGA   Status Area IOVA[63:32]
 *  0x14    REG_CTRL                Host→FPGA   command register
 *
 * TODO: confirm all offsets with EE
 * ============================================================ */
#define REG_TABLE_ADDR_LOW          0x00
#define REG_TABLE_ADDR_HIGH         0x04
#define REG_TABLE_COUNT             0x08
#define REG_STATUS_ADDR_LOW         0x0C
#define REG_STATUS_ADDR_HIGH        0x10
#define REG_CTRL                    0x14

/* ============================================================
 * REG_CTRL command values
 * TODO: confirm values with EE
 * ============================================================ */
#define CTRL_CMD_START              0x00000001  /* start DMA transfer     */
#define CTRL_CMD_STOP               0x00000002  /* abort current transfer  */
#define CTRL_CMD_RESET              0x00000004  /* reset FPGA DMA engine  */

/* ============================================================
 * MSI-X vector assignments
 * QDMA IP supports multiple MSI-X vectors.
 * We use one vector for the Swath End interrupt.
 * TODO: confirm vector index with EE
 * ============================================================ */
#define IDA_MSIX_SWATH_END_VECTOR   0           /* Swath End interrupt    */
#define IDA_MSIX_VECTORS_MIN        1           /* minimum vectors needed */
#define IDA_MSIX_VECTORS_MAX        2           /* request up to this many*/

/* ============================================================
 * Misc hardware limits
 * ============================================================ */
#define IDA_MAX_CHANNELS            4
#define IDA_STATUS_AREA_SIZE        4096        /* bytes, one page        */
#define IDA_MAX_SG_ENTRIES          8192        /* max sg segs per channel*/

#endif /* _IDA_HW_H */
