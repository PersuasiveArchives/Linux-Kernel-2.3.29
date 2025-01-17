#include <linux/config.h>

#ifndef _PPC_PAGE_H
#define _PPC_PAGE_H

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE-1))

#define PAGE_OFFSET	0xc0000000
#define KERNELBASE	PAGE_OFFSET

#ifndef __ASSEMBLY__
#ifdef __KERNEL__

#ifdef CONFIG_XMON
#define BUG() do { \
	printk("kernel BUG at %s:%d!\n", __FILE__, __LINE__); \
	xmon(0); \
} while (0)
#else
#define BUG() do { \
	printk("kernel BUG at %s:%d!\n", __FILE__, __LINE__); \
	__asm__ __volatile__(".long 0x0"); \
} while (0)
#endif
#define PAGE_BUG(page) do { BUG(); } while (0)

#define STRICT_MM_TYPECHECKS

#ifdef STRICT_MM_TYPECHECKS
/*
 * These are used to make use of C type-checking..
 */
typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pgprot; } pgprot_t;

#define pte_val(x)	((x).pte)
#define pmd_val(x)	((x).pmd)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)	((x).pgprot)

#define __pte(x)	((pte_t) { (x) } )
#define __pmd(x)	((pmd_t) { (x) } )
#define __pgd(x)	((pgd_t) { (x) } )
#define __pgprot(x)	((pgprot_t) { (x) } )

#else
/*
 * .. while these make it easier on the compiler
 */
typedef unsigned long pte_t;
typedef unsigned long pmd_t;
typedef unsigned long pgd_t;
typedef unsigned long pgprot_t;

#define pte_val(x)	(x)
#define pmd_val(x)	(x)
#define pgd_val(x)	(x)
#define pgprot_val(x)	(x)

#define __pte(x)	(x)
#define __pmd(x)	(x)
#define __pgd(x)	(x)
#define __pgprot(x)	(x)

#endif


/* align addr on a size boundry - adjust address up if needed -- Cort */
#define _ALIGN(addr,size)	(((addr)+size-1)&(~(size-1)))

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

extern void clear_page(void *page);
#define copy_page(to,from)	memcpy((void *)(to), (void *)(from), PAGE_SIZE)

/* map phys->virtual and virtual->phys for RAM pages */
static inline unsigned long ___pa(unsigned long v)
{ 
	unsigned long p;
	asm volatile ("1: addis %0, %1, %2;" 
		      ".section \".vtop_fixup\",\"aw\";"
		      ".align  1;"
		      ".long   1b;"
		      ".previous;"
		      : "=r" (p) 
		      : "b" (v), "K" (((-PAGE_OFFSET) >> 16) & 0xffff));

	return p;
}
static inline void* ___va(unsigned long p)
{ 
	unsigned long v;
	asm volatile ("1: addis %0, %1, %2;" 
		      ".section \".ptov_fixup\",\"aw\";"
		      ".align  1;"
		      ".long   1b;"
		      ".previous;"
		      : "=r" (v) 
		      : "b" (p), "K" (((PAGE_OFFSET) >> 16) & 0xffff));

	return (void*) v;
}
#define __pa(x) ___pa ((unsigned long)(x))
#define __va(x) ___va ((unsigned long)(x))

#define MAP_NR(addr)		(((unsigned long)addr-PAGE_OFFSET) >> PAGE_SHIFT)
#define MAP_PAGE_RESERVED	(1<<15)

extern unsigned long get_zero_page_fast(void);
#endif /* __KERNEL__ */
#endif /* __ASSEMBLY__ */
#endif /* _PPC_PAGE_H */
