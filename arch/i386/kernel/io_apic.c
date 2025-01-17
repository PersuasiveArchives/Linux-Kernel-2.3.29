/*
 *	Intel IO-APIC support for multi-Pentium hosts.
 *
 *	Copyright (C) 1997, 1998, 1999 Ingo Molnar, Hajnalka Szabo
 *
 *	Many thanks to Stig Venaas for trying out countless experimental
 *	patches and reporting/debugging problems patiently!
 *
 *	(c) 1999, Multiple IO-APIC support, developed by
 *	Ken-ichi Yaku <yaku@css1.kbnes.nec.co.jp> and
 *      Hidemi Kishimoto <kisimoto@css1.kbnes.nec.co.jp>,
 *	further tested and cleaned up by Zach Brown <zab@redhat.com>
 *	and Ingo Molnar <mingo@redhat.com>
 */

#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/desc.h>

#include <linux/irq.h>

#undef __init
#define __init

/*
 * volatile is justified in this case, IO-APIC register contents
 * might change spontaneously, GCC should not cache it
 */
#define IO_APIC_BASE(idx) ((volatile int *)__fix_to_virt(FIX_IO_APIC_BASE_0 + idx))

extern int nmi_watchdog;

/*
 * The structure of the IO-APIC:
 */

struct IO_APIC_reg_00 {
	__u32	__reserved_2	: 24,
		ID		:  4,
		__reserved_1	:  4;
} __attribute__ ((packed));

struct IO_APIC_reg_01 {
	__u32	version		:  8,
		__reserved_2	:  8,
		entries		:  8,
		__reserved_1	:  8;
} __attribute__ ((packed));

struct IO_APIC_reg_02 {
	__u32	__reserved_2	: 24,
		arbitration	:  4,
		__reserved_1	:  4;
} __attribute__ ((packed));

/*
 * # of IO-APICs and # of IRQ routing registers
 */
int nr_ioapics = 0;
int nr_ioapic_registers[MAX_IO_APICS];

enum ioapic_irq_destination_types {
	dest_Fixed = 0,
	dest_LowestPrio = 1,
	dest_SMI = 2,
	dest__reserved_1 = 3,
	dest_NMI = 4,
	dest_INIT = 5,
	dest__reserved_2 = 6,
	dest_ExtINT = 7
};

struct IO_APIC_route_entry {
	__u32	vector		:  8,
		delivery_mode	:  3,	/* 000: FIXED
					 * 001: lowest prio
					 * 111: ExtINT
					 */
		dest_mode	:  1,	/* 0: physical, 1: logical */
		delivery_status	:  1,
		polarity	:  1,
		irr		:  1,
		trigger		:  1,	/* 0: edge, 1: level */
		mask		:  1,	/* 0: enabled, 1: disabled */
		__reserved_2	: 15;

	union {		struct { __u32
					__reserved_1	: 24,
					physical_dest	:  4,
					__reserved_2	:  4;
			} physical;

			struct { __u32
					__reserved_1	: 24,
					logical_dest	:  8;
			} logical;
	} dest;

} __attribute__ ((packed));

/*
 * MP-BIOS irq configuration table structures:
 */

struct mpc_config_ioapic mp_ioapics[MAX_IO_APICS];/* I/O APIC entries */
int mp_irq_entries = 0;				/* # of MP IRQ source entries */
struct mpc_config_intsrc mp_irqs[MAX_IRQ_SOURCES];
						/* MP IRQ source entries */
int mpc_default_type = 0;			/* non-0 if default (table-less)
						   MP configuration */


/*
 * This is performance-critical, we want to do it O(1)
 *
 * the indexing order of this array favors 1:1 mappings
 * between pins and IRQs.
 */

static inline unsigned int io_apic_read(unsigned int apic, unsigned int reg)
{
	*IO_APIC_BASE(apic) = reg;
	return *(IO_APIC_BASE(apic)+4);
}

static inline void io_apic_write(unsigned int apic, unsigned int reg, unsigned int value)
{
	*IO_APIC_BASE(apic) = reg;
	*(IO_APIC_BASE(apic)+4) = value;
}

/*
 * Re-write a value: to be used for read-modify-write
 * cycles where the read already set up the index register.
 */
static inline void io_apic_modify(unsigned int apic, unsigned int value)
{
	*(IO_APIC_BASE(apic)+4) = value;
}

/*
 * Synchronize the IO-APIC and the CPU by doing
 * a dummy read from the IO-APIC
 */
static inline void io_apic_sync(unsigned int apic)
{
	(void) *(IO_APIC_BASE(apic)+4);
}

/*
 * Rough estimation of how many shared IRQs there are, can
 * be changed anytime.
 */
#define MAX_PLUS_SHARED_IRQS NR_IRQS
#define PIN_MAP_SIZE (MAX_PLUS_SHARED_IRQS + NR_IRQS)

static struct irq_pin_list {
	int apic, pin, next;
} irq_2_pin[PIN_MAP_SIZE];

/*
 * The common case is 1:1 IRQ<->pin mappings. Sometimes there are
 * shared ISA-space IRQs, so we have to support them. We are super
 * fast in the common case, and fast for shared ISA-space IRQs.
 */
static void add_pin_to_irq(unsigned int irq, int apic, int pin)
{
	static int first_free_entry = NR_IRQS;
	struct irq_pin_list *entry = irq_2_pin + irq;

	while (entry->next)
		entry = irq_2_pin + entry->next;

	if (entry->pin != -1) {
		entry->next = first_free_entry;
		entry = irq_2_pin + entry->next;
		if (++first_free_entry >= PIN_MAP_SIZE)
			panic("io_apic.c: whoops");
	}
	entry->apic = apic;
	entry->pin = pin;
}

#define DO_ACTION(name,R,ACTION, FINAL)					\
									\
static void name##_IO_APIC_irq(unsigned int irq)			\
{									\
	int pin;							\
	struct irq_pin_list *entry = irq_2_pin + irq;			\
									\
	for (;;) {							\
		unsigned int reg;					\
		pin = entry->pin;					\
		if (pin == -1)						\
			break;						\
		reg = io_apic_read(entry->apic, 0x10 + R + pin*2);	\
		reg ACTION;						\
		io_apic_modify(entry->apic, reg);			\
		if (!entry->next)					\
			break;						\
		entry = irq_2_pin + entry->next;			\
	}								\
	FINAL;								\
}

DO_ACTION( mask,    0, |= 0x00010000, io_apic_sync(entry->apic))/* mask = 1 */
DO_ACTION( unmask,  0, &= 0xfffeffff, )				/* mask = 0 */

void clear_IO_APIC_pin(unsigned int apic, unsigned int pin)
{
	struct IO_APIC_route_entry entry;

	/*
	 * Disable it in the IO-APIC irq-routing table:
	 */
	memset(&entry, 0, sizeof(entry));
	entry.mask = 1;
	io_apic_write(apic, 0x10 + 2 * pin, *(((int *)&entry) + 0));
	io_apic_write(apic, 0x11 + 2 * pin, *(((int *)&entry) + 1));
}

static void clear_IO_APIC (void)
{
	int apic, pin;

	for (apic = 0; apic < nr_ioapics; apic++)
		for (pin = 0; pin < nr_ioapic_registers[apic]; pin++)
			clear_IO_APIC_pin(apic, pin);
}

/*
 * support for broken MP BIOSs, enables hand-redirection of PIRQ0-7 to
 * specific CPU-side IRQs.
 */

#define MAX_PIRQS 8
int pirq_entries [MAX_PIRQS];
int pirqs_enabled;

static int __init ioapic_setup(char *str)
{
	extern int skip_ioapic_setup;	/* defined in arch/i386/kernel/smp.c */

	skip_ioapic_setup = 1;
	return 1;
}

__setup("noapic", ioapic_setup);

static int __init ioapic_pirq_setup(char *str)
{
	int i, max;
	int ints[MAX_PIRQS+1];

	get_options(str, ARRAY_SIZE(ints), ints);

	for (i = 0; i < MAX_PIRQS; i++)
		pirq_entries[i] = -1;

	pirqs_enabled = 1;
	printk("PIRQ redirection, working around broken MP-BIOS.\n");
	max = MAX_PIRQS;
	if (ints[0] < MAX_PIRQS)
		max = ints[0];

	for (i = 0; i < max; i++) {
		printk("... PIRQ%d -> IRQ %d\n", i, ints[i+1]);
		/*
		 * PIRQs are mapped upside down, usually.
		 */
		pirq_entries[MAX_PIRQS-i-1] = ints[i+1];
	}
	return 1;
}

__setup("pirq=", ioapic_pirq_setup);

/*
 * Find the IRQ entry number of a certain pin.
 */
static int __init find_irq_entry(int apic, int pin, int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++)
		if ( (mp_irqs[i].mpc_irqtype == type) &&
			(mp_irqs[i].mpc_dstapic == mp_ioapics[apic].mpc_apicid) &&
			(mp_irqs[i].mpc_dstirq == pin))

			return i;

	return -1;
}

/*
 * Find the pin to which IRQ0 (ISA) is connected
 */
static int __init find_timer_pin(int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++) {
		int lbus = mp_irqs[i].mpc_srcbus;

		if ((mp_bus_id_to_type[lbus] == MP_BUS_ISA ||
		     mp_bus_id_to_type[lbus] == MP_BUS_EISA) &&
		    (mp_irqs[i].mpc_irqtype == type) &&
		    (mp_irqs[i].mpc_srcbusirq == 0x00))

			return mp_irqs[i].mpc_dstirq;
	}
	return -1;
}

/*
 * Find a specific PCI IRQ entry.
 * Not an __init, possibly needed by modules
 */
static int __init pin_2_irq(int idx, int apic, int pin);
int IO_APIC_get_PCI_irq_vector(int bus, int slot, int pci_pin)
{
	int apic, i;

	for (i = 0; i < mp_irq_entries; i++) {
		int lbus = mp_irqs[i].mpc_srcbus;

		for (apic = 0; apic < nr_ioapics; apic++)
			if (mp_ioapics[apic].mpc_apicid == mp_irqs[i].mpc_dstapic)
				break;

		if ((apic || IO_APIC_IRQ(mp_irqs[i].mpc_dstirq)) &&
		    (mp_bus_id_to_type[lbus] == MP_BUS_PCI) &&
		    !mp_irqs[i].mpc_irqtype &&
		    (bus == mp_bus_id_to_pci_bus[mp_irqs[i].mpc_srcbus]) &&
		    (slot == ((mp_irqs[i].mpc_srcbusirq >> 2) & 0x1f)) &&
		    (pci_pin == (mp_irqs[i].mpc_srcbusirq & 3)))

			return pin_2_irq(i,apic,mp_irqs[i].mpc_dstirq);
	}
	return -1;
}

/*
 * EISA Edge/Level control register, ELCR
 */
static int __init EISA_ELCR(unsigned int irq)
{
	if (irq < 16) {
		unsigned int port = 0x4d0 + (irq >> 3);
		return (inb(port) >> (irq & 7)) & 1;
	}
	printk("Broken MPtable reports ISA irq %d\n", irq);
	return 0;
}

/* EISA interrupts are always polarity zero and can be edge or level
 * trigger depending on the ELCR value.  If an interrupt is listed as
 * EISA conforming in the MP table, that means its trigger type must
 * be read in from the ELCR */

#define default_EISA_trigger(idx)	(EISA_ELCR(mp_irqs[idx].mpc_dstirq))
#define default_EISA_polarity(idx)	(0)

/* ISA interrupts are always polarity zero edge triggered, even when
 * listed as conforming in the MP table. */

#define default_ISA_trigger(idx)	(0)
#define default_ISA_polarity(idx)	(0)

static int __init MPBIOS_polarity(int idx)
{
	int bus = mp_irqs[idx].mpc_srcbus;
	int polarity;

	/*
	 * Determine IRQ line polarity (high active or low active):
	 */
	switch (mp_irqs[idx].mpc_irqflag & 3)
	{
		case 0: /* conforms, ie. bus-type dependent polarity */
		{
			switch (mp_bus_id_to_type[bus])
			{
				case MP_BUS_ISA: /* ISA pin */
				{
					polarity = default_ISA_polarity(idx);
					break;
				}
				case MP_BUS_EISA:
				{
					polarity = default_EISA_polarity(idx);
					break;
				}
				case MP_BUS_PCI: /* PCI pin */
				{
					polarity = 1;
					break;
				}
				default:
				{
					printk("broken BIOS!!\n");
					polarity = 1;
					break;
				}
			}
			break;
		}
		case 1: /* high active */
		{
			polarity = 0;
			break;
		}
		case 2: /* reserved */
		{
			printk("broken BIOS!!\n");
			polarity = 1;
			break;
		}
		case 3: /* low active */
		{
			polarity = 1;
			break;
		}
		default: /* invalid */
		{
			printk("broken BIOS!!\n");
			polarity = 1;
			break;
		}
	}
	return polarity;
}

static int __init MPBIOS_trigger(int idx)
{
	int bus = mp_irqs[idx].mpc_srcbus;
	int trigger;

	/*
	 * Determine IRQ trigger mode (edge or level sensitive):
	 */
	switch ((mp_irqs[idx].mpc_irqflag>>2) & 3)
	{
		case 0: /* conforms, ie. bus-type dependent */
		{
			switch (mp_bus_id_to_type[bus])
			{
				case MP_BUS_ISA:
				{
					trigger = default_ISA_trigger(idx);
					break;
				}
				case MP_BUS_EISA:
				{
					trigger = default_EISA_trigger(idx);
					break;
				}
				case MP_BUS_PCI: /* PCI pin, level */
				{
					trigger = 1;
					break;
				}
				default:
				{
					printk("broken BIOS!!\n");
					trigger = 1;
					break;
				}
			}
			break;
		}
		case 1: /* edge */
		{
			trigger = 0;
			break;
		}
		case 2: /* reserved */
		{
			printk("broken BIOS!!\n");
			trigger = 1;
			break;
		}
		case 3: /* level */
		{
			trigger = 1;
			break;
		}
		default: /* invalid */
		{
			printk("broken BIOS!!\n");
			trigger = 0;
			break;
		}
	}
	return trigger;
}

static inline int irq_polarity(int idx)
{
	return MPBIOS_polarity(idx);
}

static inline int irq_trigger(int idx)
{
	return MPBIOS_trigger(idx);
}

static int __init pin_2_irq(int idx, int apic, int pin)
{
	int irq, i;
	int bus = mp_irqs[idx].mpc_srcbus;

	/*
	 * Debugging check, we are in big trouble if this message pops up!
	 */
	if (mp_irqs[idx].mpc_dstirq != pin)
		printk("broken BIOS or MPTABLE parser, ayiee!!\n");

	switch (mp_bus_id_to_type[bus])
	{
		case MP_BUS_ISA: /* ISA pin */
		case MP_BUS_EISA:
		{
			irq = mp_irqs[idx].mpc_srcbusirq;
			break;
		}
		case MP_BUS_PCI: /* PCI pin */
		{
			/*
			 * PCI IRQs are mapped in order
			 */
			i = irq = 0;
			while (i < apic)
				irq += nr_ioapic_registers[i++];
			irq += pin;
			break;
		}
		default:
		{
			printk("unknown bus type %d.\n",bus); 
			irq = 0;
			break;
		}
	}

	/*
	 * PCI IRQ command line redirection. Yes, limits are hardcoded.
	 */
	if ((pin >= 16) && (pin <= 23)) {
		if (pirq_entries[pin-16] != -1) {
			if (!pirq_entries[pin-16]) {
				printk("disabling PIRQ%d\n", pin-16);
			} else {
				irq = pirq_entries[pin-16];
				printk("using PIRQ%d -> IRQ %d\n",
						pin-16, irq);
			}
		}
	}
	return irq;
}

static inline int IO_APIC_irq_trigger(int irq)
{
	int apic, idx, pin;

	for (apic = 0; apic < nr_ioapics; apic++) {
		for (pin = 0; pin < nr_ioapic_registers[apic]; pin++) {
			idx = find_irq_entry(apic,pin,mp_INT);
			if ((idx != -1) && (irq == pin_2_irq(idx,apic,pin)))
				return irq_trigger(idx);
		}
	}
	/*
	 * nonexistent IRQs are edge default
	 */
	return 0;
}

int irq_vector[NR_IRQS] = { IRQ0_TRAP_VECTOR , 0 };

static int __init assign_irq_vector(int irq)
{
	static int current_vector = IRQ0_TRAP_VECTOR, offset = 0;
	if (IO_APIC_VECTOR(irq) > 0)
		return IO_APIC_VECTOR(irq);
	if (current_vector == 0xFF)
		panic("ran out of interrupt sources!");
next:
	current_vector += 8;
	if (current_vector == SYSCALL_VECTOR)
		goto next;

	if (current_vector > 0xFF) {
		offset++;
		current_vector = IRQ0_TRAP_VECTOR + offset;
	}

	IO_APIC_VECTOR(irq) = current_vector;
	return current_vector;
}

extern void (*interrupt[NR_IRQS])(void);
static struct hw_interrupt_type ioapic_level_irq_type;
static struct hw_interrupt_type ioapic_edge_irq_type;

void __init setup_IO_APIC_irqs(void)
{
	struct IO_APIC_route_entry entry;
	int apic, pin, idx, irq, first_notcon = 1, vector;

	printk("init IO_APIC IRQs\n");

	for (apic = 0; apic < nr_ioapics; apic++) {
	for (pin = 0; pin < nr_ioapic_registers[apic]; pin++) {

		/*
		 * add it to the IO-APIC irq-routing table:
		 */
		memset(&entry,0,sizeof(entry));

		entry.delivery_mode = dest_LowestPrio;
		entry.dest_mode = 1;			/* logical delivery */
		entry.mask = 0;				/* enable IRQ */
		entry.dest.logical.logical_dest = APIC_ALL_CPUS; /* all CPUs */

		idx = find_irq_entry(apic,pin,mp_INT);
		if (idx == -1) {
			if (first_notcon) {
				printk(" IO-APIC (apicid-pin) %d-%d", mp_ioapics[apic].mpc_apicid, pin);
				first_notcon = 0;
			} else
				printk(", %d-%d", mp_ioapics[apic].mpc_apicid, pin);
			continue;
		}

		entry.trigger = irq_trigger(idx);
		entry.polarity = irq_polarity(idx);

		if (irq_trigger(idx)) {
			entry.trigger = 1;
			entry.mask = 1;
			entry.dest.logical.logical_dest = APIC_ALL_CPUS;
		}

		irq = pin_2_irq(idx, apic, pin);
		add_pin_to_irq(irq, apic, pin);

		if (!apic && !IO_APIC_IRQ(irq))
			continue;

		if (IO_APIC_IRQ(irq)) {
			vector = assign_irq_vector(irq);
			entry.vector = vector;

			if (IO_APIC_irq_trigger(irq))
				irq_desc[irq].handler = &ioapic_level_irq_type;
			else
				irq_desc[irq].handler = &ioapic_edge_irq_type;

			set_intr_gate(vector, interrupt[irq]);
		
			if (!apic && (irq < 16))
				disable_8259A_irq(irq);
		}
		io_apic_write(apic, 0x11+2*pin, *(((int *)&entry)+1));
		io_apic_write(apic, 0x10+2*pin, *(((int *)&entry)+0));
	}
	}

	if (!first_notcon)
		printk(" not connected.\n");
}

/*
 * Set up the 8259A-master output pin as broadcast to all
 * CPUs.
 */
void __init setup_ExtINT_IRQ0_pin(unsigned int pin, int vector)
{
	struct IO_APIC_route_entry entry;

	memset(&entry,0,sizeof(entry));

	disable_8259A_irq(0);

	apic_readaround(APIC_LVT0);
	apic_write(APIC_LVT0, 0x00010700);	// mask LVT0

	init_8259A(1);

	/*
	 * We use logical delivery to get the timer IRQ
	 * to the first CPU.
	 */
	entry.dest_mode = 1;				/* logical delivery */
	entry.mask = 0;					/* unmask IRQ now */
	entry.dest.logical.logical_dest = APIC_ALL_CPUS;
	entry.delivery_mode = dest_LowestPrio;
	entry.polarity = 0;
	entry.trigger = 0;
	entry.vector = vector;

	/*
	 * The timer IRQ doesnt have to know that behind the
	 * scene we have a 8259A-master in AEOI mode ...
	 */
	irq_desc[0].handler = &ioapic_edge_irq_type;

	/*
	 * Add it to the IO-APIC irq-routing table:
	 */
	io_apic_write(0, 0x10+2*pin, *(((int *)&entry)+0));
	io_apic_write(0, 0x11+2*pin, *(((int *)&entry)+1));

	enable_8259A_irq(0);
}

void __init UNEXPECTED_IO_APIC(void)
{
	printk(" WARNING: unexpected IO-APIC, please mail\n");
	printk("          to linux-smp@vger.rutgers.edu\n");
}

void __init print_IO_APIC(void)
{
	int apic, i;
	struct IO_APIC_reg_00 reg_00;
	struct IO_APIC_reg_01 reg_01;
	struct IO_APIC_reg_02 reg_02;

 	printk("number of MP IRQ sources: %d.\n", mp_irq_entries);
	for (i = 0; i < nr_ioapics; i++)
		printk("number of IO-APIC #%d registers: %d.\n", mp_ioapics[i].mpc_apicid, nr_ioapic_registers[i]);

	/*
	 * We are a bit conservative about what we expect.  We have to
	 * know about every hardware change ASAP.
	 */
	printk("testing the IO APIC.......................\n");

	for (apic = 0; apic < nr_ioapics; apic++) {

	*(int *)&reg_00 = io_apic_read(apic, 0);
	*(int *)&reg_01 = io_apic_read(apic, 1);
	if (reg_01.version >= 0x10)
		*(int *)&reg_02 = io_apic_read(apic, 2);

	printk("\nIO APIC #%d......\n", mp_ioapics[apic].mpc_apicid);
	printk(".... register #00: %08X\n", *(int *)&reg_00);
	printk(".......    : physical APIC id: %02X\n", reg_00.ID);
	if (reg_00.__reserved_1 || reg_00.__reserved_2)
		UNEXPECTED_IO_APIC();

	printk(".... register #01: %08X\n", *(int *)&reg_01);
	printk(".......     : max redirection entries: %04X\n", reg_01.entries);
	if (	(reg_01.entries != 0x0f) && /* older (Neptune) boards */
		(reg_01.entries != 0x17) && /* typical ISA+PCI boards */
		(reg_01.entries != 0x1b) && /* Compaq Proliant boards */
		(reg_01.entries != 0x1f) && /* dual Xeon boards */
		(reg_01.entries != 0x22) && /* bigger Xeon boards */
		(reg_01.entries != 0x2E) &&
		(reg_01.entries != 0x3F)
	)
		UNEXPECTED_IO_APIC();

	printk(".......     : IO APIC version: %04X\n", reg_01.version);
	if (	(reg_01.version != 0x01) && /* 82489DX IO-APICs */
		(reg_01.version != 0x10) && /* oldest IO-APICs */
		(reg_01.version != 0x11) && /* Pentium/Pro IO-APICs */
		(reg_01.version != 0x13)    /* Xeon IO-APICs */
	)
		UNEXPECTED_IO_APIC();
	if (reg_01.__reserved_1 || reg_01.__reserved_2)
		UNEXPECTED_IO_APIC();

	if (reg_01.version >= 0x10) {
		printk(".... register #02: %08X\n", *(int *)&reg_02);
		printk(".......     : arbitration: %02X\n", reg_02.arbitration);
		if (reg_02.__reserved_1 || reg_02.__reserved_2)
			UNEXPECTED_IO_APIC();
	}

	printk(".... IRQ redirection table:\n");

	printk(" NR Log Phy ");
	printk("Mask Trig IRR Pol Stat Dest Deli Vect:   \n");

	for (i = 0; i <= reg_01.entries; i++) {
		struct IO_APIC_route_entry entry;

		*(((int *)&entry)+0) = io_apic_read(apic, 0x10+i*2);
		*(((int *)&entry)+1) = io_apic_read(apic, 0x11+i*2);

		printk(" %02x %03X %02X  ",
			i,
			entry.dest.logical.logical_dest,
			entry.dest.physical.physical_dest
		);

		printk("%1d    %1d    %1d   %1d   %1d    %1d    %1d    %02X\n",
			entry.mask,
			entry.trigger,
			entry.irr,
			entry.polarity,
			entry.delivery_status,
			entry.dest_mode,
			entry.delivery_mode,
			entry.vector
		);
	}
	}
	printk(KERN_DEBUG "IRQ to pin mappings:\n");
	for (i = 0; i < NR_IRQS; i++) {
		struct irq_pin_list *entry = irq_2_pin + i;
		if (entry->pin < 0)
			continue;
		printk(KERN_DEBUG "IRQ%d ", i);
		for (;;) {
			printk("-> %d", entry->pin);
			if (!entry->next)
				break;
			entry = irq_2_pin + entry->next;
		}
		printk("\n");
	}

	printk(".................................... done.\n");

	return;
}

static void print_APIC_bitfield (int base)
{
	unsigned int v;
	int i, j;

	printk("0123456789abcdef0123456789abcdef\n");
	for (i = 0; i < 8; i++) {
		v = apic_read(base + i*0x10);
		for (j = 0; j < 32; j++) {
			if (v & (1<<j))
				printk("1");
			else
				printk("0");
		}
		printk("\n");
	}
}

void /*__init*/ print_local_APIC(void * dummy)
{
	unsigned int v, ver, maxlvt;

	printk("\nprinting local APIC contents on CPU#%d/%d:\n",
		smp_processor_id(), hard_smp_processor_id());
	v = apic_read(APIC_ID);
	printk("... APIC ID:      %08x (%01x)\n", v, GET_APIC_ID(v));
	v = apic_read(APIC_LVR);
	printk("... APIC VERSION: %08x\n", v);
	ver = GET_APIC_VERSION(v);
	maxlvt = get_maxlvt();

	v = apic_read(APIC_TASKPRI);
	printk("... APIC TASKPRI: %08x (%02x)\n", v, v & APIC_TPRI_MASK);

	if (APIC_INTEGRATED(ver)) {			/* !82489DX */
		v = apic_read(APIC_ARBPRI);
		printk("... APIC ARBPRI: %08x (%02x)\n", v,
			v & APIC_ARBPRI_MASK);
		v = apic_read(APIC_PROCPRI);
		printk("... APIC PROCPRI: %08x\n", v);
	}

	v = apic_read(APIC_EOI);
	printk("... APIC EOI: %08x\n", v);
	v = apic_read(APIC_LDR);
	printk("... APIC LDR: %08x\n", v);
	v = apic_read(APIC_DFR);
	printk("... APIC DFR: %08x\n", v);
	v = apic_read(APIC_SPIV);
	printk("... APIC SPIV: %08x\n", v);

	printk("... APIC ISR field:\n");
	print_APIC_bitfield(APIC_ISR);
	printk("... APIC TMR field:\n");
	print_APIC_bitfield(APIC_TMR);
	printk("... APIC IRR field:\n");
	print_APIC_bitfield(APIC_IRR);

	if (APIC_INTEGRATED(ver)) {		/* !82489DX */
		/*
		 * Due to the Pentium erratum 3AP.
		 */
		if (maxlvt > 3) {
			apic_readaround(APIC_SPIV); // not strictly necessery
			apic_write(APIC_ESR, 0);
		}
		v = apic_read(APIC_ESR);
		printk("... APIC ESR: %08x\n", v);
	}

	v = apic_read(APIC_ICR);
	printk("... APIC ICR: %08x\n", v);
	v = apic_read(APIC_ICR2);
	printk("... APIC ICR2: %08x\n", v);

	v = apic_read(APIC_LVTT);
	printk("... APIC LVTT: %08x\n", v);

	if (maxlvt > 3) {                       /* PC is LVT#4. */
		v = apic_read(APIC_LVTPC);
		printk("... APIC LVTPC: %08x\n", v);
	}
	v = apic_read(APIC_LVT0);
	printk("... APIC LVT0: %08x\n", v);
	v = apic_read(APIC_LVT1);
	printk("... APIC LVT1: %08x\n", v);

	if (maxlvt > 2) {			/* ERR is LVT#3. */
		v = apic_read(APIC_LVTERR);
		printk("... APIC LVTERR: %08x\n", v);
	}

	v = apic_read(APIC_TMICT);
	printk("... APIC TMICT: %08x\n", v);
	v = apic_read(APIC_TMCCT);
	printk("... APIC TMCCT: %08x\n", v);
	v = apic_read(APIC_TDCR);
	printk("... APIC TDCR: %08x\n", v);
	printk("\n");
}

void print_all_local_APICs (void)
{
	smp_call_function(print_local_APIC, NULL, 1, 1);
	print_local_APIC(NULL);
}

static void __init init_sym_mode(void)
{
	struct IO_APIC_reg_01 reg_01;
	int i;

	for (i = 0; i < PIN_MAP_SIZE; i++) {
		irq_2_pin[i].pin = -1;
		irq_2_pin[i].next = 0;
	}
	if (!pirqs_enabled)
		for (i = 0; i < MAX_PIRQS; i++)
			pirq_entries[i] =- 1;

	if (pic_mode) {
		/*
		 * PIC mode, enable symmetric IO mode in the IMCR.
		 */
		printk("leaving PIC mode, enabling symmetric IO mode.\n");
		outb(0x70, 0x22);
		outb(0x01, 0x23);
	}

	/*
	 * The number of IO-APIC IRQ registers (== #pins):
	 */
	for (i = 0; i < nr_ioapics; i++) {
		*(int *)&reg_01 = io_apic_read(i, 1);
		nr_ioapic_registers[i] = reg_01.entries+1;
	}

	/*
	 * Do not trust the IO-APIC being empty at bootup
	 */
	clear_IO_APIC();
}

static void clear_lapic_ints (void * dummy)
{
	int maxlvt;

	maxlvt = get_maxlvt();
	apic_write_around(APIC_LVTT, 0x00010000);
	apic_write_around(APIC_LVT0, 0x00010000);
	apic_write_around(APIC_LVT1, 0x00010000);
	if (maxlvt >= 3)
		apic_write_around(APIC_LVTERR, 0x00010000);
	if (maxlvt >= 4)
		apic_write_around(APIC_LVTPC, 0x00010000);
}

/*
 * Not an __init, needed by the reboot code
 */
void init_pic_mode(void)
{
	/*
	 * Clear the IO-APIC and local APICs before rebooting:
	 */
	clear_IO_APIC();
	smp_call_function(clear_lapic_ints, NULL, 1, 1);
	clear_lapic_ints(NULL);

	/*
	 * Put it back into PIC mode (has an effect only on
	 * certain older boards)
	 */
	if (pic_mode) {
		printk("disabling symmetric IO mode, entering PIC mode.\n");
		outb_p(0x70, 0x22);
		outb_p(0x00, 0x23);
	}
}

static void __init setup_ioapic_id(void)
{
	struct IO_APIC_reg_00 reg_00;

	/*
	 * 'default' mptable configurations mean a hardwired setup,
	 * 2 CPUs, 16 APIC registers. IO-APIC ID is usually set to 0,
	 * setting it to ID 2 should be fine.
	 */

	/*
	 * Sanity check, is ID 2 really free? Every APIC in the
	 * system must have a unique ID or we get lots of nice
	 * 'stuck on smp_invalidate_needed IPI wait' messages.
	 */
	if (cpu_present_map & (1<<0x2))
		panic("APIC ID 2 already used");

	/*
	 * Set the ID
	 */
	*(int *)&reg_00 = io_apic_read(0, 0);
	printk("...changing IO-APIC physical APIC ID to 2...\n");
	reg_00.ID = 0x2;
	io_apic_write(0, 0, *(int *)&reg_00);

	/*
	 * Sanity check
	 */
	*(int *)&reg_00 = io_apic_read(0, 0);
	if (reg_00.ID != 0x2)
		panic("could not set ID");
}

static void __init construct_default_ISA_mptable(void)
{
	int i, pos = 0;
	const int bus_type = (mpc_default_type == 2 || mpc_default_type == 3 ||
			      mpc_default_type == 6) ? MP_BUS_EISA : MP_BUS_ISA;

	for (i = 0; i < 16; i++) {
		if (!IO_APIC_IRQ(i))
			continue;

		mp_irqs[pos].mpc_irqtype = mp_INT;
		mp_irqs[pos].mpc_irqflag = 0;		/* default */
		mp_irqs[pos].mpc_srcbus = 0;
		mp_irqs[pos].mpc_srcbusirq = i;
		mp_irqs[pos].mpc_dstapic = 0;
		mp_irqs[pos].mpc_dstirq = i;
		pos++;
	}
	mp_irq_entries = pos;
	mp_bus_id_to_type[0] = bus_type;

	/*
	 * MP specification 1.4 defines some extra rules for default
	 * configurations, fix them up here:
	 */
	switch (mpc_default_type)
	{
		case 2:
		/*
		 * IRQ0 is not connected:
		 */
			mp_irqs[0].mpc_irqtype = mp_ExtINT;
			break;
		default:
		/*
		 * pin 2 is IRQ0:
		 */
			mp_irqs[0].mpc_dstirq = 2;
	}

	setup_ioapic_id();
}

/*
 * There is a nasty bug in some older SMP boards, their mptable lies
 * about the timer IRQ. We do the following to work around the situation:
 *
 *	- timer IRQ defaults to IO-APIC IRQ
 *	- if this function detects that timer IRQs are defunct, then we fall
 *	  back to ISA timer IRQs
 */
static int __init timer_irq_works(void)
{
	unsigned int t1 = jiffies;

	sti();
	mdelay(40);

	if (jiffies-t1>1)
		return 1;

	return 0;
}

extern atomic_t nmi_counter[NR_CPUS];

static int __init nmi_irq_works(void)
{
	atomic_t tmp[NR_CPUS];
	int j, cpu;

	memcpy(tmp, nmi_counter, sizeof(tmp));
	sti();
	mdelay(50);

	for (j = 0; j < smp_num_cpus; j++) {
		cpu = cpu_logical_map(j);
		if (atomic_read(nmi_counter+cpu) - atomic_read(tmp+cpu) <= 3) {
			printk("CPU#%d NMI appears to be stuck.\n", cpu);
			return 0;
		}
	}
	return 1;
}

/*
 * In the SMP+IOAPIC case it might happen that there are an unspecified
 * number of pending IRQ events unhandled. These cases are very rare,
 * so we 'resend' these IRQs via IPIs, to the same CPU. It's much
 * better to do it this way as thus we do not have to be aware of
 * 'pending' interrupts in the IRQ path, except at this point.
 */
/*
 * Edge triggered needs to resend any interrupt
 * that was delayed but this is now handled in the device
 * independent code.
 */
static void enable_edge_ioapic_irq(unsigned int irq)
{
	unmask_IO_APIC_irq(irq);
}

static void disable_edge_ioapic_irq(unsigned int irq)
{
}

/*
 * Starting up a edge-triggered IO-APIC interrupt is
 * nasty - we need to make sure that we get the edge.
 * If it is already asserted for some reason, we need
 * return 1 to indicate that is was pending.
 *
 * This is not complete - we should be able to fake
 * an edge even if it isn't on the 8259A...
 */

static unsigned int startup_edge_ioapic_irq(unsigned int irq)
{
	int was_pending = 0;
	if (irq < 16) {
		disable_8259A_irq(irq);
		if (i8259A_irq_pending(irq))
			was_pending = 1;
	}
	enable_edge_ioapic_irq(irq);
	return was_pending;
}

#define shutdown_edge_ioapic_irq	disable_edge_ioapic_irq

/*
 * Once we have recorded IRQ_PENDING already, we can mask the
 * interrupt for real. This prevents IRQ storms from unhandled
 * devices.
 */
void static ack_edge_ioapic_irq(unsigned int irq)
{
	if ((irq_desc[irq].status & (IRQ_PENDING | IRQ_DISABLED))
					== (IRQ_PENDING | IRQ_DISABLED))
		mask_IO_APIC_irq(irq);
	ack_APIC_irq();
}
void static end_edge_ioapic_irq(unsigned int i){}


/*
 * Level triggered interrupts can just be masked,
 * and shutting down and starting up the interrupt
 * is the same as enabling and disabling them -- except
 * with a startup need to return a "was pending" value.
 */
static unsigned int startup_level_ioapic_irq(unsigned int irq)
{
	unmask_IO_APIC_irq(irq);
	return 0; /* don't check for pending */
}

#define shutdown_level_ioapic_irq	mask_IO_APIC_irq
#define enable_level_ioapic_irq		unmask_IO_APIC_irq
#define disable_level_ioapic_irq	mask_IO_APIC_irq
#define end_level_ioapic_irq		unmask_IO_APIC_irq	
void static mask_and_ack_level_ioapic_irq(unsigned int i)
{
	mask_IO_APIC_irq(i);
	ack_APIC_irq();
}

/*
 * Level and edge triggered IO-APIC interrupts need different handling,
 * so we use two separate IRQ descriptors. Edge triggered IRQs can be
 * handled with the level-triggered descriptor, but that one has slightly
 * more overhead. Level-triggered interrupts cannot be handled with the
 * edge-triggered handler, without risking IRQ storms and other ugly
 * races.
 */

static struct hw_interrupt_type ioapic_edge_irq_type = {
	"IO-APIC-edge",
	startup_edge_ioapic_irq,
	shutdown_edge_ioapic_irq,
	enable_edge_ioapic_irq,
	disable_edge_ioapic_irq,
	ack_edge_ioapic_irq,
	end_edge_ioapic_irq
};

static struct hw_interrupt_type ioapic_level_irq_type = {
	"IO-APIC-level",
	startup_level_ioapic_irq,
	shutdown_level_ioapic_irq,
	enable_level_ioapic_irq,
	disable_level_ioapic_irq,
	mask_and_ack_level_ioapic_irq,
	end_level_ioapic_irq
};

static inline void init_IO_APIC_traps(void)
{
	int irq;

	/*
	 * NOTE! The local APIC isn't very good at handling
	 * multiple interrupts at the same interrupt level.
	 * As the interrupt level is determined by taking the
	 * vector number and shifting that right by 4, we
	 * want to spread these out a bit so that they don't
	 * all fall in the same interrupt level.
	 *
	 * Also, we've got to be careful not to trash gate
	 * 0x80, because int 0x80 is hm, kind of importantish. ;)
	 */
	for (irq = 0; irq < NR_IRQS ; irq++) {
		if (IO_APIC_IRQ(irq) && !IO_APIC_VECTOR(irq)) {
			/*
			 * Hmm.. We don't have an entry for this,
			 * so default to an old-fashioned 8259
			 * interrupt if we can..
			 */
			if (irq < 16)
				make_8259A_irq(irq);
			else
				/* Strange. Oh, well.. */
				irq_desc[irq].handler = &no_irq_type;
		}
	}
}

void static ack_lapic_irq (unsigned int irq)
{
	ack_APIC_irq();
}

void static end_lapic_irq (unsigned int i) { /* nothing */ }

static struct hw_interrupt_type lapic_irq_type = {
	"local-APIC-edge",
	NULL, /* startup_irq() not used for IRQ0 */
	NULL, /* shutdown_irq() not used for IRQ0 */
	NULL, /* enable_irq() not used for IRQ0 */
	NULL, /* disable_irq() not used for IRQ0 */
	ack_lapic_irq,
	end_lapic_irq
};

static void enable_NMI_through_LVT0 (void * dummy)
{
	apic_readaround(APIC_LVT0);
	apic_write(APIC_LVT0, 0x00000400);	// unmask and set to NMI
}

static void setup_nmi (void)
{
	/*
 	 * Dirty trick to enable the NMI watchdog ...
	 * We put the 8259A master into AEOI mode and
	 * unmask on all local APICs LVT0 as NMI.
	 *
	 * The idea to use the 8259A in AEOI mode ('8259A Virtual Wire')
	 * is from Maciej W. Rozycki - so we do not have to EOI from
	 * the NMI handler or the timer interrupt.
	 */ 
	printk("activating NMI Watchdog ...");

	smp_call_function(enable_NMI_through_LVT0, NULL, 1, 1);
	enable_NMI_through_LVT0(NULL);

	printk(" done.\n");
}

/*
 * This code may look a bit paranoid, but it's supposed to cooperate with
 * a wide range of boards and BIOS bugs.  Fortunately only the timer IRQ
 * is so screwy.  Thanks to Brian Perkins for testing/hacking this beast
 * fanatically on his truly buggy board.
 */
static inline void check_timer(void)
{
	int pin1, pin2;
	int vector;

	/*
	 * get/set the timer IRQ vector:
	 */
	vector = assign_irq_vector(0);
	set_intr_gate(vector, interrupt[0]);

	pin1 = find_timer_pin(mp_INT);
	pin2 = find_timer_pin(mp_ExtINT);

	/*
	 * Ok, does IRQ0 through the IOAPIC work?
	 */
	if (timer_irq_works()) {
		if (nmi_watchdog) {
			disable_8259A_irq(0);
			init_8259A(1);
			setup_nmi();
			enable_8259A_irq(0);
			if (nmi_irq_works())
				return;
		} else
			return;
	}

	if (pin1 != -1) {
		printk("..MP-BIOS bug: 8254 timer not connected to IO-APIC\n");
		clear_IO_APIC_pin(0, pin1);
	}

	printk("...trying to set up timer (IRQ0) through the 8259A ... ");
	if (pin2 != -1) {
		printk("\n..... (found pin %d) ...", pin2);
		/*
		 * legacy devices should be connected to IO APIC #0
		 */
		setup_ExtINT_IRQ0_pin(pin2, vector);
		if (timer_irq_works()) {
			printk("works.\n");
			if (nmi_watchdog) {
				setup_nmi();
				if (nmi_irq_works())
					return;
			} else
				return;
		}
		/*
		 * Cleanup, just in case ...
		 */
		clear_IO_APIC_pin(0, pin2);
	}
	printk(" failed.\n");

	if (nmi_watchdog)
		printk("timer doesnt work through the IO-APIC - cannot activate NMI Watchdog!\n");

	printk("...trying to set up timer as Virtual Wire IRQ...");

	disable_8259A_irq(0);
	irq_desc[0].handler = &lapic_irq_type;
	init_8259A(1);					// AEOI mode
	apic_readaround(APIC_LVT0);
	apic_write(APIC_LVT0, 0x00000000 | vector);	// Fixed mode
	enable_8259A_irq(0);

	if (timer_irq_works()) {
		printk(" works.\n");
		return;
	}
	printk(" failed :(.\n");
	panic("IO-APIC + timer doesn't work! pester mingo@redhat.com");
}

/*
 *
 * IRQ's that are handled by the old PIC in all cases:
 * - IRQ2 is the cascade IRQ, and cannot be a io-apic IRQ.
 *   Linux doesn't really care, as it's not actually used
 *   for any interrupt handling anyway.
 * - IRQ13 is the FPU error IRQ, and may be connected
 *   directly from the FPU to the old PIC. Linux doesn't
 *   really care, because Linux doesn't want to use IRQ13
 *   anyway (exception 16 is the proper FPU error signal)
 *
 * Additionally, something is definitely wrong with irq9
 * on PIIX4 boards.
 */
#define PIC_IRQS	((1<<2)|(1<<13))

void __init setup_IO_APIC(void)
{
	init_sym_mode();

	printk("ENABLING IO-APIC IRQs\n");
	io_apic_irqs = ~PIC_IRQS;

	/*
	 * If there are no explicit MP IRQ entries, it's either one of the
	 * default configuration types or we are broken. In both cases it's
	 * fine to set up most of the low 16 IO-APIC pins to ISA defaults.
	 */
	if (!mp_irq_entries) {
		printk("no explicit IRQ entries, using default mptable\n");
		construct_default_ISA_mptable();
	}

	/*
	 * Set up the IO-APIC IRQ routing table by parsing the MP-BIOS
	 * mptable:
	 */
	setup_IO_APIC_irqs();
	init_IO_APIC_traps();
	check_timer();
	print_IO_APIC();
}
