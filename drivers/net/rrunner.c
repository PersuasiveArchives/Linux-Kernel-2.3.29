/*
 * rrunner.c: Linux driver for the Essential RoadRunner HIPPI board.
 *
 * Written 1998 by Jes Sorensen, <Jes.Sorensen@cern.ch>.
 *
 * Thanks to Essential Communication for providing us with hardware
 * and very comprehensive documentation without which I would not have
 * been able to write this driver. A special thank you to John Gibbon
 * for sorting out the legal issues, with the NDA, allowing the code to
 * be released under the GPL.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define DEBUG 1
#define RX_DMA_SKBUFF 1
#define PKT_COPY_THRESHOLD 512

#include <linux/module.h>

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/hippidevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <net/sock.h>

#include <asm/system.h>
#include <asm/cache.h>
#include <asm/byteorder.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

#include "rrunner.h"


/*
 * Implementation notes:
 *
 * The DMA engine only allows for DMA within physical 64KB chunks of
 * memory. The current approach of the driver (and stack) is to use
 * linear blocks of memory for the skbuffs. However, as the data block
 * is always the first part of the skb and skbs are 2^n aligned so we
 * are guarantted to get the whole block within one 64KB align 64KB
 * chunk.
 *
 * On the long term, relying on being able to allocate 64KB linear
 * chunks of memory is not feasible and the skb handling code and the
 * stack will need to know about I/O vectors or something similar.
 */

static const char __initdata *version = "rrunner.c: v0.17 03/09/99  Jes Sorensen (Jes.Sorensen@cern.ch)\n";


/*
 * These are checked at init time to see if they are at least 256KB
 * and increased to 256KB if they are not. This is done to avoid ending
 * up with socket buffers smaller than the MTU size,
 */
extern __u32 sysctl_wmem_max;
extern __u32 sysctl_rmem_max;

static int probed __initdata = 0;

int __init rr_hippi_probe (void)
{
	int boards_found = 0;
	int version_disp;	/* was version info already displayed? */
	struct net_device *dev;
	struct pci_dev *pdev = NULL;
	struct pci_dev *opdev = NULL;
	u8 pci_latency;
	struct rr_private *rrpriv;

	if (probed)
		return -ENODEV;
	probed++;

	if (!pci_present())		/* is PCI BIOS even present? */
		return -ENODEV;

	version_disp = 0;

	while((pdev = pci_find_device(PCI_VENDOR_ID_ESSENTIAL,
				      PCI_DEVICE_ID_ESSENTIAL_ROADRUNNER,
				      pdev)))
	{
		if (pdev == opdev)
			return 0;

		/*
		 * So we found our HIPPI ... time to tell the system.
		 */

		dev = init_hippi_dev(NULL, sizeof(struct rr_private));

		if (!dev)
			break;

		if (!dev->priv)
			dev->priv = kmalloc(sizeof(*rrpriv), GFP_KERNEL);

		if (!dev->priv)
			return -ENOMEM;

		rrpriv = (struct rr_private *)dev->priv;
		memset(rrpriv, 0, sizeof(*rrpriv));

#ifdef __SMP__
		spin_lock_init(&rrpriv->lock);
#endif
		sprintf(rrpriv->name, "RoadRunner serial HIPPI");

		dev->irq = pdev->irq;
		dev->open = &rr_open;
		dev->hard_start_xmit = &rr_start_xmit;
		dev->stop = &rr_close;
		dev->get_stats = &rr_get_stats;
		dev->do_ioctl = &rr_ioctl;

		/*
		 * Dummy value.
		 */
		dev->base_addr = 42;

		/* display version info if adapter is found */
		if (!version_disp)
		{
			/* set display flag to TRUE so that */
			/* we only display this string ONCE */
			version_disp = 1;
			printk(version);
		}

		pci_read_config_byte(pdev, PCI_LATENCY_TIMER, &pci_latency);
		if (pci_latency <= 0x58){
			pci_latency = 0x58;
			pci_write_config_byte(pdev, PCI_LATENCY_TIMER,
					      pci_latency);
		}

		pci_set_master(pdev);

		printk(KERN_INFO "%s: Essential RoadRunner serial HIPPI "
		       "at 0x%08lx, irq %i, PCI latency %i\n", dev->name,
		       pdev->resource[0].start, dev->irq, pci_latency);

		/*
		 * Remap the regs into kernel space.
		 */

		rrpriv->regs = (struct rr_regs *)
			ioremap(pdev->resource[0].start, 0x1000);

		if (!rrpriv->regs){
			printk(KERN_ERR "%s:  Unable to map I/O register, "
			       "RoadRunner %i will be disabled.\n",
			       dev->name, boards_found);
			break;
		}

		/*
		 * Don't access any registes before this point!
		 */
#ifdef __BIG_ENDIAN
		writel(readl(&regs->HostCtrl) | NO_SWAP, &regs->HostCtrl);
#endif
		/*
		 * Need to add a case for little-endian 64-bit hosts here.
		 */

		rr_init(dev);

		boards_found++;
		dev->base_addr = 0;
		dev = NULL;
		opdev = pdev;
	}

	/*
	 * If we're at this point we're going through rr_hippi_probe()
	 * for the first time.  Return success (0) if we've initialized
	 * 1 or more boards. Otherwise, return failure (-ENODEV).
	 */

	return boards_found;
}

static struct net_device *root_dev = NULL;

#ifdef MODULE
#if LINUX_VERSION_CODE > 0x20118
MODULE_AUTHOR("Jes Sorensen <Jes.Sorensen@cern.ch>");
MODULE_DESCRIPTION("Essential RoadRunner HIPPI driver");
#endif


int init_module(void)
{
	return rr_hippi_probe()? 0 : -ENODEV;
}

void cleanup_module(void)
{
	struct rr_private *rr;
	struct net_device *next;

	while (root_dev) {
		next = ((struct rr_private *)root_dev->priv)->next;
		rr = (struct rr_private *)root_dev->priv;

		if (!(readl(&rr->regs->HostCtrl) & NIC_HALTED)){
			printk(KERN_ERR "%s: trying to unload running NIC\n",
			       root_dev->name);
			writel(HALT_NIC, &rr->regs->HostCtrl);
		}

		iounmap(rr->regs);
		unregister_hipdev(root_dev);
		kfree(root_dev);

		root_dev = next;
	}
}
#endif


/*
 * Commands are considered to be slow, thus there is no reason to
 * inline this.
 */
static void rr_issue_cmd(struct rr_private *rrpriv, struct cmd *cmd)
{
	struct rr_regs *regs;
	u32 idx;

	regs = rrpriv->regs;
	/*
	 * This is temporary - it will go away in the final version.
	 * We probably also want to make this function inline.
	 */
	if (readl(&regs->HostCtrl) & NIC_HALTED){
		printk("issuing command for halted NIC, code 0x%x, "
		       "HostCtrl %08x\n", cmd->code, readl(&regs->HostCtrl));
		if (readl(&regs->Mode) & FATAL_ERR)
			printk("error codes Fail1 %02x, Fail2 %02x\n",
			       readl(&regs->Fail1), readl(&regs->Fail2));
	}

	idx = rrpriv->info->cmd_ctrl.pi;

	writel(*(u32*)(cmd), &regs->CmdRing[idx]);
	mb();

	idx = (idx - 1) % CMD_RING_ENTRIES;
	rrpriv->info->cmd_ctrl.pi = idx;
	mb();

	if (readl(&regs->Mode) & FATAL_ERR)
		printk("error code %02x\n", readl(&regs->Fail1));
}


/*
 * Reset the board in a sensible manner. The NIC is already halted
 * when we get here and a spin-lock is held.
 */
static int rr_reset(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	struct eeprom *hw = NULL;
	u32 start_pc;
	int i;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	rr_load_firmware(dev);

	writel(0x01000000, &regs->TX_state);
	writel(0xff800000, &regs->RX_state);
	writel(0, &regs->AssistState);
	writel(CLEAR_INTA, &regs->LocalCtrl);
	writel(0x01, &regs->BrkPt);
	writel(0, &regs->Timer);
	writel(0, &regs->TimerRef);
	writel(RESET_DMA, &regs->DmaReadState);
	writel(RESET_DMA, &regs->DmaWriteState);
	writel(0, &regs->DmaWriteHostHi);
	writel(0, &regs->DmaWriteHostLo);
	writel(0, &regs->DmaReadHostHi);
	writel(0, &regs->DmaReadHostLo);
	writel(0, &regs->DmaReadLen);
	writel(0, &regs->DmaWriteLen);
	writel(0, &regs->DmaWriteLcl);
	writel(0, &regs->DmaWriteIPchecksum);
	writel(0, &regs->DmaReadLcl);
	writel(0, &regs->DmaReadIPchecksum);
	writel(0, &regs->PciState);
#if (BITS_PER_LONG == 64) && defined __LITTLE_ENDIAN
	writel(SWAP_DATA | PTR64BIT | PTR_WD_SWAP, &regs->Mode);
#elif (BITS_PER_LONG == 64)
	writel(SWAP_DATA | PTR64BIT | PTR_WD_NOSWAP, &regs->Mode);
#else
	writel(SWAP_DATA | PTR32BIT | PTR_WD_NOSWAP, &regs->Mode);
#endif

#if 0
	/*
	 * Don't worry, this is just black magic.
	 */
	writel(0xdf000, &regs->RxBase);
	writel(0xdf000, &regs->RxPrd);
	writel(0xdf000, &regs->RxCon);
	writel(0xce000, &regs->TxBase);
	writel(0xce000, &regs->TxPrd);
	writel(0xce000, &regs->TxCon);
	writel(0, &regs->RxIndPro);
	writel(0, &regs->RxIndCon);
	writel(0, &regs->RxIndRef);
	writel(0, &regs->TxIndPro);
	writel(0, &regs->TxIndCon);
	writel(0, &regs->TxIndRef);
	writel(0xcc000, &regs->pad10[0]);
	writel(0, &regs->DrCmndPro);
	writel(0, &regs->DrCmndCon);
	writel(0, &regs->DwCmndPro);
	writel(0, &regs->DwCmndCon);
	writel(0, &regs->DwCmndRef);
	writel(0, &regs->DrDataPro);
	writel(0, &regs->DrDataCon);
	writel(0, &regs->DrDataRef);
	writel(0, &regs->DwDataPro);
	writel(0, &regs->DwDataCon);
	writel(0, &regs->DwDataRef);
#endif

	writel(0xffffffff, &regs->MbEvent);
	writel(0, &regs->Event);

	writel(0, &regs->TxPi);
	writel(0, &regs->IpRxPi);

	writel(0, &regs->EvtCon);
	writel(0, &regs->EvtPrd);

	rrpriv->info->evt_ctrl.pi = 0;

	for (i = 0; i < CMD_RING_ENTRIES; i++)
		writel(0, &regs->CmdRing[i]);

/*
 * Why 32 ? is this not cache line size dependant?
 */
	writel(WBURST_32, &regs->PciState);
	mb();

	start_pc = rr_read_eeprom_word(rrpriv, &hw->rncd_info.FwStart);

#if (DEBUG > 1)
	printk("%s: Executing firmware at address 0x%06x\n",
	       dev->name, start_pc);
#endif

	writel(start_pc + 0x800, &regs->Pc);
	mb();
	udelay(5);

	writel(start_pc, &regs->Pc);
	mb();

	return 0;
}


/*
 * Read a string from the EEPROM.
 */
static unsigned int rr_read_eeprom(struct rr_private *rrpriv,
				unsigned long offset,
				unsigned char *buf,
				unsigned long length)
{
	struct rr_regs *regs = rrpriv->regs;
	u32 misc, io, host, i;

	io = readl(&regs->ExtIo);
	writel(0, &regs->ExtIo);
	misc = readl(&regs->LocalCtrl);
	writel(0, &regs->LocalCtrl);
	host = readl(&regs->HostCtrl);
	writel(host | HALT_NIC, &regs->HostCtrl);
	mb();

	for (i = 0; i < length; i++){
		writel((EEPROM_BASE + ((offset+i) << 3)), &regs->WinBase);
		mb();
		buf[i] = (readl(&regs->WinData) >> 24) & 0xff;
		mb();
	}

	writel(host, &regs->HostCtrl);
	writel(misc, &regs->LocalCtrl);
	writel(io, &regs->ExtIo);
	mb();
	return i;
}


/*
 * Shortcut to read one word (4 bytes) out of the EEPROM and convert
 * it to our CPU byte-order.
 */
static u32 rr_read_eeprom_word(struct rr_private *rrpriv,
			    void * offset)
{
	u32 word;

	if ((rr_read_eeprom(rrpriv, (unsigned long)offset,
			    (char *)&word, 4) == 4))
		return be32_to_cpu(word);
	return 0;
}


/*
 * Write a string to the EEPROM.
 *
 * This is only called when the firmware is not running.
 */
static unsigned int write_eeprom(struct rr_private *rrpriv,
				 unsigned long offset,
				 unsigned char *buf,
				 unsigned long length)
{
	struct rr_regs *regs = rrpriv->regs;
	u32 misc, io, data, i, j, ready, error = 0;

	io = readl(&regs->ExtIo);
	writel(0, &regs->ExtIo);
	misc = readl(&regs->LocalCtrl);
	writel(ENABLE_EEPROM_WRITE, &regs->LocalCtrl);
	mb();

	for (i = 0; i < length; i++){
		writel((EEPROM_BASE + ((offset+i) << 3)), &regs->WinBase);
		mb();
		data = buf[i] << 24;
		/*
		 * Only try to write the data if it is not the same
		 * value already.
		 */
		if ((readl(&regs->WinData) & 0xff000000) != data){
			writel(data, &regs->WinData);
			ready = 0;
			j = 0;
			mb();
			while(!ready){
				udelay(20);
				if ((readl(&regs->WinData) & 0xff000000) ==
				    data)
					ready = 1;
				mb();
				if (j++ > 5000){
					printk("data mismatch: %08x, "
					       "WinData %08x\n", data,
					       readl(&regs->WinData));
					ready = 1;
					error = 1;
				}
			}
		}
	}

	writel(misc, &regs->LocalCtrl);
	writel(io, &regs->ExtIo);
	mb();

	return error;
}


static int __init rr_init(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	u32 sram_size, rev;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	rev = readl(&regs->FwRev);
	rrpriv->fw_rev = rev;
	if (rev > 0x00020024)
		printk("  Firmware revision: %i.%i.%i\n", (rev >> 16),
		       ((rev >> 8) & 0xff), (rev & 0xff));
	else if (rev >= 0x00020000) {
		printk("  Firmware revision: %i.%i.%i (2.0.37 or "
		       "later is recommended)\n", (rev >> 16),
		       ((rev >> 8) & 0xff), (rev & 0xff));
	}else{
		printk("  Firmware revision too old: %i.%i.%i, please "
		       "upgrade to 2.0.37 or later.\n",
		       (rev >> 16), ((rev >> 8) & 0xff), (rev & 0xff));
	}

#if (DEBUG > 2)
	printk("  Maximum receive rings %i\n", readl(&regs->MaxRxRng));
#endif

	sram_size = rr_read_eeprom_word(rrpriv, (void *)8);
	printk("  SRAM size 0x%06x\n", sram_size);

	if (sysctl_rmem_max < 262144){
		printk("  Receive socket buffer limit too low (%i), "
		       "setting to 262144\n", sysctl_rmem_max);
		sysctl_rmem_max = 262144;
	}

	if (sysctl_wmem_max < 262144){
		printk("  Transmit socket buffer limit too low (%i), "
		       "setting to 262144\n", sysctl_wmem_max);
		sysctl_wmem_max = 262144;
	}

	rrpriv->next = root_dev;
	root_dev = dev;

	return 0;
}


static int rr_init1(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	u32 hostctrl;
	unsigned long myjif, flags;
	struct cmd cmd;
	short i;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	spin_lock_irqsave(&rrpriv->lock, flags);

	hostctrl = readl(&regs->HostCtrl);
	writel(hostctrl | HALT_NIC | RR_CLEAR_INT, &regs->HostCtrl);
	mb();

	if (hostctrl & PARITY_ERR){
		printk("%s: Parity error halting NIC - this is serious!\n",
		       dev->name);
		spin_unlock_irqrestore(&rrpriv->lock, flags);
		return -EFAULT;
	}

	set_rxaddr(regs, rrpriv->rx_ctrl);
	set_infoaddr(regs, rrpriv->info);

	rrpriv->info->evt_ctrl.entry_size = sizeof(struct event);
	rrpriv->info->evt_ctrl.entries = EVT_RING_ENTRIES;
	rrpriv->info->evt_ctrl.mode = 0;
	rrpriv->info->evt_ctrl.pi = 0;
	set_rraddr(&rrpriv->info->evt_ctrl.rngptr, rrpriv->evt_ring);

	rrpriv->info->cmd_ctrl.entry_size = sizeof(struct cmd);
	rrpriv->info->cmd_ctrl.entries = CMD_RING_ENTRIES;
	rrpriv->info->cmd_ctrl.mode = 0;
	rrpriv->info->cmd_ctrl.pi = 15;

	for (i = 0; i < CMD_RING_ENTRIES; i++) {
		writel(0, &regs->CmdRing[i]);
	}

	for (i = 0; i < TX_RING_ENTRIES; i++) {
		rrpriv->tx_ring[i].size = 0;
		set_rraddr(&rrpriv->tx_ring[i].addr, 0);
		rrpriv->tx_skbuff[i] = 0;
	}
	rrpriv->info->tx_ctrl.entry_size = sizeof(struct tx_desc);
	rrpriv->info->tx_ctrl.entries = TX_RING_ENTRIES;
	rrpriv->info->tx_ctrl.mode = 0;
	rrpriv->info->tx_ctrl.pi = 0;
	set_rraddr(&rrpriv->info->tx_ctrl.rngptr, rrpriv->tx_ring);

	/*
	 * Set dirty_tx before we start receiving interrupts, otherwise
	 * the interrupt handler might think it is supposed to process
	 * tx ints before we are up and running, which may cause a null
	 * pointer access in the int handler.
	 */
	rrpriv->tx_full = 0;
	rrpriv->cur_rx = 0;
	rrpriv->dirty_rx = rrpriv->dirty_tx = 0;

	rr_reset(dev);

	writel(0x60, &regs->IntrTmr);
	/*
	 * These seem to have no real effect as the Firmware sets
	 * it's own default values
	 */
	writel(0x10, &regs->WriteDmaThresh);
	writel(0x20, &regs->ReadDmaThresh);

	rrpriv->fw_running = 0;
	mb();

	hostctrl &= ~(HALT_NIC | INVALID_INST_B | PARITY_ERR);
	writel(hostctrl, &regs->HostCtrl);
	mb();

	spin_unlock_irqrestore(&rrpriv->lock, flags);

	udelay(1000);

	/*
	 * Now start the FirmWare.
	 */
	cmd.code = C_START_FW;
	cmd.ring = 0;
	cmd.index = 0;

	rr_issue_cmd(rrpriv, &cmd);

	/*
	 * Give the FirmWare time to chew on the `get running' command.
	 */
	myjif = jiffies + 5 * HZ;
	while ((jiffies < myjif) && !rrpriv->fw_running);

	for (i = 0; i < RX_RING_ENTRIES; i++) {
		struct sk_buff *skb;

		rrpriv->rx_ring[i].mode = 0;
		skb = alloc_skb(dev->mtu + HIPPI_HLEN, GFP_ATOMIC);
		rrpriv->rx_skbuff[i] = skb;
		/*
		 * Sanity test to see if we conflict with the DMA
		 * limitations of the Roadrunner.
		 */
		if ((((unsigned long)skb->data) & 0xfff) > ~65320)
			printk("skb alloc error\n");

		set_rraddr(&rrpriv->rx_ring[i].addr, skb->data);
		rrpriv->rx_ring[i].size = dev->mtu + HIPPI_HLEN;
	}

	rrpriv->rx_ctrl[4].entry_size = sizeof(struct rx_desc);
	rrpriv->rx_ctrl[4].entries = RX_RING_ENTRIES;
	rrpriv->rx_ctrl[4].mode = 8;
	rrpriv->rx_ctrl[4].pi = 0;
	mb();
	set_rraddr(&rrpriv->rx_ctrl[4].rngptr, rrpriv->rx_ring);

	cmd.code = C_NEW_RNG;
	cmd.ring = 4;
	cmd.index = 0;
	rr_issue_cmd(rrpriv, &cmd);

#if 0
{
	u32 tmp;
	tmp = readl(&regs->ExtIo);
	writel(0x80, &regs->ExtIo);
	
	i = jiffies + 1 * HZ;
	while (jiffies < i);
	writel(tmp, &regs->ExtIo);
}
#endif
	dev->tbusy = 0;
	dev->start = 1;
	return 0;
}


/*
 * All events are considered to be slow (RX/TX ints do not generate
 * events) and are handled here, outside the main interrupt handler,
 * to reduce the size of the handler.
 */
static u32 rr_handle_event(struct net_device *dev, u32 prodidx, u32 eidx)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	u32 tmp;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	while (prodidx != eidx){
		switch (rrpriv->evt_ring[eidx].code){
		case E_NIC_UP:
			tmp = readl(&regs->FwRev);
			printk("%s: Firmware revision %i.%i.%i up and running\n",
			       dev->name, (tmp >> 16), ((tmp >> 8) & 0xff),
			       (tmp & 0xff));
			rrpriv->fw_running = 1;
			mb();
			break;
		case E_LINK_ON:
			printk("%s: Optical link ON\n", dev->name);
			break;
		case E_LINK_OFF:
			printk("%s: Optical link OFF\n", dev->name);
			break;
		case E_RX_IDLE:
			printk("%s: RX data not moving\n", dev->name);
			break;
		case E_WATCHDOG:
			printk("%s: The watchdog is here to see us\n",
			       dev->name);
			break;
		/*
		 * TX events.
		 */
		case E_CON_REJ:
			printk("%s: Connection rejected\n", dev->name);
			rrpriv->stats.tx_aborted_errors++;
			break;
		case E_CON_TMOUT:
			printk("%s: Connection timeout\n", dev->name);
			break;
		case E_DISC_ERR:
			printk("%s: HIPPI disconnect error\n", dev->name);
			rrpriv->stats.tx_aborted_errors++;
			break;
		case E_TX_IDLE:
			printk("%s: Transmitter idle\n", dev->name);
			break;
		case E_TX_LINK_DROP:
			printk("%s: Link lost during transmit\n", dev->name);
			rrpriv->stats.tx_aborted_errors++;
			break;
		/*
		 * RX events.
		 */
		case E_VAL_RNG:		/* Should be ignored */
#if (DEBUG > 2)
			printk("%s: RX ring valid event\n", dev->name);
#endif
			writel(RX_RING_ENTRIES - 1, &regs->IpRxPi);
			break;
		case E_INV_RNG:
			printk("%s: RX ring invalid event\n", dev->name);
			break;
		case E_RX_RNG_OUT:
			printk("%s: Receive ring full\n", dev->name);
			break;

		case E_RX_PAR_ERR:
			printk("%s: Receive parity error.\n", dev->name);
			break;
		case E_RX_LLRC_ERR:
			printk("%s: Receive LLRC error.\n", dev->name);
			break;
		case E_PKT_LN_ERR:
			printk("%s: Receive packet length error.\n",
			       dev->name);
			break;
		default:
			printk("%s: Unhandled event 0x%02x\n",
			       dev->name, rrpriv->evt_ring[eidx].code);
		}
		eidx = (eidx + 1) % EVT_RING_ENTRIES;
	}

	rrpriv->info->evt_ctrl.pi = eidx;
	mb();
	return eidx;
}


static void rx_int(struct net_device *dev, u32 rxlimit, u32 index)
{
	struct rr_private *rrpriv = (struct rr_private *)dev->priv;
	u32 pkt_len;
	struct rr_regs *regs = rrpriv->regs;

	do {
		pkt_len = rrpriv->rx_ring[index].size;
#if (DEBUG > 2)
		printk("index %i, rxlimit %i\n", index, rxlimit);
		printk("len %x, mode %x\n", pkt_len,
		       rrpriv->rx_ring[index].mode);
#endif
		if (pkt_len > 0){
			struct sk_buff *skb;

			if (pkt_len < PKT_COPY_THRESHOLD) {
				skb = alloc_skb(pkt_len, GFP_ATOMIC);
				if (skb == NULL){
					printk("%s: Out of memory deferring "
					       "packet\n", dev->name);
					rrpriv->stats.rx_dropped++;
					goto defer;
				}else
					memcpy(skb_put(skb, pkt_len),
					       rrpriv->rx_skbuff[index]->data,
					       pkt_len);
			}else{
				struct sk_buff *newskb;

				newskb = alloc_skb(dev->mtu + HIPPI_HLEN,
						   GFP_ATOMIC);
				if (newskb){
					skb = rrpriv->rx_skbuff[index];
					skb_put(skb, pkt_len);
					rrpriv->rx_skbuff[index] = newskb;
					set_rraddr(&rrpriv->rx_ring[index].addr, newskb->data);
				}else{
					printk("%s: Out of memory, deferring "
					       "packet\n", dev->name);
					rrpriv->stats.rx_dropped++;
					goto defer;
				}
			}
			skb->dev = dev;
			skb->protocol = hippi_type_trans(skb, dev);

			netif_rx(skb);		/* send it up */

			rrpriv->stats.rx_packets++;
			rrpriv->stats.rx_bytes += skb->len;
		}
	defer:
		rrpriv->rx_ring[index].mode = 0;
		rrpriv->rx_ring[index].size = dev->mtu + HIPPI_HLEN;

		if ((index & 7) == 7)
			writel(index, &regs->IpRxPi);

		index = (index + 1) % RX_RING_ENTRIES;
	} while(index != rxlimit);

	rrpriv->cur_rx = index;
	mb();
}


static void rr_interrupt(int irq, void *dev_id, struct pt_regs *ptregs)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	struct net_device *dev = (struct net_device *)dev_id;
	u32 prodidx, rxindex, eidx, txcsmr, rxlimit, txcon;
	unsigned long flags;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	if (!(readl(&regs->HostCtrl) & RR_INT))
		return;

	spin_lock_irqsave(&rrpriv->lock, flags);

	prodidx = readl(&regs->EvtPrd);
	txcsmr = (prodidx >> 8) & 0xff;
	rxlimit = (prodidx >> 16) & 0xff;
	prodidx &= 0xff;

#if (DEBUG > 2)
	printk("%s: interrupt, prodidx = %i, eidx = %i\n", dev->name,
	       prodidx, rrpriv->info->evt_ctrl.pi);
#endif

	rxindex = rrpriv->cur_rx;
	if (rxindex != rxlimit)
		rx_int(dev, rxlimit, rxindex);

	txcon = rrpriv->dirty_tx;
	if (txcsmr != txcon) {
		do {
			rrpriv->stats.tx_packets++;
			rrpriv->stats.tx_bytes +=rrpriv->tx_skbuff[txcon]->len;
			dev_kfree_skb(rrpriv->tx_skbuff[txcon]);

			rrpriv->tx_skbuff[txcon] = NULL;
			rrpriv->tx_ring[txcon].size = 0;
			set_rraddr(&rrpriv->tx_ring[txcon].addr, 0);
			rrpriv->tx_ring[txcon].mode = 0;

			txcon = (txcon + 1) % TX_RING_ENTRIES;
		} while (txcsmr != txcon);
		mb();

		rrpriv->dirty_tx = txcon;
		if (rrpriv->tx_full && dev->tbusy &&
		    (((rrpriv->info->tx_ctrl.pi + 1) % TX_RING_ENTRIES)
		     != rrpriv->dirty_tx)){
			rrpriv->tx_full = 0;
			dev->tbusy = 0;
			mark_bh(NET_BH);
		}
	}

	eidx = rrpriv->info->evt_ctrl.pi;
	if (prodidx != eidx)
		eidx = rr_handle_event(dev, prodidx, eidx);

	eidx |= ((txcsmr << 8) | (rxlimit << 16));
	writel(eidx, &regs->EvtCon);
	mb();

	spin_unlock_irqrestore(&rrpriv->lock, flags);
}


static int rr_open(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	int ecode = 0;
	unsigned long flags;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	if (rrpriv->fw_rev < 0x00020000) {
		printk(KERN_WARNING "%s: trying to configure device with "
		       "obsolete firmware\n", dev->name);
		ecode = -EBUSY;
		goto error;
	}

	rrpriv->rx_ctrl = kmalloc(256*sizeof(struct ring_ctrl),
				  GFP_KERNEL | GFP_DMA);
	if (!rrpriv->rx_ctrl) {
		ecode = -ENOMEM;
		goto error;
	}

	rrpriv->info = kmalloc(sizeof(struct rr_info), GFP_KERNEL | GFP_DMA);
	if (!rrpriv->info){
		kfree(rrpriv->rx_ctrl);
		ecode = -ENOMEM;
		goto error;
	}
	memset(rrpriv->rx_ctrl, 0, 256 * sizeof(struct ring_ctrl));
	memset(rrpriv->info, 0, sizeof(struct rr_info));
	mb();

	spin_lock_irqsave(&rrpriv->lock, flags);
	writel(readl(&regs->HostCtrl)|HALT_NIC|RR_CLEAR_INT, &regs->HostCtrl);
	spin_unlock_irqrestore(&rrpriv->lock, flags);

	if (request_irq(dev->irq, rr_interrupt, SA_SHIRQ, rrpriv->name, dev))
	{
		printk(KERN_WARNING "%s: Requested IRQ %d is busy\n",
		       dev->name, dev->irq);
		ecode = -EAGAIN;
		goto error;
	}

	rr_init1(dev);

	dev->tbusy = 0;
	dev->start = 1;

	MOD_INC_USE_COUNT;
	return 0;

 error:
	spin_lock_irqsave(&rrpriv->lock, flags);
	writel(readl(&regs->HostCtrl)|HALT_NIC|RR_CLEAR_INT, &regs->HostCtrl);
	spin_unlock_irqrestore(&rrpriv->lock, flags);

	dev->tbusy = 1;
	dev->start = 0;
	return -ENOMEM;
}


static void rr_dump(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	u32 index, cons;
	short i;
	int len;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	printk("%s: dumping NIC TX rings\n", dev->name);

	printk("RxPrd %08x, TxPrd %02x, EvtPrd %08x, TxPi %02x, TxCtrlPi %02x\n",
	       readl(&regs->RxPrd), readl(&regs->TxPrd),
	       readl(&regs->EvtPrd), readl(&regs->TxPi),
	       rrpriv->info->tx_ctrl.pi);

	printk("Error code 0x%x\n", readl(&regs->Fail1));

	index = (((readl(&regs->EvtPrd) >> 8) & 0xff ) - 1) % EVT_RING_ENTRIES;
	cons = rrpriv->dirty_tx;
	printk("TX ring index %i, TX consumer %i\n",
	       index, cons);

	if (rrpriv->tx_skbuff[index]){
		len = min(0x80, rrpriv->tx_skbuff[index]->len);
		printk("skbuff for index %i is valid - dumping data (0x%x bytes - DMA len 0x%x)\n", index, len, rrpriv->tx_ring[index].size);
		for (i = 0; i < len; i++){
			if (!(i & 7))
				printk("\n");
			printk("%02x ", (unsigned char) rrpriv->tx_skbuff[index]->data[i]);
		}
		printk("\n");
	}

	if (rrpriv->tx_skbuff[cons]){
		len = min(0x80, rrpriv->tx_skbuff[cons]->len);
		printk("skbuff for cons %i is valid - dumping data (0x%x bytes - skbuff len 0x%x)\n", cons, len, rrpriv->tx_skbuff[cons]->len);
		printk("mode 0x%x, size 0x%x,\n phys %08x (virt %08lx), skbuff-addr %08lx, truesize 0x%x\n",
		       rrpriv->tx_ring[cons].mode,
		       rrpriv->tx_ring[cons].size,
		       rrpriv->tx_ring[cons].addr.addrlo,
		       (unsigned long)bus_to_virt(rrpriv->tx_ring[cons].addr.addrlo),
		       (unsigned long)rrpriv->tx_skbuff[cons]->data,
		       (unsigned int)rrpriv->tx_skbuff[cons]->truesize);
		for (i = 0; i < len; i++){
			if (!(i & 7))
				printk("\n");
			printk("%02x ", (unsigned char)rrpriv->tx_ring[cons].size);
		}
		printk("\n");
	}

	printk("dumping TX ring info:\n");
	for (i = 0; i < TX_RING_ENTRIES; i++)
		printk("mode 0x%x, size 0x%x, phys-addr %08x\n",
		       rrpriv->tx_ring[i].mode,
		       rrpriv->tx_ring[i].size,
		       rrpriv->tx_ring[i].addr.addrlo);

}


static int rr_close(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	u32 tmp;
	short i;

	dev->start = 0;
	set_bit(0, (void*)&dev->tbusy);

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	/*
	 * Lock to make sure we are not cleaning up while another CPU
	 * handling interrupts.
	 */
	spin_lock(&rrpriv->lock);

	tmp = readl(&regs->HostCtrl);
	if (tmp & NIC_HALTED){
		printk("%s: NIC already halted\n", dev->name);
		rr_dump(dev);
	}else{
		tmp |= HALT_NIC | RR_CLEAR_INT;
		writel(tmp, &regs->HostCtrl);
		mb();
	}

	rrpriv->fw_running = 0;

	writel(0, &regs->TxPi);
	writel(0, &regs->IpRxPi);

	writel(0, &regs->EvtCon);
	writel(0, &regs->EvtPrd);

	for (i = 0; i < CMD_RING_ENTRIES; i++)
		writel(0, &regs->CmdRing[i]);

	rrpriv->info->tx_ctrl.entries = 0;
	rrpriv->info->cmd_ctrl.pi = 0;
	rrpriv->info->evt_ctrl.pi = 0;
	rrpriv->rx_ctrl[4].entries = 0;

	for (i = 0; i < TX_RING_ENTRIES; i++) {
		if (rrpriv->tx_skbuff[i]) {
			rrpriv->tx_ring[i].size = 0;
			set_rraddr(&rrpriv->tx_ring[i].addr, 0);
			dev_kfree_skb(rrpriv->tx_skbuff[i]);
		}
	}

	for (i = 0; i < RX_RING_ENTRIES; i++) {
		if (rrpriv->rx_skbuff[i]) {
			rrpriv->rx_ring[i].size = 0;
			set_rraddr(&rrpriv->rx_ring[i].addr, 0);
			dev_kfree_skb(rrpriv->rx_skbuff[i]);
		}
	}

	kfree(rrpriv->rx_ctrl);
	kfree(rrpriv->info);

	free_irq(dev->irq, dev);
	spin_unlock(&rrpriv->lock);

	MOD_DEC_USE_COUNT;
	return 0;
}


static int rr_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct rr_private *rrpriv = (struct rr_private *)dev->priv;
	struct rr_regs *regs = rrpriv->regs;
	struct ring_ctrl *txctrl;
	unsigned long flags;
	u32 index, len = skb->len;
	u32 *ifield;
	struct sk_buff *new_skb;

	if (readl(&regs->Mode) & FATAL_ERR)
		printk("error codes Fail1 %02x, Fail2 %02x\n",
		       readl(&regs->Fail1), readl(&regs->Fail2));

	/*
	 * We probably need to deal with tbusy here to prevent overruns.
	 */

	if (skb_headroom(skb) < 8){
		printk("incoming skb too small - reallocating\n");
		if (!(new_skb = dev_alloc_skb(len + 8))) {
			dev_kfree_skb(skb);
			dev->tbusy = 0;
			return -EBUSY;
		}
		skb_reserve(new_skb, 8);
		skb_put(new_skb, len);
		memcpy(new_skb->data, skb->data, len);
		dev_kfree_skb(skb);
		skb = new_skb;
	}

	ifield = (u32 *)skb_push(skb, 8);

	ifield[0] = 0;
	ifield[1] = skb->private.ifield;

	/*
	 * We don't need the lock before we are actually going to start
	 * fiddling with the control blocks.
	 */
	spin_lock_irqsave(&rrpriv->lock, flags);

	txctrl = &rrpriv->info->tx_ctrl;

	index = txctrl->pi;

	rrpriv->tx_skbuff[index] = skb;
	set_rraddr(&rrpriv->tx_ring[index].addr, skb->data);
	rrpriv->tx_ring[index].size = len + 8; /* include IFIELD */
	rrpriv->tx_ring[index].mode = PACKET_START | PACKET_END;
	txctrl->pi = (index + 1) % TX_RING_ENTRIES;
	writel(txctrl->pi, &regs->TxPi);

	if (txctrl->pi == rrpriv->dirty_tx){
		rrpriv->tx_full = 1;
		set_bit(0, (void*)&dev->tbusy);
	}

	spin_unlock_irqrestore(&rrpriv->lock, flags);

	dev->trans_start = jiffies;
	return 0;
}


static struct net_device_stats *rr_get_stats(struct net_device *dev)
{
	struct rr_private *rrpriv;

	rrpriv = (struct rr_private *)dev->priv;

	return(&rrpriv->stats);
}


/*
 * Read the firmware out of the EEPROM and put it into the SRAM
 * (or from user space - later)
 *
 * This operation requires the NIC to be halted and is performed with
 * interrupts disabled and with the spinlock hold.
 */
static int rr_load_firmware(struct net_device *dev)
{
	struct rr_private *rrpriv;
	struct rr_regs *regs;
	unsigned long eptr, segptr;
	int i, j;
	u32 localctrl, sptr, len, tmp;
	u32 p2len, p2size, nr_seg, revision, io, sram_size;
	struct eeprom *hw = NULL;

	rrpriv = (struct rr_private *)dev->priv;
	regs = rrpriv->regs;

	if (dev->flags & IFF_UP)
		return -EBUSY;

	if (!(readl(&regs->HostCtrl) & NIC_HALTED)){
		printk("%s: Trying to load firmware to a running NIC.\n", 
		       dev->name);
		return -EBUSY;
	}

	localctrl = readl(&regs->LocalCtrl);
	writel(0, &regs->LocalCtrl);

	writel(0, &regs->EvtPrd);
	writel(0, &regs->RxPrd);
	writel(0, &regs->TxPrd);

	/*
	 * First wipe the entire SRAM, otherwise we might run into all
	 * kinds of trouble ... sigh, this took almost all afternoon
	 * to track down ;-(
	 */
	io = readl(&regs->ExtIo);
	writel(0, &regs->ExtIo);
	sram_size = rr_read_eeprom_word(rrpriv, (void *)8);

	for (i = 200; i < sram_size / 4; i++){
		writel(i * 4, &regs->WinBase);
		mb();
		writel(0, &regs->WinData);
		mb();
	}
	writel(io, &regs->ExtIo);
	mb();

	eptr = (unsigned long)rr_read_eeprom_word(rrpriv,
					       &hw->rncd_info.AddrRunCodeSegs);
	eptr = ((eptr & 0x1fffff) >> 3);

	p2len = rr_read_eeprom_word(rrpriv, (void *)(0x83*4));
	p2len = (p2len << 2);
	p2size = rr_read_eeprom_word(rrpriv, (void *)(0x84*4));
	p2size = ((p2size & 0x1fffff) >> 3);

	if ((eptr < p2size) || (eptr > (p2size + p2len))){
		printk("%s: eptr is invalid\n", dev->name);
		goto out;
	}

	revision = rr_read_eeprom_word(rrpriv, &hw->manf.HeaderFmt);

	if (revision != 1){
		printk("%s: invalid firmware format (%i)\n",
		       dev->name, revision);
		goto out;
	}

	nr_seg = rr_read_eeprom_word(rrpriv, (void *)eptr);
	eptr +=4;
#if (DEBUG > 1)
	printk("%s: nr_seg %i\n", dev->name, nr_seg);
#endif

	for (i = 0; i < nr_seg; i++){
		sptr = rr_read_eeprom_word(rrpriv, (void *)eptr);
		eptr += 4;
		len = rr_read_eeprom_word(rrpriv, (void *)eptr);
		eptr += 4;
		segptr = (unsigned long)rr_read_eeprom_word(rrpriv, (void *)eptr);
		segptr = ((segptr & 0x1fffff) >> 3);
		eptr += 4;
#if (DEBUG > 1)
		printk("%s: segment %i, sram address %06x, length %04x, segptr %06x\n",
		       dev->name, i, sptr, len, segptr);
#endif
		for (j = 0; j < len; j++){
			tmp = rr_read_eeprom_word(rrpriv, (void *)segptr);
			writel(sptr, &regs->WinBase);
			mb();
			writel(tmp, &regs->WinData);
			mb();
			segptr += 4;
			sptr += 4;
		}
	}

out:
	writel(localctrl, &regs->LocalCtrl);
	mb();
	return 0;
}


static int rr_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct rr_private *rrpriv;
	unsigned char *image, *oldimage;
	unsigned int i;
	int error = -EOPNOTSUPP;

	rrpriv = (struct rr_private *)dev->priv;

	spin_lock(&rrpriv->lock);

	switch(cmd){
	case SIOCRRGFW:
		if (!suser()){
			error = -EPERM;
			goto out;
		}

		if (rrpriv->fw_running){
			printk("%s: Firmware already running\n", dev->name);
			error = -EPERM;
			goto out;
		}

		image = kmalloc(EEPROM_WORDS * sizeof(u32), GFP_KERNEL);
		if (!image){
			printk(KERN_ERR "%s: Unable to allocate memory "
			       "for EEPROM image\n", dev->name);
			error = -ENOMEM;
			goto out;
		}
		i = rr_read_eeprom(rrpriv, 0, image, EEPROM_BYTES);
		if (i != EEPROM_BYTES){
			kfree(image);
			printk(KERN_ERR "%s: Error reading EEPROM\n",
			       dev->name);
			error = -EFAULT;
			goto out;
		}
		error = copy_to_user(rq->ifr_data, image, EEPROM_BYTES);
		if (error)
			error = -EFAULT;
		kfree(image);
		break;
	case SIOCRRPFW:
		if (!suser()){
			error = -EPERM;
			goto out;
		}

		if (rrpriv->fw_running){
			printk("%s: Firmware already running\n", dev->name);
			error = -EPERM;
			goto out;
		}

		image = kmalloc(EEPROM_WORDS * sizeof(u32), GFP_KERNEL);
		if (!image){
			printk(KERN_ERR "%s: Unable to allocate memory "
			       "for EEPROM image\n", dev->name);
			error = -ENOMEM;
			goto out;
		}

		oldimage = kmalloc(EEPROM_WORDS * sizeof(u32), GFP_KERNEL);
		if (!oldimage){
			printk(KERN_ERR "%s: Unable to allocate memory "
			       "for old EEPROM image\n", dev->name);
			error = -ENOMEM;
			goto out;
		}

		error = copy_from_user(image, rq->ifr_data, EEPROM_BYTES);
		if (error)
			error = -EFAULT;

		printk("%s: Updating EEPROM firmware\n", dev->name);

		error = write_eeprom(rrpriv, 0, image, EEPROM_BYTES);
		if (error)
			printk(KERN_ERR "%s: Error writing EEPROM\n",
			       dev->name);

		i = rr_read_eeprom(rrpriv, 0, oldimage, EEPROM_BYTES);
		if (i != EEPROM_BYTES)
			printk(KERN_ERR "%s: Error reading back EEPROM "
			       "image\n", dev->name);

		error = memcmp(image, oldimage, EEPROM_BYTES);
		if (error){
			printk(KERN_ERR "%s: Error verifying EEPROM image\n",
			       dev->name);
			error = -EFAULT;
		}
		kfree(image);
		kfree(oldimage);
		break;
	case SIOCRRID:
		error = put_user(0x52523032, (int *)(&rq->ifr_data[0]));
		if (error)
			error = -EFAULT;
		break;
	default:
	}

 out:
	spin_unlock(&rrpriv->lock);
	return error;
}


/*
 * Local variables:
 * compile-command: "gcc -D__SMP__ -D__KERNEL__ -I../../include -Wall -Wstrict-prototypes -O2 -pipe -fomit-frame-pointer -fno-strength-reduce -m486 -malign-loops=2 -malign-jumps=2 -malign-functions=2 -DCPU=686 -DMODULE -DMODVERSIONS -include ../../include/linux/modversions.h -c rrunner.c"
 * End:
 */
