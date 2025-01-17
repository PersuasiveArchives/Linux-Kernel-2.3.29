/*
 *  linux/drivers/block/gayle.c -- Amiga Gayle IDE Driver
 *
 *     Created 9 Jul 1997 by Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/ide.h>

#include <asm/amigahw.h>
#include <asm/amigaints.h>


    /*
     *  Bases of the IDE interfaces
     */

#define GAYLE_BASE_4000	0xdd2020	/* A4000/A4000T */
#define GAYLE_BASE_1200	0xda0000	/* A1200/A600 */

    /*
     *  Offsets from one of the above bases
     */

#define GAYLE_DATA	0x00
#define GAYLE_ERROR	0x06		/* see err-bits */
#define GAYLE_NSECTOR	0x0a		/* nr of sectors to read/write */
#define GAYLE_SECTOR	0x0e		/* starting sector */
#define GAYLE_LCYL	0x12		/* starting cylinder */
#define GAYLE_HCYL	0x16		/* high byte of starting cyl */
#define GAYLE_SELECT	0x1a		/* 101dhhhh , d=drive, hhhh=head */
#define GAYLE_STATUS	0x1e		/* see status-bits */
#define GAYLE_CONTROL	0x101a

static int gayle_offsets[IDE_NR_PORTS] = {
    GAYLE_DATA, GAYLE_ERROR, GAYLE_NSECTOR, GAYLE_SECTOR, GAYLE_LCYL,
    GAYLE_HCYL, GAYLE_SELECT, GAYLE_STATUS, -1, -1
};


    /*
     *  These are at different offsets from the base
     */

#define GAYLE_IRQ_4000	0xdd3020	/* MSB = 1, Harddisk is source of */
#define GAYLE_IRQ_1200	0xda9000	/* interrupt */


    /*
     *  Offset of the secondary port for IDE doublers
     *  Note that GAYLE_CONTROL is NOT available then!
     */

#define GAYLE_NEXT_PORT	0x1000

#ifndef CONFIG_BLK_DEV_IDEDOUBLER
#define GAYLE_NUM_HWIFS		1
#define GAYLE_NUM_PROBE_HWIFS	GAYLE_NUM_HWIFS
#define GAYLE_HAS_CONTROL_REG	1
#else /* CONFIG_BLK_DEV_IDEDOUBLER */
#define GAYLE_NUM_HWIFS		2
#define GAYLE_NUM_PROBE_HWIFS	(ide_doubler ? GAYLE_NUM_HWIFS : \
					       GAYLE_NUM_HWIFS-1)
#define GAYLE_HAS_CONTROL_REG	(!ide_doubler)
int ide_doubler = 0;	/* support IDE doublers? */
#endif /* CONFIG_BLK_DEV_IDEDOUBLER */


    /*
     *  Check and acknowledge the interrupt status
     */

static int gayle_ack_intr_a4000(ide_hwif_t *hwif)
{
    unsigned char ch;

    ch = inb(hwif->io_ports[IDE_IRQ_OFFSET]);
    if (!(ch & 0x80))
	return 0;
    return 1;
}

static int gayle_ack_intr_a1200(ide_hwif_t *hwif)
{
    unsigned char ch;

    ch = inb(hwif->io_ports[IDE_IRQ_OFFSET]);
    if (!(ch & 0x80))
	return 0;
    (void)inb(hwif->io_ports[IDE_STATUS_OFFSET]);
    outb(0x7c | (ch & 0x03), hwif->io_ports[IDE_IRQ_OFFSET]);
    return 1;
}

    /*
     *  Probe for a Gayle IDE interface (and optionally for an IDE doubler)
     */

void gayle_init(void)
{
    int a4000, i;

    if (!MACH_IS_AMIGA)
	return;

    if (!(a4000 = AMIGAHW_PRESENT(A4000_IDE)) && !AMIGAHW_PRESENT(A1200_IDE))
	return;

    for (i = 0; i < GAYLE_NUM_PROBE_HWIFS; i++) {
	ide_ioreg_t base, ctrlport, irqport;
	ide_ack_intr_t *ack_intr;
	hw_regs_t hw;
	int index;

	if (a4000) {
	    base = (ide_ioreg_t)ZTWO_VADDR(GAYLE_BASE_4000);
	    irqport = (ide_ioreg_t)ZTWO_VADDR(GAYLE_IRQ_4000);
	    ack_intr = gayle_ack_intr_a4000;
	} else {
	    base = (ide_ioreg_t)ZTWO_VADDR(GAYLE_BASE_1200);
	    irqport = (ide_ioreg_t)ZTWO_VADDR(GAYLE_IRQ_1200);
	    ack_intr = gayle_ack_intr_a1200;
	}

	if (GAYLE_HAS_CONTROL_REG)
	    ctrlport = base + GAYLE_CONTROL;
	else
	    ctrlport = 0;

	base += i*GAYLE_NEXT_PORT;

	ide_setup_ports(&hw, base, gayle_offsets,
			ctrlport, irqport, ack_intr, IRQ_AMIGA_PORTS);

	index = ide_register_hw(&hw, NULL);
	if (index != -1) {
	    switch (i) {
		case 0:
		    printk("ide%d: Gayle IDE interface (A%d style)\n", index,
			   a4000 ? 4000 : 1200);
		    break;
#ifdef CONFIG_BLK_DEV_IDEDOUBLER
		case 1:
		    printk("ide%d: IDE doubler\n", index);
		    break;
#endif /* CONFIG_BLK_DEV_IDEDOUBLER */
	    }
	}
#if 1 /* TESTING */
	if (i == 1) {
	    volatile u_short *addr = (u_short *)base;
	    u_short data;
	    printk("+++ Probing for IDE doubler... ");
	    *addr = 0xffff;
	    data = *addr;
	    printk("probe returned 0x%02x (PLEASE REPORT THIS!!)\n", data);
	}
#endif /* TESTING */
    }
}
