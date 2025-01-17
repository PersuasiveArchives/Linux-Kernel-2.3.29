/* 	smc-ultra32.c: An SMC Ultra32 EISA ethernet driver for linux.

Sources:

	This driver is based on (cloned from) the ISA SMC Ultra driver
	written by Donald Becker. Modifications to support the EISA
	version of the card by Paul Gortmaker and Leonard N. Zubkoff.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.

Theory of Operation:

	The SMC Ultra32C card uses the SMC 83c790 chip which is also
	found on the ISA SMC Ultra cards. It has a shared memory mode of
	operation that makes it similar to the ISA version of the card.
	The main difference is that the EISA card has 32KB of RAM, but
	only an 8KB window into that memory. The EISA card also can be
	set for a bus-mastering mode of operation via the ECU, but that
	is not (and probably will never be) supported by this driver.
	The ECU should be run to enable shared memory and to disable the
	bus-mastering feature for use with linux.

	By programming the 8390 to use only 8KB RAM, the modifications
	to the ISA driver can be limited to the probe and initialization
	code. This allows easy integration of EISA support into the ISA
	driver. However, the driver development kit from SMC provided the
	register information for sliding the 8KB window, and hence the 8390
	is programmed to use the full 32KB RAM.

	Unfortunately this required code changes outside the probe/init
	routines, and thus we decided to separate the EISA driver from
	the ISA one. In this way, ISA users don't end up with a larger
	driver due to the EISA code, and EISA users don't end up with a
	larger driver due to the ISA EtherEZ PIO code. The driver is
	similar to the 3c503/16 driver, in that the window must be set
	back to the 1st 8KB of space for access to the two 8390 Tx slots.

	In testing, using only 8KB RAM (3 Tx / 5 Rx) didn't appear to
	be a limiting factor, since the EISA bus could get packets off
	the card fast enough, but having the use of lots of RAM as Rx
	space is extra insurance if interrupt latencies become excessive.

*/

static const char *version = "smc-ultra32.c: 06/97 v1.00\n";


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/system.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include "8390.h"

int ultra32_probe(struct net_device *dev);
int ultra32_probe1(struct net_device *dev, int ioaddr);
static int ultra32_open(struct net_device *dev);
static void ultra32_reset_8390(struct net_device *dev);
static void ultra32_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr,
				 int ring_page);
static void ultra32_block_input(struct net_device *dev, int count,
				struct sk_buff *skb, int ring_offset);
static void ultra32_block_output(struct net_device *dev, int count,
				 const unsigned char *buf,
				 const int start_page);
static int ultra32_close(struct net_device *dev);

#define ULTRA32_CMDREG	0	/* Offset to ASIC command register. */
#define	 ULTRA32_RESET	0x80	/* Board reset, in ULTRA32_CMDREG. */
#define	 ULTRA32_MEMENB	0x40	/* Enable the shared memory. */
#define ULTRA32_NIC_OFFSET 16	/* NIC register offset from the base_addr. */
#define ULTRA32_IO_EXTENT 32
#define EN0_ERWCNT		0x08	/* Early receive warning count. */

/*
 * Defines that apply only to the Ultra32 EISA card. Note that
 * "smc" = 10011 01101 00011 = 0x4da3, and hence !smc8010.cfg translates
 * into an EISA ID of 0x1080A34D
 */
#define ULTRA32_BASE	0xca0
#define ULTRA32_ID	0x1080a34d
#define ULTRA32_IDPORT	(-0x20)	/* 0xc80 */
/* Config regs 1->7 from the EISA !SMC8010.CFG file. */
#define ULTRA32_CFG1	0x04	/* 0xca4 */
#define ULTRA32_CFG2	0x05	/* 0xca5 */
#define ULTRA32_CFG3	(-0x18)	/* 0xc88 */
#define ULTRA32_CFG4	(-0x17)	/* 0xc89 */
#define ULTRA32_CFG5	(-0x16)	/* 0xc8a */
#define ULTRA32_CFG6	(-0x15)	/* 0xc8b */
#define ULTRA32_CFG7	0x0d	/* 0xcad */


/*	Probe for the Ultra32.  This looks like a 8013 with the station
	address PROM at I/O ports <base>+8 to <base>+13, with a checksum
	following.
*/

int __init ultra32_probe(struct net_device *dev)
{
	const char *ifmap[] = {"UTP No Link", "", "UTP/AUI", "UTP/BNC"};
	int ioaddr, edge, media;

	if (!EISA_bus) return ENODEV;

	/* EISA spec allows for up to 16 slots, but 8 is typical. */
	for (ioaddr = 0x1000 + ULTRA32_BASE; ioaddr < 0x9000; ioaddr += 0x1000)
	if (check_region(ioaddr, ULTRA32_IO_EXTENT) == 0 &&
	    inb(ioaddr + ULTRA32_IDPORT) != 0xff &&
	    inl(ioaddr + ULTRA32_IDPORT) == ULTRA32_ID) {
		media = inb(ioaddr + ULTRA32_CFG7) & 0x03;
		edge = inb(ioaddr + ULTRA32_CFG5) & 0x08;
		printk("SMC Ultra32 in EISA Slot %d, Media: %s, %s IRQs.\n",
		       ioaddr >> 12, ifmap[media],
		       (edge ? "Edge Triggered" : "Level Sensitive"));
		if (ultra32_probe1(dev, ioaddr) == 0)
		  return 0;
	}
	return ENODEV;
}

int __init ultra32_probe1(struct net_device *dev, int ioaddr)
{
	int i;
	int checksum = 0;
	const char *model_name;
	static unsigned version_printed = 0;
	/* Values from various config regs. */
	unsigned char idreg = inb(ioaddr + 7);
	unsigned char reg4 = inb(ioaddr + 4) & 0x7f;

	/* Check the ID nibble. */
	if ((idreg & 0xf0) != 0x20) 			/* SMC Ultra */
		return ENODEV;

	/* Select the station address register set. */
	outb(reg4, ioaddr + 4);

	for (i = 0; i < 8; i++)
		checksum += inb(ioaddr + 8 + i);
	if ((checksum & 0xff) != 0xff)
		return ENODEV;

	if (load_8390_module("smc-ultra32.c"))
		return -ENOSYS;

	/* We should have a "dev" from Space.c or the static module table. */
	if (dev == NULL) {
		printk("smc-ultra32.c: Passed a NULL device.\n");
		dev = init_etherdev(0, 0);
	}

	if (ei_debug  &&  version_printed++ == 0)
		printk(version);

	model_name = "SMC Ultra32";

	printk("%s: %s at 0x%X,", dev->name, model_name, ioaddr);

	for (i = 0; i < 6; i++)
		printk(" %2.2X", dev->dev_addr[i] = inb(ioaddr + 8 + i));

	/* Switch from the station address to the alternate register set and
	   read the useful registers there. */
	outb(0x80 | reg4, ioaddr + 4);

	/* Enable FINE16 mode to avoid BIOS ROM width mismatches @ reboot. */
	outb(0x80 | inb(ioaddr + 0x0c), ioaddr + 0x0c);

	/* Reset RAM addr. */
	outb(0x00, ioaddr + 0x0b);

	/* Switch back to the station address register set so that the
	   MS-DOS driver can find the card after a warm boot. */
	outb(reg4, ioaddr + 4);

	if ((inb(ioaddr + ULTRA32_CFG5) & 0x40) == 0) {
		printk("\nsmc-ultra32: Card RAM is disabled!  "
		       "Run EISA config utility.\n");
		return ENODEV;
	}
	if ((inb(ioaddr + ULTRA32_CFG2) & 0x04) == 0)
		printk("\nsmc-ultra32: Ignoring Bus-Master enable bit.  "
		       "Run EISA config utility.\n");

	if (dev->irq < 2) {
		unsigned char irqmap[] = {0, 9, 3, 5, 7, 10, 11, 15};
		int irq = irqmap[inb(ioaddr + ULTRA32_CFG5) & 0x07];
		if (irq == 0) {
			printk(", failed to detect IRQ line.\n");
			return -EAGAIN;
		}
		dev->irq = irq;
	}

	/* Allocate dev->priv and fill in 8390 specific dev fields. */
	if (ethdev_init(dev)) {
		printk (", no memory for dev->priv.\n");
                return -ENOMEM;
        }

	/* OK, we are certain this is going to work.  Setup the device. */
	request_region(ioaddr, ULTRA32_IO_EXTENT, model_name);

	/* The 8390 isn't at the base address, so fake the offset */
	dev->base_addr = ioaddr + ULTRA32_NIC_OFFSET;

	/* Save RAM address in the unused reg0 to avoid excess inb's. */
	ei_status.reg0 = inb(ioaddr + ULTRA32_CFG3) & 0xfc;

	dev->mem_start =  0xc0000 + ((ei_status.reg0 & 0x7c) << 11);

	ei_status.name = model_name;
	ei_status.word16 = 1;
	ei_status.tx_start_page = 0;
	ei_status.rx_start_page = TX_PAGES;
	/* All Ultra32 cards have 32KB memory with an 8KB window. */
	ei_status.stop_page = 128;

	dev->rmem_start = dev->mem_start + TX_PAGES*256;
	dev->mem_end = dev->rmem_end = dev->mem_start + 0x1fff;

	printk(", IRQ %d, 32KB memory, 8KB window at 0x%lx-0x%lx.\n",
	       dev->irq, dev->mem_start, dev->mem_end);
	ei_status.block_input = &ultra32_block_input;
	ei_status.block_output = &ultra32_block_output;
	ei_status.get_8390_hdr = &ultra32_get_8390_hdr;
	ei_status.reset_8390 = &ultra32_reset_8390;
	dev->open = &ultra32_open;
	dev->stop = &ultra32_close;
	NS8390_init(dev, 0);

	return 0;
}

static int ultra32_open(struct net_device *dev)
{
	int ioaddr = dev->base_addr - ULTRA32_NIC_OFFSET; /* ASIC addr */
	int irq_flags = (inb(ioaddr + ULTRA32_CFG5) & 0x08) ? 0 : SA_SHIRQ;

	if (request_irq(dev->irq, ei_interrupt, irq_flags, ei_status.name, dev))
		return -EAGAIN;

	outb(ULTRA32_MEMENB, ioaddr); /* Enable Shared Memory. */
	outb(0x80, ioaddr + ULTRA32_CFG6); /* Enable Interrupts. */
	outb(0x84, ioaddr + 5);	/* Enable MEM16 & Disable Bus Master. */
	outb(0x01, ioaddr + 6);	/* Enable Interrupts. */
	/* Set the early receive warning level in window 0 high enough not
	   to receive ERW interrupts. */
	outb_p(E8390_NODMA+E8390_PAGE0, dev->base_addr);
	outb(0xff, dev->base_addr + EN0_ERWCNT);
	ei_open(dev);
	MOD_INC_USE_COUNT;
	return 0;
}

static int ultra32_close(struct net_device *dev)
{
	int ioaddr = dev->base_addr - ULTRA32_NIC_OFFSET; /* CMDREG */

	dev->start = 0;
	dev->tbusy = 1;

	if (ei_debug > 1)
		printk("%s: Shutting down ethercard.\n", dev->name);

	outb(0x00, ioaddr + ULTRA32_CFG6); /* Disable Interrupts. */
	outb(0x00, ioaddr + 6);		/* Disable interrupts. */
	free_irq(dev->irq, dev);

	NS8390_init(dev, 0);

	MOD_DEC_USE_COUNT;

	return 0;
}

static void ultra32_reset_8390(struct net_device *dev)
{
	int ioaddr = dev->base_addr - ULTRA32_NIC_OFFSET; /* ASIC base addr */

	outb(ULTRA32_RESET, ioaddr);
	if (ei_debug > 1) printk("resetting Ultra32, t=%ld...", jiffies);
	ei_status.txing = 0;

	outb(ULTRA32_MEMENB, ioaddr); /* Enable Shared Memory. */
	outb(0x80, ioaddr + ULTRA32_CFG6); /* Enable Interrupts. */
	outb(0x84, ioaddr + 5);	/* Enable MEM16 & Disable Bus Master. */
	outb(0x01, ioaddr + 6);	/* Enable Interrupts. */
	if (ei_debug > 1) printk("reset done\n");
	return;
}

/* Grab the 8390 specific header. Similar to the block_input routine, but
   we don't need to be concerned with ring wrap as the header will be at
   the start of a page, so we optimize accordingly. */

static void ultra32_get_8390_hdr(struct net_device *dev,
				 struct e8390_pkt_hdr *hdr,
				 int ring_page)
{
	unsigned long hdr_start = dev->mem_start + ((ring_page & 0x1f) << 8);
	unsigned int RamReg = dev->base_addr - ULTRA32_NIC_OFFSET + ULTRA32_CFG3;

	/* Select correct 8KB Window. */
	outb(ei_status.reg0 | ((ring_page & 0x60) >> 5), RamReg);

#ifdef notdef
	/* Officially this is what we are doing, but the readl() is faster */
	isa_memcpy_fromio(hdr, hdr_start, sizeof(struct e8390_pkt_hdr));
#else
	((unsigned int*)hdr)[0] = isa_readl(hdr_start);
#endif
}

/* Block input and output are easy on shared memory ethercards, the only
   complication is when the ring buffer wraps, or in this case, when a
   packet spans an 8KB boundary. Note that the current 8KB segment is
   already set by the get_8390_hdr routine. */

static void ultra32_block_input(struct net_device *dev,
				int count,
				struct sk_buff *skb,
				int ring_offset)
{
	unsigned long xfer_start = dev->mem_start + (ring_offset & 0x1fff);
	unsigned int RamReg = dev->base_addr - ULTRA32_NIC_OFFSET + ULTRA32_CFG3;

	if ((ring_offset & ~0x1fff) != ((ring_offset + count - 1) & ~0x1fff)) {
		int semi_count = 8192 - (ring_offset & 0x1FFF);
		isa_memcpy_fromio(skb->data, xfer_start, semi_count);
		count -= semi_count;
		if (ring_offset < 96*256) {
			/* Select next 8KB Window. */
			ring_offset += semi_count;
			outb(ei_status.reg0 | ((ring_offset & 0x6000) >> 13), RamReg);
			isa_memcpy_fromio(skb->data + semi_count, dev->mem_start, count);
		} else {
			/* Select first 8KB Window. */
			outb(ei_status.reg0, RamReg);
			isa_memcpy_fromio(skb->data + semi_count, dev->rmem_start, count);
		}
	} else {
		/* Packet is in one chunk -- we can copy + cksum. */
		isa_eth_io_copy_and_sum(skb, xfer_start, count, 0);
	}
}

static void ultra32_block_output(struct net_device *dev,
				 int count,
				 const unsigned char *buf,
				 int start_page)
{
	unsigned long xfer_start = dev->mem_start + (start_page<<8);
	unsigned int RamReg = dev->base_addr - ULTRA32_NIC_OFFSET + ULTRA32_CFG3;

	/* Select first 8KB Window. */
	outb(ei_status.reg0, RamReg);

	isa_memcpy_toio(xfer_start, buf, count);
}

#ifdef MODULE
#define MAX_ULTRA32_CARDS   4	/* Max number of Ultra cards per module */
#define NAMELEN		    8	/* # of chars for storing dev->name */
static char namelist[NAMELEN * MAX_ULTRA32_CARDS] = { 0, };
static struct net_device dev_ultra[MAX_ULTRA32_CARDS] = {
	{
		NULL,		/* assign a chunk of namelist[] below */
		0, 0, 0, 0,
		0, 0,
		0, 0, 0, NULL, NULL
	},
};

int init_module(void)
{
	int this_dev, found = 0;

	for (this_dev = 0; this_dev < MAX_ULTRA32_CARDS; this_dev++) {
		struct net_device *dev = &dev_ultra[this_dev];
		dev->name = namelist+(NAMELEN*this_dev);
		dev->init = ultra32_probe;
		if (register_netdev(dev) != 0) {
			if (found > 0) { /* Got at least one. */
				lock_8390_module();
				return 0;
			}
			printk(KERN_WARNING "smc-ultra32.c: No SMC Ultra32 found.\n");
			return -ENXIO;
		}
		found++;
	}
	lock_8390_module();
	return 0;
}

void cleanup_module(void)
{
	int this_dev;

	for (this_dev = 0; this_dev < MAX_ULTRA32_CARDS; this_dev++) {
		struct net_device *dev = &dev_ultra[this_dev];
		if (dev->priv != NULL) {
			int ioaddr = dev->base_addr - ULTRA32_NIC_OFFSET;
			void *priv = dev->priv;
			/* NB: ultra32_close_card() does free_irq */
			release_region(ioaddr, ULTRA32_IO_EXTENT);
			unregister_netdev(dev);
			kfree(priv);
		}
	}
	unlock_8390_module();
}
#endif /* MODULE */
