/*
 * include/asm-m68k/processor.h
 *
 * Copyright (C) 1995 Hamish Macdonald
 */

#ifndef __ASM_M68K_PROCESSOR_H
#define __ASM_M68K_PROCESSOR_H

/*
 * Default implementation of macro that returns current
 * instruction pointer ("program counter").
 */
#define current_text_addr() ({ __label__ _l; _l: &&_l;})

#include <linux/config.h>
#include <asm/segment.h>
#include <asm/fpu.h>
#include <asm/ptrace.h>

extern inline unsigned long rdusp(void) {
  	unsigned long usp;

	__asm__ __volatile__("move %/usp,%0" : "=a" (usp));
	return usp;
}

extern inline void wrusp(unsigned long usp) {
	__asm__ __volatile__("move %0,%/usp" : : "a" (usp));
}

/*
 * User space process size: 3.75GB. This is hardcoded into a few places,
 * so don't change it unless you know what you are doing.
 */
#ifndef CONFIG_SUN3
#define TASK_SIZE	(0xF0000000UL)
#else
#ifdef __ASSEMBLY__
#define TASK_SIZE	(0x0E000000)
#else
#define TASK_SIZE	(0x0E000000UL)
#endif
#endif

/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#ifndef CONFIG_SUN3
#define TASK_UNMAPPED_BASE	0xC0000000UL
#else
#define TASK_UNMAPPED_BASE	0x0A000000UL
#endif
#define TASK_UNMAPPED_ALIGN(addr, off)	PAGE_ALIGN(addr)

/*
 * Bus types
 */
#define EISA_bus 0
#define MCA_bus 0

/* 
 * if you change this structure, you must change the code and offsets
 * in m68k/machasm.S
 */
   
struct thread_struct {
	unsigned long  ksp;		/* kernel stack pointer */
	unsigned long  usp;		/* user stack pointer */
	unsigned short sr;		/* saved status register */
	unsigned short fs;		/* saved fs (sfc, dfc) */
	unsigned long  crp[2];		/* cpu root pointer */
	unsigned long  esp0;		/* points to SR of stack frame */
	unsigned long  fp[8*3];
	unsigned long  fpcntl[3];	/* fp control regs */
	unsigned char  fpstate[FPSTATESIZE];  /* floating point state */
};

#define INIT_MMAP { &init_mm, 0, 0x40000000, NULL, __pgprot(_PAGE_PRESENT|_PAGE_ACCESSED), VM_READ | VM_WRITE | VM_EXEC, 1, NULL, NULL }

#define INIT_THREAD  { \
	sizeof(init_stack) + (unsigned long) init_stack, 0, \
	PS_S, __KERNEL_DS, \
	{0, 0}, 0, {0,}, {0, 0, 0}, {0,}, \
}

/*
 * Do necessary setup to start up a newly executed thread.
 */
static inline void start_thread(struct pt_regs * regs, unsigned long pc,
				unsigned long usp)
{
	/* reads from user space */
	set_fs(USER_DS);

	regs->pc = pc;
	regs->sr &= ~0x2000;
	wrusp(usp);
}

/* Forward declaration, a strange C thing */
struct task_struct;

/* Free all resources held by a thread. */
static inline void release_thread(struct task_struct *dead_task)
{
}

extern int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags);

#define copy_segments(tsk, mm)		do { } while (0)
#define release_segments(mm)		do { } while (0)
#define forget_segments()		do { } while (0)

/*
 * Free current thread data structures etc..
 */
static inline void exit_thread(void)
{
}

/*
 * Return saved PC of a blocked thread.
 */
extern inline unsigned long thread_saved_pc(struct thread_struct *t)
{
	extern void scheduling_functions_start_here(void);
	extern void scheduling_functions_end_here(void);
	struct switch_stack *sw = (struct switch_stack *)t->ksp;
	/* Check whether the thread is blocked in resume() */
	if (sw->retpc > (unsigned long)scheduling_functions_start_here &&
	    sw->retpc < (unsigned long)scheduling_functions_end_here)
		return ((unsigned long *)sw->a6)[1];
	else
		return sw->retpc;
}

unsigned long get_wchan(struct task_struct *p);

#define	KSTK_EIP(tsk)	\
    ({			\
	unsigned long eip = 0;	 \
	if ((tsk)->thread.esp0 > PAGE_SIZE && \
	    MAP_NR((tsk)->thread.esp0) < max_mapnr) \
	      eip = ((struct pt_regs *) (tsk)->thread.esp0)->pc; \
	eip; })
#define	KSTK_ESP(tsk)	((tsk) == current ? rdusp() : (tsk)->thread.usp)

#define THREAD_SIZE (2*PAGE_SIZE)

/* Allocation and freeing of basic task resources. */
#define alloc_task_struct() \
	((struct task_struct *) __get_free_pages(GFP_KERNEL,1))
#define free_task_struct(p)	free_pages((unsigned long)(p),1)

#define init_task	(init_task_union.task)
#define init_stack	(init_task_union.stack)

#endif
