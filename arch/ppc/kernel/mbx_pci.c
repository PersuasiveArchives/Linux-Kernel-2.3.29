/*
 * MBX pci routines.
 * The MBX uses the QSpan PCI bridge.  The config address register
 * is located 0x500 from the base of the bridge control/status registers.
 * The data register is located at 0x504.
 * This is a two step operation.  First, the address register is written,
 * then the data register is read/written as required.
 * I don't know what to do about interrupts (yet).
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/bios32.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/mbx.h>


/*
 * This blows......The MBX uses the Tundra QSpan PCI bridge.  When
 * reading the configuration space, if something does not respond
 * the bus times out and we get a machine check interrupt.  So, the
 * good ol' exception tables come to mind to trap it and return some
 * value.
 *
 * On an error we just return a -1, since that is what the caller wants
 * returned if nothing is present.  I copied this from __get_user_asm,
 * with the only difference of returning -1 instead of EFAULT.
 * There is an associated hack in the machine check trap code.
 *
 * The QSPAN is also a big endian device, that is it makes the PCI
 * look big endian to us.  This presents a problem for the Linux PCI
 * functions, which assume little endian.  For example, we see the
 * first 32-bit word like this:
 *	------------------------
 *	| Device ID | Vendor ID |
 *	------------------------
 * If we read/write as a double word, that's OK.  But in our world,
 * when read as a word, device ID is at location 0, not location 2 as
 * the little endian PCI would believe.  We have to switch bits in
 * the PCI addresses given to us to get the data to/from the correct
 * byte lanes.
 *
 * The QSPAN only supports 4 bits of "slot" in the dev_fn instead of 5.
 * It always forces the MS bit to zero.  Therefore, dev_fn values
 * greater than 128 are returned as "no device found" errors.
 *
 * The QSPAN can only perform long word (32-bit) configuration cycles.
 * The "offset" must have the two LS bits set to zero.  Read operations
 * require we read the entire word and then sort out what should be
 * returned.  Write operations other than long word require that we
 * read the long word, update the proper word or byte, then write the
 * entire long word back.
 *
 * PCI Bridge hack.  We assume (correctly) that bus 0 is the primary
 * PCI bus from the QSPAN.  If we are called with a bus number other
 * than zero, we create a Type 1 configuration access that a downstream
 * PCI bridge will interpret.
 */

#define __get_mbx_pci_config(x, addr, op)		\
	__asm__ __volatile__(				\
		"1:	"op" %0,0(%1)\n"		\
		"	eieio\n"			\
		"2:\n"					\
		".section .fixup,\"ax\"\n"		\
		"3:	li %0,-1\n"			\
		"	b 2b\n"				\
		".section __ex_table,\"a\"\n"		\
		"	.align 2\n"			\
		"	.long 1b,3b\n"			\
		".text"					\
		: "=r"(x) : "r"(addr))

#define QS_CONFIG_ADDR	((volatile uint *)(PCI_CSR_ADDR + 0x500))
#define QS_CONFIG_DATA	((volatile uint *)(PCI_CSR_ADDR + 0x504))

#define mk_config_addr(bus, dev, offset) \
	(((bus)<<16) | ((dev)<<8) | (offset & 0xfc))

#define mk_config_type1(bus, dev, offset) \
	mk_config_addr(bus, dev, offset) | 1;

int mbx_pcibios_read_config_byte(unsigned char bus, unsigned char dev_fn,
				  unsigned char offset, unsigned char *val)
{
	uint	temp;
	u_char	*cp;

	if ((bus > 7) || (dev_fn > 127)) {
		*val = 0xff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	if (bus == 0)
		*QS_CONFIG_ADDR = mk_config_addr(bus, dev_fn, offset);
	else
		*QS_CONFIG_ADDR = mk_config_type1(bus, dev_fn, offset);
	__get_mbx_pci_config(temp, QS_CONFIG_DATA, "lwz");

	offset ^= 0x03;
	cp = ((u_char *)&temp) + (offset & 0x03);
	*val = *cp;
	return PCIBIOS_SUCCESSFUL;
}

int mbx_pcibios_read_config_word(unsigned char bus, unsigned char dev_fn,
				  unsigned char offset, unsigned short *val)
{
	uint	temp;
	ushort	*sp;

	if ((bus > 7) || (dev_fn > 127)) {
		*val = 0xffff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	if (bus == 0)
		*QS_CONFIG_ADDR = mk_config_addr(bus, dev_fn, offset);
	else
		*QS_CONFIG_ADDR = mk_config_type1(bus, dev_fn, offset);
	__get_mbx_pci_config(temp, QS_CONFIG_DATA, "lwz");
	offset ^= 0x02;

	sp = ((ushort *)&temp) + ((offset >> 1) & 1);
	*val = *sp;
	return PCIBIOS_SUCCESSFUL;
}

int mbx_pcibios_read_config_dword(unsigned char bus, unsigned char dev_fn,
				   unsigned char offset, unsigned int *val)
{
	if ((bus > 7) || (dev_fn > 127)) {
		*val = 0xffffffff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	if (bus == 0)
		*QS_CONFIG_ADDR = mk_config_addr(bus, dev_fn, offset);
	else
		*QS_CONFIG_ADDR = mk_config_type1(bus, dev_fn, offset);
	__get_mbx_pci_config(*val, QS_CONFIG_DATA, "lwz");
	return PCIBIOS_SUCCESSFUL;
}

int mbx_pcibios_write_config_byte(unsigned char bus, unsigned char dev_fn,
				   unsigned char offset, unsigned char val)
{
	uint	temp;
	u_char	*cp;

	if ((bus > 7) || (dev_fn > 127))
		return PCIBIOS_DEVICE_NOT_FOUND;

	mbx_pcibios_read_config_dword(bus, dev_fn, offset, &temp);

	offset ^= 0x03;
	cp = ((u_char *)&temp) + (offset & 0x03);
	*cp = val;

	if (bus == 0)
		*QS_CONFIG_ADDR = mk_config_addr(bus, dev_fn, offset);
	else
		*QS_CONFIG_ADDR = mk_config_type1(bus, dev_fn, offset);
	*QS_CONFIG_DATA = temp;

	return PCIBIOS_SUCCESSFUL;
}

int mbx_pcibios_write_config_word(unsigned char bus, unsigned char dev_fn,
				   unsigned char offset, unsigned short val)
{
	uint	temp;
	ushort	*sp;

	if ((bus > 7) || (dev_fn > 127))
		return PCIBIOS_DEVICE_NOT_FOUND;

	mbx_pcibios_read_config_dword(bus, dev_fn, offset, &temp);

	offset ^= 0x02;
	sp = ((ushort *)&temp) + ((offset >> 1) & 1);
	*sp = val;

	if (bus == 0)
		*QS_CONFIG_ADDR = mk_config_addr(bus, dev_fn, offset);
	else
		*QS_CONFIG_ADDR = mk_config_type1(bus, dev_fn, offset);
	*QS_CONFIG_DATA = temp;

	return PCIBIOS_SUCCESSFUL;
}

int mbx_pcibios_write_config_dword(unsigned char bus, unsigned char dev_fn,
				    unsigned char offset, unsigned int val)
{
	if ((bus > 7) || (dev_fn > 127))
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (bus == 0)
		*QS_CONFIG_ADDR = mk_config_addr(bus, dev_fn, offset);
	else
		*QS_CONFIG_ADDR = mk_config_type1(bus, dev_fn, offset);
	*(unsigned int *)QS_CONFIG_DATA = val;

	return PCIBIOS_SUCCESSFUL;
}

int mbx_pcibios_find_device(unsigned short vendor, unsigned short dev_id,
			     unsigned short index, unsigned char *bus_ptr,
			     unsigned char *dev_fn_ptr)
{
    int num, devfn;
    unsigned int x, vendev;

    if (vendor == 0xffff)
	return PCIBIOS_BAD_VENDOR_ID;
    vendev = (dev_id << 16) + vendor;
    num = 0;
    for (devfn = 0;  devfn < 32;  devfn++) {
	mbx_pcibios_read_config_dword(0, devfn<<3, PCI_VENDOR_ID, &x);
	if (x == vendev) {
	    if (index == num) {
		*bus_ptr = 0;
		*dev_fn_ptr = devfn<<3;
		return PCIBIOS_SUCCESSFUL;
	    }
	    ++num;
	}
    }
    return PCIBIOS_DEVICE_NOT_FOUND;
}

int mbx_pcibios_find_class(unsigned int class_code, unsigned short index,
			    unsigned char *bus_ptr, unsigned char *dev_fn_ptr)
{
    int devnr, x, num;

    num = 0;
    for (devnr = 0;  devnr < 32;  devnr++) {
	mbx_pcibios_read_config_dword(0, devnr<<3, PCI_CLASS_REVISION, &x);
	if ((x>>8) == class_code) {
	    if (index == num) {
		*bus_ptr = 0;
		*dev_fn_ptr = devnr<<3;
		return PCIBIOS_SUCCESSFUL;
	    }
	    ++num;
	}
    }
    return PCIBIOS_DEVICE_NOT_FOUND;
}

void __init
mbx_pcibios_fixup(void)
{
   /* Nothing to do here? */
}

void __init
mbx_setup_pci_ptrs(void)
{
	set_config_access_method(mbx);

	ppc_md.pcibios_fixup = mbx_pcibios_fixup;
}

