/***    ltpc.c -- a driver for the LocalTalk PC card.
 *
 *      Copyright (c) 1995,1996 Bradford W. Johnson <johns393@maroon.tc.umn.edu>
 *
 *      This software may be used and distributed according to the terms
 *      of the GNU General Public License, incorporated herein by reference.
 *
 *      This is ALPHA code at best.  It may not work for you.  It may
 *      damage your equipment.  It may damage your relations with other
 *      users of your network.  Use it at your own risk!
 *
 *      Based in part on:
 *      skeleton.c      by Donald Becker
 *      dummy.c         by Nick Holloway and Alan Cox
 *      loopback.c      by Ross Biro, Fred van Kampen, Donald Becker
 *      the netatalk source code (UMICH)
 *      lots of work on the card...
 *
 *      I do not have access to the (proprietary) SDK that goes with the card.
 *      If you do, I don't want to know about it, and you can probably write
 *      a better driver yourself anyway.  This does mean that the pieces that
 *      talk to the card are guesswork on my part, so use at your own risk!
 *
 *      This is my first try at writing Linux networking code, and is also
 *      guesswork.  Again, use at your own risk!  (Although on this part, I'd
 *      welcome suggestions)
 *
 *      This is a loadable kernel module which seems to work at my site
 *      consisting of a 1.2.13 linux box running netatalk 1.3.3, and with
 *      the kernel support from 1.3.3b2 including patches routing.patch
 *      and ddp.disappears.from.chooser.  In order to run it, you will need
 *      to patch ddp.c and aarp.c in the kernel, but only a little...
 *
 *      I'm fairly confident that while this is arguably badly written, the
 *      problems that people experience will be "higher level", that is, with
 *      complications in the netatalk code.  The driver itself doesn't do
 *      anything terribly complicated -- it pretends to be an ether device
 *      as far as netatalk is concerned, strips the DDP data out of the ether
 *      frame and builds a LLAP packet to send out the card.  In the other
 *      direction, it receives LLAP frames from the card and builds a fake
 *      ether packet that it then tosses up to the networking code.  You can
 *      argue (correctly) that this is an ugly way to do things, but it
 *      requires a minimal amount of fooling with the code in ddp.c and aarp.c.
 *
 *      The card will do a lot more than is used here -- I *think* it has the
 *      layers up through ATP.  Even if you knew how that part works (which I
 *      don't) it would be a big job to carve up the kernel ddp code to insert
 *      things at a higher level, and probably a bad idea...
 *
 *      There are a number of other cards that do LocalTalk on the PC.  If
 *      nobody finds any insurmountable (at the netatalk level) problems
 *      here, this driver should encourage people to put some work into the
 *      other cards (some of which I gather are still commercially available)
 *      and also to put hooks for LocalTalk into the official ddp code.
 *
 *      I welcome comments and suggestions.  This is my first try at Linux
 *      networking stuff, and there are probably lots of things that I did
 *      suboptimally.  
 *
 ***/

/***
 *
 * $Log: ltpc.c,v $
 * Revision 1.8  1997/01/28 05:44:54  bradford
 * Clean up for non-module a little.
 * Hacked about a bit to clean things up - Alan Cox 
 * Probably broken it from the origina 1.8
 *

 * 1998/11/09: David Huggins-Daines <dhd@debian.org>
 * Cleaned up the initialization code to use the standard autoirq methods,
   and to probe for things in the standard order of i/o, irq, dma.  This
   removes the "reset the reset" hack, because I couldn't figure out an
   easy way to get the card to trigger an interrupt after it.
 * Added support for passing configuration parameters on the kernel command
   line and through insmod
 * Changed the device name from "ltalk0" to "lt0", both to conform with the
   other localtalk driver, and to clear up the inconsistency between the
   module and the non-module versions of the driver :-)
 * Added a bunch of comments (I was going to make some enums for the state
   codes and the register offsets, but I'm still not sure exactly what their
   semantics are)
 * Don't poll anymore in interrupt-driven mode
 * It seems to work as a module now (as of 2.1.127), but I don't think
   I'm responsible for that...

 *
 * Revision 1.7  1996/12/12 03:42:33  bradford
 * DMA alloc cribbed from 3c505.c.
 *
 * Revision 1.6  1996/12/12 03:18:58  bradford
 * Added virt_to_bus; works in 2.1.13.
 *
 * Revision 1.5  1996/12/12 03:13:22  root
 * xmitQel initialization -- think through better though.
 *
 * Revision 1.4  1996/06/18 14:55:55  root
 * Change names to ltpc. Tabs. Took a shot at dma alloc,
 * although more needs to be done eventually.
 *
 * Revision 1.3  1996/05/22 14:59:39  root
 * Change dev->open, dev->close to track dummy.c in 1.99.(around 7)
 *
 * Revision 1.2  1996/05/22 14:58:24  root
 * Change tabs mostly.
 *
 * Revision 1.1  1996/04/23 04:45:09  root
 * Initial revision
 *
 * Revision 0.16  1996/03/05 15:59:56  root
 * Change ARPHRD_LOCALTLK definition to the "real" one.
 *
 * Revision 0.15  1996/03/05 06:28:30  root
 * Changes for kernel 1.3.70.  Still need a few patches to kernel, but
 * it's getting closer.
 *
 * Revision 0.14  1996/02/25 17:38:32  root
 * More cleanups.  Removed query to card on get_stats.
 *
 * Revision 0.13  1996/02/21  16:27:40  root
 * Refix debug_print_skb.  Fix mac.raw gotcha that appeared in 1.3.65.
 * Clean up receive code a little.
 *
 * Revision 0.12  1996/02/19  16:34:53  root
 * Fix debug_print_skb.  Kludge outgoing snet to 0 when using startup
 * range.  Change debug to mask: 1 for verbose, 2 for higher level stuff
 * including packet printing, 4 for lower level (card i/o) stuff.
 *
 * Revision 0.11  1996/02/12  15:53:38  root
 * Added router sends (requires new aarp.c patch)
 *
 * Revision 0.10  1996/02/11  00:19:35  root
 * Change source LTALK_LOGGING debug switch to insmod ... debug=2.
 *
 * Revision 0.9  1996/02/10  23:59:35  root
 * Fixed those fixes for 1.2 -- DANGER!  The at.h that comes with netatalk
 * has a *different* definition of struct sockaddr_at than the Linux kernel
 * does.  This is an "insidious and invidious" bug...
 * (Actually the preceding comment is false -- it's the atalk.h in the
 * ancient atalk-0.06 that's the problem)
 *
 * Revision 0.8  1996/02/10 19:09:00  root
 * Merge 1.3 changes.  Tested OK under 1.3.60.
 *
 * Revision 0.7  1996/02/10 17:56:56  root
 * Added debug=1 parameter on insmod for debugging prints.  Tried
 * to fix timer unload on rmmod, but I don't think that's the problem.
 *
 * Revision 0.6  1995/12/31  19:01:09  root
 * Clean up rmmod, irq comments per feedback from Corin Anderson (Thanks Corey!)
 * Clean up initial probing -- sometimes the card wakes up latched in reset.
 *
 * Revision 0.5  1995/12/22  06:03:44  root
 * Added comments in front and cleaned up a bit.
 * This version sent out to people.
 *
 * Revision 0.4  1995/12/18  03:46:44  root
 * Return shortDDP to longDDP fake to 0/0.  Added command structs.
 *
 ***/

/* ltpc jumpers are:
*
*	Interrupts -- set at most one.  If none are set, the driver uses
*	polled mode.  Because the card was developed in the XT era, the
*	original documentation refers to IRQ2.  Since you'll be running
*	this on an AT (or later) class machine, that really means IRQ9.
*
*	SW1	IRQ 4
*	SW2	IRQ 3
*	SW3	IRQ 9 (2 in original card documentation only applies to XT)
*
*
*	DMA -- choose DMA 1 or 3, and set both corresponding switches.
*
*	SW4	DMA 3
*	SW5	DMA 1
*	SW6	DMA 3
*	SW7	DMA 1
*
*
*	I/O address -- choose one.  
*
*	SW8	220 / 240
*/

/*	To have some stuff logged, do 
*	insmod ltpc.o debug=1
*
*	For a whole bunch of stuff, use higher numbers.
*
*	The default is 0, i.e. no messages except for the probe results.
*/

/* insmod-tweakable variables */
static int debug=0;
#define DEBUG_VERBOSE 1
#define DEBUG_UPPER 2
#define DEBUG_LOWER 4

static int io=0;
static int irq=0;
static int dma=0;

#ifdef MODULE
#include <linux/module.h>
#include <linux/version.h>
#endif

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/dma.h>
#include <linux/errno.h>
#include <linux/init.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <linux/if_arp.h>
#include <linux/if_ltalk.h>

#include <linux/delay.h>
#include <linux/timer.h>

#include <linux/atalk.h>

/* our stuff */
#include "ltpc.h"

/* function prototypes */
static int do_read(struct net_device *dev, void *cbuf, int cbuflen,
	void *dbuf, int dbuflen);
static int sendup_buffer (struct net_device *dev);

/* Dma Memory related stuff, cribbed directly from 3c505.c */

/* Pure 2^n version of get_order */
static inline int __get_order(unsigned long size)
{
        int order;

        size = (size - 1) >> (PAGE_SHIFT - 1);
        order = -1;
        do {
                size >>= 1;
                order++;
        } while (size);
        return order;
}

static unsigned long dma_mem_alloc(int size)
{
        int order = __get_order(size);

        return __get_dma_pages(GFP_KERNEL, order);
}

/* DMA data buffer, DMA command buffer */
static unsigned char *ltdmabuf;
static unsigned char *ltdmacbuf;

/* private struct, holds our appletalk address */

struct ltpc_private
{
	struct net_device_stats stats;
	struct at_addr my_addr;
};

/* transmit queue element struct */

struct xmitQel {
	struct xmitQel *next;
	/* command buffer */
	unsigned char *cbuf;
	short cbuflen;
	/* data buffer */
	unsigned char *dbuf;
	short dbuflen;
	unsigned char QWrite;	/* read or write data */
	unsigned char mailbox;
};

/* the transmit queue itself */

static struct xmitQel *xmQhd=NULL,*xmQtl=NULL;

static void enQ(struct xmitQel *qel)
{
	unsigned long flags;
	qel->next = NULL;
	save_flags(flags);
	cli();
	if (xmQtl) {
		xmQtl->next = qel;
	} else {
		xmQhd = qel;
	}
	xmQtl = qel;
	restore_flags(flags);

	if (debug&DEBUG_LOWER)
		printk("enqueued a 0x%02x command\n",qel->cbuf[0]);
}

static struct xmitQel *deQ(void)
{
	unsigned long flags;
	int i;
	struct xmitQel *qel=NULL;
	save_flags(flags);
	cli();
	if (xmQhd) {
		qel = xmQhd;
		xmQhd = qel->next;
		if(!xmQhd) xmQtl = NULL;
	}
	restore_flags(flags);

	if ((debug&DEBUG_LOWER) && qel) {
		int n;
		printk("ltpc: dequeued command ");
		n = qel->cbuflen;
		if (n>100) n=100;
		for(i=0;i<n;i++) printk("%02x ",qel->cbuf[i]);
		printk("\n");
	}

	return qel;
}

/* and... the queue elements we'll be using */
static struct xmitQel qels[16];

/* and their corresponding mailboxes */
static unsigned char mailbox[16];
static unsigned char mboxinuse[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

static int wait_timeout(struct net_device *dev, int c)
{
	/* returns true if it stayed c */
	/* this uses base+6, but it's ok */
	int i;
	int timeout;

	/* twenty second or so total */

	for(i=0;i<20000;i++) {
		if ( c != inb_p(dev->base_addr+6) ) return 0;
		for(timeout=loops_per_sec/1000; timeout > 0; timeout--) ;
	}
	return 1; /* timed out */
}

/* get the first free mailbox */

static int getmbox(void)
{
	unsigned long flags;
	int i;

	save_flags(flags);
	cli();
	for(i=1;i<16;i++) if(!mboxinuse[i]) {
		mboxinuse[i]=1;
		restore_flags(flags);
		return i;
	}
	restore_flags(flags);
	return 0;
}

/* read a command from the card */
static void handlefc(struct net_device *dev)
{
	/* called *only* from idle, non-reentrant */
	int dma = dev->dma;
	int base = dev->base_addr;
	unsigned long flags;


	flags=claim_dma_lock();
	disable_dma(dma);
	clear_dma_ff(dma);
	set_dma_mode(dma,DMA_MODE_READ);
	set_dma_addr(dma,virt_to_bus(ltdmacbuf));
	set_dma_count(dma,50);
	enable_dma(dma);
	release_dma_lock(flags);

	inb_p(base+3);
	inb_p(base+2);

	if ( wait_timeout(dev,0xfc) ) printk("timed out in handlefc\n");
}

/* read data from the card */
static void handlefd(struct net_device *dev)
{
	int dma = dev->dma;
	int base = dev->base_addr;
	unsigned long flags;

	flags=claim_dma_lock();
	disable_dma(dma);
	clear_dma_ff(dma);
	set_dma_mode(dma,DMA_MODE_READ);
	set_dma_addr(dma,virt_to_bus(ltdmabuf));
	set_dma_count(dma,800);
	enable_dma(dma);
	release_dma_lock(flags);

	inb_p(base+3);
	inb_p(base+2);

	if ( wait_timeout(dev,0xfd) ) printk("timed out in handlefd\n");
	sendup_buffer(dev);
} 

static void handlewrite(struct net_device *dev)
{
	/* called *only* from idle, non-reentrant */
	/* on entry, 0xfb and ltdmabuf holds data */
	int dma = dev->dma;
	int base = dev->base_addr;
	unsigned long flags;
	
	flags=claim_dma_lock();
	disable_dma(dma);
	clear_dma_ff(dma);
	set_dma_mode(dma,DMA_MODE_WRITE);
	set_dma_addr(dma,virt_to_bus(ltdmabuf));
	set_dma_count(dma,800);
	enable_dma(dma);
	release_dma_lock(flags);
	
	inb_p(base+3);
	inb_p(base+2);

	if ( wait_timeout(dev,0xfb) ) {
		flags=claim_dma_lock();
		printk("timed out in handlewrite, dma res %d\n",
			get_dma_residue(dev->dma) );
		release_dma_lock(flags);
	}
}

static void handleread(struct net_device *dev)
{
	/* on entry, 0xfb */
	/* on exit, ltdmabuf holds data */
	int dma = dev->dma;
	int base = dev->base_addr;
	unsigned long flags;

	
	flags=claim_dma_lock();
	disable_dma(dma);
	clear_dma_ff(dma);
	set_dma_mode(dma,DMA_MODE_READ);
	set_dma_addr(dma,virt_to_bus(ltdmabuf));
	set_dma_count(dma,800);
	enable_dma(dma);
	release_dma_lock(flags);

	inb_p(base+3);
	inb_p(base+2);
	if ( wait_timeout(dev,0xfb) ) printk("timed out in handleread\n");
}

static void handlecommand(struct net_device *dev)
{
	/* on entry, 0xfa and ltdmacbuf holds command */
	int dma = dev->dma;
	int base = dev->base_addr;
	unsigned long flags;

	flags=claim_dma_lock();
	disable_dma(dma);
	clear_dma_ff(dma);
	set_dma_mode(dma,DMA_MODE_WRITE);
	set_dma_addr(dma,virt_to_bus(ltdmacbuf));
	set_dma_count(dma,50);
	enable_dma(dma);
	release_dma_lock(flags);
	inb_p(base+3);
	inb_p(base+2);
	if ( wait_timeout(dev,0xfa) ) printk("timed out in handlecommand\n");
} 

/* ready made command for getting the result from the card */
static unsigned char rescbuf[2] = {LT_GETRESULT,0};
static unsigned char resdbuf[2];

static int QInIdle=0;

/* idle expects to be called with the IRQ line high -- either because of
 * an interrupt, or because the line is tri-stated
 */

static void idle(struct net_device *dev)
{
	unsigned long flags;
	int state;
	/* FIXME This is initialized to shut the warning up, but I need to
	 * think this through again.
	 */
	struct xmitQel *q=0;
	int oops;
	int i;
	int base = dev->base_addr;

	save_flags(flags);
	cli();
	if(QInIdle) {
		restore_flags(flags);
		return;
	}
	QInIdle = 1;


	restore_flags(flags);

	/* this tri-states the IRQ line */
	(void) inb_p(base+6);

	oops = 100;

loop:
	if (0>oops--) { 
		printk("idle: looped too many times\n");
		goto done;
	}

	state = inb_p(base+6);
	if (state != inb_p(base+6)) goto loop;

	switch(state) {
		case 0xfc:
			/* incoming command */
			if (debug&DEBUG_LOWER) printk("idle: fc\n");
			handlefc(dev); 
			break;
		case 0xfd:
			/* incoming data */
			if(debug&DEBUG_LOWER) printk("idle: fd\n");
			handlefd(dev); 
			break;
		case 0xf9:
			/* result ready */
			if (debug&DEBUG_LOWER) printk("idle: f9\n");
			if(!mboxinuse[0]) {
				mboxinuse[0] = 1;
				qels[0].cbuf = rescbuf;
				qels[0].cbuflen = 2;
				qels[0].dbuf = resdbuf;
				qels[0].dbuflen = 2;
				qels[0].QWrite = 0;
				qels[0].mailbox = 0;
				enQ(&qels[0]);
			}
			inb_p(dev->base_addr+1);
			inb_p(dev->base_addr+0);
			if( wait_timeout(dev,0xf9) )
				printk("timed out idle f9\n");
			break;
		case 0xf8:
			/* ?? */
			if (xmQhd) {
				inb_p(dev->base_addr+1);
				inb_p(dev->base_addr+0);
				if(wait_timeout(dev,0xf8) )
					printk("timed out idle f8\n");
			} else {
				goto done;
			}
			break;
		case 0xfa:
			/* waiting for command */
			if(debug&DEBUG_LOWER) printk("idle: fa\n");
			if (xmQhd) {
				q=deQ();
				memcpy(ltdmacbuf,q->cbuf,q->cbuflen);
				ltdmacbuf[1] = q->mailbox;
				if (debug>1) { 
					int n;
					printk("ltpc: sent command     ");
					n = q->cbuflen;
					if (n>100) n=100;
					for(i=0;i<n;i++)
						printk("%02x ",ltdmacbuf[i]);
					printk("\n");
				}
				handlecommand(dev);
					if(0xfa==inb_p(base+6)) {
						/* we timed out, so return */
						goto done;
					} 
			} else {
				/* we don't seem to have a command */
				if (!mboxinuse[0]) {
					mboxinuse[0] = 1;
					qels[0].cbuf = rescbuf;
					qels[0].cbuflen = 2;
					qels[0].dbuf = resdbuf;
					qels[0].dbuflen = 2;
					qels[0].QWrite = 0;
					qels[0].mailbox = 0;
					enQ(&qels[0]);
				} else {
					printk("trouble: response command already queued\n");
					goto done;
				}
			} 
			break;
		case 0Xfb:
			/* data transfer ready */
			if(debug&DEBUG_LOWER) printk("idle: fb\n");
			if(q->QWrite) {
				memcpy(ltdmabuf,q->dbuf,q->dbuflen);
				handlewrite(dev);
			} else {
				handleread(dev);
				/* non-zero mailbox numbers are for
				   commmands, 0 is for GETRESULT
				   requests */
				if(q->mailbox) {
					memcpy(q->dbuf,ltdmabuf,q->dbuflen);
				} else { 
					/* this was a result */
					mailbox[ 0x0f & ltdmabuf[0] ] = ltdmabuf[1];
					mboxinuse[0]=0;
				}
			}
			break;
	}
	goto loop;

done:
	QInIdle=0;

	/* now set the interrupts back as appropriate */
	/* the first read takes it out of tri-state (but still high) */
	/* the second resets it */
	/* note that after this point, any read of base+6 will
	   trigger an interrupt */

	if (dev->irq) {
		inb_p(base+7);
		inb_p(base+7);
	}
	return;
}


static int do_write(struct net_device *dev, void *cbuf, int cbuflen,
	void *dbuf, int dbuflen)
{

	int i = getmbox();
	int ret;

	if(i) {
		qels[i].cbuf = (unsigned char *) cbuf;
		qels[i].cbuflen = cbuflen;
		qels[i].dbuf = (unsigned char *) dbuf;
		qels[i].dbuflen = dbuflen;
		qels[i].QWrite = 1;
		qels[i].mailbox = i;  /* this should be initted rather */
		enQ(&qels[i]);
		idle(dev);
		ret = mailbox[i];
		mboxinuse[i]=0;
		return ret;
	}
	printk("ltpc: could not allocate mbox\n");
	return -1;
}

static int do_read(struct net_device *dev, void *cbuf, int cbuflen,
	void *dbuf, int dbuflen)
{

	int i = getmbox();
	int ret;

	if(i) {
		qels[i].cbuf = (unsigned char *) cbuf;
		qels[i].cbuflen = cbuflen;
		qels[i].dbuf = (unsigned char *) dbuf;
		qels[i].dbuflen = dbuflen;
		qels[i].QWrite = 0;
		qels[i].mailbox = i;  /* this should be initted rather */
		enQ(&qels[i]);
		idle(dev);
		ret = mailbox[i];
		mboxinuse[i]=0;
		return ret;
	}
	printk("ltpc: could not allocate mbox\n");
	return -1;
}

/* end of idle handlers -- what should be seen is do_read, do_write */

static struct timer_list ltpc_timer;

static int ltpc_xmit(struct sk_buff *skb, struct net_device *dev);
static struct net_device_stats *ltpc_get_stats(struct net_device *dev);

static int ltpc_open(struct net_device *dev)
{
#ifdef MODULE
	MOD_INC_USE_COUNT;
#endif
	return 0;
}

static int ltpc_close(struct net_device *dev)
{
#ifdef MODULE
	MOD_DEC_USE_COUNT;
#endif
	return 0;
}

static int read_30 ( struct net_device *dev)
{
	lt_command c;
	c.getflags.command = LT_GETFLAGS;
	return do_read(dev, &c, sizeof(c.getflags),&c,0);
}

static int set_30 (struct net_device *dev,int x)
{
	lt_command c;
	c.setflags.command = LT_SETFLAGS;
	c.setflags.flags = x;
	return do_write(dev, &c, sizeof(c.setflags),&c,0);
}

/* LLAP to DDP translation */

static int sendup_buffer (struct net_device *dev)
{
	/* on entry, command is in ltdmacbuf, data in ltdmabuf */
	/* called from idle, non-reentrant */

	int dnode, snode, llaptype, len; 
	int sklen;
	struct sk_buff *skb;
	struct net_device_stats *stats = &((struct ltpc_private *)dev->priv)->stats;
	struct lt_rcvlap *ltc = (struct lt_rcvlap *) ltdmacbuf;

	if (ltc->command != LT_RCVLAP) {
		printk("unknown command 0x%02x from ltpc card\n",ltc->command);
		return(-1);
	}
	dnode = ltc->dnode;
	snode = ltc->snode;
	llaptype = ltc->laptype;
	len = ltc->length; 

	sklen = len;
	if (llaptype == 1) 
		sklen += 8;  /* correct for short ddp */
	if(sklen > 800) {
		printk(KERN_INFO "%s: nonsense length in ltpc command 0x14: 0x%08x\n",
			dev->name,sklen);
		return -1;
	}

	if ( (llaptype==0) || (llaptype>2) ) {
		printk(KERN_INFO "%s: unknown LLAP type: %d\n",dev->name,llaptype);
		return -1;
	}


	skb = dev_alloc_skb(3+sklen);
	if (skb == NULL) 
	{
		printk("%s: dropping packet due to memory squeeze.\n",
			dev->name);
		return -1;
	}
	skb->dev = dev;

	if (sklen > len)
		skb_reserve(skb,8);
	skb_put(skb,len+3);
	skb->protocol = htons(ETH_P_LOCALTALK);
	/* add LLAP header */
	skb->data[0] = dnode;
	skb->data[1] = snode;
	skb->data[2] = llaptype;
	skb->mac.raw = skb->data;	/* save pointer to llap header */
	skb_pull(skb,3);

	/* copy ddp(s,e)hdr + contents */
	memcpy(skb->data,(void*)ltdmabuf,len);

	skb->h.raw = skb->data;

	stats->rx_packets++;
	stats->rx_bytes+=skb->len;

	/* toss it onwards */
	netif_rx(skb);
	return 0;
}

/* the handler for the board interrupt */
 
static void ltpc_interrupt(int irq, void *dev_id, struct pt_regs *reg_ptr)
{
	struct net_device *dev = dev_id;

	if (dev==NULL) {
		printk("ltpc_interrupt: unknown device.\n");
		return;
	}

	inb_p(dev->base_addr+6);  /* disable further interrupts from board */

	idle(dev); /* handle whatever is coming in */
 
	/* idle re-enables interrupts from board */ 

	return;
}

/***
 *
 *    The ioctls that the driver responds to are:
 *
 *    SIOCSIFADDR -- do probe using the passed node hint.
 *    SIOCGIFADDR -- return net, node.
 *
 *    some of this stuff should be done elsewhere.
 *
 ***/

static int ltpc_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct sockaddr_at *sa = (struct sockaddr_at *) &ifr->ifr_addr;
	/* we'll keep the localtalk node address in dev->pa_addr */
	struct at_addr *aa = &((struct ltpc_private *)dev->priv)->my_addr;
	struct lt_init c;
	int ltflags;

	if(debug&DEBUG_VERBOSE) printk("ltpc_ioctl called\n");

	switch(cmd) {
		case SIOCSIFADDR:

			aa->s_net  = sa->sat_addr.s_net;
      
			/* this does the probe and returns the node addr */
			c.command = LT_INIT;
			c.hint = sa->sat_addr.s_node;

			aa->s_node = do_read(dev,&c,sizeof(c),&c,0);

			/* get all llap frames raw */
			ltflags = read_30(dev);
			ltflags |= LT_FLAG_ALLLAP;
			set_30 (dev,ltflags);  

			dev->broadcast[0] = 0xFF;
			dev->dev_addr[0] = aa->s_node;

			dev->addr_len=1;
   
			return 0;

		case SIOCGIFADDR:

			sa->sat_addr.s_net = aa->s_net;
			sa->sat_addr.s_node = aa->s_node;

			return 0;

		default: 
			return -EINVAL;
	}
}

static void set_multicast_list(struct net_device *dev)
{
	/* This needs to be present to keep netatalk happy. */
	/* Actually netatalk needs fixing! */
}

static int ltpc_hard_header (struct sk_buff *skb, struct net_device *dev, 
	unsigned short type, void *daddr, void *saddr, unsigned len)
{
	if(debug&DEBUG_VERBOSE)
		printk("ltpc_hard_header called for device %s\n",
			dev->name);
	return 0;
}

static int ltpc_init(struct net_device *dev)
{
	/* Initialize the device structure. */
  
	/* Fill in the fields of the device structure with ethernet-generic values. */
	ltalk_setup(dev);
	dev->hard_start_xmit = ltpc_xmit;
	dev->hard_header = ltpc_hard_header;

	dev->priv = kmalloc(sizeof(struct ltpc_private), GFP_KERNEL);
	if(!dev->priv)
	{
		printk(KERN_INFO "%s: could not allocate statistics buffer\n", dev->name);
		return -ENOMEM;
	}

	memset(dev->priv, 0, sizeof(struct ltpc_private));
	dev->get_stats = ltpc_get_stats;

	dev->open = ltpc_open;
	dev->stop = ltpc_close;

	/* add the ltpc-specific things */
	dev->do_ioctl = &ltpc_ioctl;

	dev->set_multicast_list = &set_multicast_list;
	dev->mc_list = NULL;

	return 0;
}

static int ltpc_poll_counter = 0;

static void ltpc_poll(unsigned long l)
{
	struct net_device *dev = (struct net_device *) l;

	del_timer(&ltpc_timer);

	if(debug&DEBUG_VERBOSE) {
		if (!ltpc_poll_counter) {
			ltpc_poll_counter = 50;
			printk("ltpc poll is alive\n");
		}
		ltpc_poll_counter--;
	}
  
	if (!dev)
		return;  /* we've been downed */

	idle(dev);
	ltpc_timer.expires = jiffies+5;
	
	add_timer(&ltpc_timer);
}

/* DDP to LLAP translation */

static int ltpc_xmit(struct sk_buff *skb, struct net_device *dev)
{
	/* in kernel 1.3.xx, on entry skb->data points to ddp header,
	 * and skb->len is the length of the ddp data + ddp header
	 */

	struct net_device_stats *stats = &((struct ltpc_private *)dev->priv)->stats;

	int i;
	struct lt_sendlap cbuf;

	cbuf.command = LT_SENDLAP;
	cbuf.dnode = skb->data[0];
	cbuf.laptype = skb->data[2];
	skb_pull(skb,3);	/* skip past LLAP header */
	cbuf.length = skb->len;	/* this is host order */
	skb->h.raw=skb->data;

	if(debug&DEBUG_UPPER) {
		printk("command ");
		for(i=0;i<6;i++)
			printk("%02x ",((unsigned char *)&cbuf)[i]);
		printk("\n");
	}

	do_write(dev,&cbuf,sizeof(cbuf),skb->h.raw,skb->len);

	if(debug&DEBUG_UPPER) {
		printk("sent %d ddp bytes\n",skb->len);
		for(i=0;i<skb->len;i++) printk("%02x ",skb->h.raw[i]);
		printk("\n");
	}

	stats->tx_packets++;
	stats->tx_bytes+=skb->len;

	dev_kfree_skb(skb);
	return 0;
}

static struct net_device_stats *ltpc_get_stats(struct net_device *dev)
{
	struct net_device_stats *stats = &((struct ltpc_private *) dev->priv)->stats;
	return stats;
}

/* initialization stuff */
  
int __init ltpc_probe_dma(int base)
{
	int dma = 0;
  	int timeout;
  	unsigned long f;
  
  	if (!request_dma(1,"ltpc")) {
  		f=claim_dma_lock();
  		disable_dma(1);
  		clear_dma_ff(1);
		set_dma_mode(1,DMA_MODE_WRITE);
		set_dma_addr(1,virt_to_bus(ltdmabuf));
		set_dma_count(1,sizeof(struct lt_mem));
		enable_dma(1);
		release_dma_lock(f);
		dma|=1;
	}
	if (!request_dma(3,"ltpc")) {
		f=claim_dma_lock();
		disable_dma(3);
		clear_dma_ff(3);
		set_dma_mode(3,DMA_MODE_WRITE);
		set_dma_addr(3,virt_to_bus(ltdmabuf));
		set_dma_count(3,sizeof(struct lt_mem));
		enable_dma(3);
		release_dma_lock(f);
		dma|=2;
	}

	/* set up request */

	/* FIXME -- do timings better! */

	ltdmabuf[0] = LT_READMEM;
	ltdmabuf[1] = 1;  /* mailbox */
	ltdmabuf[2] = 0; ltdmabuf[3] = 0;  /* address */
	ltdmabuf[4] = 0; ltdmabuf[5] = 1;  /* read 0x0100 bytes */
	ltdmabuf[6] = 0; /* dunno if this is necessary */

	inb_p(io+1);
	inb_p(io+0);
	timeout = jiffies+100*HZ/100;
	while(time_before(jiffies, timeout)) {
		if ( 0xfa == inb_p(io+6) ) break;
	}

	inb_p(io+3);
	inb_p(io+2);
	while(time_before(jiffies, timeout)) {
		if ( 0xfb == inb_p(io+6) ) break;
	}

	/* release the other dma channel (if we opened both of them) */

	if ( (dma&0x2) && (get_dma_residue(3)==sizeof(struct lt_mem)) ){
		dma&=1;
		free_dma(3);
	}
  
	if ( (dma&0x1) && (get_dma_residue(1)==sizeof(struct lt_mem)) ){
		dma&=0x2;
		free_dma(1);
	}

	/* fix up dma number */
	dma|=1;

	return dma;
}

int __init ltpc_probe(struct net_device *dev)
{
	int err;
	int x=0,y=0;
	int timeout;
	int autoirq;
	unsigned long flags;
	unsigned long f;

	save_flags(flags);

	/* probe for the I/O port address */
	if (io != 0x240 && !check_region(0x220,8)) {
		x = inb_p(0x220+6);
		if ( (x!=0xff) && (x>=0xf0) ) io = 0x220;
	}
	
	if (io != 0x220 && !check_region(0x240,8)) {
		y = inb_p(0x240+6);
		if ( (y!=0xff) && (y>=0xf0) ) io = 0x240;
	} 

	if(io) {
		/* found it, now grab it */
		request_region(io,8,"ltpc");
	} else {
		/* give up in despair */
		printk ("LocalTalk card not found; 220 = %02x, 240 = %02x.\n",
			x,y);
		restore_flags(flags);
		return -1;
	}

	/* probe for the IRQ line */
	if (irq < 2) {
		autoirq_setup(2);

		/* reset the interrupt line */
		inb_p(io+7);
		inb_p(io+7);
		/* trigger an interrupt (I hope) */
		inb_p(io+6);

		autoirq = autoirq_report(1);

		if (autoirq == 0) {
			printk("ltpc: probe at %#x failed to detect IRQ line.\n",
				io);
		}
		else {
			irq = autoirq;
		}
	}

	/* allocate a DMA buffer */
	ltdmabuf = (unsigned char *) dma_mem_alloc(1000);

	if (ltdmabuf) ltdmacbuf = &ltdmabuf[800];

	if (!ltdmabuf) {
		printk("ltpc: mem alloc failed\n");
		restore_flags(flags);
		return(-1);
	}

	if(debug&DEBUG_VERBOSE) {
		printk("ltdmabuf pointer %08lx\n",(unsigned long) ltdmabuf);
	}

	/* reset the card */

	inb_p(io+1);
	inb_p(io+3);
	timeout = jiffies+2*HZ/100;
	while(time_before(jiffies, timeout)) ; /* hold it in reset for a coupla jiffies */
	inb_p(io+0);
	inb_p(io+2);
	inb_p(io+7); /* clear reset */
	inb_p(io+4); 
	inb_p(io+5);
	inb_p(io+5); /* enable dma */
	inb_p(io+6); /* tri-state interrupt line */

	timeout = jiffies+100*HZ/100;
	
	while(time_before(jiffies, timeout)) {
		/* wait for the card to complete initialization */
	}
 
	/* now, figure out which dma channel we're using, unless it's
	   already been specified */
	/* well, 0 is a legal DMA channel, but the LTPC card doesn't
	   use it... */
	if (dma == 0) {
		dma = ltpc_probe_dma(io);
		if (!dma) {  /* no dma channel */
			printk("No DMA channel found on ltpc card.\n");
			restore_flags(flags);
			return -1;
		}
	}

	/* print out friendly message */

	if(irq)
		printk("Apple/Farallon LocalTalk-PC card at %03x, IR%d, DMA%d.\n",io,irq,dma);
	else
		printk("Apple/Farallon LocalTalk-PC card at %03x, DMA%d.  Using polled mode.\n",io,dma);

	/* seems more logical to do this *after* probing the card... */
	err = ltpc_init(dev);
	if (err) return err;

	dev->base_addr = io;
	dev->irq = irq;
	dev->dma = dma;

	/* the card will want to send a result at this point */
	/* (I think... leaving out this part makes the kernel crash,
           so I put it back in...) */

	f=claim_dma_lock();
	disable_dma(dma);
	clear_dma_ff(dma);
	set_dma_mode(dma,DMA_MODE_READ);
	set_dma_addr(dma,virt_to_bus(ltdmabuf));
	set_dma_count(dma,0x100);
	enable_dma(dma);
	release_dma_lock(f);

	(void) inb_p(io+3);
	(void) inb_p(io+2);
	timeout = jiffies+100*HZ/100;
	while(time_before(jiffies, timeout)) {
		if( 0xf9 == inb_p(io+6)) break;
	}

	if(debug&DEBUG_VERBOSE) {
		printk("setting up timer and irq\n");
	}

	if (irq) {
		/* grab it and don't let go :-) */
		(void) request_irq( irq, &ltpc_interrupt, 0, "ltpc", dev);
		(void) inb_p(io+7);  /* enable interrupts from board */
		(void) inb_p(io+7);  /* and reset irq line */
	} else {
		/* polled mode -- 20 times per second */
		/* this is really, really slow... should it poll more often? */
		init_timer(&ltpc_timer);
		ltpc_timer.function=ltpc_poll;
		ltpc_timer.data = (unsigned long) dev;

		ltpc_timer.expires = jiffies + 5;
		add_timer(&ltpc_timer);
		restore_flags(flags); 
	}

	return 0;
}

/* handles "ltpc=io,irq,dma" kernel command lines */
void __init ltpc_setup(char *str, int *ints)
{
	if (ints[0] == 0) {
		if (str && !strncmp(str, "auto", 4)) {
			/* do nothing :-) */
		}
		else {
			/* usage message */
			printk (KERN_ERR
				"ltpc: usage: ltpc=auto|iobase[,irq[,dma]]\n");
		}
		return;
	} else {
		io = ints[1];
		if (ints[0] > 1) {
			irq = ints[2];
			return;
		}
		if (ints[0] > 2) {
			dma = ints[3];
			return;
		}
		/* ignore any other paramters */
	}
	return;
}

#ifdef MODULE

static char dev_name[8];

static struct net_device dev_ltpc = {
		dev_name, 
		0, 0, 0, 0,
	 	0x0, 0,
	 	0, 0, 0, NULL, ltpc_probe };

MODULE_PARM(debug, "i");
MODULE_PARM(io, "i");
MODULE_PARM(irq, "i");
MODULE_PARM(dma, "i");

int init_module(void)
{
	int err, result;
	
        if(io == 0)
		printk(KERN_NOTICE
		       "ltpc: Autoprobing is not recommended for modules\n");

	/* Find a name for this unit */
	err=dev_alloc_name(&dev_ltpc,"lt%d");
	
	if(err<0)
		return err;

	if ((result = register_netdev(&dev_ltpc)) != 0) {
		printk(KERN_DEBUG "could not register Localtalk-PC device\n");
		return result;
	} else {
		if(debug&DEBUG_VERBOSE) printk("0 from register_netdev\n");
		return 0;
	}
}

void cleanup_module(void)
{
	long timeout;

	ltpc_timer.data = 0;  /* signal the poll routine that we're done */

	if(debug&DEBUG_VERBOSE) printk("freeing irq\n");

	if(dev_ltpc.irq) {
		free_irq(dev_ltpc.irq,&dev_ltpc);
		dev_ltpc.irq = 0;
	}

	if(del_timer(&ltpc_timer)) 
	{
		/* either the poll was never started, or a poll is in process */
		if(debug&DEBUG_VERBOSE) printk("waiting\n");
		/* if it's in process, wait a bit for it to finish */
		timeout = jiffies+HZ; 
		add_timer(&ltpc_timer);
		while(del_timer(&ltpc_timer) && time_after(timeout, jiffies))
		{
			add_timer(&ltpc_timer);
			schedule();
		}
	}

	if(debug&DEBUG_VERBOSE) printk("freeing dma\n");

	if(dev_ltpc.dma) {
		free_dma(dev_ltpc.dma);
		dev_ltpc.dma = 0;
	}

	if(debug&DEBUG_VERBOSE) printk("freeing ioaddr\n");

	if(dev_ltpc.base_addr) {
		release_region(dev_ltpc.base_addr,8);
		dev_ltpc.base_addr = 0;
	}

	if(debug&DEBUG_VERBOSE) printk("free_pages\n");

	free_pages( (unsigned long) ltdmabuf, __get_order(1000));
	ltdmabuf=NULL;
	ltdmacbuf=NULL;

	if(debug&DEBUG_VERBOSE) printk("unregister_netdev\n");

	unregister_netdev(&dev_ltpc);

	if(debug&DEBUG_VERBOSE) printk("returning from cleanup_module\n");
}
#endif /* MODULE */

