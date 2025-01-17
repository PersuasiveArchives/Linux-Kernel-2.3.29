/*
 *	linux/arch/alpha/kernel/sys_mikasa.c
 *
 *	Copyright (C) 1995 David A Rusling
 *	Copyright (C) 1996 Jay A Estabrook
 *	Copyright (C) 1998, 1999 Richard Henderson
 *
 * Code supporting the MIKASA (AlphaServer 1000).
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/init.h>

#include <asm/ptrace.h>
#include <asm/system.h>
#include <asm/dma.h>
#include <asm/irq.h>
#include <asm/bitops.h>
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/core_apecs.h>
#include <asm/core_cia.h>

#include "proto.h"
#include "irq_impl.h"
#include "pci_impl.h"
#include "machvec_impl.h"

static void
mikasa_update_irq_hw(unsigned long irq, unsigned long mask, int unmask_p)
{
	if (irq >= 16)
		outw(~(mask >> 16), 0x536); /* note invert */
	else if (irq >= 8)
		outb(mask >> 8, 0xA1);
	else
		outb(mask, 0x21);
}

static void 
mikasa_device_interrupt(unsigned long vector, struct pt_regs *regs)
{
	unsigned long pld;
	unsigned int i;

	/* Read the interrupt summary registers */
	pld = (((unsigned long) (~inw(0x534)) & 0x0000ffffUL) << 16) |
		(((unsigned long) inb(0xa0))  <<  8) |
		((unsigned long) inb(0x20));

	/*
	 * Now for every possible bit set, work through them and call
	 * the appropriate interrupt handler.
	 */
	while (pld) {
		i = ffz(~pld);
		pld &= pld - 1; /* clear least bit set */
		if (i < 16) {
			isa_device_interrupt(vector, regs);
		} else {
			handle_irq(i, i, regs);
		}
	}
}

static void __init
mikasa_init_irq(void)
{
	STANDARD_INIT_IRQ_PROLOG;

	if (alpha_using_srm)
		alpha_mv.device_interrupt = srm_device_interrupt;

	outw(~(alpha_irq_mask >> 16), 0x536);	/* note invert */
	enable_irq(2);				/* enable cascade */
}


/*
 * PCI Fixup configuration.
 *
 * Summary @ 0x536:
 * Bit      Meaning
 * 0        Interrupt Line A from slot 0
 * 1        Interrupt Line B from slot 0
 * 2        Interrupt Line C from slot 0
 * 3        Interrupt Line D from slot 0
 * 4        Interrupt Line A from slot 1
 * 5        Interrupt line B from slot 1
 * 6        Interrupt Line C from slot 1
 * 7        Interrupt Line D from slot 1
 * 8        Interrupt Line A from slot 2
 * 9        Interrupt Line B from slot 2
 *10        Interrupt Line C from slot 2
 *11        Interrupt Line D from slot 2
 *12        NCR 810 SCSI
 *13        Power Supply Fail
 *14        Temperature Warn
 *15        Reserved
 *
 * The device to slot mapping looks like:
 *
 * Slot     Device
 *  6       NCR SCSI controller
 *  7       Intel PCI-EISA bridge chip
 * 11       PCI on board slot 0
 * 12       PCI on board slot 1
 * 13       PCI on board slot 2
 *   
 *
 * This two layered interrupt approach means that we allocate IRQ 16 and 
 * above for PCI interrupts.  The IRQ relates to which bit the interrupt
 * comes in on.  This makes interrupt processing much easier.
 */

static int __init
mikasa_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[8][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{16+12, 16+12, 16+12, 16+12, 16+12},	/* IdSel 17,  SCSI */
		{   -1,    -1,    -1,    -1,    -1},	/* IdSel 18,  PCEB */
		{   -1,    -1,    -1,    -1,    -1},	/* IdSel 19,  ???? */
		{   -1,    -1,    -1,    -1,    -1},	/* IdSel 20,  ???? */
		{   -1,    -1,    -1,    -1,    -1},	/* IdSel 21,  ???? */
		{ 16+0,  16+0,  16+1,  16+2,  16+3},	/* IdSel 22,  slot 0 */
		{ 16+4,  16+4,  16+5,  16+6,  16+7},	/* IdSel 23,  slot 1 */
		{ 16+8,  16+8,  16+9, 16+10, 16+11},	/* IdSel 24,  slot 2 */
	};
	const long min_idsel = 6, max_idsel = 13, irqs_per_slot = 5;
	return COMMON_TABLE_LOOKUP;
}


#if defined(CONFIG_ALPHA_GENERIC) || !defined(CONFIG_ALPHA_PRIMO)
static void
mikasa_apecs_machine_check(unsigned long vector, unsigned long la_ptr,
		           struct pt_regs * regs)
{
#define MCHK_NO_DEVSEL 0x205L
#define MCHK_NO_TABT 0x204L

	struct el_common *mchk_header;
	unsigned int code;

	mchk_header = (struct el_common *)la_ptr;

	/* Clear the error before any reporting.  */
	mb();
	mb(); /* magic */
	draina();
	apecs_pci_clr_err();
	wrmces(0x7);
	mb();

	code = mchk_header->code;
	process_mcheck_info(vector, la_ptr, regs, "MIKASA APECS",
			    (mcheck_expected(0)
			     && (code == MCHK_NO_DEVSEL
			         || code == MCHK_NO_TABT)));
}
#endif


/*
 * The System Vector
 */

#if defined(CONFIG_ALPHA_GENERIC) || !defined(CONFIG_ALPHA_PRIMO)
struct alpha_machine_vector mikasa_mv __initmv = {
	vector_name:		"Mikasa",
	DO_EV4_MMU,
	DO_DEFAULT_RTC,
	DO_APECS_IO,
	DO_APECS_BUS,
	machine_check:		mikasa_apecs_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	APECS_AND_LCA_DEFAULT_MEM_BASE,

	nr_irqs:		32,
	irq_probe_mask:		_PROBE_MASK(32),
	update_irq_hw:		mikasa_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	mikasa_device_interrupt,

	init_arch:		apecs_init_arch,
	init_irq:		mikasa_init_irq,
	init_pit:		common_init_pit,
	init_pci:		common_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		mikasa_map_irq,
	pci_swizzle:		common_swizzle,
};
ALIAS_MV(mikasa)
#endif

#if defined(CONFIG_ALPHA_GENERIC) || defined(CONFIG_ALPHA_PRIMO)
struct alpha_machine_vector mikasa_primo_mv __initmv = {
	vector_name:		"Mikasa-Primo",
	DO_EV5_MMU,
	DO_DEFAULT_RTC,
	DO_CIA_IO,
	DO_CIA_BUS,
	machine_check:		cia_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	CIA_DEFAULT_MEM_BASE,

	nr_irqs:		32,
	irq_probe_mask:		_PROBE_MASK(32),
	update_irq_hw:		mikasa_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	mikasa_device_interrupt,

	init_arch:		cia_init_arch,
	init_irq:		mikasa_init_irq,
	init_pit:		common_init_pit,
	init_pci:		common_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		mikasa_map_irq,
	pci_swizzle:		common_swizzle,
};
ALIAS_MV(mikasa_primo)
#endif
