/*
 *  linux/drivers/block/opti621.c       Version 0.6  Jan 02, 1999
 *
 *  Copyright (C) 1996-1998  Linus Torvalds & authors (see below)
 */

/*
 * Authors:
 * Jaromir Koutek <miri@punknet.cz>,
 * Jan Harkes <jaharkes@cwi.nl>,
 * Mark Lord <mlord@pobox.com>
 * Some parts of code are from ali14xx.c and from rz1000.c.
 *
 * OPTi is trademark of OPTi, Octek is trademark of Octek.
 *
 * I used docs from OPTi databook, from ftp.opti.com, file 9123-0002.ps
 * and disassembled/traced setupvic.exe (DOS program).
 * It increases kernel code about 2 kB.
 * I don't have this card no more, but I hope I can get some in case
 * of needed development.
 * My card is Octek PIDE 1.01 (on card) or OPTiViC (program).
 * It has a place for a secondary connector in circuit, but nothing
 * is there. Also BIOS says no address for
 * secondary controller (see bellow in ide_init_opti621).
 * I've only tested this on my system, which only has one disk.
 * It's Western Digital WDAC2850, with PIO mode 3. The PCI bus
 * is at 20 MHz (I have DX2/80, I tried PCI at 40, but I got random
 * lockups). I tried the OCTEK double speed CD-ROM and
 * it does not work! But I can't boot DOS also, so it's probably
 * hardware fault. I have connected Conner 80MB, the Seagate 850MB (no
 * problems) and Seagate 1GB (as slave, WD as master). My experiences
 * with the third, 1GB drive: I got 3MB/s (hdparm), but sometimes
 * it slows to about 100kB/s! I don't know why and I have
 * not this drive now, so I can't try it again.
 * I write this driver because I lost the paper ("manual") with
 * settings of jumpers on the card and I have to boot Linux with
 * Loadlin except LILO, cause I have to run the setupvic.exe program
 * already or I get disk errors (my test: rpm -Vf
 * /usr/X11R6/bin/XF86_SVGA - or any big file).
 * Some numbers from hdparm -t /dev/hda:
 * Timing buffer-cache reads:   32 MB in  3.02 seconds =10.60 MB/sec
 * Timing buffered disk reads:  16 MB in  5.52 seconds = 2.90 MB/sec
 * I have 4 Megs/s before, but I don't know why (maybe changes
 * in hdparm test).
 * After release of 0.1, I got some successful reports, so it might work.
 *
 * The main problem with OPTi is that some timings for master
 * and slave must be the same. For example, if you have master
 * PIO 3 and slave PIO 0, driver have to set some timings of
 * master for PIO 0. Second problem is that opti621_tune_drive
 * got only one drive to set, but have to set both drives.
 * This is solved in compute_pios. If you don't set
 * the second drive, compute_pios use ide_get_best_pio_mode
 * for autoselect mode (you can change it to PIO 0, if you want).
 * If you then set the second drive to another PIO, the old value
 * (automatically selected) will be overrided by yours.
 * There is a 25/33MHz switch in configuration
 * register, but driver is written for use at any frequency which get
 * (use idebus=xx to select PCI bus speed).
 * Use ide0=autotune for automatical tune of the PIO modes.
 * If you get strange results, do not use this and set PIO manually
 * by hdparm.
 *
 * Version 0.1, Nov 8, 1996
 * by Jaromir Koutek, for 2.1.8. 
 * Initial version of driver.
 * 
 * Version 0.2
 * Number 0.2 skipped.
 *
 * Version 0.3, Nov 29, 1997
 * by Mark Lord (probably), for 2.1.68
 * Updates for use with new IDE block driver.
 *
 * Version 0.4, Dec 14, 1997
 * by Jan Harkes
 * Fixed some errors and cleaned the code.
 *
 * Version 0.5, Jan 2, 1998
 * by Jaromir Koutek
 * Updates for use with (again) new IDE block driver.
 * Update of documentation.
 * 
 * Version 0.6, Jan 2, 1999
 * by Jaromir Koutek
 * Reversed to version 0.3 of the driver, because
 * 0.5 doesn't work.
 */

#undef REALLY_SLOW_IO	/* most systems can safely undef this */
#define OPTI621_DEBUG		/* define for debug messages */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/ide.h>

#include <asm/io.h>

#include "ide_modes.h"

#define OPTI621_MAX_PIO 3
/* In fact, I do not have any PIO 4 drive
 * (address: 25 ns, data: 70 ns, recovery: 35 ns),
 * but OPTi 82C621 is programmable and it can do (minimal values):
 * on 40MHz PCI bus (pulse 25 ns):
 *  address: 25 ns, data: 25 ns, recovery: 50 ns;
 * on 20MHz PCI bus (pulse 50 ns):
 *  address: 50 ns, data: 50 ns, recovery: 100 ns.
 */

/* #define READ_PREFETCH 0 */
/* Uncommnent for disable read prefetch.
 * There is some readprefetch capatibility in hdparm,
 * but when I type hdparm -P 1 /dev/hda, I got errors
 * and till reset drive is inacessible.
 * This (hw) read prefetch is safe on my drive.
 */

#ifndef READ_PREFETCH
#define READ_PREFETCH 0x40 /* read prefetch is enabled */
#endif /* else read prefetch is disabled */

#define READ_REG 0	/* index of Read cycle timing register */
#define WRITE_REG 1	/* index of Write cycle timing register */
#define CNTRL_REG 3	/* index of Control register */
#define STRAP_REG 5	/* index of Strap register */
#define MISC_REG 6	/* index of Miscellaneous register */

int reg_base;

#define PIO_NOT_EXIST 254
#define PIO_DONT_KNOW 255

/* there are stored pio numbers from other calls of opti621_tune_drive */

static void compute_pios(ide_drive_t *drive, byte pio)
/* Store values into drive->drive_data
 *	second_contr - 0 for primary controller, 1 for secondary
 *	slave_drive - 0 -> pio is for master, 1 -> pio is for slave
 *	pio - PIO mode for selected drive (for other we don't know)
 */
{
	int d;
	ide_hwif_t *hwif = HWIF(drive);

	drive->drive_data = ide_get_best_pio_mode(drive, pio, OPTI621_MAX_PIO, NULL);
	for (d = 0; d < 2; ++d) {
		drive = &hwif->drives[d];
		if (drive->present) {
			if (drive->drive_data == PIO_DONT_KNOW)
				drive->drive_data = ide_get_best_pio_mode(drive, 255, OPTI621_MAX_PIO, NULL);
#ifdef OPTI621_DEBUG
			printk("%s: Selected PIO mode %d\n", drive->name, drive->drive_data);
#endif
		} else {
			drive->drive_data = PIO_NOT_EXIST;
		}
	}
}

int cmpt_clk(int time, int bus_speed)
/* Returns (rounded up) time in clocks for time in ns,
 * with bus_speed in MHz.
 * Example: bus_speed = 40 MHz, time = 80 ns
 * 1000/40 = 25 ns (clk value),
 * 80/25 = 3.2, rounded up to 4 (I hope ;-)).
 * Use idebus=xx to select right frequency.
 */
{
	return ((time*bus_speed+999)/1000);
}

static void write_reg(byte value, int reg)
/* Write value to register reg, base of register
 * is at reg_base (0x1f0 primary, 0x170 secondary,
 * if not changed by PCI configuration).
 * This is from setupvic.exe program.
 */
{
	inw(reg_base+1);
	inw(reg_base+1);
	outb(3, reg_base+2);
	outb(value, reg_base+reg);
	outb(0x83, reg_base+2);
}

static byte read_reg(int reg)
/* Read value from register reg, base of register
 * is at reg_base (0x1f0 primary, 0x170 secondary,
 * if not changed by PCI configuration).
 * This is from setupvic.exe program.
 */
{
	byte ret;
	inw(reg_base+1);
	inw(reg_base+1);
	outb(3, reg_base+2);
	ret=inb(reg_base+reg);
	outb(0x83, reg_base+2);
	return ret;
}

typedef struct pio_clocks_s {
	int	address_time;	/* Address setup (clocks) */
	int	data_time;	/* Active/data pulse (clocks) */
	int	recovery_time;	/* Recovery time (clocks) */
} pio_clocks_t;

static void compute_clocks(int pio, pio_clocks_t *clks)
{
        if (pio != PIO_NOT_EXIST) {
        	int adr_setup, data_pls, bus_speed;
        	bus_speed = ide_system_bus_speed();
 	       	adr_setup = ide_pio_timings[pio].setup_time;
  	      	data_pls = ide_pio_timings[pio].active_time;
	  	clks->address_time = cmpt_clk(adr_setup, bus_speed);
	     	clks->data_time = cmpt_clk(data_pls, bus_speed);
     		clks->recovery_time = cmpt_clk(ide_pio_timings[pio].cycle_time
     			- adr_setup-data_pls, bus_speed);
     		if (clks->address_time<1) clks->address_time = 1;
     		if (clks->address_time>4) clks->address_time = 4;
     		if (clks->data_time<1) clks->data_time = 1;
     		if (clks->data_time>16) clks->data_time = 16;
     		if (clks->recovery_time<2) clks->recovery_time = 2;
     		if (clks->recovery_time>17) clks->recovery_time = 17;
	} else {
		clks->address_time = 1;
		clks->data_time = 1;
		clks->recovery_time = 2;
		/* minimal values */
	}
 
}

/* Main tune procedure, called from tuneproc. */
static void opti621_tune_drive (ide_drive_t *drive, byte pio)
{
	/* primary and secondary drives share some registers,
	 * so we have to program both drives
	 */
	unsigned long flags;
	byte pio1, pio2;
	pio_clocks_t first, second;
	int ax, drdy;
	byte cycle1, cycle2, misc;
	ide_hwif_t *hwif = HWIF(drive);

	/* sets drive->drive_data for both drives */
	compute_pios(drive, pio);
 	pio1 = hwif->drives[0].drive_data;
 	pio2 = hwif->drives[1].drive_data;

	compute_clocks(pio1, &first);
	compute_clocks(pio2, &second);

	/* ax = max(a1,a2) */
	ax = (first.address_time < second.address_time) ? second.address_time : first.address_time;

	drdy = 2; /* DRDY is default 2 (by OPTi Databook) */

	cycle1 = ((first.data_time-1)<<4)  | (first.recovery_time-2);
	cycle2 = ((second.data_time-1)<<4) | (second.recovery_time-2);
	misc = READ_PREFETCH | ((ax-1)<<4) | ((drdy-2)<<1);

#ifdef OPTI621_DEBUG
	printk("%s: master: address: %d, data: %d, recovery: %d, drdy: %d [clk]\n",
		hwif->name, ax, first.data_time, first.recovery_time, drdy);
	printk("%s: slave:  address: %d, data: %d, recovery: %d, drdy: %d [clk]\n",
		hwif->name, ax, second.data_time, second.recovery_time, drdy);
#endif

	save_flags(flags);	/* all CPUs */
	cli();			/* all CPUs */

     	reg_base = hwif->io_ports[IDE_DATA_OFFSET];
	outb(0xc0, reg_base+CNTRL_REG);	/* allow Register-B */
	outb(0xff, reg_base+5);		/* hmm, setupvic.exe does this ;-) */
	inb(reg_base+CNTRL_REG); 	/* if reads 0xff, adapter not exist? */
	read_reg(CNTRL_REG);		/* if reads 0xc0, no interface exist? */
	read_reg(STRAP_REG);		/* read version, probably 0 */

	/* program primary drive */
	write_reg(0,      MISC_REG);	/* select Index-0 for Register-A */
	write_reg(cycle1, READ_REG);	/* set read cycle timings */
	write_reg(cycle1, WRITE_REG);	/* set write cycle timings */

	/* program secondary drive */
	write_reg(1,      MISC_REG);	/* select Index-1 for Register-B */
	write_reg(cycle2, READ_REG);	/* set read cycle timings */
	write_reg(cycle2, WRITE_REG);	/* set write cycle timings */

	write_reg(0x85, CNTRL_REG);	/* use Register-A for drive 0 */
					/* use Register-B for drive 1 */

 	write_reg(misc, MISC_REG);	/* set address setup, DRDY timings,   */
 					/*  and read prefetch for both drives */

	restore_flags(flags);	/* all CPUs */
}

/*
 * ide_init_opti621() is called once for each hwif found at boot.
 */
void ide_init_opti621 (ide_hwif_t *hwif)
{
	hwif->drives[0].drive_data = PIO_DONT_KNOW;
	hwif->drives[1].drive_data = PIO_DONT_KNOW;
	hwif->tuneproc = &opti621_tune_drive;
}
