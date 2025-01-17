/*
 *  linux/arch/arm/mm/mm-armv.c
 *
 *  Page table sludge for ARM v3 and v4 processor architectures.
 *
 *  Copyright (C) 1998-1999 Russell King
 */
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/bootmem.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/io.h>
#include <asm/setup.h>

#include "map.h"

unsigned long *valid_addr_bitmap;

extern unsigned long get_page_2k(int priority);
extern void free_page_2k(unsigned long page);
extern pte_t *get_bad_pte_table(void);

/*
 * need to get a 16k page for level 1
 */
pgd_t *get_pgd_slow(void)
{
	pgd_t *pgd = (pgd_t *)__get_free_pages(GFP_KERNEL,2);
	pmd_t *new_pmd;

	if (pgd) {
		pgd_t *init = pgd_offset_k(0);
		
		memzero(pgd, USER_PTRS_PER_PGD * sizeof(pgd_t));
		memcpy(pgd + USER_PTRS_PER_PGD, init + USER_PTRS_PER_PGD,
			(PTRS_PER_PGD - USER_PTRS_PER_PGD) * sizeof(pgd_t));
		clean_cache_area(pgd, PTRS_PER_PGD * sizeof(pgd_t));

		/*
		 * On ARM, first page must always be allocated
		 */
		if (!pmd_alloc(pgd, 0))
			goto nomem;
		else {
			pmd_t *old_pmd = pmd_offset(init, 0);
			new_pmd = pmd_offset(pgd, 0);

			if (!pte_alloc(new_pmd, 0))
				goto nomem_pmd;
			else {
				pte_t *new_pte = pte_offset(new_pmd, 0);
				pte_t *old_pte = pte_offset(old_pmd, 0);

				set_pte(new_pte, *old_pte);
			}
		}
	}
	return pgd;

nomem_pmd:
	pmd_free(new_pmd);
nomem:
	free_pages((unsigned long)pgd, 2);
	return NULL;
}

void free_pgd_slow(pgd_t *pgd)
{
	if (pgd) { /* can pgd be NULL? */
		pmd_t *pmd;
		pte_t *pte;

		/* pgd is always present and good */
		pmd = (pmd_t *)pgd;
		if (pmd_none(*pmd))
			goto free;
		if (pmd_bad(*pmd)) {
			pmd_ERROR(*pmd);
			pmd_clear(pmd);
			goto free;
		}

		pte = pte_offset(pmd, 0);
		pmd_clear(pmd);
		pte_free(pte);
		pmd_free(pmd);
	}
free:
	free_pages((unsigned long) pgd, 2);
}

pte_t *get_pte_slow(pmd_t *pmd, unsigned long offset)
{
	pte_t *pte;

	pte = (pte_t *)get_page_2k(GFP_KERNEL);
	if (pmd_none(*pmd)) {
		if (pte) {
			memzero(pte, 2 * PTRS_PER_PTE * sizeof(pte_t));
			clean_cache_area(pte, PTRS_PER_PTE * sizeof(pte_t));
			pte += PTRS_PER_PTE;
			set_pmd(pmd, mk_user_pmd(pte));
			return pte + offset;
		}
		set_pmd(pmd, mk_user_pmd(get_bad_pte_table()));
		return NULL;
	}
	free_page_2k((unsigned long)pte);
	if (pmd_bad(*pmd)) {
		__handle_bad_pmd(pmd);
		return NULL;
	}
	return (pte_t *) pmd_page(*pmd) + offset;
}

pte_t *get_pte_kernel_slow(pmd_t *pmd, unsigned long offset)
{
	pte_t *pte;

	pte = (pte_t *)get_page_2k(GFP_KERNEL);
	if (pmd_none(*pmd)) {
		if (pte) {
			memzero(pte, 2 * PTRS_PER_PTE * sizeof(pte_t));
			clean_cache_area(pte, PTRS_PER_PTE * sizeof(pte_t));
			pte += PTRS_PER_PTE;
			set_pmd(pmd, mk_kernel_pmd(pte));
			return pte + offset;
		}
		set_pmd(pmd, mk_kernel_pmd(get_bad_pte_table()));
		return NULL;
	}
	free_page_2k((unsigned long)pte);
	if (pmd_bad(*pmd)) {
		__handle_bad_pmd_kernel(pmd);
		return NULL;
	}
	return (pte_t *) pmd_page(*pmd) + offset;
}

void free_pte_slow(pte_t *pte)
{
	free_page_2k((unsigned long)(pte - PTRS_PER_PTE));
}

/*
 * Create a SECTION PGD between VIRT and PHYS in domain
 * DOMAIN with protection PROT
 */
static inline void
alloc_init_section(unsigned long virt, unsigned long phys, int prot)
{
	pmd_t pmd;

	pmd_val(pmd) = phys | prot;

	set_pmd(pmd_offset(pgd_offset_k(virt), virt), pmd);
}

/*
 * Add a PAGE mapping between VIRT and PHYS in domain
 * DOMAIN with protection PROT.  Note that due to the
 * way we map the PTEs, we must allocate two PTE_SIZE'd
 * blocks - one for the Linux pte table, and one for
 * the hardware pte table.
 */
static inline void
alloc_init_page(unsigned long virt, unsigned long phys, int domain, int prot)
{
	pmd_t *pmdp;
	pte_t *ptep;

	pmdp = pmd_offset(pgd_offset_k(virt), virt);

	if (pmd_none(*pmdp)) {
		pte_t *ptep = alloc_bootmem_low_pages(2 * PTRS_PER_PTE *
						      sizeof(pte_t));

		ptep += PTRS_PER_PTE;

		set_pmd(pmdp, __mk_pmd(ptep, PMD_TYPE_TABLE | PMD_DOMAIN(domain)));
	}
	ptep = pte_offset(pmdp, virt);

	set_pte(ptep, mk_pte_phys(phys, __pgprot(prot)));
}

/*
 * Clear any PGD mapping.  On a two-level page table system,
 * the clearance is done by the middle-level functions (pmd)
 * rather than the top-level (pgd) functions.
 */
static inline void free_init_section(unsigned long virt)
{
	pmd_clear(pmd_offset(pgd_offset_k(virt), virt));
}

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections.
 */
static void __init create_mapping(struct map_desc *md)
{
	unsigned long virt, length;
	int prot_sect, prot_pte;
	long off;

	prot_pte = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
		   (md->prot_read  ? L_PTE_USER       : 0) |
		   (md->prot_write ? L_PTE_WRITE      : 0) |
		   (md->cacheable  ? L_PTE_CACHEABLE  : 0) |
		   (md->bufferable ? L_PTE_BUFFERABLE : 0);

	prot_sect = PMD_TYPE_SECT | PMD_DOMAIN(md->domain) |
		    (md->prot_read  ? PMD_SECT_AP_READ    : 0) |
		    (md->prot_write ? PMD_SECT_AP_WRITE   : 0) |
		    (md->cacheable  ? PMD_SECT_CACHEABLE  : 0) |
		    (md->bufferable ? PMD_SECT_BUFFERABLE : 0);

	virt   = md->virtual;
	off    = md->physical - virt;
	length = md->length;

	while ((virt & 1048575 || (virt + off) & 1048575) && length >= PAGE_SIZE) {
		alloc_init_page(virt, virt + off, md->domain, prot_pte);

		virt   += PAGE_SIZE;
		length -= PAGE_SIZE;
	}

	while (length >= PGDIR_SIZE) {
		alloc_init_section(virt, virt + off, prot_sect);

		virt   += PGDIR_SIZE;
		length -= PGDIR_SIZE;
	}

	while (length >= PAGE_SIZE) {
		alloc_init_page(virt, virt + off, md->domain, prot_pte);

		virt   += PAGE_SIZE;
		length -= PAGE_SIZE;
	}
}

/*
 * Initial boot-time mapping.  This covers just the zero page, kernel and
 * the flush area.  NB: it must be sorted by virtual address, and no
 * virtual address overlaps.
 *  init_map[2..4] are for architectures with banked memory.
 */
static struct map_desc init_map[] __initdata = {
	{ 0, 0, PAGE_SIZE,  DOMAIN_USER,   0, 0, 1, 0 }, /* zero page     */
	{ 0, 0, 0,          DOMAIN_KERNEL, 0, 1, 1, 1 }, /* kernel memory */
	{ 0, 0, 0,          DOMAIN_KERNEL, 0, 1, 1, 1 }, /* (4 banks)	  */
	{ 0, 0, 0,          DOMAIN_KERNEL, 0, 1, 1, 1 },
	{ 0, 0, 0,          DOMAIN_KERNEL, 0, 1, 1, 1 },
	{ 0, 0, PGDIR_SIZE, DOMAIN_KERNEL, 1, 0, 1, 1 }, /* cache flush 1 */
	{ 0, 0, 0,          DOMAIN_KERNEL, 1, 0, 1, 0 }  /* cache flush 2 */
};

#define NR_INIT_MAPS (sizeof(init_map) / sizeof(init_map[0]))

/*
 * Calculate the size of the DMA, normal and highmem zones.
 * On ARM, we don't have any problems with DMA, so all memory
 * is allocated to the DMA zone.  We also don't have any
 * highmem either.
 */
void __init zonesize_init(unsigned int *zone_size)
{
	int i;

	zone_size[0] = 0;
	zone_size[1] = 0;
	zone_size[2] = 0;

	for (i = 0; i < meminfo.nr_banks; i++) {
		if (meminfo.bank[i].size) {
			unsigned int end;

			end = (meminfo.bank[i].start +
				meminfo.bank[i].size) >> PAGE_SHIFT;
			if (end > zone_size[0])
				zone_size[0] = end;
		}
	}
}

void __init pagetable_init(void)
{
	unsigned long address = 0;
	int i;

	/*
	 * Setup the above mappings
	 */
	init_map[0].physical = PHYS_OFFSET;
	init_map[5].physical = FLUSH_BASE_PHYS;
	init_map[5].virtual  = FLUSH_BASE;
#ifdef FLUSH_BASE_MINICACHE
	init_map[6].physical = FLUSH_BASE_PHYS + PGDIR_SIZE;
	init_map[6].virtual  = FLUSH_BASE_MINICACHE;
	init_map[6].length   = PGDIR_SIZE;
#endif

	for (i = 0; i < meminfo.nr_banks; i++) {
		init_map[i+1].physical = PHYS_OFFSET + meminfo.bank[i].start;
		init_map[i+1].virtual  = PAGE_OFFSET + meminfo.bank[i].start;
		init_map[i+1].length   = meminfo.bank[i].size;
	}

	/*
	 * Go through the initial mappings, but clear out any
	 * pgdir entries that are not in the description.
	 */
	i = 0;
	do {
		if (address < init_map[i].virtual || i == NR_INIT_MAPS) {
			free_init_section(address);
			address += PGDIR_SIZE;
		} else {
			create_mapping(init_map + i);

			address = init_map[i].virtual + init_map[i].length;
			address = (address + PGDIR_SIZE - 1) & PGDIR_MASK;

			do {
				i += 1;
			} while (init_map[i].length == 0 && i < NR_INIT_MAPS);
		}
	} while (address != 0);

	/*
	 * Create the architecture specific mappings
	 */
	for (i = 0; i < io_desc_size; i++)
		create_mapping(io_desc + i);

	flush_cache_all();
}

/*
 * The mem_map array can get very big.  Mark the end of the valid mem_map
 * banks with PG_skip, and setup the address validity bitmap.
 */
void __init create_memmap_holes(void)
{
	unsigned int start_pfn, end_pfn = -1;
	struct page *pg = NULL;
	unsigned int sz, i;

	for (i = 0; i < meminfo.nr_banks; i++) {
		if (meminfo.bank[i].size == 0)
			continue;

		start_pfn = meminfo.bank[i].start >> PAGE_SHIFT;

		/*
		 * subtle here - if we have a full bank, then
		 * start_pfn == end_pfn, and we don't want to
		 * set PG_skip, or next_hash
		 */
		if (pg && start_pfn != end_pfn) {
			set_bit(PG_skip, &pg->flags);
			pg->next_hash = mem_map + start_pfn;

			start_pfn = PAGE_ALIGN(__pa(pg + 1));
			end_pfn   = __pa(pg->next_hash) & PAGE_MASK;

			if (end_pfn != start_pfn)
				free_bootmem(start_pfn, end_pfn - start_pfn);

			pg = NULL;
		}

		end_pfn = (meminfo.bank[i].start +
			   meminfo.bank[i].size) >> PAGE_SHIFT;

		if (end_pfn != meminfo.end >> PAGE_SHIFT)
			pg = mem_map + end_pfn;
	}

	if (pg) {
		set_bit(PG_skip, &pg->flags);
		pg->next_hash = NULL;
	}
}
