#ifndef __ASMm68k_ELF_H
#define __ASMm68k_ELF_H

/*
 * ELF register definitions..
 */

#include <linux/config.h>
#include <asm/ptrace.h>
#include <asm/user.h>

typedef unsigned long elf_greg_t;

#define ELF_NGREG (sizeof(struct user_regs_struct) / sizeof(elf_greg_t))
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

typedef struct user_m68kfp_struct elf_fpregset_t;

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
#define elf_check_arch(x) ((x) == EM_68K)

/*
 * These are used to set parameters in the core dumps.
 */
#define ELF_CLASS	ELFCLASS32
#define ELF_DATA	ELFDATA2MSB
#define ELF_ARCH	EM_68K

/* For SVR4/m68k the function pointer to be registered with `atexit' is
   passed in %a1.  Although my copy of the ABI has no such statement, it
   is actually used on ASV.  */
#define ELF_PLAT_INIT(_r)	_r->a1 = 0

#define USE_ELF_CORE_DUMP
#ifndef CONFIG_SUN3
#define ELF_EXEC_PAGESIZE	4096
#else
#define ELF_EXEC_PAGESIZE	8192
#endif

/* This is the location that an ET_DYN program is loaded if exec'ed.  Typical
   use of this is to invoke "./ld.so someprog" to test out a new version of
   the loader.  We need to make sure that it is out of the way of the program
   that it will "exec", and that there is sufficient room for the brk.  */

#ifndef CONFIG_SUN3
#define ELF_ET_DYN_BASE         0xD0000000UL
#else
#define ELF_ET_DYN_BASE         0x0D800000UL
#endif

#define ELF_CORE_COPY_REGS(pr_reg, regs)				\
	/* Bleech. */							\
	pr_reg[0] = regs->d1;						\
	pr_reg[1] = regs->d2;						\
	pr_reg[2] = regs->d3;						\
	pr_reg[3] = regs->d4;						\
	pr_reg[4] = regs->d5;						\
	pr_reg[7] = regs->a0;						\
	pr_reg[8] = regs->a1;						\
	pr_reg[9] = regs->a2;						\
	pr_reg[14] = regs->d0;						\
	pr_reg[15] = rdusp();						\
	pr_reg[16] = regs->orig_d0;					\
	pr_reg[17] = regs->sr;						\
	pr_reg[18] = regs->pc;						\
	pr_reg[19] = (regs->format << 12) | regs->vector;		\
	{								\
	  struct switch_stack *sw = ((struct switch_stack *)regs) - 1;	\
	  pr_reg[5] = sw->d6;						\
	  pr_reg[6] = sw->d7;						\
	  pr_reg[10] = sw->a3;						\
	  pr_reg[11] = sw->a4;						\
	  pr_reg[12] = sw->a5;						\
	  pr_reg[13] = sw->a6;						\
	}

/* This yields a mask that user programs can use to figure out what
   instruction set this cpu supports.  */

#define ELF_HWCAP	(0)

/* This yields a string that ld.so will use to load implementation
   specific libraries for optimization.  This is more specific in
   intent than poking at uname or /proc/cpuinfo.  */

#define ELF_PLATFORM  (NULL)

#ifdef __KERNEL__
#define SET_PERSONALITY(ex, ibcs2) \
	current->personality = (ibcs2 ? PER_SVR4 : PER_LINUX)
#endif

#endif
