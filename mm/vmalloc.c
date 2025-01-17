/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 */

#include <linux/malloc.h>
#include <linux/vmalloc.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>

struct vm_struct * vmlist = NULL;

static inline void free_area_pte(pmd_t * pmd, unsigned long address, unsigned long size)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return;
	}
	pte = pte_offset(pmd, address);
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		pte_t page = *pte;
		pte_clear(pte);
		address += PAGE_SIZE;
		pte++;
		if (pte_none(page))
			continue;
		if (pte_present(page)) {
			unsigned long map_nr = pte_pagenr(page);
			if (map_nr < max_mapnr)
				__free_page(mem_map + map_nr);
			continue;
		}
		printk(KERN_CRIT "Whee.. Swapped out page in kernel page table\n");
	} while (address < end);
}

static inline void free_area_pmd(pgd_t * dir, unsigned long address, unsigned long size)
{
	pmd_t * pmd;
	unsigned long end;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return;
	}
	pmd = pmd_offset(dir, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		free_area_pte(pmd, address, end - address);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
}

void vmfree_area_pages(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	dir = pgd_offset_k(address);
	flush_cache_all();
	do {
		free_area_pmd(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));
	flush_tlb_all();
}

static inline int alloc_area_pte(pte_t * pte, unsigned long address, unsigned long size)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		struct page * page;
		if (!pte_none(*pte))
			printk(KERN_ERR "alloc_area_pte: page already exists\n");
		page = alloc_page(GFP_KERNEL|__GFP_HIGHMEM);
		if (!page)
			return -ENOMEM;
		set_pte(pte, mk_pte(page, PAGE_KERNEL));
		address += PAGE_SIZE;
		pte++;
	} while (address < end);
	return 0;
}

static inline int alloc_area_pmd(pmd_t * pmd, unsigned long address, unsigned long size)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		pte_t * pte = pte_alloc_kernel(pmd, address);
		if (!pte)
			return -ENOMEM;
		if (alloc_area_pte(pte, address, end - address))
			return -ENOMEM;
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}

int vmalloc_area_pages(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	dir = pgd_offset_k(address);
	flush_cache_all();
	do {
		pmd_t *pmd;
		pgd_t olddir = *dir;
		
		pmd = pmd_alloc_kernel(dir, address);
		if (!pmd)
			return -ENOMEM;
		if (alloc_area_pmd(pmd, address, end - address))
			return -ENOMEM;
		if (pgd_val(olddir) != pgd_val(*dir))
			set_pgdir(address, *dir);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));
	flush_tlb_all();
	return 0;
}

struct vm_struct * get_vm_area(unsigned long size, unsigned long flags)
{
	unsigned long addr;
	struct vm_struct **p, *tmp, *area;

	area = (struct vm_struct *) kmalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return NULL;
	addr = VMALLOC_START;
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		if (size + addr < (unsigned long) tmp->addr)
			break;
		addr = tmp->size + (unsigned long) tmp->addr;
		if (addr > VMALLOC_END-size) {
			kfree(area);
			return NULL;
		}
	}
	area->flags = flags;
	area->addr = (void *)addr;
	area->size = size + PAGE_SIZE;
	area->next = *p;
	*p = area;
	return area;
}

void vfree(void * addr)
{
	struct vm_struct **p, *tmp;

	if (!addr)
		return;
	if ((PAGE_SIZE-1) & (unsigned long) addr) {
		printk(KERN_ERR "Trying to vfree() bad address (%p)\n", addr);
		return;
	}
	for (p = &vmlist ; (tmp = *p) ; p = &tmp->next) {
		if (tmp->addr == addr) {
			*p = tmp->next;
			vmfree_area_pages(VMALLOC_VMADDR(tmp->addr), tmp->size);
			kfree(tmp);
			return;
		}
	}
	printk(KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n", addr);
}

void * vmalloc(unsigned long size)
{
	void * addr;
	struct vm_struct *area;

	size = PAGE_ALIGN(size);
	if (!size || (size >> PAGE_SHIFT) > max_mapnr) {
		BUG();
		return NULL;
	}
	area = get_vm_area(size, VM_ALLOC);
	if (!area) {
		BUG();
		return NULL;
	}
	addr = area->addr;
	if (vmalloc_area_pages(VMALLOC_VMADDR(addr), size)) {
		vfree(addr);
		BUG();
		return NULL;
	}
	return addr;
}

long vread(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr, *buf_start = buf;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	for (tmp = vmlist; tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			put_user('\0', buf);
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		do {
			if (count == 0)
				goto finished;
			put_user(*addr, buf);
			buf++;
			addr++;
			count--;
		} while (--n > 0);
	}
finished:
	return buf - buf_start;
}
