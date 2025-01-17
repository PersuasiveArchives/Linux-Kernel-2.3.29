/* $Id: pci.c,v 1.7 1999/01/04 16:03:58 ralf Exp $
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * SNI specific PCI support for RM200/RM300.
 *
 * Copyright (C) 1997, 1998 Ralf Baechle
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <asm/byteorder.h>
#include <asm/sni.h>

#ifdef CONFIG_PCI

#define mkaddr(bus, dev_fn, where)                                           \
do {                                                                         \
	if (bus == 0 && dev_fn >= PCI_DEVFN(8, 0))                           \
		return -1;                                                   \
	*(volatile u32 *)PCIMT_CONFIG_ADDRESS = ((bus    & 0xff) << 0x10) |  \
	                                        ((dev_fn & 0xff) << 0x08) |  \
	                                        (where  & 0xfc);             \
} while(0);

static void sni_rm200_pcibios_fixup (void)
{
	struct pci_dev *dev;

	for (dev=pci_devices; dev; dev=dev->next) {
		/*
		 * TODO: Take care of RM300 revision D boards for where the
		 * network slot became an ordinary PCI slot.
		 */
		if (dev->devfn == PCI_DEVFN(1, 0)) {
			/* Evil hack ...  */
			set_cp0_config(CONF_CM_CMASK, CONF_CM_CACHABLE_NO_WA);
			dev->irq = PCIMT_IRQ_SCSI;
			continue;
		}
		if (dev->devfn == PCI_DEVFN(2, 0)) {
			dev->irq = PCIMT_IRQ_ETHERNET;
			continue;
		}

		switch(dev->irq) {
		case 1 ... 4:
			dev->irq += PCIMT_IRQ_INTA - 1;
			break;
		case 0:
			break;
		default:
			printk("PCI device on bus %d, dev %d, function %d "
			       "impossible interrupt configured.\n",
			       dev->bus->number, PCI_SLOT(dev->devfn),
			       PCI_SLOT(dev->devfn));
		}
	}
}

/*
 * We can't address 8 and 16 bit words directly.  Instead we have to
 * read/write a 32bit word and mask/modify the data we actually want.
 */
static int sni_rm200_pcibios_read_config_byte (unsigned char bus,
                                               unsigned char dev_fn,
                                               unsigned char where,
                                               unsigned char *val)
{
	u32 res;

	mkaddr(bus, dev_fn, where);
	res = *(volatile u32 *)PCIMT_CONFIG_DATA;
	res = (le32_to_cpu(res) >> ((where & 3) << 3)) & 0xff;
	*val = res;

	return PCIBIOS_SUCCESSFUL;
}

static int sni_rm200_pcibios_read_config_word (unsigned char bus,
                                               unsigned char dev_fn,
                                               unsigned char where,
                                               unsigned short *val)
{
	u32 res;

	if (where & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	mkaddr(bus, dev_fn, where);
	res = *(volatile u32 *)PCIMT_CONFIG_DATA;
	res = (le32_to_cpu(res) >> ((where & 3) << 3)) & 0xffff;
	*val = res;

	return PCIBIOS_SUCCESSFUL;
}

static int sni_rm200_pcibios_read_config_dword (unsigned char bus,
                                                unsigned char dev_fn,
                                                unsigned char where,
                                                unsigned int *val)
{
	u32 res;

		if (where & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	mkaddr(bus, dev_fn, where);
	res = *(volatile u32 *)PCIMT_CONFIG_DATA;
	res = le32_to_cpu(res);
	*val = res;

	return PCIBIOS_SUCCESSFUL;
}

static int sni_rm200_pcibios_write_config_byte (unsigned char bus,
                                                unsigned char dev_fn,
                                                unsigned char where,
                                                unsigned char val)
{
	mkaddr(bus, dev_fn, where);
	*(volatile u8 *)(PCIMT_CONFIG_DATA + (where & 3)) = val;

	return PCIBIOS_SUCCESSFUL;
}

static int sni_rm200_pcibios_write_config_word (unsigned char bus,
                                                unsigned char dev_fn,
                                                unsigned char where,
                                                unsigned short val)
{
	if (where & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	mkaddr(bus, dev_fn, where);
	*(volatile u16 *)(PCIMT_CONFIG_DATA + (where & 3)) = le16_to_cpu(val);

	return PCIBIOS_SUCCESSFUL;
}

static int sni_rm200_pcibios_write_config_dword (unsigned char bus,
                                                 unsigned char dev_fn,
                                                 unsigned char where,
                                                 unsigned int val)
{
	if (where & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	mkaddr(bus, dev_fn, where);
	*(volatile u32 *)PCIMT_CONFIG_DATA = le32_to_cpu(val);

	return PCIBIOS_SUCCESSFUL;
}

struct pci_ops sni_pci_ops = {
	sni_rm200_pcibios_fixup,
	sni_rm200_pcibios_read_config_byte,
	sni_rm200_pcibios_read_config_word,
	sni_rm200_pcibios_read_config_dword,
	sni_rm200_pcibios_write_config_byte,
	sni_rm200_pcibios_write_config_word,
	sni_rm200_pcibios_write_config_dword
};

#endif /* CONFIG_PCI */
