/*
 * acenic.c: Linux driver for the Alteon AceNIC Gigabit Ethernet card
 *           and other Tigon based cards.
 *
 * Copyright 1998, 1999 by Jes Sorensen, <Jes.Sorensen@cern.ch>.
 *
 * Thanks to Alteon and 3Com for providing hardware and documentation
 * enabling me to write this driver.
 *
 * A mailing list for discussing the use of this driver has been
 * setup, please subscribe to the lists if you have any questions
 * about the driver. Send mail to linux-acenic-help@sunsite.auc.dk to
 * see how to subscribe.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Additional work by Pete Wyckoff <wyckoff@ca.sandia.gov> for initial
 * Alpha and trace dump support. The trace dump support has not been
 * integrated yet however.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/mm.h>
#ifdef ETHTOOL
#include <linux/ethtool.h>
#endif
#include <net/sock.h>
#include <net/ip.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>


#ifdef CONFIG_ACENIC_OMIT_TIGON_I
#define ACE_IS_TIGON_I(ap)	0
#else
#define ACE_IS_TIGON_I(ap)	(ap->version == 1)
#endif

#ifndef PCI_VENDOR_ID_ALTEON
#define PCI_VENDOR_ID_ALTEON		0x12ae	
#define PCI_DEVICE_ID_ALTEON_ACENIC	0x0001
#endif
#ifndef PCI_DEVICE_ID_3COM_3C985
#define PCI_DEVICE_ID_3COM_3C985	0x0001
#endif
#ifndef PCI_VENDOR_ID_NETGEAR
#define PCI_VENDOR_ID_NETGEAR		0x1385
#define PCI_DEVICE_ID_NETGEAR_GA620	0x620a
#endif
/*
 * They used the DEC vendor ID by mistake
 */
#ifndef PCI_DEVICE_ID_FARALLON_PN9000SX
#define PCI_DEVICE_ID_FARALLON_PN9000SX 0x1a
#endif
#ifndef PCI_VENDOR_ID_SGI
#define PCI_VENDOR_ID_SGI             0x10a9
#endif
#ifndef PCI_DEVICE_ID_SGI_ACENIC
#define PCI_DEVICE_ID_SGI_ACENIC      0x0009
#endif

#ifndef wmb
#define wmb()	mb()
#endif

#if (LINUX_VERSION_CODE < 0x02030e)
#define net_device device
#endif

#include "acenic.h"

/*
 * These must be defined before the firmware is included.
 */
#define MAX_TEXT_LEN	96*1024
#define MAX_RODATA_LEN	8*1024
#define MAX_DATA_LEN	2*1024

#include "acenic_firmware.h"

/*
 * This driver currently supports Tigon I and Tigon II based cards
 * including the Alteon AceNIC and the 3Com 3C985. The driver should
 * also work on the NetGear GA620, however I have not been able to
 * test that myself.
 *
 * This card is really neat, it supports receive hardware checksumming
 * and jumbo frames (up to 9000 bytes) and does a lot of work in the
 * firmware. Also the programming interface is quite neat, except for
 * the parts dealing with the i2c eeprom on the card ;-)
 *
 * Using jumbo frames:
 *
 * To enable jumbo frames, simply specify an mtu between 1500 and 9000
 * bytes to ifconfig. Jumbo frames can be enabled or disabled at any time
 * by running `ifconfig eth<X> mtu <MTU>' with <X> being the Ethernet
 * interface number and <MTU> being the MTU value.
 *
 * Module parameters:
 *
 * When compiled as a loadable module, the driver allows for a number
 * of module parameters to be specified. The driver supports the
 * following module parameters:
 *
 *  trace=<val> - Firmware trace level. This requires special traced
 *                firmware to replace the firmware supplied with
 *                the driver - for debugging purposes only.
 *
 *  link=<val>  - Link state. Normally you want to use the default link
 *                parameters set by the driver. This can be used to
 *                override these in case your switch doesn't negotiate
 *                the link properly. Valid values are:
 *         0x0001 - Force half duplex link.
 *         0x0002 - Do not negotiate line speed with the other end.
 *         0x0010 - 10Mbit/sec link.
 *         0x0020 - 100Mbit/sec link.
 *         0x0040 - 1000Mbit/sec link.
 *         0x0100 - Do not negotiate flow control.
 *         0x0200 - Enable RX flow control Y
 *         0x0400 - Enable TX flow control Y (Tigon II NICs only).
 *                Default value is 0x0270, ie. enable link+flow
 *                control negotiation. Negotiating the highest
 *                possible link speed with RX flow control enabled.
 *
 *                When disabling link speed negotiation, only one link
 *                speed is allowed to be specified!
 *
 *  tx_coal_tick=<val> - number of coalescing clock ticks (us) allowed
 *                to wait for more packets to arive before
 *                interrupting the host, from the time the first
 *                packet arrives.
 *
 *  rx_coal_tick=<val> - number of coalescing clock ticks (us) allowed
 *                to wait for more packets to arive in the transmit ring,
 *                before interrupting the host, after transmitting the
 *                first packet in the ring.
 *
 *  max_tx_desc=<val> - maximum number of transmit descriptors
 *                (packets) transmitted before interrupting the host.
 *
 *  max_rx_desc=<val> - maximum number of receive descriptors
 *                (packets) received before interrupting the host.
 *
 *  tx_ratio=<val> - 7 bit value (0 - 63) specifying the split in 64th
 *                increments of the NIC's on board memory to be used for
 *                transmit and receive buffers. For the 1MB NIC app. 800KB
 *                is available, on the 1/2MB NIC app. 300KB is available.
 *                68KB will always be available as a minimum for both
 *                directions. The default value is a 50/50 split.
 *  dis_pci_mem_inval=<val> - disable PCI memory write and invalidate
 *                operations, default (1) is to always disable this as
 *                that is what Alteon does on NT. I have not been able
 *                to measure any real performance differences with
 *                this on my systems. Set <val>=0 if you want to
 *                enable these operations.
 *
 * If you use more than one NIC, specify the parameters for the
 * individual NICs with a comma, ie. trace=0,0x00001fff,0 you want to
 * run tracing on NIC #2 but not on NIC #1 and #3.
 *
 * TODO:
 *
 * - Proper multicast support.
 * - NIC dump support.
 * - More tuning parameters.
 *
 * The mini ring is not used under Linux and I am not sure it makes sense
 * to actually use it.
 *
 * New interrupt handler strategy:
 *
 * The old interrupt handler worked using the traditional method of
 * replacing an skbuff with a new one when a packet arrives. However
 * the rx rings do not need to contain a static number of buffer
 * descriptors, thus it makes sense to move the memory allocation out
 * of the main interrupt handler and do it in a bottom half handler
 * and only allocate new buffers when the number of buffers in the
 * ring is below a certain threshold. In order to avoid starving the
 * NIC under heavy load it is however necessary to force allocation
 * when hitting a minimum threshold. The strategy for alloction is as
 * follows:
 *
 *     RX_LOW_BUF_THRES    - allocate buffers in the bottom half
 *     RX_PANIC_LOW_THRES  - we are very low on buffers, allocate
 *                           the buffers in the interrupt handler
 *     RX_RING_THRES       - maximum number of buffers in the rx ring
 *     RX_MINI_THRES       - maximum number of buffers in the mini ring
 *     RX_JUMBO_THRES      - maximum number of buffers in the jumbo ring
 *
 * One advantagous side effect of this allocation approach is that the
 * entire rx processing can be done without holding any spin lock
 * since the rx rings and registers are totally independant of the tx
 * ring and its registers.  This of course includes the kmalloc's of
 * new skb's. Thus start_xmit can run in parallel with rx processing
 * and the memory allocation on SMP systems.
 *
 * Note that running the skb reallocation in a bottom half opens up
 * another can of races which needs to be handled properly. In
 * particular it can happen that the interrupt handler tries to run
 * the reallocation while the bottom half is either running on another
 * CPU or was interrupted on the same CPU. To get around this the
 * driver uses bitops to prevent the reallocation routines from being
 * reentered.
 *
 * TX handling can also be done without holding any spin lock, wheee
 * this is fun! since tx_ret_csm is only written to by the interrupt
 * handler. The case to be aware of is when shutting down the device
 * and cleaning up where it is necessary to make sure that
 * start_xmit() is not running while this is happening. Well DaveM
 * informs me that this case is already protected against ... bye bye
 * Mr. Spin Lock, it was nice to know you.
 *
 * TX interrupts are now partly disabled so the NIC will only generate
 * TX interrupts for the number of coal ticks, not for the number of
 * TX packets in the queue. This should reduce the number of TX only,
 * ie. when no RX processing is done, interrupts seen.
 */

/*
 * Threshold values for RX buffer allocation - the low water marks for
 * when to start refilling the rings are set to 75% of the ring
 * sizes. It seems to make sense to refill the rings entirely from the
 * intrrupt handler once it gets below the panic threshold, that way
 * we don't risk that the refilling is moved to another CPU when the
 * one running the interrupt handler just got the slab code hot in its
 * cache.
 */
#define RX_RING_SIZE		72
#define RX_MINI_SIZE		64
#define RX_JUMBO_SIZE		48

#define RX_PANIC_STD_THRES	16
#define RX_PANIC_STD_REFILL	(3*RX_PANIC_STD_THRES)/2
#define RX_LOW_STD_THRES	(3*RX_RING_SIZE)/4
#define RX_PANIC_MINI_THRES	12
#define RX_PANIC_MINI_REFILL	(3*RX_PANIC_MINI_THRES)/2
#define RX_LOW_MINI_THRES	(3*RX_MINI_SIZE)/4
#define RX_PANIC_JUMBO_THRES	6
#define RX_PANIC_JUMBO_REFILL	(3*RX_PANIC_JUMBO_THRES)/2
#define RX_LOW_JUMBO_THRES	(3*RX_JUMBO_SIZE)/4


/*
 * Size of the mini ring entries, basically these just should be big
 * enough to take TCP ACKs
 */
#define ACE_MINI_SIZE		100

#define ACE_MINI_BUFSIZE	(ACE_MINI_SIZE + 2 + 16)
#define ACE_STD_BUFSIZE		(ACE_STD_MTU + ETH_HLEN + 2+4+16)
#define ACE_JUMBO_BUFSIZE	(ACE_JUMBO_MTU + ETH_HLEN + 2+4+16)

#define DEF_TX_RATIO		24
#define DEF_TX_COAL		1000
#define DEF_TX_MAX_DESC		40
#define DEF_RX_COAL		1000
#define DEF_RX_MAX_DESC		20
#define TX_COAL_INTS_ONLY	0	/* seems not worth it */
#define DEF_TRACE		0
#define DEF_STAT		2 * TICKS_PER_SEC

static int link[8] = {0, };
static int trace[8] = {0, };
static int tx_coal_tick[8] = {0, };
static int rx_coal_tick[8] = {0, };
static int max_tx_desc[8] = {0, };
static int max_rx_desc[8] = {0, };
static int tx_ratio[8] = {0, };
static int dis_pci_mem_inval[8] = {1, 1, 1, 1, 1, 1, 1, 1};

static const char __initdata *version = "acenic.c: v0.34 09/03/99  Jes Sorensen (Jes.Sorensen@cern.ch)\n";

static struct net_device *root_dev = NULL;

static int probed __initdata = 0;


int __init acenic_probe(void)
{
	int boards_found = 0;
	int version_disp;
	struct ace_private *ap;
	struct pci_dev *pdev = NULL;
	struct net_device *dev;

	if (probed)
		return -ENODEV;
	probed ++;

	if (!pci_present())		/* is PCI support present? */
		return -ENODEV;

	version_disp = 0;

	while ((pdev = pci_find_class(PCI_CLASS_NETWORK_ETHERNET<<8, pdev))){

		if (!((pdev->vendor == PCI_VENDOR_ID_ALTEON) &&
		      (pdev->device == PCI_DEVICE_ID_ALTEON_ACENIC)) &&
		    !((pdev->vendor == PCI_VENDOR_ID_3COM) &&
		      (pdev->device == PCI_DEVICE_ID_3COM_3C985)) &&
		    !((pdev->vendor == PCI_VENDOR_ID_NETGEAR) &&
		      (pdev->device == PCI_DEVICE_ID_NETGEAR_GA620)) &&
		/*
		 * Farallon used the DEC vendor ID on their cards by
		 * mistake for a while
		 */
		    !((pdev->vendor == PCI_VENDOR_ID_DEC) &&
		      (pdev->device == PCI_DEVICE_ID_FARALLON_PN9000SX)) &&
		    !((pdev->vendor == PCI_VENDOR_ID_SGI) &&
		      (pdev->device == PCI_DEVICE_ID_SGI_ACENIC)))
			continue;

		dev = init_etherdev(NULL, sizeof(struct ace_private));

		if (dev == NULL){
			printk(KERN_ERR "acenic: Unable to allocate net_device "
			       "structure!\n");
			break;
		}

		if (!dev->priv)
			dev->priv = kmalloc(sizeof(*ap), GFP_KERNEL);
		if (!dev->priv)
		{
			printk(KERN_ERR "acenic: Unable to allocate memory.\n");
			return -ENOMEM;
		}
		ap = dev->priv;
		ap->pdev = pdev;

		dev->irq = pdev->irq;

		dev->open = &ace_open;
		dev->hard_start_xmit = &ace_start_xmit;
		dev->stop = &ace_close;
		dev->get_stats = &ace_get_stats;
		dev->set_multicast_list = &ace_set_multicast_list;
		dev->do_ioctl = &ace_ioctl;
		dev->set_mac_address = &ace_set_mac_addr;
		dev->change_mtu = &ace_change_mtu;

		/* display version info if adapter is found */
		if (!version_disp)
		{
			/* set display flag to TRUE so that */
			/* we only display this string ONCE */
			version_disp = 1;
			printk(version);
		}

		pci_read_config_word(pdev, PCI_COMMAND, &ap->pci_command);

		pci_read_config_byte(pdev, PCI_LATENCY_TIMER,
				     &ap->pci_latency);
		if (ap->pci_latency <= 0x40){
			ap->pci_latency = 0x40;
			pci_write_config_byte(pdev, PCI_LATENCY_TIMER,
					      ap->pci_latency);
		}

		pci_set_master(pdev);

		/*
		 * Remap the regs into kernel space - this is abuse of
		 * dev->base_addr since it was means for I/O port
		 * addresses but who gives a damn.
		 */
#if (LINUX_VERSION_CODE < 0x02030d)
		dev->base_addr = pdev->base_address[0];
#else
		dev->base_addr = pdev->resource[0].start;
#endif

		ap->regs = (struct ace_regs *)ioremap(dev->base_addr, 0x4000);
		if (!ap->regs){
			printk(KERN_ERR "%s:  Unable to map I/O register, "
			       "AceNIC %i will be disabled.\n",
			       dev->name, boards_found);
			break;
		}

		switch(pdev->vendor){
		case PCI_VENDOR_ID_ALTEON:
			sprintf(ap->name, "AceNIC Gigabit Ethernet");
			printk(KERN_INFO "%s: Alteon AceNIC ", dev->name);
			break;
		case PCI_VENDOR_ID_3COM:
			sprintf(ap->name, "3Com 3C985 Gigabit Ethernet");
			printk(KERN_INFO "%s: 3Com 3C985 ", dev->name);
			break;
		case PCI_VENDOR_ID_NETGEAR:
			sprintf(ap->name, "NetGear GA620 Gigabit Ethernet");
			printk(KERN_INFO "%s: NetGear GA620 ", dev->name);
			break;
		case PCI_VENDOR_ID_DEC:
			if (pdev->device == PCI_DEVICE_ID_FARALLON_PN9000SX) {
				sprintf(ap->name, "Farallon PN9000-SX "
					"Gigabit Ethernet");
				printk(KERN_INFO "%s: Farallon PN9000-SX ",
				       dev->name);
				break;
			}
		case PCI_VENDOR_ID_SGI:
			sprintf(ap->name, "SGI AceNIC Gigabit Ethernet");
			printk(KERN_INFO "%s: SGI AceNIC ", dev->name);
			break;
		default:
			sprintf(ap->name, "Unknown AceNIC based Gigabit Ethernet");
			printk(KERN_INFO "%s: Unknown AceNIC ", dev->name);
			break;
		}
		printk("Gigabit Ethernet at 0x%08lx, irq %i\n",
		       dev->base_addr, dev->irq);

#ifdef CONFIG_ACENIC_OMIT_TIGON_I
		if ((readl(&ap->regs->HostCtrl) >> 28) == 4) {
			printk(KERN_ERR "%s: Driver compiled without Tigon I"
			       " support - NIC disabled\n", dev->name);
			iounmap(ap->regs);
			unregister_netdev(dev);
			continue;
		}
#endif

#ifdef MODULE
		if (ace_init(dev, boards_found))
			continue;
#else
		if (ace_init(dev, -1))
			continue;
#endif

		boards_found++;
	}

	/*
	 * If we're at this point we're going through ace_probe() for
	 * the first time.  Return success (0) if we've initialized 1
	 * or more boards. Otherwise, return failure (-ENODEV).
	 */

#ifdef MODULE
	return boards_found;
#else
	if (boards_found > 0)
		return 0;
	else
		return -ENODEV;
#endif
}


#ifdef MODULE
#if LINUX_VERSION_CODE > 0x20118
MODULE_AUTHOR("Jes Sorensen <Jes.Sorensen@cern.ch>");
MODULE_DESCRIPTION("AceNIC/3C985 Gigabit Ethernet driver");
MODULE_PARM(link, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(trace, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(tx_coal_tick, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(max_tx_desc, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(rx_coal_tick, "1-" __MODULE_STRING(8) "i");
MODULE_PARM(max_rx_desc, "1-" __MODULE_STRING(8) "i");
#endif


int init_module(void)
{
	int cards;

	root_dev = NULL;

	cards = acenic_probe();
	return cards ? 0 : -ENODEV;
}


void cleanup_module(void)
{
	struct ace_private *ap;
	struct ace_regs *regs;
	struct net_device *next;
	short i;

	while (root_dev){
		next = ((struct ace_private *)root_dev->priv)->next;
		ap = (struct ace_private *)root_dev->priv;

		regs = ap->regs;

		writel(readl(&regs->CpuCtrl) | CPU_HALT, &regs->CpuCtrl);
		if (ap->version >= 2)
			writel(readl(&regs->CpuBCtrl) | CPU_HALT,
			       &regs->CpuBCtrl);
		/*
		 * This clears any pending interrupts
		 */
		writel(0, &regs->Mb0Lo);

		/*
		 * Make sure no other CPUs are processing interrupts
		 * on the card before the buffers are being released.
		 * Otherwise one might experience some `interesting'
		 * effects.
		 *
		 * Then release the RX buffers - jumbo buffers were
		 * already released in ace_close().
		 */
		synchronize_irq();

		for (i = 0; i < RX_STD_RING_ENTRIES; i++) {
			if (ap->skb->rx_std_skbuff[i]) {
				ap->rx_std_ring[i].size = 0;
				set_aceaddr_bus(&ap->rx_std_ring[i].addr, 0);
				dev_kfree_skb(ap->skb->rx_std_skbuff[i]);
			}
		}
		if (ap->version >= 2) {
			for (i = 0; i < RX_MINI_RING_ENTRIES; i++) {
				if (ap->skb->rx_mini_skbuff[i]) {
					ap->rx_mini_ring[i].size = 0;
					set_aceaddr_bus(&ap->rx_mini_ring[i].addr, 0);
					dev_kfree_skb(ap->skb->rx_mini_skbuff[i]);
				}
			}
		}

		iounmap(regs);
		if(ap->trace_buf)
			kfree(ap->trace_buf);
		kfree(ap->info);
		kfree(ap->skb);
		free_irq(root_dev->irq, root_dev);
		unregister_netdev(root_dev);
		kfree(root_dev);

		root_dev = next;
	}
}
#endif


/*
 * Commands are considered to be slow.
 */
static inline void ace_issue_cmd(struct ace_regs *regs, struct cmd *cmd)
{
	u32 idx;

	idx = readl(&regs->CmdPrd);

	writel(*(u32 *)(cmd), &regs->CmdRng[idx]);
	idx = (idx + 1) % CMD_RING_ENTRIES;

	writel(idx, &regs->CmdPrd);
}


static int __init ace_init(struct net_device *dev, int board_idx)
{
	struct ace_private *ap;
	struct ace_regs *regs;
	struct ace_info *info;
	unsigned long tmp_ptr, myjif;
	u32 tig_ver, mac1, mac2, tmp, pci_state;
	short i;

	ap = dev->priv;
	regs = ap->regs;

	/*
	 * Don't access any other registes before this point!
	 */
#ifdef __BIG_ENDIAN
	writel(((BYTE_SWAP | WORD_SWAP | CLR_INT) |
		((BYTE_SWAP | WORD_SWAP | CLR_INT) << 24)),
	       &regs->HostCtrl);
#else
	writel((CLR_INT | WORD_SWAP | ((CLR_INT | WORD_SWAP) << 24)),
	       &regs->HostCtrl);
#endif
	mb();

	/*
	 * Stop the NIC CPU and clear pending interrupts
	 */
	writel(readl(&regs->CpuCtrl) | CPU_HALT, &regs->CpuCtrl);
	writel(0, &regs->Mb0Lo);

	tig_ver = readl(&regs->HostCtrl) >> 28;

	switch(tig_ver){
#ifndef CONFIG_ACENIC_OMIT_TIGON_I
	case 4:
		printk(KERN_INFO"  Tigon I (Rev. 4), Firmware: %i.%i.%i, ",
		       tigonFwReleaseMajor, tigonFwReleaseMinor,
		       tigonFwReleaseFix);
		writel(0, &regs->LocalCtrl);
		ap->version = 1;
		break;
#endif
	case 6:
		printk(KERN_INFO"  Tigon II (Rev. %i), Firmware: %i.%i.%i, ",
		       tig_ver, tigon2FwReleaseMajor, tigon2FwReleaseMinor,
		       tigon2FwReleaseFix);
		writel(readl(&regs->CpuBCtrl) | CPU_HALT, &regs->CpuBCtrl);
		writel(SRAM_BANK_512K, &regs->LocalCtrl);
		writel(SYNC_SRAM_TIMING, &regs->MiscCfg);
		ap->version = 2;
		break;
	default:
		printk(KERN_INFO"  Unsupported Tigon version detected (%i), ",
		       tig_ver);
		return -ENODEV;
	}

	/*
	 * ModeStat _must_ be set after the SRAM settings as this change
	 * seems to corrupt the ModeStat and possible other registers.
	 * The SRAM settings survive resets and setting it to the same
	 * value a second time works as well. This is what caused the
	 * `Firmware not running' problem on the Tigon II.
	 */
#ifdef __LITTLE_ENDIAN
	writel(ACE_BYTE_SWAP_DATA | ACE_WARN | ACE_FATAL |
	       ACE_WORD_SWAP | ACE_NO_JUMBO_FRAG, &regs->ModeStat);
#else
#error "this driver doesn't run on big-endian machines yet!"
#endif

	mac1 = 0;
	for(i = 0; i < 4; i++){
		mac1 = mac1 << 8;
		mac1 |= read_eeprom_byte(regs, 0x8c+i);
	}
	mac2 = 0;
	for(i = 4; i < 8; i++){
		mac2 = mac2 << 8;
		mac2 |= read_eeprom_byte(regs, 0x8c+i);
	}

	writel(mac1, &regs->MacAddrHi);
	writel(mac2, &regs->MacAddrLo);

	printk("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       (mac1 >> 8) & 0xff, mac1 & 0xff, (mac2 >> 24) &0xff,
	       (mac2 >> 16) & 0xff, (mac2 >> 8) & 0xff, mac2 & 0xff);

	dev->dev_addr[0] = (mac1 >> 8) & 0xff;
	dev->dev_addr[1] = mac1 & 0xff;
	dev->dev_addr[2] = (mac2 >> 24) & 0xff;
	dev->dev_addr[3] = (mac2 >> 16) & 0xff;
	dev->dev_addr[4] = (mac2 >> 8) & 0xff;
	dev->dev_addr[5] = mac2 & 0xff;

	pci_state = readl(&regs->PciState);
	printk(KERN_INFO "  PCI bus speed: %iMHz, latency: %i clks\n",
	       (pci_state & PCI_66MHZ) ? 66 : 33, ap->pci_latency);

	/*
	 * Set the max DMA transfer size. Seems that for most systems
	 * the performance is better when no MAX parameter is
	 * set. However for systems enabling PCI write and invalidate,
	 * DMA writes must be set to the L1 cache line size to get
	 * optimal performance.
	 *
	 * The default is now to turn the PCI write and invalidate off
	 * - that is what Alteon does for NT.
	 */
	tmp = READ_CMD_MEM | WRITE_CMD_MEM;
	if (ap->version >= 2){
		tmp |= (MEM_READ_MULTIPLE | (pci_state & PCI_66MHZ));
		/*
		 * Tuning parameters only supported for 8 cards
		 */
		if (board_idx > 7 || dis_pci_mem_inval[board_idx]) {
			if (ap->pci_command & PCI_COMMAND_INVALIDATE) {
				ap->pci_command &= ~PCI_COMMAND_INVALIDATE;
				pci_write_config_word(ap->pdev, PCI_COMMAND,
						      ap->pci_command);
				printk(KERN_INFO "%s: disabling PCI memory "
				       "write and invalidate\n", dev->name);
			}
		} else if (ap->pci_command & PCI_COMMAND_INVALIDATE){
			printk(KERN_INFO "%s: PCI memory write & invalidate "
			       "enabled by BIOS, enabling counter "
			       "measures\n", dev->name);
			switch(L1_CACHE_BYTES){
			case 16:
				tmp |= DMA_WRITE_MAX_16;
				break;
			case 32:
				tmp |= DMA_WRITE_MAX_32;
				break;
			case 64:
				tmp |= DMA_WRITE_MAX_64;
				break;
			default:
				printk(KERN_INFO "  Cache line size %i not "
				       "supported, PCI write and invalidate "
				       "disabled\n", L1_CACHE_BYTES);
				ap->pci_command &= ~PCI_COMMAND_INVALIDATE;
				pci_write_config_word(ap->pdev, PCI_COMMAND,
						      ap->pci_command);
			}
		}
	}
	writel(tmp, &regs->PciState);

	/*
	 * Initialize the generic info block and the command+event rings
	 * and the control blocks for the transmit and receive rings
	 * as they need to be setup once and for all.
	 */
	if (!(info = kmalloc(sizeof(struct ace_info), GFP_KERNEL)))
		return -EAGAIN;

	/*
	 * Get the memory for the skb rings.
	 */
	if (!(ap->skb = kmalloc(sizeof(struct ace_skb), GFP_KERNEL)))
		return -EAGAIN;

	if (request_irq(dev->irq, ace_interrupt, SA_SHIRQ, ap->name, dev)) {
		printk(KERN_WARNING "%s: Requested IRQ %d is busy\n",
		       dev->name, dev->irq);
		return -EAGAIN;
	}

	/*
	 * Register the device here to be able to catch allocated
	 * interrupt handlers in case the firmware doesn't come up.
	 */
	ap->next = root_dev;
	root_dev = dev;

	ap->info = info;
	memset(info, 0, sizeof(struct ace_info));
	memset(ap->skb, 0, sizeof(struct ace_skb));

	ace_load_firmware(dev);
	ap->fw_running = 0;

	tmp_ptr = virt_to_bus((void *)info);
#if (BITS_PER_LONG == 64)
	writel(tmp_ptr >> 32, &regs->InfoPtrHi);
#else
	writel(0, &regs->InfoPtrHi);
#endif
	writel(tmp_ptr & 0xffffffff, &regs->InfoPtrLo);

	memset(ap->evt_ring, 0, EVT_RING_ENTRIES * sizeof(struct event));

	set_aceaddr(&info->evt_ctrl.rngptr, ap->evt_ring);
	info->evt_ctrl.flags = 0;

	set_aceaddr(&info->evt_prd_ptr, &ap->evt_prd);
	ap->evt_prd = 0;
	wmb();
	writel(0, &regs->EvtCsm);

	set_aceaddr_bus(&info->cmd_ctrl.rngptr, (void *)0x100);
	info->cmd_ctrl.flags = 0;
	info->cmd_ctrl.max_len = 0;

	for (i = 0; i < CMD_RING_ENTRIES; i++)
		writel(0, &regs->CmdRng[i]);

	writel(0, &regs->CmdPrd);
	writel(0, &regs->CmdCsm);

	set_aceaddr(&info->stats2_ptr, &info->s.stats);

	set_aceaddr(&info->rx_std_ctrl.rngptr, ap->rx_std_ring);
	info->rx_std_ctrl.max_len = ACE_STD_MTU + ETH_HLEN + 4;
	info->rx_std_ctrl.flags = RCB_FLG_TCP_UDP_SUM;

	memset(ap->rx_std_ring, 0,
	       RX_STD_RING_ENTRIES * sizeof(struct rx_desc));

	for (i = 0; i < RX_STD_RING_ENTRIES; i++)
		ap->rx_std_ring[i].flags = BD_FLG_TCP_UDP_SUM;

	ap->rx_std_skbprd = 0;
	atomic_set(&ap->cur_rx_bufs, 0);

	set_aceaddr(&info->rx_jumbo_ctrl.rngptr, ap->rx_jumbo_ring);
	info->rx_jumbo_ctrl.max_len = 0;
	info->rx_jumbo_ctrl.flags = RCB_FLG_TCP_UDP_SUM;

	memset(ap->rx_jumbo_ring, 0,
	       RX_JUMBO_RING_ENTRIES * sizeof(struct rx_desc));

	for (i = 0; i < RX_JUMBO_RING_ENTRIES; i++)
		ap->rx_jumbo_ring[i].flags = BD_FLG_TCP_UDP_SUM | BD_FLG_JUMBO;

	ap->rx_jumbo_skbprd = 0;
	atomic_set(&ap->cur_jumbo_bufs, 0);

	memset(ap->rx_mini_ring, 0,
	       RX_MINI_RING_ENTRIES * sizeof(struct rx_desc));

	if (ap->version >= 2) {
		set_aceaddr(&info->rx_mini_ctrl.rngptr, ap->rx_mini_ring);
		info->rx_mini_ctrl.max_len = ACE_MINI_SIZE;
		info->rx_mini_ctrl.flags = RCB_FLG_TCP_UDP_SUM;

		for (i = 0; i < RX_MINI_RING_ENTRIES; i++)
			ap->rx_mini_ring[i].flags =
				BD_FLG_TCP_UDP_SUM | BD_FLG_MINI;
	} else {
		set_aceaddr(&info->rx_mini_ctrl.rngptr, 0);
		info->rx_mini_ctrl.flags = RCB_FLG_RNG_DISABLE;
		info->rx_mini_ctrl.max_len = 0;
	}

	ap->rx_mini_skbprd = 0;
	atomic_set(&ap->cur_mini_bufs, 0);

	set_aceaddr(&info->rx_return_ctrl.rngptr, ap->rx_return_ring);
	info->rx_return_ctrl.flags = 0;
	info->rx_return_ctrl.max_len = RX_RETURN_RING_ENTRIES;

	memset(ap->rx_return_ring, 0,
	       RX_RETURN_RING_ENTRIES * sizeof(struct rx_desc));

	set_aceaddr(&info->rx_ret_prd_ptr, &ap->rx_ret_prd);

	writel(TX_RING_BASE, &regs->WinBase);
	ap->tx_ring = (struct tx_desc *)regs->Window;
	for (i = 0; i < (TX_RING_ENTRIES * sizeof(struct tx_desc) / 4); i++){
		writel(0, (unsigned long)ap->tx_ring + i * 4);
	}

	set_aceaddr_bus(&info->tx_ctrl.rngptr, (void *)TX_RING_BASE);
	info->tx_ctrl.max_len = TX_RING_ENTRIES;
#if TX_COAL_INTS_ONLY
	info->tx_ctrl.flags = RCB_FLG_COAL_INT_ONLY;
#else
	info->tx_ctrl.flags = 0;
#endif

	set_aceaddr(&info->tx_csm_ptr, &ap->tx_csm);

	/*
	 * Potential item for tuning parameter
	 */
	writel(DMA_THRESH_8W, &regs->DmaReadCfg);
	writel(DMA_THRESH_8W, &regs->DmaWriteCfg);

	writel(0, &regs->MaskInt);
	writel(1, &regs->IfIdx);
	writel(1, &regs->AssistState);

	writel(DEF_STAT, &regs->TuneStatTicks);

	writel(DEF_TX_COAL, &regs->TuneTxCoalTicks);
	writel(DEF_TX_MAX_DESC, &regs->TuneMaxTxDesc);
	writel(DEF_RX_COAL, &regs->TuneRxCoalTicks);
	writel(DEF_RX_MAX_DESC, &regs->TuneMaxRxDesc);
	writel(DEF_TRACE, &regs->TuneTrace);
	writel(DEF_TX_RATIO, &regs->TxBufRat);

	if (board_idx >= 8) {
		printk(KERN_WARNING "%s: more then 8 NICs detected, "
		       "ignoring module parameters!\n", dev->name);
		board_idx = -1;
	}

	if (board_idx >= 0) {
		if (tx_coal_tick[board_idx])
			writel(tx_coal_tick[board_idx],
			       &regs->TuneTxCoalTicks);
		if (max_tx_desc[board_idx])
			writel(max_tx_desc[board_idx], &regs->TuneMaxTxDesc);

		if (rx_coal_tick[board_idx])
			writel(rx_coal_tick[board_idx],
			       &regs->TuneRxCoalTicks);
		if (max_rx_desc[board_idx])
			writel(max_rx_desc[board_idx], &regs->TuneMaxRxDesc);

		if (trace[board_idx])
			writel(trace[board_idx], &regs->TuneTrace);

		if ((tx_ratio[board_idx] >= 0) && (tx_ratio[board_idx] < 64))
			writel(tx_ratio[board_idx], &regs->TxBufRat);
	}

	/*
	 * Default link parameters
	 */
	tmp = LNK_ENABLE | LNK_FULL_DUPLEX | LNK_1000MB | LNK_100MB |
		LNK_10MB | LNK_RX_FLOW_CTL_Y | LNK_NEG_FCTL | LNK_NEGOTIATE;
	if(ap->version >= 2)
		tmp |= LNK_TX_FLOW_CTL_Y;

	/*
	 * Override link default parameters
	 */
	if ((board_idx >= 0) && link[board_idx]) {
		int option = link[board_idx];

		tmp = LNK_ENABLE;

		if (option & 0x01){
			printk(KERN_INFO "%s: Setting half duplex link\n",
			       dev->name);
			tmp &= ~LNK_FULL_DUPLEX;
		}
		if (option & 0x02)
			tmp &= ~LNK_NEGOTIATE;
		if (option & 0x10)
			tmp |= LNK_10MB;
		if (option & 0x20)
			tmp |= LNK_100MB;
		if (option & 0x40)
			tmp |= LNK_1000MB;
		if ((option & 0x70) == 0){
			printk(KERN_WARNING "%s: No media speed specified, "
			       "forcing auto negotiation\n", dev->name);
			tmp |= LNK_NEGOTIATE | LNK_1000MB |
				LNK_100MB | LNK_10MB;
		}
		if ((option & 0x100) == 0)
			tmp |= LNK_NEG_FCTL;
		else
			printk(KERN_INFO "%s: Disabling flow control "
			       "negotiation\n", dev->name);
		if (option & 0x200)
			tmp |= LNK_RX_FLOW_CTL_Y;
		if ((option & 0x400) && (ap->version >= 2)){
			printk(KERN_INFO "%s: Enabling TX flow control\n",
			       dev->name);
			tmp |= LNK_TX_FLOW_CTL_Y;
		}
	}

	ap->link = tmp;
	writel(tmp, &regs->TuneLink);
	if (ap->version >= 2)
		writel(tmp, &regs->TuneFastLink);

	if (ACE_IS_TIGON_I(ap))
		writel(tigonFwStartAddr, &regs->Pc);
	if (ap->version == 2)
		writel(tigon2FwStartAddr, &regs->Pc);

	writel(0, &regs->Mb0Lo);

	/*
	 * Set tx_csm before we start receiving interrupts, otherwise
	 * the interrupt handler might think it is supposed to process
	 * tx ints before we are up and running, which may cause a null
	 * pointer access in the int handler.
	 */
	ap->tx_full = 0;
	ap->cur_rx = 0;
	ap->tx_prd = ap->tx_csm = ap->tx_ret_csm = 0;

	wmb();
	writel(0, &regs->TxPrd);
	writel(0, &regs->RxRetCsm);

	/*
	 * Start the NIC CPU
	 */
	writel(readl(&regs->CpuCtrl) & ~(CPU_HALT|CPU_TRACE), &regs->CpuCtrl);

	/*
	 * Wait for the firmware to spin up - max 3 seconds.
	 */
	myjif = jiffies + 3 * HZ;
	while (time_before(jiffies, myjif) && !ap->fw_running);
	if (!ap->fw_running){
		printk(KERN_ERR "%s: Firmware NOT running!\n", dev->name);
		ace_dump_trace(ap);
		writel(readl(&regs->CpuCtrl) | CPU_HALT, &regs->CpuCtrl);
		return -EBUSY;
	}

	/*
	 * We load the ring here as there seem to be no way to tell the
	 * firmware to wipe the ring without re-initializing it.
	 */
	if (!test_and_set_bit(0, &ap->std_refill_busy))
		ace_load_std_rx_ring(ap, RX_RING_SIZE);
	else
		printk(KERN_ERR "%s: Someone is busy refilling the RX ring\n",
		       dev->name);
	if (ap->version >= 2) {
		if (!test_and_set_bit(0, &ap->mini_refill_busy))
			ace_load_mini_rx_ring(ap, RX_MINI_SIZE);
		else
			printk(KERN_ERR "%s: Someone is busy refilling "
			       "the RX mini ring\n", dev->name);
	}
	return 0;
}


/*
 * Monitor the card to detect hangs.
 */
static void ace_timer(unsigned long data)
{
	struct net_device *dev = (struct net_device *)data;
	struct ace_private *ap = (struct ace_private *)dev->priv;
	struct ace_regs *regs = ap->regs;

	/*
	 * We haven't received a stats update event for more than 2.5
	 * seconds and there is data in the transmit queue, thus we
	 * asume the card is stuck.
	 */
	if (ap->tx_csm != ap->tx_ret_csm){
		printk(KERN_WARNING "%s: Transmitter is stuck, %08x\n",
		       dev->name, (unsigned int)readl(&regs->HostCtrl));
	}

	ap->timer.expires = jiffies + (5/2*HZ);
	add_timer(&ap->timer);
}


static void ace_bh(struct net_device *dev)
{
	struct ace_private *ap = dev->priv;
	int cur_size;

	cur_size = atomic_read(&ap->cur_rx_bufs);
	if ((cur_size < RX_LOW_STD_THRES) &&
	    !test_and_set_bit(0, &ap->std_refill_busy)) {
#if DEBUG
		printk("refilling buffers (current %i)\n", cur_size);
#endif
		ace_load_std_rx_ring(ap, RX_RING_SIZE - cur_size);
	}

	if (ap->version >= 2) {
		cur_size = atomic_read(&ap->cur_mini_bufs);
		if ((cur_size < RX_LOW_MINI_THRES) &&
		    !test_and_set_bit(0, &ap->mini_refill_busy)) {
#if DEBUG
			printk("refilling mini buffers (current %i)\n",
			       cur_size);
#endif
			ace_load_mini_rx_ring(ap, RX_MINI_SIZE - cur_size);
		}
	}

	cur_size = atomic_read(&ap->cur_jumbo_bufs);
	if (ap->jumbo && (cur_size < RX_LOW_JUMBO_THRES) &&
	    !test_and_set_bit(0, &ap->jumbo_refill_busy)) {
#if DEBUG
		printk("refilling jumbo buffers (current %i)\n", >cur_size);
#endif
		ace_load_jumbo_rx_ring(ap, RX_JUMBO_SIZE - cur_size);
	}
	ap->bh_pending = 0;
}


/*
 * Copy the contents of the NIC's trace buffer to kernel memory.
 */
static void ace_dump_trace(struct ace_private *ap)
{
#if 0
	if (!ap->trace_buf)
		if (!(ap->trace_buf = kmalloc(ACE_TRACE_SIZE, GFP_KERNEL)));
		    return;
#endif
}


/*
 * Load the standard rx ring.
 *
 * Loading rings is safe without holding the spin lock since this is
 * done only before the device is enabled, thus no interrupts are
 * generated and by the interrupt handler/bh handler.
 */
static void ace_load_std_rx_ring(struct ace_private *ap, int nr_bufs)
{
	struct ace_regs *regs;
	short i, idx;

	regs = ap->regs;

	idx = ap->rx_std_skbprd;

	for (i = 0; i < nr_bufs; i++) {
		struct sk_buff *skb;
		struct rx_desc *rd;

		skb = alloc_skb(ACE_STD_BUFSIZE, GFP_ATOMIC);
		/*
		 * Make sure IP header starts on a fresh cache line.
		 */
		skb_reserve(skb, 2 + 16);
		ap->skb->rx_std_skbuff[idx] = skb;

		rd = &ap->rx_std_ring[idx];
		set_aceaddr(&rd->addr, skb->data);
		rd->size = ACE_STD_MTU + ETH_HLEN + 4;
		rd->idx = idx;
		idx = (idx + 1) % RX_STD_RING_ENTRIES;
	}

	atomic_add(nr_bufs, &ap->cur_rx_bufs);
	ap->rx_std_skbprd = idx;

	if (ACE_IS_TIGON_I(ap)) {
		struct cmd cmd;
		cmd.evt = C_SET_RX_PRD_IDX;
		cmd.code = 0;
		cmd.idx = ap->rx_std_skbprd;
		ace_issue_cmd(regs, &cmd);
	} else {
		writel(idx, &regs->RxStdPrd);
		wmb();
	}

	clear_bit(0, &ap->std_refill_busy);
	return;
}


static void ace_load_mini_rx_ring(struct ace_private *ap, int nr_bufs)
{
	struct ace_regs *regs;
	short i, idx;

	regs = ap->regs;

	idx = ap->rx_mini_skbprd;
	for (i = 0; i < nr_bufs; i++) {
		struct sk_buff *skb;
		struct rx_desc *rd;

		skb = alloc_skb(ACE_MINI_BUFSIZE, GFP_ATOMIC);
		/*
		 * Make sure the IP header ends up on a fresh cache line
		 */
		skb_reserve(skb, 2 + 16);
		ap->skb->rx_mini_skbuff[idx] = skb;

		rd = &ap->rx_mini_ring[idx];
		set_aceaddr(&rd->addr, skb->data);
		rd->size = ACE_MINI_SIZE;
		rd->idx = idx;
		idx = (idx + 1) % RX_MINI_RING_ENTRIES;
	}

	atomic_add(nr_bufs, &ap->cur_mini_bufs);

	ap->rx_mini_skbprd = idx;

	writel(idx, &regs->RxMiniPrd);
	wmb();

	clear_bit(0, &ap->mini_refill_busy);
	return;
}


/*
 * Load the jumbo rx ring, this may happen at any time if the MTU
 * is changed to a value > 1500.
 */
static void ace_load_jumbo_rx_ring(struct ace_private *ap, int nr_bufs)
{
	struct ace_regs *regs;
	short i, idx;

	regs = ap->regs;

	idx = ap->rx_jumbo_skbprd;

	for (i = 0; i < nr_bufs; i++) {
		struct sk_buff *skb;
		struct rx_desc *rd;

		skb = alloc_skb(ACE_JUMBO_BUFSIZE, GFP_ATOMIC);
		/*
		 * Make sure the IP header ends up on a fresh cache line
		 */
		skb_reserve(skb, 2 + 16);
		ap->skb->rx_jumbo_skbuff[idx] = skb;

		rd = &ap->rx_jumbo_ring[idx];
		set_aceaddr(&rd->addr, skb->data);
		rd->size = ACE_JUMBO_MTU + ETH_HLEN + 4;
		rd->idx = idx;
		idx = (idx + 1) % RX_JUMBO_RING_ENTRIES;
	}

	atomic_add(nr_bufs, &ap->cur_jumbo_bufs);
	ap->rx_jumbo_skbprd = idx;

	if (ACE_IS_TIGON_I(ap)) {
		struct cmd cmd;
		cmd.evt = C_SET_RX_JUMBO_PRD_IDX;
		cmd.code = 0;
		cmd.idx = ap->rx_jumbo_skbprd;
		ace_issue_cmd(regs, &cmd);
	} else {
		writel(idx, &regs->RxJumboPrd);
		wmb();
	}

	clear_bit(0, &ap->jumbo_refill_busy);
	return;
}


/*
 * Tell the firmware not to accept jumbos and flush the jumbo ring.
 */
static int ace_flush_jumbo_rx_ring(struct net_device *dev)
{
	struct ace_private *ap;
	struct ace_regs *regs;
	struct cmd cmd;
	short i;

	ap = (struct ace_private *)dev->priv;
	regs = ap->regs;

	if (ap->jumbo){
		cmd.evt = C_RESET_JUMBO_RNG;
		cmd.code = 0;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);

		for (i = 0; i < RX_JUMBO_RING_ENTRIES; i++) {
			if (ap->skb->rx_jumbo_skbuff[i]) {
				ap->rx_jumbo_ring[i].size = 0;
				set_aceaddr_bus(&ap->rx_jumbo_ring[i].addr, 0);
				dev_kfree_skb(ap->skb->rx_jumbo_skbuff[i]);
			}
		}
	}else
		printk(KERN_ERR "%s: Trying to flush Jumbo ring without "
		       "Jumbo support enabled\n", dev->name);

	return 0;
}


/*
 * All events are considered to be slow (RX/TX ints do not generate
 * events) and are handled here, outside the main interrupt handler,
 * to reduce the size of the handler.
 */
static u32 ace_handle_event(struct net_device *dev, u32 evtcsm, u32 evtprd)
{
	struct ace_private *ap;

	ap = (struct ace_private *)dev->priv;

	while (evtcsm != evtprd){
		switch (ap->evt_ring[evtcsm].evt){
		case E_FW_RUNNING:
			printk(KERN_INFO "%s: Firmware up and running\n",
			       dev->name);
			ap->fw_running = 1;
			break;
		case E_STATS_UPDATED:
			break;
		case E_LNK_STATE:
		{
			u16 code = ap->evt_ring[evtcsm].code;
			if (code == E_C_LINK_UP){
				printk(KERN_WARNING "%s: Optical link UP\n",
				       dev->name);
			}
			else if (code == E_C_LINK_DOWN)
				printk(KERN_WARNING "%s: Optical link DOWN\n",
				       dev->name);
			else
				printk(KERN_ERR "%s: Unknown optical link "
				       "state %02x\n", dev->name, code);
			break;
		}
		case E_ERROR:
			switch(ap->evt_ring[evtcsm].code){
			case E_C_ERR_INVAL_CMD:
				printk(KERN_ERR "%s: invalid command error\n",
				       dev->name);
				break;
			case E_C_ERR_UNIMP_CMD:
				printk(KERN_ERR "%s: unimplemented command "
				       "error\n", dev->name);
				break;
			case E_C_ERR_BAD_CFG:
				printk(KERN_ERR "%s: bad config error\n",
				       dev->name);
				break;
			default:
				printk(KERN_ERR "%s: unknown error %02x\n",
				       dev->name, ap->evt_ring[evtcsm].code);
			}
			break;
		case E_RESET_JUMBO_RNG:
			break;
		default:
			printk(KERN_ERR "%s: Unhandled event 0x%02x\n",
			       dev->name, ap->evt_ring[evtcsm].evt);
		}
		evtcsm = (evtcsm + 1) % EVT_RING_ENTRIES;
	}

	return evtcsm;
}


static void ace_rx_int(struct net_device *dev, u32 rxretprd, u32 rxretcsm)
{
	struct ace_private *ap = (struct ace_private *)dev->priv;
	u32 idx;
	int mini_count = 0, std_count = 0;

	idx = rxretcsm;

	while (idx != rxretprd){
		struct sk_buff *skb, **oldskb_p;
		struct rx_desc *rxdesc;
		u32 skbidx;
		int desc_type;
		u16 csum;

		skbidx = ap->rx_return_ring[idx].idx;
		desc_type = ap->rx_return_ring[idx].flags &
			(BD_FLG_JUMBO | BD_FLG_MINI);

		switch(desc_type) {
			/*
			 * Normal frames do not have any flags set
			 *
			 * Mini and normal frames arrive frequently,
			 * so use a local counter to avoid doing
			 * atomic operations for each packet arriving.
			 */
		case 0:
			oldskb_p = &ap->skb->rx_std_skbuff[skbidx];
			rxdesc = &ap->rx_std_ring[skbidx];
			std_count++;
			break;
		case BD_FLG_JUMBO:
			oldskb_p = &ap->skb->rx_jumbo_skbuff[skbidx];
			rxdesc = &ap->rx_jumbo_ring[skbidx];
			atomic_dec(&ap->cur_jumbo_bufs);
			break;
		case BD_FLG_MINI:
			oldskb_p = &ap->skb->rx_mini_skbuff[skbidx];
			rxdesc = &ap->rx_mini_ring[skbidx];
			mini_count++; 
			break;
		default:
			printk(KERN_INFO "%s: unknown frame type (0x%02x) "
			       "returned by NIC\n", dev->name,
			       ap->rx_return_ring[idx].flags);
			goto error;
		}

		skb = *oldskb_p;
#if DEBUG
		if (skb == NULL) {
			printk("Mayday! illegal skb received! (idx %i)\n", skbidx);
			goto error;
		}
#endif
		*oldskb_p = NULL;
		skb_put(skb, rxdesc->size);
		rxdesc->size = 0;

		/*
		 * Fly baby, fly!
		 */
		csum = ap->rx_return_ring[idx].tcp_udp_csum;

		skb->dev = dev;
		skb->protocol = eth_type_trans(skb, dev);

		/*
		 * If the checksum is correct and this is not a
		 * fragment, tell the stack that the data is correct.
		 */
		if(!(csum ^ 0xffff) &&
		   (!(((struct iphdr *)skb->data)->frag_off &
		      __constant_htons(IP_MF|IP_OFFSET))))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb->ip_summed = CHECKSUM_NONE;

		netif_rx(skb);		/* send it up */

		ap->stats.rx_packets++;
		ap->stats.rx_bytes += skb->len;

		idx = (idx + 1) % RX_RETURN_RING_ENTRIES;
	}

	atomic_sub(std_count, &ap->cur_rx_bufs);
	if (!ACE_IS_TIGON_I(ap))
		atomic_sub(mini_count, &ap->cur_mini_bufs);

 out:
	/*
	 * According to the documentation RxRetCsm is obsolete with
	 * the 12.3.x Firmware - my Tigon I NICs seem to disagree!
	 */
	if (ACE_IS_TIGON_I(ap)) {
		struct ace_regs *regs = ap->regs;
		writel(idx, &regs->RxRetCsm);
	}
	ap->cur_rx = idx;

	return;
 error:
	idx = rxretprd;
	goto out;
}


static void ace_interrupt(int irq, void *dev_id, struct pt_regs *ptregs)
{
	struct ace_private *ap;
	struct ace_regs *regs;
	struct net_device *dev = (struct net_device *)dev_id;
	u32 idx;
	u32 txcsm, rxretcsm, rxretprd;
	u32 evtcsm, evtprd;

	ap = (struct ace_private *)dev->priv;
	regs = ap->regs;

	/*
	 * In case of PCI shared interrupts or spurious interrupts,
	 * we want to make sure it is actually our interrupt before
	 * spending any time in here.
	 */
	if (!(readl(&regs->HostCtrl) & IN_INT))
		return;

	/*
	 * Tell the card not to generate interrupts while we are in here.
	 */
	writel(1, &regs->Mb0Lo);

	/*
	 * There is no conflict between transmit handling in
	 * start_xmit and receive processing, thus there is no reason
	 * to take a spin lock for RX handling. Wait until we start
	 * working on the other stuff - hey we don't need a spin lock
	 * anymore.
	 */
	rxretprd = ap->rx_ret_prd;
	rxretcsm = ap->cur_rx;

	if (rxretprd != rxretcsm)
		ace_rx_int(dev, rxretprd, rxretcsm);

	txcsm = ap->tx_csm;
	idx = ap->tx_ret_csm;

	if (txcsm != idx) {
		do {
			ap->stats.tx_packets++;
			ap->stats.tx_bytes += ap->skb->tx_skbuff[idx]->len;
			dev_kfree_skb(ap->skb->tx_skbuff[idx]);

			ap->skb->tx_skbuff[idx] = NULL;

			/*
			 * Question here is whether one should not skip
			 * these writes - I have never seen any errors
			 * caused by the NIC actually trying to access
			 * these incorrectly.
			 */
#if (BITS_PER_LONG == 64)
			writel(0, &ap->tx_ring[idx].addr.addrhi);
#endif
			writel(0, &ap->tx_ring[idx].addr.addrlo);
			writel(0, &ap->tx_ring[idx].flagsize);

			idx = (idx + 1) % TX_RING_ENTRIES;
		} while (idx != txcsm);

		/*
		 * Once we actually get to this point the tx ring has
		 * already been trimmed thus it cannot be full!
		 * Ie. skip the comparison of the tx producer vs. the
		 * consumer.
		 */
		if (ap->tx_full && dev->tbusy) {
			ap->tx_full = 0;
			/*
			 * This does not need to be atomic (and expensive),
			 * I've seen cases where it would fail otherwise ;-(
			 */
			clear_bit(0, &dev->tbusy);
			mark_bh(NET_BH);

			/*
			 * TX ring is no longer full, aka the
			 * transmitter is working fine - kill timer.
			 */
			del_timer(&ap->timer);
		}

		ap->tx_ret_csm = txcsm;
		wmb();
	}

	evtcsm = readl(&regs->EvtCsm);
	evtprd = ap->evt_prd;

	if (evtcsm != evtprd) {
		evtcsm = ace_handle_event(dev, evtcsm, evtprd);
		writel(evtcsm, &regs->EvtCsm);
	}

	/*
	 * This has to go last in the interrupt handler and run with
	 * the spin lock released ... what lock?
	 */
	if (dev->start) {
		int cur_size;
		int run_bh = 0;

		cur_size = atomic_read(&ap->cur_rx_bufs);
		if (cur_size < RX_LOW_STD_THRES) {
			if ((cur_size < RX_PANIC_STD_THRES) &&
			    !test_and_set_bit(0, &ap->std_refill_busy)) {
#if DEBUG
				printk("low on std buffers %i\n", cur_size);
#endif
				ace_load_std_rx_ring(ap,
						     RX_RING_SIZE - cur_size);
			}
				run_bh = 1;
		}

		if (!ACE_IS_TIGON_I(ap)) {
			cur_size = atomic_read(&ap->cur_mini_bufs);
			if (cur_size < RX_LOW_MINI_THRES) {
				if ((cur_size < RX_PANIC_MINI_THRES) &&
				    !test_and_set_bit(0,
						      &ap->mini_refill_busy)) {
#if DEBUG
					printk("low on mini buffers %i\n",
					       cur_size);
#endif
					ace_load_mini_rx_ring(ap, RX_MINI_SIZE - cur_size);
				} else
					run_bh = 1;
			}
		}

		if (ap->jumbo) {
			cur_size = atomic_read(&ap->cur_jumbo_bufs);
			if (cur_size < RX_LOW_JUMBO_THRES) {
				if ((cur_size < RX_PANIC_JUMBO_THRES) &&
				    !test_and_set_bit(0,
						      &ap->jumbo_refill_busy)){
#if DEBUG
					printk("low on jumbo buffers %i\n",
					       cur_size);
#endif
					ace_load_jumbo_rx_ring(ap, RX_JUMBO_SIZE - cur_size);
				} else
					run_bh = 1;
			}
		}
		if (run_bh && !ap->bh_pending) {
			ap->bh_pending = 1;
			queue_task(&ap->immediate, &tq_immediate);
			mark_bh(IMMEDIATE_BH);
		}
	}

	/*
	 * Allow the card to generate interrupts again
	 */
	writel(0, &regs->Mb0Lo);
}


static int ace_open(struct net_device *dev)
{
	struct ace_private *ap;
	struct ace_regs *regs;
	struct cmd cmd;

	ap = dev->priv;
	regs = ap->regs;

	if (!(ap->fw_running)){
		printk(KERN_WARNING "%s: Firmware not running!\n", dev->name);
		return -EBUSY;
	}

	writel(dev->mtu + ETH_HLEN + 4, &regs->IfMtu);

	cmd.evt = C_HOST_STATE;
	cmd.code = C_C_STACK_UP;
	cmd.idx = 0;
	ace_issue_cmd(regs, &cmd);

	if (ap->jumbo &&
	    !test_and_set_bit(0, &ap->jumbo_refill_busy))
		ace_load_jumbo_rx_ring(ap, RX_JUMBO_SIZE);

	if (dev->flags & IFF_PROMISC){
		cmd.evt = C_SET_PROMISC_MODE;
		cmd.code = C_C_PROMISC_ENABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);

		ap->promisc = 1;
	}else
		ap->promisc = 0;
	ap->mcast_all = 0;

#if 0
	cmd.evt = C_LNK_NEGOTIATION;
	cmd.code = 0;
	cmd.idx = 0;
	ace_issue_cmd(regs, &cmd);
#endif

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;

	MOD_INC_USE_COUNT;

	/*
	 * Setup the timer
	 */
	init_timer(&ap->timer);
	ap->timer.data = (unsigned long)dev;
	ap->timer.function = ace_timer;

	/*
	 * Setup the bottom half rx ring refill handler
	 */
	ap->immediate.next = NULL;
	ap->immediate.sync = 0;
	ap->immediate.routine = (void *)(void *)ace_bh;
	ap->immediate.data = dev;

	return 0;
}


static int ace_close(struct net_device *dev)
{
	struct ace_private *ap;
	struct ace_regs *regs;
	struct cmd cmd;
	unsigned long flags;
	short i;

	dev->start = 0;
	set_bit(0, &dev->tbusy);

	ap = (struct ace_private *)dev->priv;
	regs = ap->regs;

	del_timer(&ap->timer);

	if (ap->promisc){
		cmd.evt = C_SET_PROMISC_MODE;
		cmd.code = C_C_PROMISC_DISABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
		ap->promisc = 0;
	}

	cmd.evt = C_HOST_STATE;
	cmd.code = C_C_STACK_DOWN;
	cmd.idx = 0;
	ace_issue_cmd(regs, &cmd);

	/*
	 * Make sure one CPU is not processing packets while
	 * buffers are being released by another.
	 */
	save_flags(flags);
	cli();

	for (i = 0; i < TX_RING_ENTRIES; i++) {
		if (ap->skb->tx_skbuff[i]) {
			writel(0, &ap->tx_ring[i].addr.addrhi);
			writel(0, &ap->tx_ring[i].addr.addrlo);
			writel(0, &ap->tx_ring[i].flagsize);
			dev_kfree_skb(ap->skb->tx_skbuff[i]);
		}
	}

	if (ap->jumbo)
		ace_flush_jumbo_rx_ring(dev);

	restore_flags(flags);

	MOD_DEC_USE_COUNT;
	return 0;
}


static int ace_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ace_private *ap = (struct ace_private *)dev->priv;
	struct ace_regs *regs = ap->regs;
	unsigned long addr;
	u32 idx, flagsize;

	if (test_and_set_bit(0, &dev->tbusy))
		return 1;

	idx = ap->tx_prd;

	if ((idx + 1) % TX_RING_ENTRIES == ap->tx_ret_csm) {
		ap->tx_full = 1;
#if DEBUG
		printk("%s: trying to transmit while the tx ring is full "
		       "- this should not happen!\n", dev->name);
#endif
		return 1;
	}

	ap->skb->tx_skbuff[idx] = skb;
	addr = virt_to_bus(skb->data);
#if (BITS_PER_LONG == 64)
	writel(addr >> 32, &ap->tx_ring[idx].addr.addrhi);
#endif
	writel(addr & 0xffffffff, &ap->tx_ring[idx].addr.addrlo);
	flagsize = (skb->len << 16) | (BD_FLG_END) ;
	writel(flagsize, &ap->tx_ring[idx].flagsize);
	wmb();
	idx = (idx + 1) % TX_RING_ENTRIES;

	ap->tx_prd = idx;
	writel(idx, &regs->TxPrd);
	wmb();

	/*
	 * tx_csm is set by the NIC whereas we set tx_ret_csm which
	 * is always trying to catch tx_csm
	 */
	if ((idx + 2) % TX_RING_ENTRIES == ap->tx_ret_csm){
		ap->tx_full = 1;
		/*
		 * Queue is full, add timer to detect whether the
		 * transmitter is stuck. Use mod_timer as we can get
		 * into the situation where we risk adding several
		 * timers.
		 */
		mod_timer(&ap->timer, jiffies + (3 * HZ));
	} else {
		/*
		 * No need for it to be atomic - seems it needs to be
		 */
		clear_bit(0, &dev->tbusy);
	}

	dev->trans_start = jiffies;
	return 0;
}


static int ace_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ace_private *ap = dev->priv;
	struct ace_regs *regs = ap->regs;

	if ((new_mtu < 68) || (new_mtu > ACE_JUMBO_MTU))
		return -EINVAL;

	writel(new_mtu + ETH_HLEN + 4, &regs->IfMtu);
	dev->mtu = new_mtu;

	if (new_mtu > ACE_STD_MTU){
		if (!(ap->jumbo)){
			printk(KERN_INFO "%s: Enabling Jumbo frame "
			       "support\n", dev->name);
			ap->jumbo = 1;
			if (!test_and_set_bit(0, &ap->jumbo_refill_busy))
				ace_load_jumbo_rx_ring(ap, RX_JUMBO_SIZE);
		}
		ap->jumbo = 1;
	}else{
		if (ap->jumbo){
			ace_flush_jumbo_rx_ring(dev);

			printk(KERN_INFO "%s: Disabling Jumbo frame support\n",
			       dev->name);
		}
		ap->jumbo = 0;
	}

	return 0;
}


static int ace_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
#ifdef ETHTOOL
	struct ace_private *ap = (struct ace_private *) dev->priv;
	struct ace_regs *regs = ap->regs;
	struct ethtool_cmd ecmd;
	u32 link, speed;

	if (cmd != SIOCETHTOOL)
		return -EOPNOTSUPP;
	if (copy_from_user(&ecmd, ifr->ifr_data, sizeof(ecmd)))
		return -EFAULT;

	if (ecmd.cmd == ETH_GSET) {
		ecmd.supported =
			(SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
			 SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
			 SUPPORTED_1000baseT_Half | SUPPORTED_1000baseT_Full |
			 SUPPORTED_Autoneg | SUPPORTED_FIBRE);

		ecmd.port = PORT_FIBRE;
		ecmd.transceiver = XCVR_INTERNAL;
		ecmd.phy_address = 0;

		link = readl(&regs->GigLnkState);
		if (link & LNK_1000MB)
			ecmd.speed = SPEED_1000;
		else {
			link = readl(&regs->FastLnkState);
			if (link & LNK_100MB)
				ecmd.speed = SPEED_100;
			else if (link & LNK_100MB)
				ecmd.speed = SPEED_10;
			else
				ecmd.speed = 0;
		}
		if (link & LNK_FULL_DUPLEX)
			ecmd.duplex = DUPLEX_FULL;
		else
			ecmd.duplex = DUPLEX_HALF;

		if (link & LNK_NEGOTIATE)
			ecmd.autoneg = AUTONEG_ENABLE;
		else
			ecmd.autoneg = AUTONEG_DISABLE;

		ecmd.trace = readl(&regs->TuneTrace);

		ecmd.txcoal = readl(&regs->TuneTxCoalTicks);
		ecmd.rxcoal = readl(&regs->TuneRxCoalTicks);
		ecmd.maxtxpkt = readl(&regs->TuneMaxTxDesc);
		ecmd.maxrxpkt = readl(&regs->TuneMaxRxDesc);

		if(copy_to_user(ifr->ifr_data, &ecmd, sizeof(ecmd)))
			return -EFAULT;
		return 0;
	} else if (ecmd.cmd == ETH_SSET) {
		if(!capable(CAP_NET_ADMIN))
			return -EPERM;

		link = readl(&regs->GigLnkState);
		if (link & LNK_1000MB)
			speed = SPEED_1000;
		else {
			link = readl(&regs->FastLnkState);
			if (link & LNK_100MB)
				speed = SPEED_100;
			else if (link & LNK_100MB)
				speed = SPEED_10;
			else
				speed = SPEED_100;
		}

		link = LNK_ENABLE | LNK_1000MB | LNK_100MB | LNK_10MB |
			LNK_RX_FLOW_CTL_Y | LNK_NEG_FCTL;
		if (!ACE_IS_TIGON_I(ap))
			link |= LNK_TX_FLOW_CTL_Y;
		if (ecmd.autoneg == AUTONEG_ENABLE)
			link |= LNK_NEGOTIATE;
		if (ecmd.speed != speed) {
			link &= ~(LNK_1000MB | LNK_100MB | LNK_10MB);
			switch (speed) {
			case SPEED_1000:
				link |= LNK_1000MB;
				break;
			case SPEED_100:
				link |= LNK_100MB;
				break;
			case SPEED_10:
				link |= LNK_10MB;
				break;
			}
		}
		if (ecmd.duplex == DUPLEX_FULL)
			link |= LNK_FULL_DUPLEX;

		if (link != ap->link) {
			struct cmd cmd;
			printk(KERN_INFO "%s: Renegotiating link state\n",
			       dev->name);

			ap->link = link;
			writel(link, &regs->TuneLink);
			if (!ACE_IS_TIGON_I(ap))
				writel(link, &regs->TuneFastLink);
			wmb();

			cmd.evt = C_LNK_NEGOTIATION;
			cmd.code = 0;
			cmd.idx = 0;
			ace_issue_cmd(regs, &cmd);
		}
		return 0;
	}
#endif

	return -EOPNOTSUPP;
}


/*
 * Set the hardware MAC address.
 */
static int ace_set_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr=p;
	struct ace_regs *regs;
	u16 *da;
	struct cmd cmd;

	if(dev->start)
		return -EBUSY;

	memcpy(dev->dev_addr, addr->sa_data,dev->addr_len);

	da = (u16 *)dev->dev_addr;

	regs = ((struct ace_private *)dev->priv)->regs;
	writel(da[0], &regs->MacAddrHi);
	writel((da[1] << 16) | da[2], &regs->MacAddrLo);

	cmd.evt = C_SET_MAC_ADDR;
	cmd.code = 0;
	cmd.idx = 0;
	ace_issue_cmd(regs, &cmd);

	return 0;
}


static void ace_set_multicast_list(struct net_device *dev)
{
	struct ace_private *ap = dev->priv;
	struct ace_regs *regs = ap->regs;
	struct cmd cmd;

	if ((dev->flags & IFF_ALLMULTI) && !(ap->mcast_all)) {
		cmd.evt = C_SET_MULTICAST_MODE;
		cmd.code = C_C_MCAST_ENABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
		ap->mcast_all = 1;
	} else if (ap->mcast_all){
		cmd.evt = C_SET_MULTICAST_MODE;
		cmd.code = C_C_MCAST_ENABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
		ap->mcast_all = 0;
	}

	if ((dev->flags & IFF_PROMISC) && !(ap->promisc)) {
		cmd.evt = C_SET_PROMISC_MODE;
		cmd.code = C_C_PROMISC_ENABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
		ap->promisc = 1;
	}else if (!(dev->flags & IFF_PROMISC) && (ap->promisc)){
		cmd.evt = C_SET_PROMISC_MODE;
		cmd.code = C_C_PROMISC_DISABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
		ap->promisc = 0;
	}

	/*
	 * For the time being multicast relies on the upper layers
	 * filtering it properly. The Firmware does not allow one to
	 * set the entire multicast list at a time and keeping track of
	 * it here is going to be messy.
	 */
	if ((dev->mc_count) && !(ap->mcast_all)) {
		cmd.evt = C_SET_MULTICAST_MODE;
		cmd.code = C_C_MCAST_ENABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
	}else if (!ap->mcast_all) {
		cmd.evt = C_SET_MULTICAST_MODE;
		cmd.code = C_C_MCAST_DISABLE;
		cmd.idx = 0;
		ace_issue_cmd(regs, &cmd);
	}
}


static struct net_device_stats *ace_get_stats(struct net_device *dev)
{
	struct ace_private *ap = dev->priv;

	return(&ap->stats);
}


void __init ace_copy(struct ace_regs *regs, void *src, u32 dest, int size)
{
	unsigned long tdest;
	u32 *wsrc;
	short tsize, i;

	if (size <= 0)
		return;

	while (size > 0){
		tsize = min(((~dest & (ACE_WINDOW_SIZE - 1)) + 1),
			    min(size, ACE_WINDOW_SIZE));
		tdest = (unsigned long)&regs->Window +
			(dest & (ACE_WINDOW_SIZE - 1));
		writel(dest & ~(ACE_WINDOW_SIZE - 1), &regs->WinBase);
#ifdef __BIG_ENDIAN
#error "data must be swapped here"
#else
		wsrc = src;
		for (i = 0; i < (tsize / 4); i++){
			writel(wsrc[i], tdest + i*4);
		}
#endif
		dest += tsize;
		src += tsize;
		size -= tsize;
	}

	return;
}


void __init ace_clear(struct ace_regs *regs, u32 dest, int size)
{
	unsigned long tdest;
	short tsize = 0, i;

	if (size <= 0)
		return;

	while (size > 0){
		tsize = min(((~dest & (ACE_WINDOW_SIZE - 1)) + 1),
			    min(size, ACE_WINDOW_SIZE));
		tdest = (unsigned long)&regs->Window +
			(dest & (ACE_WINDOW_SIZE - 1));
		writel(dest & ~(ACE_WINDOW_SIZE - 1), &regs->WinBase);

		for (i = 0; i < (tsize / 4); i++){
			writel(0, tdest + i*4);
		}

		dest += tsize;
		size -= tsize;
	}

	return;
}


/*
 * Download the firmware into the SRAM on the NIC
 *
 * This operation requires the NIC to be halted and is performed with
 * interrupts disabled and with the spinlock hold.
 */
int __init ace_load_firmware(struct net_device *dev)
{
	struct ace_private *ap;
	struct ace_regs *regs;

	ap = (struct ace_private *)dev->priv;
	regs = ap->regs;

	if (!(readl(&regs->CpuCtrl) & CPU_HALTED)){
		printk(KERN_ERR "%s: trying to download firmware while the "
		       "CPU is running!\n", dev->name);
		return -EFAULT;
	}

	/*
	 * Do not try to clear more than 512KB or we end up seeing
	 * funny things on NICs with only 512KB SRAM
	 */
	ace_clear(regs, 0x2000, 0x80000-0x2000);
	if (ACE_IS_TIGON_I(ap)){
		ace_copy(regs, tigonFwText, tigonFwTextAddr, tigonFwTextLen);
		ace_copy(regs, tigonFwData, tigonFwDataAddr, tigonFwDataLen);
		ace_copy(regs, tigonFwRodata, tigonFwRodataAddr,
			 tigonFwRodataLen);
		ace_clear(regs, tigonFwBssAddr, tigonFwBssLen);
		ace_clear(regs, tigonFwSbssAddr, tigonFwSbssLen);
	}else if (ap->version == 2){
		ace_clear(regs, tigon2FwBssAddr, tigon2FwBssLen);
		ace_clear(regs, tigon2FwSbssAddr, tigon2FwSbssLen);
		ace_copy(regs, tigon2FwText, tigon2FwTextAddr,tigon2FwTextLen);
		ace_copy(regs, tigon2FwRodata, tigon2FwRodataAddr,
			 tigon2FwRodataLen);
		ace_copy(regs, tigon2FwData, tigon2FwDataAddr,tigon2FwDataLen);
	}

	return 0;
}


/*
 * The eeprom on the AceNIC is an Atmel i2c EEPROM.
 *
 * Accessing the EEPROM is `interesting' to say the least - don't read
 * this code right after dinner.
 *
 * This is all about black magic and bit-banging the device .... I
 * wonder in what hospital they have put the guy who designed the i2c
 * specs.
 *
 * Oh yes, this is only the beginning!
 */
static void __init eeprom_start(struct ace_regs *regs)
{
	u32 local = readl(&regs->LocalCtrl);

	udelay(1);
	local |= EEPROM_DATA_OUT | EEPROM_WRITE_ENABLE;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	local |= EEPROM_CLK_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	local &= ~EEPROM_DATA_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	local &= ~EEPROM_CLK_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
}


static void __init eeprom_prep(struct ace_regs *regs, u8 magic)
{
	short i;
	u32 local;

	udelay(2);
	local = readl(&regs->LocalCtrl);
	local &= ~EEPROM_DATA_OUT;
	local |= EEPROM_WRITE_ENABLE;
	writel(local, &regs->LocalCtrl);
	mb();

	for (i = 0; i < 8; i++, magic <<= 1) {
		udelay(2);
		if (magic & 0x80) 
			local |= EEPROM_DATA_OUT;
		else
			local &= ~EEPROM_DATA_OUT;
		writel(local, &regs->LocalCtrl);
		mb();

		udelay(1);
		local |= EEPROM_CLK_OUT;
		writel(local, &regs->LocalCtrl);
		mb();
		udelay(1);
		local &= ~(EEPROM_CLK_OUT | EEPROM_DATA_OUT);
		writel(local, &regs->LocalCtrl);
		mb();
	}
}


static int __init eeprom_check_ack(struct ace_regs *regs)
{
	int state;
	u32 local;

	local = readl(&regs->LocalCtrl);
	local &= ~EEPROM_WRITE_ENABLE;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(2);
	local |= EEPROM_CLK_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	/* sample data in middle of high clk */
	state = (readl(&regs->LocalCtrl) & EEPROM_DATA_IN) != 0;
	udelay(1);
	mb();
	writel(readl(&regs->LocalCtrl) & ~EEPROM_CLK_OUT, &regs->LocalCtrl);
	mb();

	return state;
}


static void __init eeprom_stop(struct ace_regs *regs)
{
	u32 local;

	local = readl(&regs->LocalCtrl);
	local |= EEPROM_WRITE_ENABLE;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	local &= ~EEPROM_DATA_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	local |= EEPROM_CLK_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(1);
	local |= EEPROM_DATA_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
	udelay(2);
	local &= ~EEPROM_CLK_OUT;
	writel(local, &regs->LocalCtrl);
	mb();
}


/*
 * Read a whole byte from the EEPROM.
 */
static u8 __init read_eeprom_byte(struct ace_regs *regs, unsigned long offset)
{
	u32 local;
	short i;
	u8 result = 0;

	if (!regs){
		printk(KERN_ERR "No regs!\n");
		return 0;
	}

	eeprom_start(regs);

	eeprom_prep(regs, EEPROM_WRITE_SELECT);
	if (eeprom_check_ack(regs)){
		printk("Unable to sync eeprom\n");
		return 0;
	}

	eeprom_prep(regs, (offset >> 8) & 0xff);
	if (eeprom_check_ack(regs))
		return 0;

	eeprom_prep(regs, offset & 0xff);
	if (eeprom_check_ack(regs))
		return 0;

	eeprom_start(regs);
	eeprom_prep(regs, EEPROM_READ_SELECT);
	if (eeprom_check_ack(regs))
		return 0;

	for (i = 0; i < 8; i++) {
		local = readl(&regs->LocalCtrl);
		local &= ~EEPROM_WRITE_ENABLE;
		writel(local, &regs->LocalCtrl);
		udelay(2);
		mb();
		local |= EEPROM_CLK_OUT;
		writel(local, &regs->LocalCtrl);
		udelay(1);
		mb();
		/* sample data mid high clk */
		result = (result << 1) |
			((readl(&regs->LocalCtrl) & EEPROM_DATA_IN) != 0);
		udelay(1);
		mb();
		local = readl(&regs->LocalCtrl);
		local &= ~EEPROM_CLK_OUT;
		writel(local, &regs->LocalCtrl);
		mb();
		if (i == 7){
			local |= EEPROM_WRITE_ENABLE;
			writel(local, &regs->LocalCtrl);
			mb();
		}
	}

	local |= EEPROM_DATA_OUT;
	writel(local, &regs->LocalCtrl);
	udelay(1);
	writel(readl(&regs->LocalCtrl) | EEPROM_CLK_OUT, &regs->LocalCtrl);
	udelay(2);
	writel(readl(&regs->LocalCtrl) & ~EEPROM_CLK_OUT, &regs->LocalCtrl);
	eeprom_stop(regs);

	return result;
}


/*
 * Local variables:
 * compile-command: "gcc -D__SMP__ -D__KERNEL__ -DMODULE -I../../include -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -fno-strength-reduce -DMODVERSIONS -include ../../include/linux/modversions.h   -c -o acenic.o acenic.c"
 * End:
 */
