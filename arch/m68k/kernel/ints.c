/*
 * linux/arch/m68k/kernel/ints.c -- Linux/m68k general interrupt handling code
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * 07/03/96: Timer initialization, and thus mach_sched_init(),
 *           removed from request_irq() and moved to init_time().
 *           We should therefore consider renaming our add_isr() and
 *           remove_isr() to request_irq() and free_irq()
 *           respectively, so they are compliant with the other
 *           architectures.                                     /Jes
 * 11/07/96: Changed all add_/remove_isr() to request_/free_irq() calls.
 *           Removed irq list support, if any machine needs an irq server
 *           it must implement this itself (as it's already done), instead
 *           only default handler are used with mach_default_handler.
 *           request_irq got some flags different from other architectures:
 *           - IRQ_FLG_REPLACE : Replace an existing handler (the default one
 *                               can be replaced without this flag)
 *           - IRQ_FLG_LOCK : handler can't be replaced
 *           There are other machine depending flags, see there
 *           If you want to replace a default handler you should know what
 *           you're doing, since it might handle different other irq sources
 *           which must be served                               /Roman Zippel
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/errno.h>
#include <linux/init.h>

#include <asm/setup.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/traps.h>
#include <asm/page.h>
#include <asm/machdep.h>

#ifdef CONFIG_Q40
#include <asm/q40ints.h>
#endif

/* table for system interrupt handlers */
static irq_handler_t irq_list[SYS_IRQS];

static const char *default_names[SYS_IRQS] = {
	"spurious int", "int1 handler", "int2 handler", "int3 handler",
	"int4 handler", "int5 handler", "int6 handler", "int7 handler"
};

/* The number of spurious interrupts */
volatile unsigned int num_spurious;

#define NUM_IRQ_NODES 100
static irq_node_t nodes[NUM_IRQ_NODES];

unsigned int local_irq_count[NR_CPUS];

unsigned int local_bh_count[NR_CPUS];

static void dummy_enable_irq(unsigned int irq);
static void dummy_disable_irq(unsigned int irq);
static int dummy_request_irq(unsigned int irq,
		void (*handler) (int, void *, struct pt_regs *),
		unsigned long flags, const char *devname, void *dev_id);
static void dummy_free_irq(unsigned int irq, void *dev_id);

void (*enable_irq) (unsigned int) = dummy_enable_irq;
void (*disable_irq) (unsigned int) = dummy_disable_irq;

int (*mach_request_irq) (unsigned int, void (*)(int, void *, struct pt_regs *),
                      unsigned long, const char *, void *) = dummy_request_irq;
void (*mach_free_irq) (unsigned int, void *) = dummy_free_irq;

/*
 * void init_IRQ(void)
 *
 * Parameters:	None
 *
 * Returns:	Nothing
 *
 * This function should be called during kernel startup to initialize
 * the IRQ handling routines.
 */

void __init init_IRQ(void)
{
	int i;

	for (i = 0; i < SYS_IRQS; i++) {
		if (mach_default_handler)
			irq_list[i].handler = (*mach_default_handler)[i];
		irq_list[i].flags   = 0;
		irq_list[i].dev_id  = NULL;
		irq_list[i].devname = default_names[i];
	}

	for (i = 0; i < NUM_IRQ_NODES; i++)
		nodes[i].handler = NULL;

	mach_init_IRQ ();
}

irq_node_t *new_irq_node(void)
{
	irq_node_t *node;
	short i;

	for (node = nodes, i = NUM_IRQ_NODES-1; i >= 0; node++, i--)
		if (!node->handler)
			return node;

	printk ("new_irq_node: out of nodes\n");
	return NULL;
}

/*
 * We will keep these functions until I have convinced Linus to move
 * the declaration of them from include/linux/sched.h to
 * include/asm/irq.h.
 */
int request_irq(unsigned int irq,
		void (*handler) (int, void *, struct pt_regs *),
		unsigned long flags, const char *devname, void *dev_id)
{
	return mach_request_irq(irq, handler, flags, devname, dev_id);
}

void free_irq(unsigned int irq, void *dev_id)
{
	mach_free_irq(irq, dev_id);
}

int sys_request_irq(unsigned int irq, 
                    void (*handler)(int, void *, struct pt_regs *), 
                    unsigned long flags, const char *devname, void *dev_id)
{
	if (irq < IRQ1 || irq > IRQ7) {
		printk("%s: Incorrect IRQ %d from %s\n",
		       __FUNCTION__, irq, devname);
		return -ENXIO;
	}

#if 0
	if (!(irq_list[irq].flags & IRQ_FLG_STD)) {
		if (irq_list[irq].flags & IRQ_FLG_LOCK) {
			printk("%s: IRQ %d from %s is not replaceable\n",
			       __FUNCTION__, irq, irq_list[irq].devname);
			return -EBUSY;
		}
		if (!(flags & IRQ_FLG_REPLACE)) {
			printk("%s: %s can't replace IRQ %d from %s\n",
			       __FUNCTION__, devname, irq, irq_list[irq].devname);
			return -EBUSY;
		}
	}
#endif

	irq_list[irq].handler = handler;
	irq_list[irq].flags   = flags;
	irq_list[irq].dev_id  = dev_id;
	irq_list[irq].devname = devname;
	return 0;
}

void sys_free_irq(unsigned int irq, void *dev_id)
{
	if (irq < IRQ1 || irq > IRQ7) {
		printk("%s: Incorrect IRQ %d\n", __FUNCTION__, irq);
		return;
	}

	if (irq_list[irq].dev_id != dev_id)
		printk("%s: Removing probably wrong IRQ %d from %s\n",
		       __FUNCTION__, irq, irq_list[irq].devname);

	irq_list[irq].handler = (*mach_default_handler)[irq];
	irq_list[irq].flags   = 0;
	irq_list[irq].dev_id  = NULL;
	irq_list[irq].devname = default_names[irq];
}

/*
 * Do we need these probe functions on the m68k?
 *
 *  ... may be usefull with ISA devices
 */
unsigned long probe_irq_on (void)
{
#ifdef CONFIG_Q40
	if (MACH_IS_Q40)
		return q40_probe_irq_on();
#endif
	return 0;
}

int probe_irq_off (unsigned long irqs)
{
#ifdef CONFIG_Q40
	if (MACH_IS_Q40)
		return q40_probe_irq_off(irqs);
#endif
	return 0;
}

static void dummy_enable_irq(unsigned int irq)
{
	printk("calling uninitialized enable_irq()\n");
}

static void dummy_disable_irq(unsigned int irq)
{
	printk("calling uninitialized disable_irq()\n");
}

static int dummy_request_irq(unsigned int irq,
		void (*handler) (int, void *, struct pt_regs *),
		unsigned long flags, const char *devname, void *dev_id)
{
	printk("calling uninitialized request_irq()\n");
	return 0;
}

static void dummy_free_irq(unsigned int irq, void *dev_id)
{
	printk("calling uninitialized disable_irq()\n");
}

asmlinkage void process_int(unsigned long vec, struct pt_regs *fp)
{
	if (vec >= VEC_INT1 && vec <= VEC_INT7 && !MACH_IS_BVME6000) {
		vec -= VEC_SPUR;
		kstat.irqs[0][vec]++;
		irq_list[vec].handler(vec, irq_list[vec].dev_id, fp);
	} else {
		if (mach_process_int)
			mach_process_int(vec, fp);
		else
			panic("Can't process interrupt vector %ld\n", vec);
		return;
	}
}

int get_irq_list(char *buf)
{
	int i, len = 0;

	/* autovector interrupts */
	if (mach_default_handler) {
		for (i = 0; i < SYS_IRQS; i++) {
			len += sprintf(buf+len, "auto %2d: %10u ", i,
			               i ? kstat.irqs[0][i] : num_spurious);
				len += sprintf(buf+len, "  ");
			len += sprintf(buf+len, "%s\n", irq_list[i].devname);
		}
	}

	len += mach_get_irq_list(buf+len);
	return len;
}
