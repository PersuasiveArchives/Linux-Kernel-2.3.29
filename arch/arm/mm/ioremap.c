/*
 * arch/arm/mm/ioremap.c
 *
 * Re-map IO memory to kernel address space so that we can access it.
 *
 * (C) Copyright 1995 1996 Linus Torvalds
 *
 * Hacked for ARM by Phil Blundell <philb@gnu.org>
 * Hacked to allow all architectures to build, and various cleanups
 * by Russell King
 */

/*
 * This allows a driver to remap an arbitrary region of bus memory into
 * virtual space.  One should *only* use readl, writel, memcpy_toio and
 * so on with such remapped areas.
 *
 * Because the ARM only has a 32-bit address space we can't address the
 * whole of the (physical) PCI space at once.  PCI huge-mode addressing
 * allows us to circumvent this restriction by splitting PCI space into
 * two 2GB chunks and mapping only one at a time into processor memory.
 * We use MMU protection domains to trap any attempt to access the bank
 * that is not currently mapped.  (This isn't fully implemented yet.)
 *
 * DC21285 currently has a bug in that the PCI address extension
 * register affects the address of any writes waiting in the outbound
 * FIFO.  Unfortunately, it is not possible to tell the DC21285 to
 * flush this - flushing the area causes the bus to lock.
 */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/io.h>

static inline void remap_area_pte(pte_t * pte, unsigned long address, unsigned long size,
	unsigned long phys_addr, pgprot_t pgprot)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		if (!pte_none(*pte))
			printk("remap_area_pte: page already exists\n");
		set_pte(pte, mk_pte_phys(phys_addr, pgprot));
		address += PAGE_SIZE;
		phys_addr += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline int remap_area_pmd(pmd_t * pmd, unsigned long address, unsigned long size,
	unsigned long phys_addr, unsigned long flags)
{
	unsigned long end;
	pgprot_t pgprot;

	address &= ~PGDIR_MASK;
	end = address + size;

	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;

	phys_addr -= address;
	pgprot = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY | L_PTE_WRITE | flags);
	do {
		pte_t * pte = pte_alloc_kernel(pmd, address);
		if (!pte)
			return -ENOMEM;
		remap_area_pte(pte, address, end - address, address + phys_addr, pgprot);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}

static int remap_area_pages(unsigned long address, unsigned long phys_addr,
				 unsigned long size, unsigned long flags)
{
	pgd_t * dir;
	unsigned long end = address + size;

	phys_addr -= address;
	dir = pgd_offset(&init_mm, address);
	flush_cache_all();
	while (address < end) {
		pmd_t *pmd = pmd_alloc_kernel(dir, address);
		if (!pmd)
			return -ENOMEM;
		if (remap_area_pmd(pmd, address, end - address,
					 phys_addr + address, flags))
			return -ENOMEM;
		set_pgdir(address, *dir);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	flush_tlb_all();
	return 0;
}

/*
 * Remap an arbitrary physical address space into the kernel virtual
 * address space. Needed when the kernel wants to access high addresses
 * directly.
 *
 * NOTE! We need to allow non-page-aligned mappings too: we will obviously
 * have to convert them into an offset in a page-aligned mapping, but the
 * caller shouldn't need to know that small detail.
 *
 * 'flags' are the extra L_PTE_ flags that you want to specify for this
 * mapping.  See include/asm-arm/proc-armv/pgtable.h for more information.
 */
void * __ioremap(unsigned long phys_addr, unsigned long size, unsigned long flags)
{
	void * addr;
	struct vm_struct * area;
	unsigned long offset, last_addr;

	/* Don't allow wraparound or zero size */
	last_addr = phys_addr + size - 1;
	if (!size || last_addr < phys_addr)
		return NULL;

	/*
	 * Mappings have to be page-aligned
	 */
	offset = phys_addr & ~PAGE_MASK;
	phys_addr &= PAGE_MASK;
	size = PAGE_ALIGN(last_addr) - phys_addr;

	/*
	 * Ok, go for it..
	 */
	area = get_vm_area(size, VM_IOREMAP);
	if (!area)
		return NULL;
	addr = area->addr;
	if (remap_area_pages(VMALLOC_VMADDR(addr), phys_addr, size, flags)) {
		vfree(addr);
		return NULL;
	}
	return (void *) (offset + (char *)addr);
}

void __iounmap(void *addr)
{
	vfree((void *) (PAGE_MASK & (unsigned long) addr));
}
