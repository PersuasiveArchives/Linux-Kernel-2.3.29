/*
 * memory.c: PROM library functions for acquiring/using memory descriptors
 *           given to us from the ARCS firmware.
 *
 * Copyright (C) 1996 David S. Miller (dm@engr.sgi.com)
 *
 * $Id: memory.c,v 1.5 1999/04/14 21:25:02 tsbogend Exp $
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/swap.h>

#include <asm/sgialib.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/bootinfo.h>

/* #define DEBUG */

struct linux_mdesc * __init prom_getmdesc(struct linux_mdesc *curr)
{
	return romvec->get_mdesc(curr);
}

#ifdef DEBUG /* convenient for debugging */
static char *arcs_mtypes[8] = {
	"Exception Block",
	"ARCS Romvec Page",
	"Free/Contig RAM",
	"Generic Free RAM",
	"Bad Memory",
	"Standlong Program Pages",
	"ARCS Temp Storage Area",
	"ARCS Permanent Storage Area"
};

static char *arc_mtypes[8] = {
	"Exception Block",
	"SystemParameterBlock",
	"FreeMemory",
	"Bad Memory",
	"LoadedProgram",
	"FirmwareTemporary",
	"FirmwarePermanent",
	"FreeContigiuous"
};
#define mtypes(a) (prom_flags & PROM_FLAG_ARCS) ? arcs_mtypes[a.arcs] : arc_mtypes[a.arc]
#endif

static struct prom_pmemblock prom_pblocks[PROM_MAX_PMEMBLOCKS];

struct prom_pmemblock * __init prom_getpblock_array(void)
{
	return &prom_pblocks[0];
}

#define MEMTYPE_DONTUSE   0
#define MEMTYPE_PROM      1
#define MEMTYPE_FREE      2

static int __init prom_memtype_classify (union linux_memtypes type)
{
    if (prom_flags & PROM_FLAG_ARCS) {
	switch (type.arcs) {
	 case arcs_free:
	 case arcs_fcontig:
	    return MEMTYPE_FREE;
	 case arcs_atmp:
	 case arcs_aperm:
	    return MEMTYPE_PROM;
	 default:
	    return MEMTYPE_DONTUSE;
	}
    } else {
	switch (type.arc) {
	 case arc_free:
	 case arc_fcontig:
	    return MEMTYPE_FREE;
	 case arc_rvpage:
	 case arc_atmp:
	 case arc_aperm:
	    return MEMTYPE_PROM;
	 default:
	    return MEMTYPE_DONTUSE;
	}
    }
}

static void __init prom_setup_memupper(void)
{
	struct prom_pmemblock *p, *highest;

	for(p = prom_getpblock_array(), highest = 0; p->size != 0; p++) {
		if(p->base == 0xdeadbeef)
			prom_printf("WHEEE, bogus pmemblock\n");
		if(!highest || p->base > highest->base)
			highest = p;
	}
	mips_memory_upper = highest->base + highest->size;
#ifdef DEBUG
	prom_printf("prom_setup_memupper: mips_memory_upper = %08lx\n",
		    mips_memory_upper);
#endif
}

void __init prom_meminit(void)
{
	struct linux_mdesc *p;
	int totram;
	int i = 0;

	p = prom_getmdesc(PROM_NULL_MDESC);
#ifdef DEBUG
	prom_printf("ARCS MEMORY DESCRIPTOR dump:\n");
	while(p) {
		prom_printf("[%d,%p]: base<%08lx> pages<%08lx> type<%s>\n",
			    i, p, p->base, p->pages, mtypes(p->type));
		p = prom_getmdesc(p);
		i++;
	}
#endif
	p = prom_getmdesc(PROM_NULL_MDESC);
	totram = 0;
	i = 0;
	while(p) {
	    prom_pblocks[i].type = prom_memtype_classify (p->type);
	    prom_pblocks[i].base = ((p->base<<PAGE_SHIFT) + 0x80000000);
	    prom_pblocks[i].size = p->pages << PAGE_SHIFT;
	    switch (prom_pblocks[i].type) {
	     case MEMTYPE_FREE:
		totram += prom_pblocks[i].size;
#ifdef DEBUG
		prom_printf("free_chunk[%d]: base=%08lx size=%d\n",
			    i, prom_pblocks[i].base,
			    prom_pblocks[i].size);
#endif
		i++;
		break;
	     case MEMTYPE_PROM:
#ifdef DEBUG
		prom_printf("prom_chunk[%d]: base=%08lx size=%d\n",
			    i, prom_pblocks[i].base,
			    prom_pblocks[i].size);
#endif
		i++;
		break;
	     default:
		break;
	    }
	    p = prom_getmdesc(p);
	}
	prom_pblocks[i].base = 0xdeadbeef;
	prom_pblocks[i].size = 0; /* indicates last elem. of array */
	printk("PROMLIB: Total free ram %d bytes (%dK,%dMB)\n",
		    totram, (totram/1024), (totram/1024/1024));

	/* Setup upper physical memory bound. */
	prom_setup_memupper();
}

/* Called from mem_init() to fixup the mem_map page settings. */
void __init prom_fixup_mem_map(unsigned long start, unsigned long end)
{
	struct prom_pmemblock *p;
	int i, nents;

	/* Determine number of pblockarray entries. */
	p = prom_getpblock_array();
	for(i = 0; p[i].size; i++)
		;
	nents = i;
restart:
	while(start < end) {
		for(i = 0; i < nents; i++) {
			if((p[i].type == MEMTYPE_FREE) &&
			   (start >= (p[i].base)) &&
			   (start < (p[i].base + p[i].size))) {
				start = p[i].base + p[i].size;
				start &= PAGE_MASK;
				goto restart;
			}
		}
		set_bit(PG_reserved, &mem_map[MAP_NR(start)].flags);
		start += PAGE_SIZE;
	}
}

void prom_free_prom_memory (void)
{
    struct prom_pmemblock *p;
    unsigned long addr;
    unsigned long num_pages = 0;

    for(p = prom_getpblock_array(); p->size != 0; p++) {
	if (p->type == MEMTYPE_PROM) {
	    for (addr = p->base; addr < p->base + p->size; addr += PAGE_SIZE) {
		mem_map[MAP_NR(addr)].flags &= ~(1 << PG_reserved);
		atomic_set(&mem_map[MAP_NR(addr)].count, 1);
		free_page(addr);
		num_pages++;
	    }
	}
    }
    printk ("Freeing prom memory: %dk freed\n",num_pages << (PAGE_SHIFT - 10));
}
