/*
 * $Id: irq.c,v 1.113 1999/09/17 17:22:56 cort Exp $
 *
 *  arch/ppc/kernel/irq.c
 *
 *  Derived from arch/i386/kernel/irq.c
 *    Copyright (C) 1992 Linus Torvalds
 *  Adapted from arch/i386 by Gary Thomas
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *  Updated and modified by Cort Dougan (cort@cs.nmt.edu)
 *    Copyright (C) 1996 Cort Dougan
 *  Adapted for Power Macintosh by Paul Mackerras
 *    Copyright (C) 1996 Paul Mackerras (paulus@cs.anu.edu.au)
 *  Amiga/APUS changes by Jesper Skov (jskov@cygnus.co.uk).
 *  
 * This file contains the code used by various IRQ handling routines:
 * asking for different IRQ's should be done through these routines
 * instead of just grabbing them. Thus setups with different IRQ numbers
 * shouldn't result in any weird surprises, and installing new handlers
 * should be easier.
 *
 * The MPC8xx has an interrupt mask in the SIU.  If a bit is set, the
 * interrupt is _enabled_.  As expected, IRQ0 is bit 0 in the 32-bit
 * mask register (of which only 16 are defined), hence the weird shifting
 * and compliment of the cached_irq_mask.  I want to be able to stuff
 * this right into the SIU SMASK register.
 * Many of the prep/chrp functions are conditional compiled on CONFIG_8xx
 * to reduce code space and undefined function references.
 */


#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/threads.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/openpic.h>
#include <linux/pci.h>
#include <linux/delay.h>

#include <asm/bitops.h>
#include <asm/hydra.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/irq.h>
#include <asm/gg2.h>
#include <asm/cache.h>
#include <asm/prom.h>
#include <asm/amigaints.h>
#include <asm/amigahw.h>
#include <asm/amigappc.h>
#include <asm/ptrace.h>

#include "local_irq.h"

extern volatile unsigned long ipi_count;
void enable_irq(unsigned int irq_nr);
void disable_irq(unsigned int irq_nr);

volatile unsigned char *chrp_int_ack_special;

#define MAXCOUNT 10000000

#define NR_MASK_WORDS	((NR_IRQS + 31) / 32)

struct irqdesc irq_desc[NR_IRQS] = {{0, 0}, };
int ppc_spurious_interrupts = 0;
unsigned int ppc_local_bh_count[NR_CPUS];
unsigned int ppc_local_irq_count[NR_CPUS];
struct irqaction *ppc_irq_action[NR_IRQS];
unsigned int ppc_cached_irq_mask[NR_MASK_WORDS];
unsigned int ppc_lost_interrupts[NR_MASK_WORDS];
atomic_t ppc_n_lost_interrupts;


/* nasty hack for shared irq's since we need to do kmalloc calls but
 * can't very early in the boot when we need to do a request irq.
 * this needs to be removed.
 * -- Cort
 */
#define IRQ_KMALLOC_ENTRIES 8
static int cache_bitmask = 0;
static struct irqaction malloc_cache[IRQ_KMALLOC_ENTRIES];
extern int mem_init_done;

void *irq_kmalloc(size_t size, int pri)
{
	unsigned int i;
	if ( mem_init_done )
		return kmalloc(size,pri);
	for ( i = 0; i < IRQ_KMALLOC_ENTRIES ; i++ )
		if ( ! ( cache_bitmask & (1<<i) ) )
		{
			cache_bitmask |= (1<<i);
			return (void *)(&malloc_cache[i]);
		}
	return 0;
}

void irq_kfree(void *ptr)
{
	unsigned int i;
	for ( i = 0 ; i < IRQ_KMALLOC_ENTRIES ; i++ )
		if ( ptr == &malloc_cache[i] )
		{
			cache_bitmask &= ~(1<<i);
			return;
		}
	kfree(ptr);
}

#ifdef CONFIG_8xx
/* Name change so we can catch standard drivers that potentially mess up
 * the internal interrupt controller on 8xx and 82xx.  Just bear with me,
 * I don't like this either and I am searching a better solution.  For
 * now, this is what I need. -- Dan
 */
int request_8xxirq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *),
#elif defined(CONFIG_APUS)
int sys_request_irq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *),
#else
int request_irq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *),
#endif
	unsigned long irqflags, const char * devname, void *dev_id)
{
	struct irqaction *old, **p, *action;
	unsigned long flags;

	if (irq >= NR_IRQS)
		return -EINVAL;
	if (!handler)
	{
		/* Free */
		for (p = &irq_desc[irq].action; (action = *p) != NULL; p = &action->next)
		{
			/* Found it - now free it */
			save_flags(flags);
			cli();
			*p = action->next;
			restore_flags(flags);
			irq_kfree(action);
			return 0;
		}
		return -ENOENT;
	}
	
	action = (struct irqaction *)
		irq_kmalloc(sizeof(struct irqaction), GFP_KERNEL);
	if (!action)
		return -ENOMEM;
	
	save_flags(flags);
	cli();
	
	action->handler = handler;
	action->flags = irqflags;					
	action->mask = 0;
	action->name = devname;
	action->dev_id = dev_id;
	action->next = NULL;
	enable_irq(irq);
	
	p = &irq_desc[irq].action;
	
	if ((old = *p) != NULL) {
		/* Can't share interrupts unless both agree to */
		if (!(old->flags & action->flags & SA_SHIRQ))
			return -EBUSY;
		/* add new interrupt at end of irq queue */
		do {
			p = &old->next;
			old = *p;
		} while (old);
	}
	*p = action;

	restore_flags(flags);	
	return 0;
}

#ifdef CONFIG_APUS
void sys_free_irq(unsigned int irq, void *dev_id)
{
	sys_request_irq(irq, NULL, 0, NULL, dev_id);
}
#else
void free_irq(unsigned int irq, void *dev_id)
{
#ifndef CONFIG_8xx
	request_irq(irq, NULL, 0, NULL, dev_id);
#else
	request_8xxirq(irq, NULL, 0, NULL, dev_id);
#endif
}
#endif

/* XXX should implement irq disable depth like on intel */
void disable_irq_nosync(unsigned int irq_nr)
{
	mask_irq(irq_nr);
}

void disable_irq(unsigned int irq_nr)
{
	mask_irq(irq_nr);
	synchronize_irq();
}

void enable_irq(unsigned int irq_nr)
{
	unmask_irq(irq_nr);
}

int get_irq_list(char *buf)
{
#ifdef CONFIG_APUS
	return apus_get_irq_list (buf);
#else
	int i, len = 0, j;
	struct irqaction * action;

	len += sprintf(buf+len, "           ");
	for (j=0; j<smp_num_cpus; j++)
		len += sprintf(buf+len, "CPU%d       ",j);
	*(char *)(buf+len++) = '\n';

	for (i = 0 ; i < NR_IRQS ; i++) {
		action = irq_desc[i].action;
		if ( !action || !action->handler )
			continue;
		len += sprintf(buf+len, "%3d: ", i);		
#ifdef __SMP__
		for (j = 0; j < smp_num_cpus; j++)
			len += sprintf(buf+len, "%10u ",
				kstat.irqs[cpu_logical_map(j)][i]);
#else		
		len += sprintf(buf+len, "%10u ", kstat_irqs(i));
#endif /* __SMP__ */
		if ( irq_desc[i].ctl )		
			len += sprintf(buf+len, " %s ", irq_desc[i].ctl->typename );
		else
			len += sprintf(buf+len, "  None      ");
		len += sprintf(buf+len, "    %s",action->name);
		for (action=action->next; action; action = action->next) {
			len += sprintf(buf+len, ", %s", action->name);
		}
		len += sprintf(buf+len, "\n");
	}
#ifdef __SMP__
	/* should this be per processor send/receive? */
	len += sprintf(buf+len, "IPI: %10lu\n", ipi_count);
#endif		
	len += sprintf(buf+len, "BAD: %10u\n", ppc_spurious_interrupts);
	return len;
#endif /* CONFIG_APUS */
}

/*
 * Eventually, this should take an array of interrupts and an array size
 * so it can dispatch multiple interrupts.
 */
void ppc_irq_dispatch_handler(struct pt_regs *regs, int irq)
{
	int status;
	struct irqaction *action;
	int cpu = smp_processor_id();

	mask_and_ack_irq(irq);
	status = 0;
	action = irq_desc[irq].action;
	kstat.irqs[cpu][irq]++;
	if (action && action->handler) {
		if (!(action->flags & SA_INTERRUPT))
			__sti();
		do { 
			status |= action->flags;
			action->handler(irq, action->dev_id, regs);
			action = action->next;
		} while ( action );
		__cli();
		unmask_irq(irq);
	} else {
		ppc_spurious_interrupts++;
		disable_irq( irq );
	}
}

asmlinkage void do_IRQ(struct pt_regs *regs, int isfake)
{
	int cpu = smp_processor_id();
	int irq;

        hardirq_enter( cpu );

	/* every arch is required to have a get_irq -- Cort */
	irq = ppc_md.get_irq( regs );
	if ( irq < 0 )
	{
		/* -2 means ignore, already handled */
		if (irq != -2)
		{
			printk(KERN_DEBUG "Bogus interrupt %d from PC = %lx\n",
			       irq, regs->nip);
			ppc_spurious_interrupts++;
		}
		goto out;
	}
	ppc_irq_dispatch_handler( regs, irq );
	if ( ppc_md.post_irq )
		ppc_md.post_irq( irq );

 out:	
        hardirq_exit( cpu );
}



unsigned long probe_irq_on (void)
{
	return 0;
}

int probe_irq_off (unsigned long irqs)
{
	return 0;
}

void __init init_IRQ(void)
{
	static int once = 0;

	if ( once )
		return;
	else
		once++;
	
	ppc_md.init_IRQ();
}

#ifdef __SMP__
unsigned char global_irq_holder = NO_PROC_ID;
unsigned volatile int global_irq_lock;
atomic_t global_irq_count;

atomic_t global_bh_count;
atomic_t global_bh_lock;

static void show(char * str)
{
	int i;
	unsigned long *stack;
	int cpu = smp_processor_id();

	printk("\n%s, CPU %d:\n", str, cpu);
	printk("irq:  %d [%d %d]\n",
	       atomic_read(&global_irq_count),
	       ppc_local_irq_count[0],
	       ppc_local_irq_count[1]);
	printk("bh:   %d [%d %d]\n",
	       atomic_read(&global_bh_count),
	       ppc_local_bh_count[0],
	       ppc_local_bh_count[1]);
	stack = (unsigned long *) &str;
	for (i = 40; i ; i--) {
		unsigned long x = *++stack;
		if (x > (unsigned long) &init_task_union && x < (unsigned long) &vsprintf) {
			printk("<[%08lx]> ", x);
		}
	}
}

static inline void wait_on_bh(void)
{
	int count = MAXCOUNT;
	do {
		if (!--count) {
			show("wait_on_bh");
			count = ~0;
		}
		/* nothing .. wait for the other bh's to go away */
	} while (atomic_read(&global_bh_count) != 0);
}


static inline void wait_on_irq(int cpu)
{
	int count = MAXCOUNT;

	for (;;) {

		/*
		 * Wait until all interrupts are gone. Wait
		 * for bottom half handlers unless we're
		 * already executing in one..
		 */
		if (!atomic_read(&global_irq_count)) {
			if (ppc_local_bh_count[cpu]
			    || !atomic_read(&global_bh_count))
				break;
		}

		/* Duh, we have to loop. Release the lock to avoid deadlocks */
		clear_bit(0,&global_irq_lock);

		for (;;) {
			if (!--count) {
				show("wait_on_irq");
				count = ~0;
			}
			__sti();
			/* don't worry about the lock race Linus found
			 * on intel here. -- Cort
			 */
			__cli();
			if (atomic_read(&global_irq_count))
				continue;
			if (global_irq_lock)
				continue;
			if (!ppc_local_bh_count[cpu]
			    && atomic_read(&global_bh_count))
				continue;
			if (!test_and_set_bit(0,&global_irq_lock))
				break;
		}
	}
}

/*
 * This is called when we want to synchronize with
 * bottom half handlers. We need to wait until
 * no other CPU is executing any bottom half handler.
 *
 * Don't wait if we're already running in an interrupt
 * context or are inside a bh handler.
 */
void synchronize_bh(void)
{
	if (atomic_read(&global_bh_count) && !in_interrupt())
		wait_on_bh();
}

/*
 * This is called when we want to synchronize with
 * interrupts. We may for example tell a device to
 * stop sending interrupts: but to make sure there
 * are no interrupts that are executing on another
 * CPU we need to call this function.
 */
void synchronize_irq(void)
{
	if (atomic_read(&global_irq_count)) {
		/* Stupid approach */
		cli();
		sti();
	}
}

static inline void get_irqlock(int cpu)
{
	unsigned int loops = MAXCOUNT;

	if (test_and_set_bit(0,&global_irq_lock)) {
		/* do we already hold the lock? */
		if ((unsigned char) cpu == global_irq_holder)
			return;
		/* Uhhuh.. Somebody else got it. Wait.. */
		do {
			do {
				if (loops-- == 0) {
					printk("get_irqlock(%d) waiting, global_irq_holder=%d\n", cpu, global_irq_holder);
#ifdef CONFIG_XMON
					xmon(0);
#endif
				}
			} while (test_bit(0,&global_irq_lock));
		} while (test_and_set_bit(0,&global_irq_lock));		
	}
	/* 
	 * We also need to make sure that nobody else is running
	 * in an interrupt context. 
	 */
	wait_on_irq(cpu);

	/*
	 * Ok, finally..
	 */
	global_irq_holder = cpu;
}

/*
 * A global "cli()" while in an interrupt context
 * turns into just a local cli(). Interrupts
 * should use spinlocks for the (very unlikely)
 * case that they ever want to protect against
 * each other.
 *
 * If we already have local interrupts disabled,
 * this will not turn a local disable into a
 * global one (problems with spinlocks: this makes
 * save_flags+cli+sti usable inside a spinlock).
 */
void __global_cli(void)
{
	unsigned long flags;
	
	__save_flags(flags);
	if (flags & (1 << 15)) {
		int cpu = smp_processor_id();
		__cli();
		if (!ppc_local_irq_count[cpu])
			get_irqlock(cpu);
	}
}

void __global_sti(void)
{
	int cpu = smp_processor_id();

	if (!ppc_local_irq_count[cpu])
		release_irqlock(cpu);
	__sti();
}

/*
 * SMP flags value to restore to:
 * 0 - global cli
 * 1 - global sti
 * 2 - local cli
 * 3 - local sti
 */
unsigned long __global_save_flags(void)
{
	int retval;
	int local_enabled;
	unsigned long flags;

	__save_flags(flags);
	local_enabled = (flags >> 15) & 1;
	/* default to local */
	retval = 2 + local_enabled;

	/* check for global flags if we're not in an interrupt */
	if (!ppc_local_irq_count[smp_processor_id()]) {
		if (local_enabled)
			retval = 1;
		if (global_irq_holder == (unsigned char) smp_processor_id())
			retval = 0;
	}
	return retval;
}

int
tb(long vals[],
   int  max_size)
{
   register unsigned long *orig_sp __asm__ ("r1");
   register unsigned long lr __asm__ ("r3");
   unsigned long *sp;
   int i;

   asm volatile ("mflr 3");
   vals[0] = lr;
   sp = (unsigned long *) *orig_sp;
   sp = (unsigned long *) *sp;
   for (i=1; i<max_size; i++) {
      if (sp == 0) {
         break;
      }

      vals[i] = *(sp+1);
      sp = (unsigned long *) *sp;
   }

   return i;
}

void __global_restore_flags(unsigned long flags)
{
	switch (flags) {
	case 0:
		__global_cli();
		break;
	case 1:
		__global_sti();
		break;
	case 2:
		__cli();
		break;
	case 3:
		__sti();
		break;
	default:
	{
		unsigned long trace[5];
                int           count;
                int           i;

		printk("global_restore_flags: %08lx (%08lx)\n",
			flags, (&flags)[-1]);
                count = tb(trace, 5);
                printk("tb:");
                for(i=0; i<count; i++) {
			printk(" %8.8lx", trace[i]);
		}
		printk("\n");
	}
	}
}
#endif /* __SMP__ */

