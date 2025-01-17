/* iommu.h: Definitions for the sun5 IOMMU.
 *
 * Copyright (C) 1996 David S. Miller (davem@caip.rutgers.edu)
 */
#ifndef _SPARC64_IOMMU_H
#define _SPARC64_IOMMU_H

#include <linux/spinlock.h>
#include <asm/page.h>
#include <asm/sysio.h>

/* The iommu handles all virtual to physical address translations
 * that occur between the SYSIO and physical memory.  Access by
 * the cpu to IO registers and similar go over the UPA so are
 * translated by the on chip TLB's.  The iommu and the TLB do
 * not need to have the same translations at all, in fact most
 * of the time the translations they handle are a disjoint set.
 * Basically the iommu handles all SYSIO dvma translations.
 */

/* The IOMMU register set. */
#define IOMMU_CTRL_IMPL     0xf000000000000000 /* Implementation                */
#define IOMMU_CTRL_VERS     0x0f00000000000000 /* Version                       */
#define IOMMU_CTRL_TSBSZ    0x0000000000070000 /* TSB Size                      */
#define IOMMU_TSBSZ_1K      0x0000000000000000 /* TSB Table 1024 8-byte entries */
#define IOMMU_TSBSZ_2K      0x0000000000010000 /* TSB Table 2048 8-byte entries */
#define IOMMU_TSBSZ_4K      0x0000000000020000 /* TSB Table 4096 8-byte entries */
#define IOMMU_TSBSZ_8K      0x0000000000030000 /* TSB Table 8192 8-byte entries */
#define IOMMU_TSBSZ_16K     0x0000000000040000 /* TSB Table 16k 8-byte entries  */
#define IOMMU_TSBSZ_32K     0x0000000000050000 /* TSB Table 32k 8-byte entries  */
#define IOMMU_TSBSZ_64K     0x0000000000060000 /* TSB Table 64k 8-byte entries  */
#define IOMMU_TSBSZ_128K    0x0000000000070000 /* TSB Table 128k 8-byte entries */
#define IOMMU_CTRL_TBWSZ    0x0000000000000004 /* Assumed page size, 0=8k 1=64k */
#define IOMMU_CTRL_DENAB    0x0000000000000002 /* Diagnostic mode enable        */
#define IOMMU_CTRL_ENAB     0x0000000000000001 /* IOMMU Enable                  */

/* The format of an iopte in the page tables, we only use 64k pages. */
#define IOPTE_VALID         0x8000000000000000 /* IOPTE is valid                   */
#define IOPTE_64K           0x2000000000000000 /* IOPTE is for 64k page            */
#define IOPTE_STBUF         0x1000000000000000 /* DVMA can use streaming buffer    */
#define IOPTE_INTRA         0x0800000000000000 /* SBUS slot-->slot direct transfer */
#define IOPTE_CONTEXT	    0x07ff800000000000 /* Context number		   */
#define IOPTE_PAGE          0x00007fffffffe000 /* Physical page number (PA[40:13]) */
#define IOPTE_CACHE         0x0000000000000010 /* Cached (in UPA E-cache)          */
#define IOPTE_WRITE         0x0000000000000002 /* Writeable                        */

struct iommu_struct {
	struct sysio_regs	*sysio_regs;
	iopte_t			*page_table;
	volatile unsigned int	flushflag;
	unsigned int		strbuf_enabled;
	spinlock_t		iommu_lock;
};

#endif /* !(_SPARC_IOMMU_H) */
