/*
 * This file contains the driver for an XT hard disk controller
 * (at least the DTC 5150X) for Linux.
 *
 * Author: Pat Mackinlay, pat@it.com.au
 * Date: 29/09/92
 * 
 * Revised: 01/01/93, ...
 *
 * Ref: DTC 5150X Controller Specification (thanks to Kevin Fowler,
 *   kevinf@agora.rain.com)
 * Also thanks to: Salvador Abreu, Dave Thaler, Risto Kankkunen and
 *   Wim Van Dorst.
 *
 * Revised: 04/04/94 by Risto Kankkunen
 *   Moved the detection code from xd_init() to xd_geninit() as it needed
 *   interrupts enabled and Linus didn't want to enable them in that first
 *   phase. xd_geninit() is the place to do these kinds of things anyway,
 *   he says.
 *
 * Modularized: 04/10/96 by Todd Fries, tfries@umr.edu
 *
 * Revised: 13/12/97 by Andrzej Krzysztofowicz, ankry@mif.pg.gda.pl
 *   Fixed some problems with disk initialization and module initiation.
 *   Added support for manual geometry setting (except Seagate controllers)
 *   in form:
 *      xd_geo=<cyl_xda>,<head_xda>,<sec_xda>[,<cyl_xdb>,<head_xdb>,<sec_xdb>]
 *   Recovered DMA access. Abridged messages. Added support for DTC5051CX,
 *   WD1002-27X & XEBEC controllers. Driver uses now some jumper settings.
 *   Extended ioctl() support.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/ioport.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/dma.h>

#define MAJOR_NR XT_DISK_MAJOR
#include <linux/blk.h>
#include <linux/blkpg.h>

#include "xd.h"

#define XD_DONT_USE_DMA		0  /* Initial value. may be overriden using
				      "nodma" module option */
#define XD_INIT_DISK_DELAY	(30*HZ/1000)  /* 30 ms delay during disk initialization */

/* Above may need to be increased if a problem with the 2nd drive detection
   (ST11M controller) or resetting a controler (WD) appears */

XD_INFO xd_info[XD_MAXDRIVES];

/* If you try this driver and find that your card is not detected by the driver at bootup, you need to add your BIOS
   signature and details to the following list of signatures. A BIOS signature is a string embedded into the first
   few bytes of your controller's on-board ROM BIOS. To find out what yours is, use something like MS-DOS's DEBUG
   command. Run DEBUG, and then you can examine your BIOS signature with:

	d xxxx:0000

   where xxxx is the segment of your controller (like C800 or D000 or something). On the ASCII dump at the right, you should
   be able to see a string mentioning the manufacturer's copyright etc. Add this string into the table below. The parameters
   in the table are, in order:

	offset			; this is the offset (in bytes) from the start of your ROM where the signature starts
	signature		; this is the actual text of the signature
	xd_?_init_controller	; this is the controller init routine used by your controller
	xd_?_init_drive		; this is the drive init routine used by your controller

   The controllers directly supported at the moment are: DTC 5150x, WD 1004A27X, ST11M/R and override. If your controller is
   made by the same manufacturer as one of these, try using the same init routines as they do. If that doesn't work, your
   best bet is to use the "override" routines. These routines use a "portable" method of getting the disk's geometry, and
   may work with your card. If none of these seem to work, try sending me some email and I'll see what I can do <grin>.

   NOTE: You can now specify your XT controller's parameters from the command line in the form xd=TYPE,IRQ,IO,DMA. The driver
   should be able to detect your drive's geometry from this info. (eg: xd=0,5,0x320,3 is the "standard"). */

#include <asm/page.h>
/* coppied from floppy.c */
static inline int __get_order(unsigned long size)
{
	int order;

	size = (size-1) >> (PAGE_SHIFT-1);
	order = -1;
	do {
		size >>= 1;
		order++;
	} while (size);
	return order;
}
#define xd_dma_mem_alloc(size) __get_dma_pages(GFP_KERNEL,__get_order(size))
#define xd_dma_mem_free(addr, size) free_pages(addr, __get_order(size))
static char *xd_dma_buffer = 0;

static XD_SIGNATURE xd_sigs[] __initdata = {
	{ 0x0000,"Override geometry handler",NULL,xd_override_init_drive,"n unknown" }, /* Pat Mackinlay, pat@it.com.au */
	{ 0x0008,"[BXD06 (C) DTC 17-MAY-1985]",xd_dtc_init_controller,xd_dtc5150cx_init_drive," DTC 5150CX" }, /* Andrzej Krzysztofowicz, ankry@mif.pg.gda.pl */
	{ 0x000B,"CRD18A   Not an IBM rom. (C) Copyright Data Technology Corp. 05/31/88",xd_dtc_init_controller,xd_dtc_init_drive," DTC 5150X" }, /* Todd Fries, tfries@umr.edu */
	{ 0x000B,"CXD23A Not an IBM ROM (C)Copyright Data Technology Corp 12/03/88",xd_dtc_init_controller,xd_dtc_init_drive," DTC 5150X" }, /* Pat Mackinlay, pat@it.com.au */
	{ 0x0008,"07/15/86(C) Copyright 1986 Western Digital Corp.",xd_wd_init_controller,xd_wd_init_drive," Western Dig. 1002-27X" }, /* Andrzej Krzysztofowicz, ankry@mif.pg.gda.pl */
	{ 0x0008,"06/24/88(C) Copyright 1988 Western Digital Corp.",xd_wd_init_controller,xd_wd_init_drive," Western Dig. WDXT-GEN2" }, /* Dan Newcombe, newcombe@aa.csc.peachnet.edu */
	{ 0x0015,"SEAGATE ST11 BIOS REVISION",xd_seagate_init_controller,xd_seagate_init_drive," Seagate ST11M/R" }, /* Salvador Abreu, spa@fct.unl.pt */
	{ 0x0010,"ST11R BIOS",xd_seagate_init_controller,xd_seagate_init_drive," Seagate ST11M/R" }, /* Risto Kankkunen, risto.kankkunen@cs.helsinki.fi */
	{ 0x0010,"ST11 BIOS v1.7",xd_seagate_init_controller,xd_seagate_init_drive," Seagate ST11R" }, /* Alan Hourihane, alanh@fairlite.demon.co.uk */
	{ 0x1000,"(c)Copyright 1987 SMS",xd_omti_init_controller,xd_omti_init_drive,"n OMTI 5520" }, /* Dirk Melchers, dirk@merlin.nbg.sub.org */
	{ 0x0006,"COPYRIGHT XEBEC (C) 1984",xd_xebec_init_controller,xd_xebec_init_drive," XEBEC" }, /* Andrzej Krzysztofowicz, ankry@mif.pg.gda.pl */
};

static unsigned int xd_bases[] __initdata =
{
	0xC8000, 0xCA000, 0xCC000,
	0xCE000, 0xD0000, 0xD2000,
	0xD4000, 0xD6000, 0xD8000,
	0xDA000, 0xDC000, 0xDE000,
	0xE0000
};

static struct hd_struct xd_struct[XD_MAXDRIVES << 6];
static int xd_sizes[XD_MAXDRIVES << 6], xd_access[XD_MAXDRIVES] = { 0, 0 };
static int xd_blocksizes[XD_MAXDRIVES << 6];
static struct gendisk xd_gendisk = {
	MAJOR_NR,	/* Major number */
	"xd",		/* Major name */
	6,		/* Bits to shift to get real from partition */
	1 << 6,		/* Number of partitions per real */
	XD_MAXDRIVES,	/* maximum number of real */
#ifdef MODULE
	NULL,		/* called from init_module */
#else
        xd_geninit,     /* init function */
#endif
	xd_struct,	/* hd struct */
	xd_sizes,	/* block sizes */
	0,		/* number */
	(void *) xd_info,	/* internal */
	NULL		/* next */
};
static struct file_operations xd_fops = {
	NULL,			/* lseek - default */
	block_read,		/* read - general block-dev read */
	block_write,		/* write - general block-dev write */
	NULL,			/* readdir - bad */
	NULL,			/* poll */
	xd_ioctl,		/* ioctl */
	NULL,			/* mmap */
	xd_open,		/* open */
	NULL,			/* flush */
	xd_release,		/* release */
	block_fsync		/* fsync */
};
static DECLARE_WAIT_QUEUE_HEAD(xd_wait_int);
static DECLARE_WAIT_QUEUE_HEAD(xd_wait_open);
static u_char xd_valid[XD_MAXDRIVES] = { 0,0 };
static u_char xd_drives = 0, xd_irq = 5, xd_dma = 3, xd_maxsectors;
static u_char xd_override __initdata = 0, xd_type = 0;
static u_short xd_iobase = 0x320;
static int xd_geo[XD_MAXDRIVES*3] __initdata = { 0,0,0,0,0,0 };

static volatile int xdc_busy = 0;
static DECLARE_WAIT_QUEUE_HEAD(xdc_wait);

typedef void (*timeout_fn)(unsigned long);
static struct timer_list xd_timer = { NULL, NULL, 0, 0, (timeout_fn) xd_wakeup },
			 xd_watchdog_int = { NULL, NULL, 0, 0, (timeout_fn) xd_watchdog };

static volatile u_char xd_error;
static int nodma = XD_DONT_USE_DMA;

/* xd_init: register the block device number and set up pointer tables */
int __init xd_init (void)
{
	if (register_blkdev(MAJOR_NR,"xd",&xd_fops)) {
		printk("xd: Unable to get major number %d\n",MAJOR_NR);
		return -1;
	}
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	read_ahead[MAJOR_NR] = 8;	/* 8 sector (4kB) read ahead */
	xd_gendisk.next = gendisk_head;
	gendisk_head = &xd_gendisk;

	return 0;
}

/* xd_detect: scan the possible BIOS ROM locations for the signature strings */
static u_char __init xd_detect (u_char *controller, unsigned int *address)
{
	u_char i,j,found = 0;

	if (xd_override)
	{
		*controller = xd_type;
		*address = 0;
		return(1);
	}

	for (i = 0; i < (sizeof(xd_bases) / sizeof(xd_bases[0])) && !found; i++)
		for (j = 1; j < (sizeof(xd_sigs) / sizeof(xd_sigs[0])) && !found; j++)
			if (isa_check_signature(xd_bases[i] + xd_sigs[j].offset,xd_sigs[j].string,strlen(xd_sigs[j].string))) {
				*controller = j;
				xd_type = j;
				*address = xd_bases[i];
				found++;
			}
	return (found);
}

/* xd_geninit: grab the IRQ and DMA channel, initialise the drives */
/* and set up the "raw" device entries in the table */
static void __init xd_geninit (struct gendisk *ignored)
{
	u_char i,controller;
	unsigned int address;

	if (xd_detect(&controller,&address)) {

		printk("Detected a%s controller (type %d) at address %06x\n",xd_sigs[controller].name,controller,address);
		if (check_region(xd_iobase,4)) {
			printk("xd: Ports at 0x%x are not available\n",xd_iobase);
			return;
		}
		request_region(xd_iobase,4,"xd");
		if (controller)
			xd_sigs[controller].init_controller(address);
		xd_drives = xd_initdrives(xd_sigs[controller].init_drive);
		
		printk("Detected %d hard drive%s (using IRQ%d & DMA%d)\n",xd_drives,xd_drives == 1 ? "" : "s",xd_irq,xd_dma);
		for (i = 0; i < xd_drives; i++)
			printk(" xd%c: CHS=%d/%d/%d\n",'a'+i,xd_info[i].cylinders,xd_info[i].heads,xd_info[i].sectors);

	}
	if (xd_drives) {
		if (!request_irq(xd_irq,xd_interrupt_handler, 0, "XT hard disk", NULL)) {
			if (request_dma(xd_dma,"xd")) {
				printk("xd: unable to get DMA%d\n",xd_dma);
				free_irq(xd_irq, NULL);
			}
		}
		else
			printk("xd: unable to get IRQ%d\n",xd_irq);
	}

	for (i = 0; i < xd_drives; i++) {
		xd_struct[i << 6].nr_sects = xd_info[i].heads * xd_info[i].cylinders * xd_info[i].sectors;
		xd_valid[i] = 1;
	}

	xd_gendisk.nr_real = xd_drives;

	for(i=0;i<(XD_MAXDRIVES << 6);i++) xd_blocksizes[i] = 1024;
	blksize_size[MAJOR_NR] = xd_blocksizes;
}

/* xd_open: open a device */
static int xd_open (struct inode *inode,struct file *file)
{
	int dev = DEVICE_NR(inode->i_rdev);

	if (dev < xd_drives) {
		while (!xd_valid[dev])
			sleep_on(&xd_wait_open);

#ifdef MODULE
		MOD_INC_USE_COUNT;
#endif /* MODULE */

		xd_access[dev]++;

		return (0);
	}
	else
		return -ENXIO;
}

/* do_xd_request: handle an incoming request */
static void do_xd_request (void)
{
	u_int block,count,retry;
	int code;

	sti();
	if (xdc_busy)
		return;
	while (code = 0, CURRENT) {
		INIT_REQUEST;	/* do some checking on the request structure */

		if (CURRENT_DEV < xd_drives
		    && CURRENT->sector + CURRENT->nr_sectors
		         <= xd_struct[MINOR(CURRENT->rq_dev)].nr_sects) {
			block = CURRENT->sector + xd_struct[MINOR(CURRENT->rq_dev)].start_sect;
			count = CURRENT->nr_sectors;

			switch (CURRENT->cmd) {
				case READ:
				case WRITE:
					for (retry = 0; (retry < XD_RETRIES) && !code; retry++)
						code = xd_readwrite(CURRENT->cmd,CURRENT_DEV,CURRENT->buffer,block,count);
					break;
				default:
					printk("do_xd_request: unknown request\n");
					break;
			}
		}
		end_request(code);	/* wrap up, 0 = fail, 1 = success */
	}
}

/* xd_ioctl: handle device ioctl's */
static int xd_ioctl (struct inode *inode,struct file *file,u_int cmd,u_long arg)
{
	int dev;

	if ((!inode) || !(inode->i_rdev))
		return -EINVAL;
 	dev = DEVICE_NR(inode->i_rdev);

	if (dev >= xd_drives) return -EINVAL;
	switch (cmd) {
		case HDIO_GETGEO:
		{
			struct hd_geometry g;
			struct hd_geometry *geometry = (struct hd_geometry *) arg;
			if (!geometry) return -EINVAL;
			g.heads = xd_info[dev].heads;
			g.sectors = xd_info[dev].sectors;
			g.cylinders = xd_info[dev].cylinders;
			g.start = xd_struct[MINOR(inode->i_rdev)].start_sect;
			return copy_to_user(geometry, &g, sizeof g) ? -EFAULT : 0;
		}
		case BLKGETSIZE:
			if (!arg) return -EINVAL;
			return put_user(xd_struct[MINOR(inode->i_rdev)].nr_sects,(long *) arg);
		case HDIO_SET_DMA:
			if (!capable(CAP_SYS_ADMIN)) return -EACCES;
			if (xdc_busy) return -EBUSY;
			nodma = !arg;
			if (nodma && xd_dma_buffer) {
				xd_dma_mem_free((unsigned long)xd_dma_buffer, xd_maxsectors * 0x200);
				xd_dma_buffer = 0;
			}
			return 0;
		case HDIO_GET_DMA:
			return put_user(!nodma, (long *) arg);
		case HDIO_GET_MULTCOUNT:
			return put_user(xd_maxsectors, (long *) arg);
		case BLKRRPART:
			if (!capable(CAP_SYS_ADMIN)) 
				return -EACCES;
			return xd_reread_partitions(inode->i_rdev);

		case BLKFLSBUF:
		case BLKROSET:
		case BLKROGET:
		case BLKRASET:
		case BLKRAGET:
		case BLKPG:
			return blk_ioctl(inode->i_rdev, cmd, arg);

		default:
			return -EINVAL;
	}
}

/* xd_release: release the device */
static int xd_release (struct inode *inode, struct file *file)
{
	int target;

	target= DEVICE_NR(inode->i_rdev);
	if (target < xd_drives) {
		sync_dev(inode->i_rdev);
		xd_access[target]--;

#ifdef MODULE
		MOD_DEC_USE_COUNT;
#endif /* MODULE */

	}
	return 0;
}

/* xd_reread_partitions: rereads the partition table from a drive */
static int xd_reread_partitions(kdev_t dev)
{
	int target;
	int start;
	int partition;
	
	target = DEVICE_NR(dev);
 	start = target << xd_gendisk.minor_shift;

	cli();
	xd_valid[target] = (xd_access[target] != 1);
        sti();
	if (xd_valid[target])
		return -EBUSY;

	for (partition = xd_gendisk.max_p - 1; partition >= 0; partition--) {
		int minor = (start | partition);
		kdev_t devp = MKDEV(MAJOR_NR, minor);
		struct super_block * sb = get_super(devp);
		
		sync_dev(devp);
		if (sb)
			invalidate_inodes(sb);
		invalidate_buffers(devp);
		xd_gendisk.part[minor].start_sect = 0;
		xd_gendisk.part[minor].nr_sects = 0;
	};

	xd_gendisk.part[start].nr_sects = xd_info[target].heads * xd_info[target].cylinders * xd_info[target].sectors;
	resetup_one_dev(&xd_gendisk,target);

	xd_valid[target] = 1;
	wake_up(&xd_wait_open);

	return 0;
}

/* xd_readwrite: handle a read/write request */
static int xd_readwrite (u_char operation,u_char drive,char *buffer,u_int block,u_int count)
{
	u_char cmdblk[6],sense[4];
	u_short track,cylinder;
	u_char head,sector,control,mode = PIO_MODE,temp;
	char **real_buffer;
	register int i;
	
#ifdef DEBUG_READWRITE
	printk("xd_readwrite: operation = %s, drive = %d, buffer = 0x%X, block = %d, count = %d\n",operation == READ ? "read" : "write",drive,buffer,block,count);
#endif /* DEBUG_READWRITE */

	control = xd_info[drive].control;
	if (!xd_dma_buffer)
		xd_dma_buffer = (char *)xd_dma_mem_alloc(xd_maxsectors * 0x200);
	while (count) {
		temp = count < xd_maxsectors ? count : xd_maxsectors;

		track = block / xd_info[drive].sectors;
		head = track % xd_info[drive].heads;
		cylinder = track / xd_info[drive].heads;
		sector = block % xd_info[drive].sectors;

#ifdef DEBUG_READWRITE
		printk("xd_readwrite: drive = %d, head = %d, cylinder = %d, sector = %d, count = %d\n",drive,head,cylinder,sector,temp);
#endif /* DEBUG_READWRITE */

		if (xd_dma_buffer) {
			mode = xd_setup_dma(operation == READ ? DMA_MODE_READ : DMA_MODE_WRITE,(u_char *)(xd_dma_buffer),temp * 0x200);
			real_buffer = &xd_dma_buffer;
			for (i=0; i < (temp * 0x200); i++)
				xd_dma_buffer[i] = buffer[i];
		}
		else
			real_buffer = &buffer;

		xd_build(cmdblk,operation == READ ? CMD_READ : CMD_WRITE,drive,head,cylinder,sector,temp & 0xFF,control);

		switch (xd_command(cmdblk,mode,(u_char *)(*real_buffer),(u_char *)(*real_buffer),sense,XD_TIMEOUT)) {
			case 1:
				printk("xd%c: %s timeout, recalibrating drive\n",'a'+drive,(operation == READ ? "read" : "write"));
				xd_recalibrate(drive);
				return (0);
			case 2:
				if (sense[0] & 0x30) {
					printk("xd%c: %s - ",'a'+drive,(operation == READ ? "reading" : "writing"));
					switch ((sense[0] & 0x30) >> 4) {
					case 0: printk("drive error, code = 0x%X",sense[0] & 0x0F);
						break;
					case 1: printk("controller error, code = 0x%X",sense[0] & 0x0F);
						break;
					case 2: printk("command error, code = 0x%X",sense[0] & 0x0F);
						break;
					case 3: printk("miscellaneous error, code = 0x%X",sense[0] & 0x0F);
						break;
					}
				}
				if (sense[0] & 0x80)
					printk(" - CHS = %d/%d/%d\n",((sense[2] & 0xC0) << 2) | sense[3],sense[1] & 0x1F,sense[2] & 0x3F);
				/*	reported drive number = (sense[1] & 0xE0) >> 5 */
				else
					printk(" - no valid disk address\n");
				return (0);
		}
		if (xd_dma_buffer)
			for (i=0; i < (temp * 0x200); i++)
				buffer[i] = xd_dma_buffer[i];

		count -= temp, buffer += temp * 0x200, block += temp;
	}
	return (1);
}

/* xd_recalibrate: recalibrate a given drive and reset controller if necessary */
static void xd_recalibrate (u_char drive)
{
	u_char cmdblk[6];
	
	xd_build(cmdblk,CMD_RECALIBRATE,drive,0,0,0,0,0);
	if (xd_command(cmdblk,PIO_MODE,0,0,0,XD_TIMEOUT * 8))
		printk("xd%c: warning! error recalibrating, controller may be unstable\n", 'a'+drive);
}

/* xd_interrupt_handler: interrupt service routine */
static void xd_interrupt_handler(int irq, void *dev_id, struct pt_regs * regs)
{
	if (inb(XD_STATUS) & STAT_INTERRUPT) {							/* check if it was our device */
#ifdef DEBUG_OTHER
		printk("xd_interrupt_handler: interrupt detected\n");
#endif /* DEBUG_OTHER */
		outb(0,XD_CONTROL);								/* acknowledge interrupt */
		wake_up(&xd_wait_int);								/* and wake up sleeping processes */
	}
	else
		printk("xd: unexpected interrupt\n");
}

/* xd_setup_dma: set up the DMA controller for a data transfer */
static u_char xd_setup_dma (u_char mode,u_char *buffer,u_int count)
{
	unsigned long f;
	
	if (nodma)
		return (PIO_MODE);
	if (((u_int) buffer & 0xFFFF0000) != (((u_int) buffer + count) & 0xFFFF0000)) {
#ifdef DEBUG_OTHER
		printk("xd_setup_dma: using PIO, transfer overlaps 64k boundary\n");
#endif /* DEBUG_OTHER */
		return (PIO_MODE);
	}
	
	f=claim_dma_lock();
	disable_dma(xd_dma);
	clear_dma_ff(xd_dma);
	set_dma_mode(xd_dma,mode);
	set_dma_addr(xd_dma,(u_int) buffer);
	set_dma_count(xd_dma,count);
	
	release_dma_lock(f);

	return (DMA_MODE);			/* use DMA and INT */
}

/* xd_build: put stuff into an array in a format suitable for the controller */
static u_char *xd_build (u_char *cmdblk,u_char command,u_char drive,u_char head,u_short cylinder,u_char sector,u_char count,u_char control)
{
	cmdblk[0] = command;
	cmdblk[1] = ((drive & 0x07) << 5) | (head & 0x1F);
	cmdblk[2] = ((cylinder & 0x300) >> 2) | (sector & 0x3F);
	cmdblk[3] = cylinder & 0xFF;
	cmdblk[4] = count;
	cmdblk[5] = control;
	
	return (cmdblk);
}

/* xd_wakeup is called from timer interrupt */
static void xd_wakeup (void)
{
	wake_up(&xdc_wait);
}

/* xd_wakeup is called from timer interrupt */
static void xd_watchdog (void)
{
	xd_error = 1;
	wake_up(&xd_wait_int);
}

/* xd_waitport: waits until port & mask == flags or a timeout occurs. return 1 for a timeout */
static inline u_char xd_waitport (u_short port,u_char flags,u_char mask,u_long timeout)
{
	u_long expiry = jiffies + timeout;
	int success;

	xdc_busy = 1;
	while ((success = ((inb(port) & mask) != flags)) && time_before(jiffies, expiry)) {
		xd_timer.expires = jiffies;
		cli();
		add_timer(&xd_timer);
		sleep_on(&xdc_wait);
		del_timer(&xd_timer);
		sti();
	}
	xdc_busy = 0;
	return (success);
}

static inline u_int xd_wait_for_IRQ (void)
{
	unsigned long flags;
	xd_watchdog_int.expires = jiffies + 8 * HZ;
	add_timer(&xd_watchdog_int);
	
	flags=claim_dma_lock();
	enable_dma(xd_dma);
	release_dma_lock(flags);
	
	sleep_on(&xd_wait_int);
	del_timer(&xd_watchdog_int);
	xdc_busy = 0;
	
	flags=claim_dma_lock();
	disable_dma(xd_dma);
	release_dma_lock(flags);
	
	if (xd_error) {
		printk("xd: missed IRQ - command aborted\n");
		xd_error = 0;
		return (1);
	}
	return (0);
}

/* xd_command: handle all data transfers necessary for a single command */
static u_int xd_command (u_char *command,u_char mode,u_char *indata,u_char *outdata,u_char *sense,u_long timeout)
{
	u_char cmdblk[6],csb,complete = 0;

#ifdef DEBUG_COMMAND
	printk("xd_command: command = 0x%X, mode = 0x%X, indata = 0x%X, outdata = 0x%X, sense = 0x%X\n",command,mode,indata,outdata,sense);
#endif /* DEBUG_COMMAND */

	outb(0,XD_SELECT);
	outb(mode,XD_CONTROL);

	if (xd_waitport(XD_STATUS,STAT_SELECT,STAT_SELECT,timeout))
		return (1);

	while (!complete) {
		if (xd_waitport(XD_STATUS,STAT_READY,STAT_READY,timeout))
			return (1);

		switch (inb(XD_STATUS) & (STAT_COMMAND | STAT_INPUT)) {
			case 0:
				if (mode == DMA_MODE) {
					if (xd_wait_for_IRQ())
						return (1);
				} else
					outb(outdata ? *outdata++ : 0,XD_DATA);
				break;
			case STAT_INPUT:
				if (mode == DMA_MODE) {
					if (xd_wait_for_IRQ())
						return (1);
				} else
					if (indata)
						*indata++ = inb(XD_DATA);
					else
						inb(XD_DATA);
				break;
			case STAT_COMMAND:
				outb(command ? *command++ : 0,XD_DATA);
				break;
			case STAT_COMMAND | STAT_INPUT:
				complete = 1;
				break;
		}
	}
	csb = inb(XD_DATA);

	if (xd_waitport(XD_STATUS,0,STAT_SELECT,timeout))					/* wait until deselected */
		return (1);

	if (csb & CSB_ERROR) {									/* read sense data if error */
		xd_build(cmdblk,CMD_SENSE,(csb & CSB_LUN) >> 5,0,0,0,0,0);
		if (xd_command(cmdblk,0,sense,0,0,XD_TIMEOUT))
			printk("xd: warning! sense command failed!\n");
	}

#ifdef DEBUG_COMMAND
	printk("xd_command: completed with csb = 0x%X\n",csb);
#endif /* DEBUG_COMMAND */

	return (csb & CSB_ERROR);
}

static u_char __init xd_initdrives (void (*init_drive)(u_char drive))
{
	u_char cmdblk[6],i,count = 0;

	for (i = 0; i < XD_MAXDRIVES; i++) {
		xd_build(cmdblk,CMD_TESTREADY,i,0,0,0,0,0);
		if (!xd_command(cmdblk,PIO_MODE,0,0,0,XD_TIMEOUT * 8)) {
	 		xd_timer.expires = jiffies + XD_INIT_DISK_DELAY;
			add_timer(&xd_timer);
			sleep_on(&xdc_wait);

			init_drive(count);
			count++;

	 		xd_timer.expires = jiffies + XD_INIT_DISK_DELAY;
			add_timer(&xd_timer);
			sleep_on(&xdc_wait);
		}
	}
	return (count);
}

static void __init xd_manual_geo_set (u_char drive)
{
	xd_info[drive].heads = (u_char)(xd_geo[3 * drive + 1]);
	xd_info[drive].cylinders = (u_short)(xd_geo[3 * drive]);
	xd_info[drive].sectors = (u_char)(xd_geo[3 * drive + 2]);
}

static void __init xd_dtc_init_controller (unsigned int address)
{
	switch (address) {
		case 0x00000:
		case 0xC8000:	break;			/*initial: 0x320 */
		case 0xCA000:	xd_iobase = 0x324; 
		case 0xD0000:				/*5150CX*/
		case 0xD8000:	break;			/*5150CX & 5150XL*/
		default:        printk("xd_dtc_init_controller: unsupported BIOS address %06x\n",address);
				break;
	}
	xd_maxsectors = 0x01;		/* my card seems to have trouble doing multi-block transfers? */

	outb(0,XD_RESET);		/* reset the controller */
}


static void __init xd_dtc5150cx_init_drive (u_char drive)
{
	/* values from controller's BIOS - BIOS chip may be removed */
	static u_short geometry_table[][4] = {
		{0x200,8,0x200,0x100},
		{0x267,2,0x267,0x267},
		{0x264,4,0x264,0x80},
		{0x132,4,0x132,0x0},
		{0x132,2,0x80, 0x132},
		{0x177,8,0x177,0x0},
		{0x132,8,0x84, 0x0},
		{},  /* not used */
		{0x132,6,0x80, 0x100},
		{0x200,6,0x100,0x100},
		{0x264,2,0x264,0x80},
		{0x280,4,0x280,0x100},
		{0x2B9,3,0x2B9,0x2B9},
		{0x2B9,5,0x2B9,0x2B9},
		{0x280,6,0x280,0x100},
		{0x132,4,0x132,0x0}};
	u_char n;

	n = inb(XD_JUMPER);
	n = (drive ? n : (n >> 2)) & 0x33;
	n = (n | (n >> 2)) & 0x0F;
	if (xd_geo[3*drive])
		xd_manual_geo_set(drive);
	else
		if (n != 7) {	
			xd_info[drive].heads = (u_char)(geometry_table[n][1]);			/* heads */
			xd_info[drive].cylinders = geometry_table[n][0];	/* cylinders */
			xd_info[drive].sectors = 17;				/* sectors */
#if 0
			xd_info[drive].rwrite = geometry_table[n][2];	/* reduced write */
			xd_info[drive].precomp = geometry_table[n][3]		/* write precomp */
			xd_info[drive].ecc = 0x0B;				/* ecc length */
#endif /* 0 */
		}
		else {
			printk("xd%c: undetermined drive geometry\n",'a'+drive);
			return;
		}
	xd_info[drive].control = 5;				/* control byte */
	xd_setparam(CMD_DTCSETPARAM,drive,xd_info[drive].heads,xd_info[drive].cylinders,geometry_table[n][2],geometry_table[n][3],0x0B);
	xd_recalibrate(drive);
}

static void __init xd_dtc_init_drive (u_char drive)
{
	u_char cmdblk[6],buf[64];

	xd_build(cmdblk,CMD_DTCGETGEOM,drive,0,0,0,0,0);
	if (!xd_command(cmdblk,PIO_MODE,buf,0,0,XD_TIMEOUT * 2)) {
		xd_info[drive].heads = buf[0x0A];			/* heads */
		xd_info[drive].cylinders = ((u_short *) (buf))[0x04];	/* cylinders */
		xd_info[drive].sectors = 17;				/* sectors */
		if (xd_geo[3*drive])
			xd_manual_geo_set(drive);
#if 0
		xd_info[drive].rwrite = ((u_short *) (buf + 1))[0x05];	/* reduced write */
		xd_info[drive].precomp = ((u_short *) (buf + 1))[0x06];	/* write precomp */
		xd_info[drive].ecc = buf[0x0F];				/* ecc length */
#endif /* 0 */
		xd_info[drive].control = 0;				/* control byte */

		xd_setparam(CMD_DTCSETPARAM,drive,xd_info[drive].heads,xd_info[drive].cylinders,((u_short *) (buf + 1))[0x05],((u_short *) (buf + 1))[0x06],buf[0x0F]);
		xd_build(cmdblk,CMD_DTCSETSTEP,drive,0,0,0,0,7);
		if (xd_command(cmdblk,PIO_MODE,0,0,0,XD_TIMEOUT * 2))
			printk("xd_dtc_init_drive: error setting step rate for xd%c\n", 'a'+drive);
	}
	else
		printk("xd_dtc_init_drive: error reading geometry for xd%c\n", 'a'+drive);
}

static void __init xd_wd_init_controller (unsigned int address)
{
	switch (address) {
		case 0x00000:
		case 0xC8000:	break;			/*initial: 0x320 */
		case 0xCA000:	xd_iobase = 0x324; break;
		case 0xCC000:   xd_iobase = 0x328; break;
		case 0xCE000:   xd_iobase = 0x32C; break;
		case 0xD0000:	xd_iobase = 0x328; break; /* ? */
		case 0xD8000:	xd_iobase = 0x32C; break; /* ? */
		default:        printk("xd_wd_init_controller: unsupported BIOS address %06x\n",address);
				break;
	}
	xd_maxsectors = 0x01;		/* this one doesn't wrap properly either... */

	outb(0,XD_RESET);		/* reset the controller */

	xd_timer.expires = jiffies + XD_INIT_DISK_DELAY;
	add_timer(&xd_timer);
	sleep_on(&xdc_wait);
}

static void __init xd_wd_init_drive (u_char drive)
{
	/* values from controller's BIOS - BIOS may be disabled */
	static u_short geometry_table[][4] = {
		{0x264,4,0x1C2,0x1C2},   /* common part */
		{0x132,4,0x099,0x0},
		{0x267,2,0x1C2,0x1C2},
		{0x267,4,0x1C2,0x1C2},

		{0x334,6,0x335,0x335},   /* 1004 series RLL */
		{0x30E,4,0x30F,0x3DC},
		{0x30E,2,0x30F,0x30F},
		{0x267,4,0x268,0x268},

		{0x3D5,5,0x3D6,0x3D6},   /* 1002 series RLL */
		{0x3DB,7,0x3DC,0x3DC},
		{0x264,4,0x265,0x265},
		{0x267,4,0x268,0x268}};

	u_char cmdblk[6],buf[0x200];
	u_char n = 0,rll,jumper_state,use_jumper_geo;
	u_char wd_1002 = (xd_sigs[xd_type].string[7] == '6');
	
	jumper_state = ~(inb(0x322));
	if (jumper_state & 0x40)
		xd_irq = 9;
	rll = (jumper_state & 0x30) ? (0x04 << wd_1002) : 0;
	xd_build(cmdblk,CMD_READ,drive,0,0,0,1,0);
	if (!xd_command(cmdblk,PIO_MODE,buf,0,0,XD_TIMEOUT * 2)) {
		xd_info[drive].heads = buf[0x1AF];				/* heads */
		xd_info[drive].cylinders = ((u_short *) (buf + 1))[0xD6];	/* cylinders */
		xd_info[drive].sectors = 17;					/* sectors */
		if (xd_geo[3*drive])
			xd_manual_geo_set(drive);
#if 0
		xd_info[drive].rwrite = ((u_short *) (buf))[0xD8];		/* reduced write */
		xd_info[drive].wprecomp = ((u_short *) (buf))[0xDA];		/* write precomp */
		xd_info[drive].ecc = buf[0x1B4];				/* ecc length */
#endif /* 0 */
		xd_info[drive].control = buf[0x1B5];				/* control byte */
		use_jumper_geo = !(xd_info[drive].heads) || !(xd_info[drive].cylinders);
		if (xd_geo[3*drive]) {
			xd_manual_geo_set(drive);
			xd_info[drive].control = rll ? 7 : 5;
		}
		else if (use_jumper_geo) {
			n = (((jumper_state & 0x0F) >> (drive << 1)) & 0x03) | rll;
			xd_info[drive].cylinders = geometry_table[n][0];
			xd_info[drive].heads = (u_char)(geometry_table[n][1]);
			xd_info[drive].control = rll ? 7 : 5;
#if 0
			xd_info[drive].rwrite = geometry_table[n][2];
			xd_info[drive].wprecomp = geometry_table[n][3];
			xd_info[drive].ecc = 0x0B;
#endif /* 0 */
		}
		if (!wd_1002) {
			if (use_jumper_geo)
				xd_setparam(CMD_WDSETPARAM,drive,xd_info[drive].heads,xd_info[drive].cylinders,
					geometry_table[n][2],geometry_table[n][3],0x0B);
			else
				xd_setparam(CMD_WDSETPARAM,drive,xd_info[drive].heads,xd_info[drive].cylinders,
					((u_short *) (buf))[0xD8],((u_short *) (buf))[0xDA],buf[0x1B4]);
		}
	/* 1002 based RLL controler requests converted addressing, but reports physical 
	   (physical 26 sec., logical 17 sec.) 
	   1004 based ???? */
		if (rll & wd_1002) {
			if ((xd_info[drive].cylinders *= 26,
			     xd_info[drive].cylinders /= 17) > 1023)
				xd_info[drive].cylinders = 1023;  /* 1024 ? */
#if 0
			xd_info[drive].rwrite *= 26; 
			xd_info[drive].rwrite /= 17;
			xd_info[drive].wprecomp *= 26
			xd_info[drive].wprecomp /= 17;
#endif /* 0 */
		}
	}
	else
		printk("xd_wd_init_drive: error reading geometry for xd%c\n",'a'+drive);	

}

static void __init xd_seagate_init_controller (unsigned int address)
{
	switch (address) {
		case 0x00000:
		case 0xC8000:	break;			/*initial: 0x320 */
		case 0xD0000:	xd_iobase = 0x324; break;
		case 0xD8000:	xd_iobase = 0x328; break;
		case 0xE0000:	xd_iobase = 0x32C; break;
		default:	printk("xd_seagate_init_controller: unsupported BIOS address %06x\n",address);
				break;
	}
	xd_maxsectors = 0x40;

	outb(0,XD_RESET);		/* reset the controller */
}

static void __init xd_seagate_init_drive (u_char drive)
{
	u_char cmdblk[6],buf[0x200];

	xd_build(cmdblk,CMD_ST11GETGEOM,drive,0,0,0,1,0);
	if (!xd_command(cmdblk,PIO_MODE,buf,0,0,XD_TIMEOUT * 2)) {
		xd_info[drive].heads = buf[0x04];				/* heads */
		xd_info[drive].cylinders = (buf[0x02] << 8) | buf[0x03];	/* cylinders */
		xd_info[drive].sectors = buf[0x05];				/* sectors */
		xd_info[drive].control = 0;					/* control byte */
	}
	else
		printk("xd_seagate_init_drive: error reading geometry from xd%c\n", 'a'+drive);
}

/* Omti support courtesy Dirk Melchers */
static void __init xd_omti_init_controller (unsigned int address)
{
	switch (address) {
		case 0x00000:
		case 0xC8000:	break;			/*initial: 0x320 */
		case 0xD0000:	xd_iobase = 0x324; break;
		case 0xD8000:	xd_iobase = 0x328; break;
		case 0xE0000:	xd_iobase = 0x32C; break;
		default:	printk("xd_omti_init_controller: unsupported BIOS address %06x\n",address);
				break;
	}
	
	xd_maxsectors = 0x40;

	outb(0,XD_RESET);		/* reset the controller */
}

static void __init xd_omti_init_drive (u_char drive)
{
	/* gets infos from drive */
	xd_override_init_drive(drive);

	/* set other parameters, Hardcoded, not that nice :-) */
	xd_info[drive].control = 2;
}

/* Xebec support (AK) */
static void __init xd_xebec_init_controller (unsigned int address)
{
/* iobase may be set manually in range 0x300 - 0x33C
      irq may be set manually to 2(9),3,4,5,6,7
      dma may be set manually to 1,2,3
	(How to detect them ???)
BIOS address may be set manually in range 0x0 - 0xF8000
If you need non-standard settings use the xd=... command */

	switch (address) {
		case 0x00000:
		case 0xC8000:	/* initially: xd_iobase==0x320 */
		case 0xD0000:
		case 0xD2000:
		case 0xD4000:
		case 0xD6000:
		case 0xD8000:
		case 0xDA000:
		case 0xDC000:
		case 0xDE000:
		case 0xE0000:	break;
		default:	printk("xd_xebec_init_controller: unsupported BIOS address %06x\n",address);
				break;
		}

	xd_maxsectors = 0x01;
	outb(0,XD_RESET);		/* reset the controller */

	xd_timer.expires = jiffies + XD_INIT_DISK_DELAY;
	add_timer(&xd_timer);
	sleep_on(&xdc_wait);
}

static void __init xd_xebec_init_drive (u_char drive)
{
	/* values from controller's BIOS - BIOS chip may be removed */
	static u_short geometry_table[][5] = {
		{0x132,4,0x080,0x080,0x7},
		{0x132,4,0x080,0x080,0x17},
		{0x264,2,0x100,0x100,0x7},
		{0x264,2,0x100,0x100,0x17},
		{0x132,8,0x080,0x080,0x7},
		{0x132,8,0x080,0x080,0x17},
		{0x264,4,0x100,0x100,0x6},
		{0x264,4,0x100,0x100,0x17},
		{0x2BC,5,0x2BC,0x12C,0x6},
		{0x3A5,4,0x3A5,0x3A5,0x7},
		{0x26C,6,0x26C,0x26C,0x7},
		{0x200,8,0x200,0x100,0x17},
		{0x400,5,0x400,0x400,0x7},
		{0x400,6,0x400,0x400,0x7},
		{0x264,8,0x264,0x200,0x17},
		{0x33E,7,0x33E,0x200,0x7}};
	u_char n;

	n = inb(XD_JUMPER) & 0x0F; /* BIOS's drive number: same geometry 
					is assumed for BOTH drives */
	if (xd_geo[3*drive])
		xd_manual_geo_set(drive);
	else {
		xd_info[drive].heads = (u_char)(geometry_table[n][1]);			/* heads */
		xd_info[drive].cylinders = geometry_table[n][0];	/* cylinders */
		xd_info[drive].sectors = 17;				/* sectors */
#if 0
		xd_info[drive].rwrite = geometry_table[n][2];	/* reduced write */
		xd_info[drive].precomp = geometry_table[n][3]		/* write precomp */
		xd_info[drive].ecc = 0x0B;				/* ecc length */
#endif /* 0 */
	}
	xd_info[drive].control = geometry_table[n][4];			/* control byte */
	xd_setparam(CMD_XBSETPARAM,drive,xd_info[drive].heads,xd_info[drive].cylinders,geometry_table[n][2],geometry_table[n][3],0x0B);
	xd_recalibrate(drive);
}

/* xd_override_init_drive: this finds disk geometry in a "binary search" style, narrowing in on the "correct" number of heads
   etc. by trying values until it gets the highest successful value. Idea courtesy Salvador Abreu (spa@fct.unl.pt). */
static void __init xd_override_init_drive (u_char drive)
{
	u_short min[] = { 0,0,0 },max[] = { 16,1024,64 },test[] = { 0,0,0 };
	u_char cmdblk[6],i;

	if (xd_geo[3*drive])
		xd_manual_geo_set(drive);
	else {
		for (i = 0; i < 3; i++) {
			while (min[i] != max[i] - 1) {
				test[i] = (min[i] + max[i]) / 2;
				xd_build(cmdblk,CMD_SEEK,drive,(u_char) test[0],(u_short) test[1],(u_char) test[2],0,0);
				if (!xd_command(cmdblk,PIO_MODE,0,0,0,XD_TIMEOUT * 2))
					min[i] = test[i];
				else
					max[i] = test[i];
			}
			test[i] = min[i];
		}
		xd_info[drive].heads = (u_char) min[0] + 1;
		xd_info[drive].cylinders = (u_short) min[1] + 1;
		xd_info[drive].sectors = (u_char) min[2] + 1;
	}
	xd_info[drive].control = 0;
}

/* xd_setup: initialise controler from command line parameters */
void __init xd_setup (char *command,int *integers)
{
	switch (integers[0]) {
		case 4: if (integers[4] < 0)
				nodma = 1;
			else if (integers[4] < 8)
				xd_dma = integers[4];
		case 3: if ((integers[3] > 0) && (integers[3] <= 0x3FC))
				xd_iobase = integers[3];
		case 2: if ((integers[2] > 0) && (integers[2] < 16))
				xd_irq = integers[2];
		case 1: xd_override = 1;
			if ((integers[1] >= 0) && (integers[1] < (sizeof(xd_sigs) / sizeof(xd_sigs[0]))))
				xd_type = integers[1];
		case 0: break;
		default:printk("xd: too many parameters for xd\n");
	}
	xd_maxsectors = 0x01;
}

#ifndef MODULE
/* xd_manual_geo_init: initialise drive geometry from command line parameters
   (used only for WD drives) */
void __init xd_manual_geo_init (char *command,int *integers)
{
	int i;
	if (integers[0]%3 != 0) {
		printk("xd: incorrect number of parameters for xd_geo\n");
		return;
	}
	for (i = 0; (i < integers[0]) && (i < 3*XD_MAXDRIVES); i++)
		xd_geo[i] = integers[i+1];
}
#endif /* MODULE */

/* xd_setparam: set the drive characteristics */
static void __init xd_setparam (u_char command,u_char drive,u_char heads,u_short cylinders,u_short rwrite,u_short wprecomp,u_char ecc)
{
	u_char cmdblk[14];

	xd_build(cmdblk,command,drive,0,0,0,0,0);
	cmdblk[6] = (u_char) (cylinders >> 8) & 0x03;
	cmdblk[7] = (u_char) (cylinders & 0xFF);
	cmdblk[8] = heads & 0x1F;
	cmdblk[9] = (u_char) (rwrite >> 8) & 0x03;
	cmdblk[10] = (u_char) (rwrite & 0xFF);
	cmdblk[11] = (u_char) (wprecomp >> 8) & 0x03;
	cmdblk[12] = (u_char) (wprecomp & 0xFF);
	cmdblk[13] = ecc;

	/* Some controllers require geometry info as data, not command */

	if (xd_command(cmdblk,PIO_MODE,0,&cmdblk[6],0,XD_TIMEOUT * 2))
		printk("xd: error setting characteristics for xd%c\n", 'a'+drive);
}


#ifdef MODULE
static int xd[5] = { -1,-1,-1,-1, };

MODULE_PARM(xd, "1-4i");
MODULE_PARM(xd_geo, "3-6i");
MODULE_PARM(nodma, "i");

static void xd_done (void)
{
	struct gendisk ** gdp;
	
	blksize_size[MAJOR_NR] = NULL;
	blk_dev[MAJOR_NR].request_fn = NULL;
	blk_size[MAJOR_NR] = NULL;
	hardsect_size[MAJOR_NR] = NULL;
	read_ahead[MAJOR_NR] = 0;
	for (gdp = &gendisk_head; *gdp; gdp = &((*gdp)->next))
		if (*gdp == &xd_gendisk)
			break;
	if (*gdp)
		*gdp = (*gdp)->next;
	release_region(xd_iobase,4);
}

int init_module(void)
{
	int i,count = 0;
	int error = xd_init();
	if (!error)
	{
		printk(KERN_INFO "XD: Loaded as a module.\n");
		for (i = 4; i > 0; i--)
			if(((xd[i] = xd[i-1]) >= 0) && !count)
				count = i;
		if((xd[0] = count))
			xd_setup(NULL, xd);
		xd_geninit(&(struct gendisk) { 0,0,0,0,0,0,0,0,0,0,0 });
		if (!xd_drives) {
					/* no drives detected - unload module */
			unregister_blkdev(MAJOR_NR, "xd");
			xd_done();
			return (-1);
		}
		for (i = 0; i < xd_drives; i++)
			resetup_one_dev(&xd_gendisk, i);
	}
        
	return error;
}

void cleanup_module(void)
{
	int partition,dev,start;

	unregister_blkdev(MAJOR_NR, "xd");
	for (dev = 0; dev < xd_drives; dev++) {
		start = dev << xd_gendisk.minor_shift; 
		for (partition = xd_gendisk.max_p - 1; partition >= 0; partition--) {
			int minor = (start | partition);
			kdev_t devp = MKDEV(MAJOR_NR, minor);
			start = dev << xd_gendisk.minor_shift; 
			sync_dev(devp);
			invalidate_buffers(devp);
		}
	}
	xd_done();
	if (xd_drives) {
		free_irq(xd_irq, NULL);
		free_dma(xd_dma);
		if (xd_dma_buffer)
			xd_dma_mem_free((unsigned long)xd_dma_buffer, xd_maxsectors * 0x200);
	}
}
#else

__setup ("xd=", xd_setup);
__setup ("xd_geo=", xd_manual_geo_init);

#endif /* MODULE */

