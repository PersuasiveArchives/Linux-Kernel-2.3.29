/*
 * linux/arch/m68k/motorola.c
 *
 * Routines specific to the Motorola MMU, originally from:
 * linux/arch/m68k/init.c 
 * which are Copyright (C) 1995 Hamish Macdonald
 * 
 * Moved 8/20/1999 Sam Creasey
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/init.h>
#ifdef CONFIG_BLK_DEV_RAM
#include <linux/blk.h>
#endif

#include <asm/setup.h>
#include <asm/uaccess.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/machdep.h>
#include <asm/io.h>
#ifdef CONFIG_ATARI
#include <asm/atari_stram.h>
#endif

#undef DEBUG

#ifndef mm_cachebits
/*
 * Bits to add to page descriptors for "normal" caching mode.
 * For 68020/030 this is 0.
 * For 68040, this is _PAGE_CACHE040 (cachable, copyback)
 */
unsigned long mm_cachebits = 0;
#endif

static pte_t * __init kernel_page_table(unsigned long *memavailp)
{
	pte_t *ptablep;

	ptablep = (pte_t *)*memavailp;
	*memavailp += PAGE_SIZE;

	clear_page((unsigned long)ptablep);
	flush_page_to_ram((unsigned long) ptablep);
	flush_tlb_kernel_page((unsigned long) ptablep);
	nocache_page ((unsigned long)ptablep);

	return ptablep;
}

static pmd_t *last_pgtable __initdata = NULL;
pmd_t *zero_pgtable __initdata = NULL;

static pmd_t * __init kernel_ptr_table(unsigned long *memavailp)
{
	if (!last_pgtable) {
		unsigned long pmd, last;
		int i;

		/* Find the last ptr table that was used in head.S and
		 * reuse the remaining space in that page for further
		 * ptr tables.
		 */
		last = (unsigned long)kernel_pg_dir;
		for (i = 0; i < PTRS_PER_PGD; i++) {
			if (!pgd_present(kernel_pg_dir[i]))
				continue;
			pmd = pgd_page(kernel_pg_dir[i]);
			if (pmd > last)
				last = pmd;
		}

		last_pgtable = (pmd_t *)last;
#ifdef DEBUG
		printk("kernel_ptr_init: %p\n", last_pgtable);
#endif
	}

	if (((unsigned long)(last_pgtable + PTRS_PER_PMD) & ~PAGE_MASK) == 0) {
		last_pgtable = (pmd_t *)*memavailp;
		*memavailp += PAGE_SIZE;

		clear_page((unsigned long)last_pgtable);
		flush_page_to_ram((unsigned long)last_pgtable);
		flush_tlb_kernel_page((unsigned long)last_pgtable);
		nocache_page((unsigned long)last_pgtable);
	} else
		last_pgtable += PTRS_PER_PMD;

	return last_pgtable;
}

static unsigned long __init 
map_chunk (unsigned long addr, long size, unsigned long *memavailp)
{
#define PTRTREESIZE (256*1024)
#define ROOTTREESIZE (32*1024*1024)
	static unsigned long virtaddr = PAGE_OFFSET;
	unsigned long physaddr;
	pgd_t *pgd_dir;
	pmd_t *pmd_dir;
	pte_t *pte_dir;

	physaddr = (addr | m68k_supervisor_cachemode |
		    _PAGE_PRESENT | _PAGE_ACCESSED | _PAGE_DIRTY);
	if (CPU_IS_040_OR_060)
		physaddr |= _PAGE_GLOBAL040;

	while (size > 0) {
#ifdef DEBUG
		if (!(virtaddr & (PTRTREESIZE-1)))
			printk ("\npa=%#lx va=%#lx ", physaddr & PAGE_MASK,
				virtaddr);
#endif
		pgd_dir = pgd_offset_k(virtaddr);
		if (virtaddr && CPU_IS_020_OR_030) {
			if (!(virtaddr & (ROOTTREESIZE-1)) &&
			    size >= ROOTTREESIZE) {
#ifdef DEBUG
				printk ("[very early term]");
#endif
				pgd_val(*pgd_dir) = physaddr;
				size -= ROOTTREESIZE;
				virtaddr += ROOTTREESIZE;
				physaddr += ROOTTREESIZE;
				continue;
			}
		}
		if (!pgd_present(*pgd_dir)) {
			pmd_dir = kernel_ptr_table(memavailp);
#ifdef DEBUG
			printk ("[new pointer %p]", pmd_dir);
#endif
			pgd_set(pgd_dir, pmd_dir);
		} else
			pmd_dir = pmd_offset(pgd_dir, virtaddr);

		if (CPU_IS_020_OR_030) {
			if (virtaddr) {
#ifdef DEBUG
				printk ("[early term]");
#endif
				pmd_dir->pmd[(virtaddr/PTRTREESIZE) & 15] = physaddr;
				physaddr += PTRTREESIZE;
			} else {
				int i;
#ifdef DEBUG
				printk ("[zero map]");
#endif
				zero_pgtable = kernel_ptr_table(memavailp);
				pte_dir = (pte_t *)zero_pgtable;
				pmd_dir->pmd[0] = virt_to_phys(pte_dir) |
					_PAGE_TABLE | _PAGE_ACCESSED;
				pte_val(*pte_dir++) = 0;
				physaddr += PAGE_SIZE;
				for (i = 1; i < 64; physaddr += PAGE_SIZE, i++)
					pte_val(*pte_dir++) = physaddr;
			}
			size -= PTRTREESIZE;
			virtaddr += PTRTREESIZE;
		} else {
			if (!pmd_present(*pmd_dir)) {
#ifdef DEBUG
				printk ("[new table]");
#endif
				pte_dir = kernel_page_table(memavailp);
				pmd_set(pmd_dir, pte_dir);
			}
			pte_dir = pte_offset(pmd_dir, virtaddr);

			if (virtaddr) {
				if (!pte_present(*pte_dir))
					pte_val(*pte_dir) = physaddr;
			} else
				pte_val(*pte_dir) = 0;
			size -= PAGE_SIZE;
			virtaddr += PAGE_SIZE;
			physaddr += PAGE_SIZE;
		}

	}
#ifdef DEBUG
	printk("\n");
#endif

	return virtaddr;
}

extern unsigned long free_area_init(unsigned long, unsigned long);
extern unsigned long empty_bad_page_table;
extern unsigned long empty_bad_page;

/*
 * paging_init() continues the virtual memory environment setup which
 * was begun by the code in arch/head.S.
 */
unsigned long __init paging_init(unsigned long start_mem,
				 unsigned long end_mem)
{
	int chunk;
	unsigned long mem_avail = 0;

#ifdef DEBUG
	{
		extern unsigned long availmem;
		printk ("start of paging_init (%p, %lx, %lx, %lx)\n",
			kernel_pg_dir, availmem, start_mem, end_mem);
	}
#endif

	/* Fix the cache mode in the page descriptors for the 680[46]0.  */
	if (CPU_IS_040_OR_060) {
		int i;
#ifndef mm_cachebits
		mm_cachebits = _PAGE_CACHE040;
#endif
		for (i = 0; i < 16; i++)
			pgprot_val(protection_map[i]) |= _PAGE_CACHE040;
	}
	/* Fix the PAGE_NONE value. */
	if (CPU_IS_040_OR_060) {
		/* On the 680[46]0 we can use the _PAGE_SUPER bit.  */
		pgprot_val(protection_map[0]) |= _PAGE_SUPER;
		pgprot_val(protection_map[VM_SHARED]) |= _PAGE_SUPER;
	} else {
		/* Otherwise we must fake it. */
		pgprot_val(protection_map[0]) &= ~_PAGE_PRESENT;
		pgprot_val(protection_map[0]) |= _PAGE_FAKE_SUPER;
		pgprot_val(protection_map[VM_SHARED]) &= ~_PAGE_PRESENT;
		pgprot_val(protection_map[VM_SHARED]) |= _PAGE_FAKE_SUPER;
	}

	/*
	 * Map the physical memory available into the kernel virtual
	 * address space.  It may allocate some memory for page
	 * tables and thus modify availmem.
	 */

	for (chunk = 0; chunk < m68k_num_memory; chunk++) {
		mem_avail = map_chunk (m68k_memory[chunk].addr,
				       m68k_memory[chunk].size, &start_mem);

	}

	flush_tlb_all();
#ifdef DEBUG
	printk ("memory available is %ldKB\n", mem_avail >> 10);
	printk ("start_mem is %#lx\nvirtual_end is %#lx\n",
		start_mem, end_mem);
#endif

	/*
	 * initialize the bad page table and bad page to point
	 * to a couple of allocated pages
	 */
	empty_bad_page_table = start_mem;
	start_mem += PAGE_SIZE;
	empty_bad_page = start_mem;
	start_mem += PAGE_SIZE;
	empty_zero_page = start_mem;
	start_mem += PAGE_SIZE;
	memset((void *)empty_zero_page, 0, PAGE_SIZE);

	/*
	 * Set up SFC/DFC registers (user data space)
	 */
	set_fs (USER_DS);

#ifdef DEBUG
	printk ("before free_area_init\n");
#endif
	return PAGE_ALIGN(free_area_init(start_mem, end_mem));
}

extern char __init_begin, __init_end;

void free_initmem(void)
{
	unsigned long addr;

	addr = (unsigned long)&__init_begin;
	for (; addr < (unsigned long)&__init_end; addr += PAGE_SIZE) {
		mem_map[MAP_NR(addr)].flags &= ~(1 << PG_reserved);
		set_page_count(mem_map+MAP_NR(addr), 1);
		free_page(addr);
	}
}


