/* hp-plus.c: A HP PCLAN/plus ethernet driver for linux. */
/*
	Written 1994 by Donald Becker.

	This driver is for the Hewlett Packard PC LAN (27***) plus ethercards.
	These cards are sold under several model numbers, usually 2724*.

	This software may be used and distributed according to the terms
	of the GNU Public License, incorporated herein by reference.

	The author may be reached as becker@CESDIS.gsfc.nasa.gov, or C/O

	Center of Excellence in Space Data and Information Sciences
		Code 930.5, Goddard Space Flight Center, Greenbelt MD 20771

	As is often the case, a great deal of credit is owed to Russ Nelson.
	The Crynwr packet driver was my primary source of HP-specific
	programming information.
*/

static const char *version =
"hp-plus.c:v1.10 9/24/94 Donald Becker (becker@cesdis.gsfc.nasa.gov)\n";

#include <linux/module.h>

#include <linux/string.h>		/* Important -- this inlines word moves. */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/delay.h>

#include <asm/system.h>
#include <asm/io.h>


#include "8390.h"

/* A zero-terminated list of I/O addresses to be probed. */
static unsigned int hpplus_portlist[] __initdata =
{0x200, 0x240, 0x280, 0x2C0, 0x300, 0x320, 0x340, 0};

/*
   The HP EtherTwist chip implementation is a fairly routine DP8390
   implementation.  It allows both shared memory and programmed-I/O buffer
   access, using a custom interface for both.  The programmed-I/O mode is
   entirely implemented in the HP EtherTwist chip, bypassing the problem
   ridden built-in 8390 facilities used on NE2000 designs.  The shared
   memory mode is likewise special, with an offset register used to make
   packets appear at the shared memory base.  Both modes use a base and bounds
   page register to hide the Rx ring buffer wrap -- a packet that spans the
   end of physical buffer memory appears continuous to the driver. (c.f. the
   3c503 and Cabletron E2100)

   A special note: the internal buffer of the board is only 8 bits wide.
   This lays several nasty traps for the unaware:
   - the 8390 must be programmed for byte-wide operations
   - all I/O and memory operations must work on whole words (the access
     latches are serially preloaded and have no byte-swapping ability).

   This board is laid out in I/O space much like the earlier HP boards:
   the first 16 locations are for the board registers, and the second 16 are
   for the 8390.  The board is easy to identify, with both a dedicated 16 bit
   ID register and a constant 0x530* value in the upper bits of the paging
   register.
*/

#define HP_ID			0x00	/* ID register, always 0x4850. */
#define HP_PAGING		0x02	/* Registers visible @ 8-f, see PageName. */
#define HPP_OPTION		0x04	/* Bitmapped options, see HP_Option.	*/
#define HPP_OUT_ADDR	0x08	/* I/O output location in Perf_Page.	*/
#define HPP_IN_ADDR		0x0A	/* I/O input location in Perf_Page.		*/
#define HP_DATAPORT		0x0c	/* I/O data transfer in Perf_Page.		*/
#define NIC_OFFSET		0x10	/* Offset to the 8390 registers.		*/
#define HP_IO_EXTENT	32

#define HP_START_PG		0x00	/* First page of TX buffer */
#define HP_STOP_PG		0x80	/* Last page +1 of RX ring */

/* The register set selected in HP_PAGING. */
enum PageName {
	Perf_Page = 0,				/* Normal operation. */
	MAC_Page = 1,				/* The ethernet address (+checksum). */
	HW_Page = 2,				/* EEPROM-loaded hardware parameters. */
	LAN_Page = 4,				/* Transceiver selection, testing, etc. */
	ID_Page = 6 };

/* The bit definitions for the HPP_OPTION register. */
enum HP_Option {
	NICReset = 1, ChipReset = 2, 	/* Active low, really UNreset. */
	EnableIRQ = 4, FakeIntr = 8, BootROMEnb = 0x10, IOEnb = 0x20,
	MemEnable = 0x40, ZeroWait = 0x80, MemDisable = 0x1000, };

int hp_plus_probe(struct net_device *dev);
int hpp_probe1(struct net_device *dev, int ioaddr);

static void hpp_reset_8390(struct net_device *dev);
static int hpp_open(struct net_device *dev);
static int hpp_close(struct net_device *dev);
static void hpp_mem_block_input(struct net_device *dev, int count,
						  struct sk_buff *skb, int ring_offset);
static void hpp_mem_block_output(struct net_device *dev, int count,
							const unsigned char *buf, int start_page);
static void hpp_mem_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr,
						  int ring_page);
static void hpp_io_block_input(struct net_device *dev, int count,
						  struct sk_buff *skb, int ring_offset);
static void hpp_io_block_output(struct net_device *dev, int count,
							const unsigned char *buf, int start_page);
static void hpp_io_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr,
						  int ring_page);


/*	Probe a list of addresses for an HP LAN+ adaptor.
	This routine is almost boilerplate. */
#ifdef HAVE_DEVLIST
/* Support for an alternate probe manager, which will eliminate the
   boilerplate below. */
struct netdev_entry hpplus_drv =
{"hpplus", hpp_probe1, HP_IO_EXTENT, hpplus_portlist};
#else

int __init hp_plus_probe(struct net_device *dev)
{
	int i;
	int base_addr = dev ? dev->base_addr : 0;

	if (base_addr > 0x1ff)		/* Check a single specified location. */
		return hpp_probe1(dev, base_addr);
	else if (base_addr != 0)	/* Don't probe at all. */
		return ENXIO;

	for (i = 0; hpplus_portlist[i]; i++) {
		int ioaddr = hpplus_portlist[i];
		if (check_region(ioaddr, HP_IO_EXTENT))
			continue;
		if (hpp_probe1(dev, ioaddr) == 0)
			return 0;
	}

	return ENODEV;
}
#endif

/* Do the interesting part of the probe at a single address. */
int __init hpp_probe1(struct net_device *dev, int ioaddr)
{
	int i;
	unsigned char checksum = 0;
	const char *name = "HP-PC-LAN+";
	int mem_start;
	static unsigned version_printed = 0;

	/* Check for the HP+ signature, 50 48 0x 53. */
	if (inw(ioaddr + HP_ID) != 0x4850
		|| (inw(ioaddr + HP_PAGING) & 0xfff0) != 0x5300)
		return ENODEV;

	if (load_8390_module("hp-plus.c"))
		return -ENOSYS;

	/* We should have a "dev" from Space.c or the static module table. */
	if (dev == NULL) {
		printk("hp-plus.c: Passed a NULL device.\n");
		dev = init_etherdev(0, 0);
	}

	if (ei_debug  &&  version_printed++ == 0)
		printk(version);

	printk("%s: %s at %#3x,", dev->name, name, ioaddr);

	/* Retrieve and checksum the station address. */
	outw(MAC_Page, ioaddr + HP_PAGING);

	for(i = 0; i < ETHER_ADDR_LEN; i++) {
		unsigned char inval = inb(ioaddr + 8 + i);
		dev->dev_addr[i] = inval;
		checksum += inval;
		printk(" %2.2x", inval);
	}
	checksum += inb(ioaddr + 14);

	if (checksum != 0xff) {
		printk(" bad checksum %2.2x.\n", checksum);
		return ENODEV;
	} else {
		/* Point at the Software Configuration Flags. */
		outw(ID_Page, ioaddr + HP_PAGING);
		printk(" ID %4.4x", inw(ioaddr + 12));
	}

	/* Allocate dev->priv and fill in 8390 specific dev fields. */
	if (ethdev_init(dev)) {
		printk ("hp-plus.c: unable to allocate memory for dev->priv.\n");
		return -ENOMEM;
	 }

	/* Grab the region so we can find another board if something fails. */
	request_region(ioaddr, HP_IO_EXTENT,"hp-plus");

	/* Read the IRQ line. */
	outw(HW_Page, ioaddr + HP_PAGING);
	{
		int irq = inb(ioaddr + 13) & 0x0f;
		int option = inw(ioaddr + HPP_OPTION);

		dev->irq = irq;
		if (option & MemEnable) {
			mem_start = inw(ioaddr + 9) << 8;
			printk(", IRQ %d, memory address %#x.\n", irq, mem_start);
		} else {
			mem_start = 0;
			printk(", IRQ %d, programmed-I/O mode.\n", irq);
		}
	}

	/* Set the wrap registers for string I/O reads.   */
	outw((HP_START_PG + TX_2X_PAGES) | ((HP_STOP_PG - 1) << 8), ioaddr + 14);

	/* Set the base address to point to the NIC, not the "real" base! */
	dev->base_addr = ioaddr + NIC_OFFSET;

	dev->open = &hpp_open;
	dev->stop = &hpp_close;

	ei_status.name = name;
	ei_status.word16 = 0;		/* Agggghhhhh! Debug time: 2 days! */
	ei_status.tx_start_page = HP_START_PG;
	ei_status.rx_start_page = HP_START_PG + TX_2X_PAGES;
	ei_status.stop_page = HP_STOP_PG;

	ei_status.reset_8390 = &hpp_reset_8390;
	ei_status.block_input = &hpp_io_block_input;
	ei_status.block_output = &hpp_io_block_output;
	ei_status.get_8390_hdr = &hpp_io_get_8390_hdr;

	/* Check if the memory_enable flag is set in the option register. */
	if (mem_start) {
		ei_status.block_input = &hpp_mem_block_input;
		ei_status.block_output = &hpp_mem_block_output;
		ei_status.get_8390_hdr = &hpp_mem_get_8390_hdr;
		dev->mem_start = mem_start;
		dev->rmem_start = dev->mem_start + TX_2X_PAGES*256;
		dev->mem_end = dev->rmem_end
			= dev->mem_start + (HP_STOP_PG - HP_START_PG)*256;
	}

	outw(Perf_Page, ioaddr + HP_PAGING);
	NS8390_init(dev, 0);
	/* Leave the 8390 and HP chip reset. */
	outw(inw(ioaddr + HPP_OPTION) & ~EnableIRQ, ioaddr + HPP_OPTION);

	return 0;
}

static int
hpp_open(struct net_device *dev)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	int option_reg;

	if (request_irq(dev->irq, ei_interrupt, 0, "hp-plus", dev)) {
	    return -EAGAIN;
	}

	/* Reset the 8390 and HP chip. */
	option_reg = inw(ioaddr + HPP_OPTION);
	outw(option_reg & ~(NICReset + ChipReset), ioaddr + HPP_OPTION);
	udelay(5);
	/* Unreset the board and enable interrupts. */
	outw(option_reg | (EnableIRQ + NICReset + ChipReset), ioaddr + HPP_OPTION);

	/* Set the wrap registers for programmed-I/O operation.   */
	outw(HW_Page, ioaddr + HP_PAGING);
	outw((HP_START_PG + TX_2X_PAGES) | ((HP_STOP_PG - 1) << 8), ioaddr + 14);

	/* Select the operational page. */
	outw(Perf_Page, ioaddr + HP_PAGING);

	ei_open(dev);
	MOD_INC_USE_COUNT;
	return 0;
}

static int
hpp_close(struct net_device *dev)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	int option_reg = inw(ioaddr + HPP_OPTION);

	free_irq(dev->irq, dev);
	ei_close(dev);
	outw((option_reg & ~EnableIRQ) | MemDisable | NICReset | ChipReset,
		 ioaddr + HPP_OPTION);

	MOD_DEC_USE_COUNT;
	return 0;
}

static void
hpp_reset_8390(struct net_device *dev)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	int option_reg = inw(ioaddr + HPP_OPTION);

	if (ei_debug > 1) printk("resetting the 8390 time=%ld...", jiffies);

	outw(option_reg & ~(NICReset + ChipReset), ioaddr + HPP_OPTION);
	/* Pause a few cycles for the hardware reset to take place. */
	udelay(5);
	ei_status.txing = 0;
	outw(option_reg | (EnableIRQ + NICReset + ChipReset), ioaddr + HPP_OPTION);

	udelay(5);


	if ((inb_p(ioaddr+NIC_OFFSET+EN0_ISR) & ENISR_RESET) == 0)
		printk("%s: hp_reset_8390() did not complete.\n", dev->name);

	if (ei_debug > 1) printk("8390 reset done (%ld).", jiffies);
	return;
}

/* The programmed-I/O version of reading the 4 byte 8390 specific header.
   Note that transfer with the EtherTwist+ must be on word boundaries. */

static void
hpp_io_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;

	outw((ring_page<<8), ioaddr + HPP_IN_ADDR);
	insw(ioaddr + HP_DATAPORT, hdr, sizeof(struct e8390_pkt_hdr)>>1);
}

/* Block input and output, similar to the Crynwr packet driver. */

static void
hpp_io_block_input(struct net_device *dev, int count, struct sk_buff *skb, int ring_offset)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	char *buf = skb->data;

	outw(ring_offset, ioaddr + HPP_IN_ADDR);
	insw(ioaddr + HP_DATAPORT, buf, count>>1);
	if (count & 0x01)
        buf[count-1] = inw(ioaddr + HP_DATAPORT);
}

/* The corresponding shared memory versions of the above 2 functions. */

static void
hpp_mem_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	int option_reg = inw(ioaddr + HPP_OPTION);

	outw((ring_page<<8), ioaddr + HPP_IN_ADDR);
	outw(option_reg & ~(MemDisable + BootROMEnb), ioaddr + HPP_OPTION);
	isa_memcpy_fromio(hdr, dev->mem_start, sizeof(struct e8390_pkt_hdr));
	outw(option_reg, ioaddr + HPP_OPTION);
	hdr->count = (hdr->count + 3) & ~3;	/* Round up allocation. */
}

static void
hpp_mem_block_input(struct net_device *dev, int count, struct sk_buff *skb, int ring_offset)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	int option_reg = inw(ioaddr + HPP_OPTION);

	outw(ring_offset, ioaddr + HPP_IN_ADDR);

	outw(option_reg & ~(MemDisable + BootROMEnb), ioaddr + HPP_OPTION);

	/* Caution: this relies on get_8390_hdr() rounding up count!
	   Also note that we *can't* use eth_io_copy_and_sum() because
	   it will not always copy "count" bytes (e.g. padded IP).  */

	isa_memcpy_fromio(skb->data, dev->mem_start, count);
	outw(option_reg, ioaddr + HPP_OPTION);
}

/* A special note: we *must* always transfer >=16 bit words.
   It's always safe to round up, so we do. */
static void
hpp_io_block_output(struct net_device *dev, int count,
					const unsigned char *buf, int start_page)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	outw(start_page << 8, ioaddr + HPP_OUT_ADDR);
	outsl(ioaddr + HP_DATAPORT, buf, (count+3)>>2);
	return;
}

static void
hpp_mem_block_output(struct net_device *dev, int count,
				const unsigned char *buf, int start_page)
{
	int ioaddr = dev->base_addr - NIC_OFFSET;
	int option_reg = inw(ioaddr + HPP_OPTION);

	outw(start_page << 8, ioaddr + HPP_OUT_ADDR);
	outw(option_reg & ~(MemDisable + BootROMEnb), ioaddr + HPP_OPTION);
	isa_memcpy_toio(dev->mem_start, buf, (count + 3) & ~3);
	outw(option_reg, ioaddr + HPP_OPTION);

	return;
}


#ifdef MODULE
#define MAX_HPP_CARDS	4	/* Max number of HPP cards per module */
#define NAMELEN		8	/* # of chars for storing dev->name */
static char namelist[NAMELEN * MAX_HPP_CARDS] = { 0, };
static struct net_device dev_hpp[MAX_HPP_CARDS] = {
	{
		NULL,		/* assign a chunk of namelist[] below */
		0, 0, 0, 0,
		0, 0,
		0, 0, 0, NULL, NULL
	},
};

static int io[MAX_HPP_CARDS] = { 0, };
static int irq[MAX_HPP_CARDS]  = { 0, };

MODULE_PARM(io, "1-" __MODULE_STRING(MAX_HPP_CARDS) "i");
MODULE_PARM(irq, "1-" __MODULE_STRING(MAX_HPP_CARDS) "i");

/* This is set up so that only a single autoprobe takes place per call.
ISA device autoprobes on a running machine are not recommended. */
int
init_module(void)
{
	int this_dev, found = 0;

	for (this_dev = 0; this_dev < MAX_HPP_CARDS; this_dev++) {
		struct net_device *dev = &dev_hpp[this_dev];
		dev->name = namelist+(NAMELEN*this_dev);
		dev->irq = irq[this_dev];
		dev->base_addr = io[this_dev];
		dev->init = hp_plus_probe;
		if (io[this_dev] == 0)  {
			if (this_dev != 0) break; /* only autoprobe 1st one */
			printk(KERN_NOTICE "hp-plus.c: Presently autoprobing (not recommended) for a single card.\n");
		}
		if (register_netdev(dev) != 0) {
			printk(KERN_WARNING "hp-plus.c: No HP-Plus card found (i/o = 0x%x).\n", io[this_dev]);
			if (found != 0) {	/* Got at least one. */
				lock_8390_module();
				return 0;
			}
			return -ENXIO;
		}
		found++;
	}
	lock_8390_module();
	return 0;
}

void
cleanup_module(void)
{
	int this_dev;

	for (this_dev = 0; this_dev < MAX_HPP_CARDS; this_dev++) {
		struct net_device *dev = &dev_hpp[this_dev];
		if (dev->priv != NULL) {
			int ioaddr = dev->base_addr - NIC_OFFSET;
			void *priv = dev->priv;
			/* NB: hpp_close() handles free_irq */
			release_region(ioaddr, HP_IO_EXTENT);
			unregister_netdev(dev);
			kfree(priv);
		}
	}
	unlock_8390_module();
}
#endif /* MODULE */

/*
 * Local variables:
 * compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -Wall -Wstrict-prototypes -O6 -m486 -c hp-plus.c"
 * version-control: t
 * kept-new-versions: 5
 * tab-width: 4
 * c-indent-level: 4
 * End:
 */
