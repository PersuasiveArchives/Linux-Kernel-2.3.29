/*
 *	linux/arch/alpha/kernel/sys_dp264.c
 *
 *	Copyright (C) 1995 David A Rusling
 *	Copyright (C) 1996, 1999 Jay A Estabrook
 *	Copyright (C) 1998, 1999 Richard Henderson
 *
 * Code supporting the DP264 (EV6+TSUNAMI).
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
#include <asm/bitops.h>
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/core_tsunami.h>
#include <asm/hwrpb.h>

#include "proto.h"
#include "irq_impl.h"
#include "pci_impl.h"
#include "machvec_impl.h"


/*
 * HACK ALERT! only the boot cpu is used for interrupts.
 */

static void
dp264_update_irq_hw(unsigned long irq, unsigned long mask, int unmask_p)
{
	if (irq >= 16) {
		volatile unsigned long *csr;

		if (TSUNAMI_bootcpu < 2)
			if (!TSUNAMI_bootcpu)
				csr = &TSUNAMI_cchip->dim0.csr;
			else
				csr = &TSUNAMI_cchip->dim1.csr;
		else
			if (TSUNAMI_bootcpu == 2)
				csr = &TSUNAMI_cchip->dim2.csr;
			else
				csr = &TSUNAMI_cchip->dim3.csr;
		
		*csr = ~mask;
		mb();
		*csr;
	}
	else if (irq >= 8)
		outb(mask >> 8, 0xA1);	/* ISA PIC2 */
	else
		outb(mask, 0x21);	/* ISA PIC1 */
}

static void
clipper_update_irq_hw(unsigned long irq, unsigned long mask, int unmask_p)
{
	if (irq >= 16) {
		volatile unsigned long *csr;

		if (TSUNAMI_bootcpu < 2)
			if (!TSUNAMI_bootcpu)
				csr = &TSUNAMI_cchip->dim0.csr;
			else
				csr = &TSUNAMI_cchip->dim1.csr;
		else
			if (TSUNAMI_bootcpu == 2)
				csr = &TSUNAMI_cchip->dim2.csr;
			else
				csr = &TSUNAMI_cchip->dim3.csr;
		
		*csr = (~mask >> 16) | (1UL << 55); /* master ISA enable */
		mb();
		*csr;
	}
	else if (irq >= 8)
		outb(mask >> 8, 0xA1);	/* ISA PIC2 */
	else
		outb(mask, 0x21);	/* ISA PIC1 */
}

static void
dp264_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
#if 1
	printk("dp264_device_interrupt: NOT IMPLEMENTED YET!! \n");
#else
	unsigned long pld;
	unsigned int i;

	/* Read the interrupt summary register of TSUNAMI */
	pld = TSUNAMI_cchip->dir0.csr;

	/*
	 * Now for every possible bit set, work through them and call
	 * the appropriate interrupt handler.
	 */
	while (pld) {
		i = ffz(~pld);
		pld &= pld - 1; /* clear least bit set */
		if (i == 55)
			isa_device_interrupt(vector, regs);
		else
			handle_irq(16 + i, 16 + i, regs);
#if 0
		TSUNAMI_cchip->dir0.csr = 1UL << i; mb();
		tmp = TSUNAMI_cchip->dir0.csr;
#endif
	}
#endif
}

static void 
dp264_srm_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
	int irq, ack;

	ack = irq = (vector - 0x800) >> 4;

	/*
	 * The SRM console reports PCI interrupts with a vector calculated by:
	 *
	 *	0x900 + (0x10 * DRIR-bit)
	 *
	 * So bit 16 shows up as IRQ 32, etc.
	 * 
	 * On DP264/BRICK/MONET, we adjust it down by 16 because at least
	 * that many of the low order bits of the DRIR are not used, and
	 * so we don't count them.
	 */
	if (irq >= 32)
		ack = irq = irq - 16;

	handle_irq(irq, ack, regs);
}

static void 
clipper_srm_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
	int irq, ack;

	ack = irq = (vector - 0x800) >> 4;

	/*
	 * The SRM console reports PCI interrupts with a vector calculated by:
	 *
	 *	0x900 + (0x10 * DRIR-bit)
	 *
	 * So bit 16 shows up as IRQ 32, etc.
	 * 
	 * CLIPPER uses bits 8-47 for PCI interrupts, so we do not need
	 * to scale down the vector reported, we just use it.
	 *
	 * Eg IRQ 24 is DRIR bit 8, etc, etc
	 */
	handle_irq(irq, ack, regs);
}

static void __init
dp264_init_irq(void)
{
	outb(0, DMA1_RESET_REG);
	outb(0, DMA2_RESET_REG);
	outb(DMA_MODE_CASCADE, DMA2_MODE_REG);
	outb(0, DMA2_MASK_REG);

	if (alpha_using_srm)
		alpha_mv.device_interrupt = dp264_srm_device_interrupt;

	dp264_update_irq_hw(16, alpha_irq_mask, 0);

        enable_irq(55);     /* Enable ISA interrupt controller.  */
	enable_irq(2);
}

static void __init
clipper_init_irq(void)
{
	outb(0, DMA1_RESET_REG);
	outb(0, DMA2_RESET_REG);
	outb(DMA_MODE_CASCADE, DMA2_MODE_REG);
	outb(0, DMA2_MASK_REG);

	if (alpha_using_srm)
		alpha_mv.device_interrupt = clipper_srm_device_interrupt;

	clipper_update_irq_hw(16, alpha_irq_mask, 0);

        enable_irq(55);     /* Enable ISA interrupt controller.  */
	enable_irq(2);
}


/*
 * PCI Fixup configuration.
 *
 * Summary @ TSUNAMI_CSR_DIM0:
 * Bit      Meaning
 * 0-17     Unused
 *18        Interrupt SCSI B (Adaptec 7895 builtin)
 *19        Interrupt SCSI A (Adaptec 7895 builtin)
 *20        Interrupt Line D from slot 2 PCI0
 *21        Interrupt Line C from slot 2 PCI0
 *22        Interrupt Line B from slot 2 PCI0
 *23        Interrupt Line A from slot 2 PCI0
 *24        Interrupt Line D from slot 1 PCI0
 *25        Interrupt Line C from slot 1 PCI0
 *26        Interrupt Line B from slot 1 PCI0
 *27        Interrupt Line A from slot 1 PCI0
 *28        Interrupt Line D from slot 0 PCI0
 *29        Interrupt Line C from slot 0 PCI0
 *30        Interrupt Line B from slot 0 PCI0
 *31        Interrupt Line A from slot 0 PCI0
 *
 *32        Interrupt Line D from slot 3 PCI1
 *33        Interrupt Line C from slot 3 PCI1
 *34        Interrupt Line B from slot 3 PCI1
 *35        Interrupt Line A from slot 3 PCI1
 *36        Interrupt Line D from slot 2 PCI1
 *37        Interrupt Line C from slot 2 PCI1
 *38        Interrupt Line B from slot 2 PCI1
 *39        Interrupt Line A from slot 2 PCI1
 *40        Interrupt Line D from slot 1 PCI1
 *41        Interrupt Line C from slot 1 PCI1
 *42        Interrupt Line B from slot 1 PCI1
 *43        Interrupt Line A from slot 1 PCI1
 *44        Interrupt Line D from slot 0 PCI1
 *45        Interrupt Line C from slot 0 PCI1
 *46        Interrupt Line B from slot 0 PCI1
 *47        Interrupt Line A from slot 0 PCI1
 *48-52     Unused
 *53        PCI0 NMI (from Cypress)
 *54        PCI0 SMI INT (from Cypress)
 *55        PCI0 ISA Interrupt (from Cypress)
 *56-60     Unused
 *61        PCI1 Bus Error
 *62        PCI0 Bus Error
 *63        Reserved
 *
 * IdSel	
 *   5	 Cypress Bridge I/O
 *   6	 SCSI Adaptec builtin
 *   7	 64 bit PCI option slot 0 (all busses)
 *   8	 64 bit PCI option slot 1 (all busses)
 *   9	 64 bit PCI option slot 2 (all busses)
 *  10	 64 bit PCI option slot 3 (not bus 0)
 */

static int __init
dp264_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[6][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 5 ISA Bridge */
		{ 16+ 3, 16+ 3, 16+ 2, 16+ 2, 16+ 2}, /* IdSel 6 SCSI builtin*/
		{ 16+15, 16+15, 16+14, 16+13, 16+12}, /* IdSel 7 slot 0 */
		{ 16+11, 16+11, 16+10, 16+ 9, 16+ 8}, /* IdSel 8 slot 1 */
		{ 16+ 7, 16+ 7, 16+ 6, 16+ 5, 16+ 4}, /* IdSel 9 slot 2 */
		{ 16+ 3, 16+ 3, 16+ 2, 16+ 1, 16+ 0}  /* IdSel 10 slot 3 */
	};
	const long min_idsel = 5, max_idsel = 10, irqs_per_slot = 5;

	struct pci_controler *hose = dev->sysdata;
	int irq = COMMON_TABLE_LOOKUP;

	if (irq > 0)
		irq += 16 * hose->index;

	return irq;
}

static int __init
monet_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[13][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{    45,    45,    45,    45,    45}, /* IdSel 3 21143 PCI1 */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 4 unused */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 5 unused */
		{    47,    47,    47,    47,    47}, /* IdSel 6 SCSI PCI1 */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 7 ISA Bridge */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 8 P2P PCI1 */
#if 1
		{    28,    28,    29,    30,    31}, /* IdSel 14 slot 4 PCI2*/
		{    24,    24,    25,    26,    27}, /* IdSel 15 slot 5 PCI2*/
#else
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 9 unused */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 10 unused */
#endif
		{    40,    40,    41,    42,    43}, /* IdSel 11 slot 1 PCI0*/
		{    36,    36,    37,    38,    39}, /* IdSel 12 slot 2 PCI0*/
		{    32,    32,    33,    34,    35}, /* IdSel 13 slot 3 PCI0*/
		{    28,    28,    29,    30,    31}, /* IdSel 14 slot 4 PCI2*/
		{    24,    24,    25,    26,    27}  /* IdSel 15 slot 5 PCI2*/
	};
	const long min_idsel = 3, max_idsel = 15, irqs_per_slot = 5;
	return COMMON_TABLE_LOOKUP;
}

static u8 __init
monet_swizzle(struct pci_dev *dev, u8 *pinp)
{
	struct pci_controler *hose = dev->sysdata;
	int slot, pin = *pinp;

	if (hose->first_busno == dev->bus->number) {
		slot = PCI_SLOT(dev->devfn);
	}
	/* Check for the built-in bridge on hose 1. */
	else if (hose->index == 1 && PCI_SLOT(dev->bus->self->devfn) == 8) {
		slot = PCI_SLOT(dev->devfn);
	} else {
		/* Must be a card-based bridge.  */
		do {
			/* Check for built-in bridge on hose 1. */
			if (hose->index == 1 &&
			    PCI_SLOT(dev->bus->self->devfn) == 8) {
				slot = PCI_SLOT(dev->devfn);
				break;
			}
			pin = bridge_swizzle(pin, PCI_SLOT(dev->devfn)) ;

			/* Move up the chain of bridges.  */
			dev = dev->bus->self;
			/* Slot of the next bridge.  */
			slot = PCI_SLOT(dev->devfn);
		} while (dev->bus->self);
	}
	*pinp = pin;
	return slot;
}

static int __init
webbrick_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[13][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 7 ISA Bridge */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 8 unused */
		{    29,    29,    29,    29,    29}, /* IdSel 9 21143 #1 */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 10 unused */
		{    30,    30,    30,    30,    30}, /* IdSel 11 21143 #2 */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 12 unused */
		{    -1,    -1,    -1,    -1,    -1}, /* IdSel 13 unused */
		{    35,    35,    34,    33,    32}, /* IdSel 14 slot 0 */
		{    39,    39,    38,    37,    36}, /* IdSel 15 slot 1 */
		{    43,    43,    42,    41,    40}, /* IdSel 16 slot 2 */
		{    47,    47,    46,    45,    44}, /* IdSel 17 slot 3 */
	};
	const long min_idsel = 7, max_idsel = 17, irqs_per_slot = 5;
	return COMMON_TABLE_LOOKUP;
}

static int __init
clipper_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
	static char irq_tab[7][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{ 16+ 8, 16+ 8, 16+ 9, 16+10, 16+11}, /* IdSel 1 slot 1 */
		{ 16+12, 16+12, 16+13, 16+14, 16+15}, /* IdSel 2 slot 2 */
		{ 16+16, 16+16, 16+17, 16+18, 16+19}, /* IdSel 3 slot 3 */
		{ 16+20, 16+20, 16+21, 16+22, 16+23}, /* IdSel 4 slot 4 */
		{ 16+24, 16+24, 16+25, 16+26, 16+27}, /* IdSel 5 slot 5 */
		{ 16+28, 16+28, 16+29, 16+30, 16+31}, /* IdSel 6 slot 6 */
		{    -1,    -1,    -1,    -1,    -1}  /* IdSel 7 ISA Bridge */
	};
	const long min_idsel = 1, max_idsel = 7, irqs_per_slot = 5;

	struct pci_controler *hose = dev->sysdata;
	int irq = COMMON_TABLE_LOOKUP;

	if (irq > 0)
		irq += 16 * hose->index;

	return irq;
}

static void __init
dp264_init_pci(void)
{
	common_init_pci();
	SMC669_Init(0);
}

static void __init
monet_init_pci(void)
{
	common_init_pci();
	SMC669_Init(1);
	es1888_init();
}


/*
 * The System Vectors
 */

struct alpha_machine_vector dp264_mv __initmv = {
	vector_name:		"DP264",
	DO_EV6_MMU,
	DO_DEFAULT_RTC,
	DO_TSUNAMI_IO,
	DO_TSUNAMI_BUS,
	machine_check:		tsunami_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	DEFAULT_MEM_BASE,

	nr_irqs:		64,
	irq_probe_mask:		_PROBE_MASK(64),
	update_irq_hw:		dp264_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	dp264_device_interrupt,

	init_arch:		tsunami_init_arch,
	init_irq:		dp264_init_irq,
	init_pit:		common_init_pit,
	init_pci:		dp264_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		dp264_map_irq,
	pci_swizzle:		common_swizzle,
};
ALIAS_MV(dp264)

struct alpha_machine_vector monet_mv __initmv = {
	vector_name:		"Monet",
	DO_EV6_MMU,
	DO_DEFAULT_RTC,
	DO_TSUNAMI_IO,
	DO_TSUNAMI_BUS,
	machine_check:		tsunami_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	DEFAULT_MEM_BASE,

	nr_irqs:		64,
	irq_probe_mask:		_PROBE_MASK(64),
	update_irq_hw:		dp264_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	dp264_device_interrupt,

	init_arch:		tsunami_init_arch,
	init_irq:		dp264_init_irq,
	init_pit:		common_init_pit,
	init_pci:		monet_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		monet_map_irq,
	pci_swizzle:		monet_swizzle,
};

struct alpha_machine_vector webbrick_mv __initmv = {
	vector_name:		"Webbrick",
	DO_EV6_MMU,
	DO_DEFAULT_RTC,
	DO_TSUNAMI_IO,
	DO_TSUNAMI_BUS,
	machine_check:		tsunami_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	DEFAULT_MEM_BASE,

	nr_irqs:		64,
	irq_probe_mask:		_PROBE_MASK(64),
	update_irq_hw:		dp264_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	dp264_device_interrupt,

	init_arch:		tsunami_init_arch,
	init_irq:		dp264_init_irq,
	init_pit:		common_init_pit,
	init_pci:		dp264_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		webbrick_map_irq,
	pci_swizzle:		common_swizzle,
};

struct alpha_machine_vector clipper_mv __initmv = {
	vector_name:		"Clipper",
	DO_EV6_MMU,
	DO_DEFAULT_RTC,
	DO_TSUNAMI_IO,
	DO_TSUNAMI_BUS,
	machine_check:		tsunami_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,
	min_io_address:		DEFAULT_IO_BASE,
	min_mem_address:	DEFAULT_MEM_BASE,

	nr_irqs:		64,
	irq_probe_mask:		_PROBE_MASK(64),
	update_irq_hw:		clipper_update_irq_hw,
	ack_irq:		common_ack_irq,
	device_interrupt:	dp264_device_interrupt,

	init_arch:		tsunami_init_arch,
	init_irq:		clipper_init_irq,
	init_pit:		common_init_pit,
	init_pci:		common_init_pci,
	kill_arch:		common_kill_arch,
	pci_map_irq:		clipper_map_irq,
	pci_swizzle:		common_swizzle,
};

/* No alpha_mv alias for webbrick/monet/clipper, since we compile them
   in unconditionally with DP264; setup_arch knows how to cope.  */
