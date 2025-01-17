/*
 *	linux/arch/alpha/kernel/sys_rawhide.c
 *
 *	Copyright (C) 1995 David A Rusling
 *	Copyright (C) 1996 Jay A Estabrook
 *	Copyright (C) 1998, 1999 Richard Henderson
 *
 * Code supporting the RAWHIDE.
 */

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
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/core_mcpcia.h>

#include "proto.h"
#include "irq_impl.h"
#include "pci_impl.h"
#include "machvec_impl.h"


static void
rawhide_update_irq_hw(unsigned long irq, unsigned long mask, int unmask_p)
{
	if (irq >= 40) {
		/* PCI bus 1 with builtin NCR810 SCSI */
		*(vuip)MCPCIA_INT_MASK0(5) =
			(~((mask) >> 40) & 0x00ffffffU) | 0x00fe0000U;
		mb();
		/* ... and read it back to make sure it got written.  */
	  	*(vuip)MCPCIA_INT_MASK0(5);
	}
	else if (irq >= 16) {
		/* PCI bus 0 with EISA bridge */
		*(vuip)MCPCIA_INT_MASK0(4) =
			(~((mask) >> 16) & 0x00ffffffU) | 0x00ff0000U;
		mb();
		/* ... and read it back to make sure it got written.  */
	  	*(vuip)MCPCIA_INT_MASK0(4);
	}
	else if (irq >= 8)
		outb(mask >> 8, 0xA1);	/* ISA PIC2 */
	else
		outb(mask, 0x21);	/* ISA PIC1 */
}

static void 
rawhide_srm_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
	int irq, ack;

	ack = irq = (vector - 0x800) >> 4;

	/* ??? A 4 bus RAWHIDE has 67 interrupts.  Oops.  We need
	   something wider than one word for our own internal
	   manipulations.  */

        /*
         * The RAWHIDE SRM console reports PCI interrupts with a vector
	 * 0x80 *higher* than one might expect, as PCI IRQ 0 (ie bit 0)
	 * shows up as IRQ 24, etc, etc. We adjust it down by 8 to have
	 * it line up with the actual bit numbers from the REQ registers,
	 * which is how we manage the interrupts/mask. Sigh...
	 *
	 * also, PCI #1 interrupts are offset some more... :-(
         */
	if (irq == 52)
		ack = irq = 56; /* SCSI on PCI 1 is special */
	else {
		if (irq >= 24) /* adjust all PCI interrupts down 8 */
			ack = irq = irq - 8;
		if (irq >= 48) /* adjust PCI bus 1 interrupts down another 8 */
			ack = irq = irq - 8;
	}

	handle_irq(irq, ack, regs);
}

static void __init
rawhide_init_irq(void)
{
	STANDARD_INIT_IRQ_PROLOG;

	/* HACK ALERT! only PCI busses 0 and 1 are used currently,
	   (MIDs 4 and 5 respectively) and routing is only to CPU #1*/

	*(vuip)MCPCIA_INT_MASK0(4) =
		(~((alpha_irq_mask) >> 16) & 0x00ffffffU) | 0x00ff0000U; mb();
	/* ... and read it back to make sure it got written.  */
	*(vuip)MCPCIA_INT_MASK0(4);

	*(vuip)MCPCIA_INT_MASK0(5) =
		(~((alpha_irq_mask) >> 40) & 0x00ffffffU) | 0x00fe0000U; mb();
	/* ... and read it back to make sure it got written.  */
	*(vuip)MCPCIA_INT_MASK0(5);

	enable_irq(2);
}

/*
 * PCI Fixup configuration.
 *
 * Summary @ MCPCIA_PCI0_INT_REQ:
 * Bit      Meaning
 * 0        Interrupt Line A from slot 2 PCI0
 * 1        Interrupt Line B from slot 2 PCI0
 * 2        Interrupt Line C from slot 2 PCI0
 * 3        Interrupt Line D from slot 2 PCI0
 * 4        Interrupt Line A from slot 3 PCI0
 * 5        Interrupt Line B from slot 3 PCI0
 * 6        Interrupt Line C from slot 3 PCI0
 * 7        Interrupt Line D from slot 3 PCI0
 * 8        Interrupt Line A from slot 4 PCI0
 * 9        Interrupt Line B from slot 4 PCI0
 * 10       Interrupt Line C from slot 4 PCI0
 * 11       Interrupt Line D from slot 4 PCI0
 * 12       Interrupt Line A from slot 5 PCI0
 * 13       Interrupt Line B from slot 5 PCI0
 * 14       Interrupt Line C from slot 5 PCI0
 * 15       Interrupt Line D from slot 5 PCI0
 * 16       EISA interrupt (PCI 0) or SCSI interrupt (PCI 1)
 * 17-23    NA
 *
 * IdSel	
 *   1	 EISA bridge (PCI bus 0 only)
 *   2 	 PCI option slot 2
 *   3	 PCI option slot 3
 *   4   PCI option slot 4
 *   5   PCI option slot 5
 * 
 */

static int __init
rawhide_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[5][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{ 16+16, 16+16, 16+16, 16+16, 16+16}, /* IdSel 1 SCSI PCI 1 */
		{ 16+ 0, 16+ 0, 16+ 1, 16+ 2, 16+ 3}, /* IdSel 2 slot 2 */
		{ 16+ 4, 16+ 4, 16+ 5, 16+ 6, 16+ 7}, /* IdSel 3 slot 3 */
		{ 16+ 8, 16+ 8, 16+ 9, 16+10, 16+11}, /* IdSel 4 slot 4 */
		{ 16+12, 16+12, 16+13, 16+14, 16+15}  /* IdSel 5 slot 5 */
	};
	const long min_idsel = 1, max_idsel = 5, irqs_per_slot = 5;

	struct pci_controler *hose = dev->sysdata;
	int irq = COMMON_TABLE_LOOKUP;
	if (irq >= 0)
		irq += 24 * hose->index;
	return irq;
}


/*
 * The System Vector
 */

struct alpha_machine_vector rawhide_mv __initmv = {
	vector_name:		"Rawhide",
	DO_EV5_MMU,
	DO_DEFAULT_RTC,
	DO_MCPCIA_IO,
	DO_MCPCIA_BUS,
	machine_check:		mcpcia_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	MCPCIA_DEFAULT_MEM_BASE,

	nr_irqs:		64,
	irq_probe_mask:		_PROBE_MASK(64),
	update_irq_hw:		rawhide_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	rawhide_srm_device_interrupt,

	init_arch:		mcpcia_init_arch,
	init_irq:		rawhide_init_irq,
	init_pit:		common_init_pit,
	init_pci:		common_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		rawhide_map_irq,
	pci_swizzle:		common_swizzle,
};
ALIAS_MV(rawhide)
