#ifndef __ALPHA_MCPCIA__H__
#define __ALPHA_MCPCIA__H__

/* Define to experiment with fitting everything into one 128MB HAE window.
   One window per bus, that is.  */
#define MCPCIA_ONE_HAE_WINDOW 1

#include <linux/types.h>
#include <linux/pci.h>
#include <asm/compiler.h>

/*
 * MCPCIA is the internal name for a core logic chipset which provides
 * PCI access for the RAWHIDE family of systems.
 *
 * This file is based on:
 *
 * RAWHIDE System Programmer's Manual
 * 16-May-96
 * Rev. 1.4
 *
 */

/*------------------------------------------------------------------------**
**                                                                        **
**  I/O procedures                                                        **
**                                                                        **
**      inport[b|w|t|l], outport[b|w|t|l] 8:16:24:32 IO xfers             **
**	inportbxt: 8 bits only                                            **
**      inport:    alias of inportw                                       **
**      outport:   alias of outportw                                      **
**                                                                        **
**      inmem[b|w|t|l], outmem[b|w|t|l] 8:16:24:32 ISA memory xfers       **
**	inmembxt: 8 bits only                                             **
**      inmem:    alias of inmemw                                         **
**      outmem:   alias of outmemw                                        **
**                                                                        **
**------------------------------------------------------------------------*/


/* MCPCIA ADDRESS BIT DEFINITIONS
 *
 *  3333 3333 3322 2222 2222 1111 1111 11
 *  9876 5432 1098 7654 3210 9876 5432 1098 7654 3210
 *  ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
 *  1                                             000
 *  ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
 *  |                                             |\|
 *  |                               Byte Enable --+ |
 *  |                             Transfer Length --+
 *  +-- IO space, not cached
 *
 *   Byte      Transfer
 *   Enable    Length    Transfer  Byte    Address
 *   adr<6:5>  adr<4:3>  Length    Enable  Adder
 *   ---------------------------------------------
 *      00        00      Byte      1110   0x000
 *      01        00      Byte      1101   0x020
 *      10        00      Byte      1011   0x040
 *      11        00      Byte      0111   0x060
 *
 *      00        01      Word      1100   0x008
 *      01        01      Word      1001   0x028 <= Not supported in this code.
 *      10        01      Word      0011   0x048
 *
 *      00        10      Tribyte   1000   0x010
 *      01        10      Tribyte   0001   0x030
 *
 *      10        11      Longword  0000   0x058
 *
 *      Note that byte enables are asserted low.
 *
 */

#define MCPCIA_MEM_MASK 0x07ffffff /* SPARSE Mem region mask is 27 bits */

#define MCPCIA_DMA_WIN_BASE		(2UL*1024*1024*1024)
#define MCPCIA_DMA_WIN_SIZE		(2UL*1024*1024*1024)

#define MCPCIA_MID(m)		((unsigned long)(m) << 33)

/*
 * Memory spaces:
 */
#define MCPCIA_SPARSE(m)	(IDENT_ADDR + 0xf000000000UL + MCPCIA_MID(m))
#define MCPCIA_DENSE(m)		(IDENT_ADDR + 0xf100000000UL + MCPCIA_MID(m))
#define MCPCIA_IO(m)		(IDENT_ADDR + 0xf180000000UL + MCPCIA_MID(m))
#define MCPCIA_CONF(m)		(IDENT_ADDR + 0xf1c0000000UL + MCPCIA_MID(m))
#define MCPCIA_CSR(m)		(IDENT_ADDR + 0xf1e0000000UL + MCPCIA_MID(m))
#define MCPCIA_IO_IACK(m)	(IDENT_ADDR + 0xf1f0000000UL + MCPCIA_MID(m))
#define MCPCIA_DENSE_IO(m)	(IDENT_ADDR + 0xe1fc000000UL + MCPCIA_MID(m))
#define MCPCIA_DENSE_CONF(m)	(IDENT_ADDR + 0xe1fe000000UL + MCPCIA_MID(m))

/*
 *  General Registers
 */
#define MCPCIA_REV(m)		(MCPCIA_CSR(m) + 0x000)
#define MCPCIA_WHOAMI(m)	(MCPCIA_CSR(m) + 0x040)
#define MCPCIA_PCI_LAT(m)	(MCPCIA_CSR(m) + 0x080)
#define MCPCIA_CAP_CTRL(m)	(MCPCIA_CSR(m) + 0x100)
#define MCPCIA_HAE_MEM(m)	(MCPCIA_CSR(m) + 0x400)
#define MCPCIA_HAE_IO(m)	(MCPCIA_CSR(m) + 0x440)
#define _MCPCIA_IACK_SC(m)	(MCPCIA_CSR(m) + 0x480)
#define MCPCIA_HAE_DENSE(m)	(MCPCIA_CSR(m) + 0x4C0)

/*
 * Interrupt Control registers
 */
#define MCPCIA_INT_CTL(m)	(MCPCIA_CSR(m) + 0x500)
#define MCPCIA_INT_REQ(m)	(MCPCIA_CSR(m) + 0x540)
#define MCPCIA_INT_TARG(m)	(MCPCIA_CSR(m) + 0x580)
#define MCPCIA_INT_ADR(m)	(MCPCIA_CSR(m) + 0x5C0)
#define MCPCIA_INT_ADR_EXT(m)	(MCPCIA_CSR(m) + 0x600)
#define MCPCIA_INT_MASK0(m)	(MCPCIA_CSR(m) + 0x640)
#define MCPCIA_INT_MASK1(m)	(MCPCIA_CSR(m) + 0x680)
#define MCPCIA_INT_ACK0(m)	(MCPCIA_CSR(m) + 0x10003f00)
#define MCPCIA_INT_ACK1(m)	(MCPCIA_CSR(m) + 0x10003f40)

/*
 * Performance Monitor registers
 */
#define MCPCIA_PERF_MON(m)	(MCPCIA_CSR(m) + 0x300)
#define MCPCIA_PERF_CONT(m)	(MCPCIA_CSR(m) + 0x340)

/*
 * Diagnostic Registers
 */
#define MCPCIA_CAP_DIAG(m)	(MCPCIA_CSR(m) + 0x700)
#define MCPCIA_TOP_OF_MEM(m)	(MCPCIA_CSR(m) + 0x7C0)

/*
 * Error registers
 */
#define MCPCIA_MC_ERR0(m)	(MCPCIA_CSR(m) + 0x800)
#define MCPCIA_MC_ERR1(m)	(MCPCIA_CSR(m) + 0x840)
#define MCPCIA_CAP_ERR(m)	(MCPCIA_CSR(m) + 0x880)
#define MCPCIA_PCI_ERR1(m)	(MCPCIA_CSR(m) + 0x1040)
#define MCPCIA_MDPA_STAT(m)	(MCPCIA_CSR(m) + 0x4000)
#define MCPCIA_MDPA_SYN(m)	(MCPCIA_CSR(m) + 0x4040)
#define MCPCIA_MDPA_DIAG(m)	(MCPCIA_CSR(m) + 0x4080)
#define MCPCIA_MDPB_STAT(m)	(MCPCIA_CSR(m) + 0x8000)
#define MCPCIA_MDPB_SYN(m)	(MCPCIA_CSR(m) + 0x8040)
#define MCPCIA_MDPB_DIAG(m)	(MCPCIA_CSR(m) + 0x8080)

/*
 * PCI Address Translation Registers.
 */
#define MCPCIA_SG_TBIA(m)	(MCPCIA_CSR(m) + 0x1300)
#define MCPCIA_HBASE(m)		(MCPCIA_CSR(m) + 0x1340)

#define MCPCIA_W0_BASE(m)	(MCPCIA_CSR(m) + 0x1400)
#define MCPCIA_W0_MASK(m)	(MCPCIA_CSR(m) + 0x1440)
#define MCPCIA_T0_BASE(m)	(MCPCIA_CSR(m) + 0x1480)

#define MCPCIA_W1_BASE(m)	(MCPCIA_CSR(m) + 0x1500)
#define MCPCIA_W1_MASK(m)	(MCPCIA_CSR(m) + 0x1540)
#define MCPCIA_T1_BASE(m)	(MCPCIA_CSR(m) + 0x1580)

#define MCPCIA_W2_BASE(m)	(MCPCIA_CSR(m) + 0x1600)
#define MCPCIA_W2_MASK(m)	(MCPCIA_CSR(m) + 0x1640)
#define MCPCIA_T2_BASE(m)	(MCPCIA_CSR(m) + 0x1680)

#define MCPCIA_W3_BASE(m)	(MCPCIA_CSR(m) + 0x1700)
#define MCPCIA_W3_MASK(m)	(MCPCIA_CSR(m) + 0x1740)
#define MCPCIA_T3_BASE(m)	(MCPCIA_CSR(m) + 0x1780)

/* Hack!  Only words for bus 0.  */

#if !MCPCIA_ONE_HAE_WINDOW
#define MCPCIA_HAE_ADDRESS	MCPCIA_HAE_MEM(4)
#endif
#define MCPCIA_IACK_SC		_MCPCIA_IACK_SC(4)

/* 
 * The canonical non-remaped I/O and MEM addresses have these values
 * subtracted out.  This is arranged so that folks manipulating ISA
 * devices can use their familiar numbers and have them map to bus 0.
 */

#define MCPCIA_IO_BIAS		MCPCIA_IO(4)
#define MCPCIA_MEM_BIAS		MCPCIA_DENSE(4)


/*
 * Data structure for handling MCPCIA machine checks:
 */
struct el_MCPCIA_uncorrected_frame_mcheck {
	struct el_common header;
	struct el_common_EV5_uncorrectable_mcheck procdata;
};


#ifdef __KERNEL__

#ifndef __EXTERN_INLINE
#define __EXTERN_INLINE extern inline
#define __IO_EXTERN_INLINE
#endif

/*
 * Translate physical memory address as seen on (PCI) bus into
 * a kernel virtual address and vv.
 */

__EXTERN_INLINE unsigned long mcpcia_virt_to_bus(void * address)
{
	return virt_to_phys(address) + MCPCIA_DMA_WIN_BASE;
}

__EXTERN_INLINE void * mcpcia_bus_to_virt(unsigned long address)
{
	return phys_to_virt(address - MCPCIA_DMA_WIN_BASE);
}

/*
 * I/O functions:
 *
 * MCPCIA, the RAWHIDE family PCI/memory support chipset for the EV5 (21164)
 * and EV56 (21164a) processors, can use either a sparse address mapping
 * scheme, or the so-called byte-word PCI address space, to get at PCI memory
 * and I/O.
 *
 * Unfortunately, we can't use BWIO with EV5, so for now, we always use SPARSE.
 */

#define vucp	volatile unsigned char *
#define vusp	volatile unsigned short *
#define vip	volatile int *
#define vuip	volatile unsigned int *
#define vulp	volatile unsigned long *

__EXTERN_INLINE unsigned int mcpcia_inb(unsigned long in_addr)
{
	unsigned long addr, hose, result;

	addr = in_addr & 0xffffUL;
	hose = in_addr & ~0xffffUL;

	/* ??? I wish I could get rid of this.  But there's no ioremap
	   equivalent for I/O space.  PCI I/O can be forced into the
	   correct hose's I/O region, but that doesn't take care of
	   legacy ISA crap.  */
	hose += MCPCIA_IO_BIAS;

	result = *(vip) ((addr << 5) + hose + 0x00);
	return __kernel_extbl(result, addr & 3);
}

__EXTERN_INLINE void mcpcia_outb(unsigned char b, unsigned long in_addr)
{
	unsigned long addr, hose, w;

	addr = in_addr & 0xffffUL;
	hose = in_addr & ~0xffffUL;
	hose += MCPCIA_IO_BIAS;

	w = __kernel_insbl(b, addr & 3);
	*(vuip) ((addr << 5) + hose + 0x00) = w;
	mb();
}

__EXTERN_INLINE unsigned int mcpcia_inw(unsigned long in_addr)
{
	unsigned long addr, hose, result;

	addr = in_addr & 0xffffUL;
	hose = in_addr & ~0xffffUL;
	hose += MCPCIA_IO_BIAS;

	result = *(vip) ((addr << 5) + hose + 0x08);
	return __kernel_extwl(result, addr & 3);
}

__EXTERN_INLINE void mcpcia_outw(unsigned short b, unsigned long in_addr)
{
	unsigned long addr, hose, w;

	addr = in_addr & 0xffffUL;
	hose = in_addr & ~0xffffUL;
	hose += MCPCIA_IO_BIAS;

	w = __kernel_inswl(b, addr & 3);
	*(vuip) ((addr << 5) + hose + 0x08) = w;
	mb();
}

__EXTERN_INLINE unsigned int mcpcia_inl(unsigned long in_addr)
{
	unsigned long addr, hose;

	addr = in_addr & 0xffffUL;
	hose = in_addr & ~0xffffUL;
	hose += MCPCIA_IO_BIAS;

	return *(vuip) ((addr << 5) + hose + 0x18);
}

__EXTERN_INLINE void mcpcia_outl(unsigned int b, unsigned long in_addr)
{
	unsigned long addr, hose;

	addr = in_addr & 0xffffUL;
	hose = in_addr & ~0xffffUL;
	hose += MCPCIA_IO_BIAS;

	*(vuip) ((addr << 5) + hose + 0x18) = b;
	mb();
}


/*
 * Memory functions.  64-bit and 32-bit accesses are done through
 * dense memory space, everything else through sparse space.
 *
 * For reading and writing 8 and 16 bit quantities we need to
 * go through one of the three sparse address mapping regions
 * and use the HAE_MEM CSR to provide some bits of the address.
 * The following few routines use only sparse address region 1
 * which gives 1Gbyte of accessible space which relates exactly
 * to the amount of PCI memory mapping *into* system address space.
 * See p 6-17 of the specification but it looks something like this:
 *
 * 21164 Address:
 *
 *          3         2         1
 * 9876543210987654321098765432109876543210
 * 1ZZZZ0.PCI.QW.Address............BBLL
 *
 * ZZ = SBZ
 * BB = Byte offset
 * LL = Transfer length
 *
 * PCI Address:
 *
 * 3         2         1
 * 10987654321098765432109876543210
 * HHH....PCI.QW.Address........ 00
 *
 * HHH = 31:29 HAE_MEM CSR
 *
 */

__EXTERN_INLINE unsigned long mcpcia_ioremap(unsigned long addr)
{
	return addr + MCPCIA_MEM_BIAS;
}

__EXTERN_INLINE int mcpcia_is_ioaddr(unsigned long addr)
{
	return addr >= MCPCIA_SPARSE(0);
}

__EXTERN_INLINE unsigned long mcpcia_readb(unsigned long in_addr)
{
	unsigned long addr = in_addr & 0xffffffffUL;
	unsigned long hose = in_addr & ~0xffffffffUL;
	unsigned long result, work;

#if !MCPCIA_ONE_HAE_WINDOW
	unsigned long msb;
	msb = addr & ~MCPCIA_MEM_MASK;
	set_hae(msb);
#endif
	addr = addr & MCPCIA_MEM_MASK;

	hose = hose - MCPCIA_DENSE(4) + MCPCIA_SPARSE(4);
	work = ((addr << 5) + hose + 0x00);
	result = *(vip) work;
	return __kernel_extbl(result, addr & 3);
}

__EXTERN_INLINE unsigned long mcpcia_readw(unsigned long in_addr)
{
	unsigned long addr = in_addr & 0xffffffffUL;
	unsigned long hose = in_addr & ~0xffffffffUL;
	unsigned long result, work;

#if !MCPCIA_ONE_HAE_WINDOW
	unsigned long msb;
	msb = addr & ~MCPCIA_MEM_MASK;
	set_hae(msb);
#endif
	addr = addr & MCPCIA_MEM_MASK;

	hose = hose - MCPCIA_DENSE(4) + MCPCIA_SPARSE(4);
	work = ((addr << 5) + hose + 0x08);
	result = *(vip) work;
	return __kernel_extwl(result, addr & 3);
}

__EXTERN_INLINE void mcpcia_writeb(unsigned char b, unsigned long in_addr)
{
	unsigned long addr = in_addr & 0xffffffffUL;
	unsigned long hose = in_addr & ~0xffffffffUL;
	unsigned long w;

#if !MCPCIA_ONE_HAE_WINDOW
	unsigned long msb;
	msb = addr & ~MCPCIA_MEM_MASK;
	set_hae(msb);
#endif
	addr = addr & MCPCIA_MEM_MASK;

	w = __kernel_insbl(b, in_addr & 3);
	hose = hose - MCPCIA_DENSE(4) + MCPCIA_SPARSE(4);
	*(vuip) ((addr << 5) + hose + 0x00) = w;
}

__EXTERN_INLINE void mcpcia_writew(unsigned short b, unsigned long in_addr)
{
	unsigned long addr = in_addr & 0xffffffffUL;
	unsigned long hose = in_addr & ~0xffffffffUL;
	unsigned long w;

#if !MCPCIA_ONE_HAE_WINDOW
	unsigned long msb;
	msb = addr & ~MCPCIA_MEM_MASK;
	set_hae(msb);
#endif
	addr = addr & MCPCIA_MEM_MASK;

	w = __kernel_inswl(b, in_addr & 3);
	hose = hose - MCPCIA_DENSE(4) + MCPCIA_SPARSE(4);
	*(vuip) ((addr << 5) + hose + 0x08) = w;
}

__EXTERN_INLINE unsigned long mcpcia_readl(unsigned long addr)
{
	return *(vuip)addr;
}

__EXTERN_INLINE unsigned long mcpcia_readq(unsigned long addr)
{
	return *(vulp)addr;
}

__EXTERN_INLINE void mcpcia_writel(unsigned int b, unsigned long addr)
{
	*(vuip)addr = b;
}

__EXTERN_INLINE void mcpcia_writeq(unsigned long b, unsigned long addr)
{
	*(vulp)addr = b;
}

#undef vucp
#undef vusp
#undef vip
#undef vuip
#undef vulp

#ifdef __WANT_IO_DEF

#define virt_to_bus	mcpcia_virt_to_bus
#define bus_to_virt	mcpcia_bus_to_virt

#define __inb		mcpcia_inb
#define __inw		mcpcia_inw
#define __inl		mcpcia_inl
#define __outb		mcpcia_outb
#define __outw		mcpcia_outw
#define __outl		mcpcia_outl
#define __readb		mcpcia_readb
#define __readw		mcpcia_readw
#define __writeb	mcpcia_writeb
#define __writew	mcpcia_writew
#define __readl		mcpcia_readl
#define __readq		mcpcia_readq
#define __writel	mcpcia_writel
#define __writeq	mcpcia_writeq
#define __ioremap	mcpcia_ioremap
#define __is_ioaddr	mcpcia_is_ioaddr

# define inb(port) \
  (__builtin_constant_p((port))?__inb(port):_inb(port))
# define outb(x, port) \
  (__builtin_constant_p((port))?__outb((x),(port)):_outb((x),(port)))

#define __raw_readl(a)		__readl((unsigned long)(a))
#define __raw_readq(a)		__readq((unsigned long)(a))
#define __raw_writel(v,a)	__writel((v),(unsigned long)(a))
#define __raw_writeq(v,a)	__writeq((v),(unsigned long)(a))

#endif /* __WANT_IO_DEF */

#ifdef __IO_EXTERN_INLINE
#undef __EXTERN_INLINE
#undef __IO_EXTERN_INLINE
#endif

#endif /* __KERNEL__ */

#endif /* __ALPHA_MCPCIA__H__ */
