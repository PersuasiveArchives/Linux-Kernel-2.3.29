/*
 * linux/include/asm-arm/procinfo.h
 *
 * Copyright (C) 1996-1999 Russell King
 */
#ifndef __ASM_PROCINFO_H
#define __ASM_PROCINFO_H

#ifndef __ASSEMBLY__

#include <asm/proc-fns.h>

struct proc_info_item {
	const char	 *manufacturer;
	const char	 *cpu_name;
};

/*
 * Note!  struct processor is always defined if we're
 * using MULTI_CPU, otherwise this entry is unused,
 * but still exists.  NOTE! This structure is used
 * by assembler code!  Check:
 *  arch/arm/mm/proc-*.S and arch/arm/kernel/head-armv.S
 */
struct proc_info_list {
	unsigned int	 cpu_val;
	unsigned int	 cpu_mask;
	unsigned long	 __cpu_mmu_flags;	/* used by head-armv.S */
	unsigned long	 __cpu_flush;		/* used by head-armv.S */
	const char	 *arch_name;
	const char	 *elf_name;
	unsigned int	 elf_hwcap;
	struct proc_info_item *info;
#ifdef MULTI_CPU
	struct processor *proc;
#else
	void		 *unused;
#endif
};

#endif	/* __ASSEMBLY__ */

#define HWCAP_SWP	1
#define HWCAP_HALF	2
#define HWCAP_THUMB	4
#define HWCAP_26BIT	8	/* Play it safe */

#endif

