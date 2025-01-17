/*  Generic MTRR (Memory Type Range Register) driver.

    Copyright (C) 1997-1999  Richard Gooch

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    Richard Gooch may be reached by email at  rgooch@atnf.csiro.au
    The postal address is:
      Richard Gooch, c/o ATNF, P. O. Box 76, Epping, N.S.W., 2121, Australia.

    Source: "Pentium Pro Family Developer's Manual, Volume 3:
    Operating System Writer's Guide" (Intel document number 242692),
    section 11.11.7

    ChangeLog

    Prehistory Martin Tischhäuser <martin@ikcbarka.fzk.de>
	       Initial register-setting code (from proform-1.0).
    19971216   Richard Gooch <rgooch@atnf.csiro.au>
               Original version for /proc/mtrr interface, SMP-safe.
  v1.0
    19971217   Richard Gooch <rgooch@atnf.csiro.au>
               Bug fix for ioctls()'s.
	       Added sample code in Documentation/mtrr.txt
  v1.1
    19971218   Richard Gooch <rgooch@atnf.csiro.au>
               Disallow overlapping regions.
    19971219   Jens Maurer <jmaurer@menuett.rhein-main.de>
               Register-setting fixups.
  v1.2
    19971222   Richard Gooch <rgooch@atnf.csiro.au>
               Fixups for kernel 2.1.75.
  v1.3
    19971229   David Wragg <dpw@doc.ic.ac.uk>
               Register-setting fixups and conformity with Intel conventions.
    19971229   Richard Gooch <rgooch@atnf.csiro.au>
               Cosmetic changes and wrote this ChangeLog ;-)
    19980106   Richard Gooch <rgooch@atnf.csiro.au>
               Fixups for kernel 2.1.78.
  v1.4
    19980119   David Wragg <dpw@doc.ic.ac.uk>
               Included passive-release enable code (elsewhere in PCI setup).
  v1.5
    19980131   Richard Gooch <rgooch@atnf.csiro.au>
               Replaced global kernel lock with private spinlock.
  v1.6
    19980201   Richard Gooch <rgooch@atnf.csiro.au>
               Added wait for other CPUs to complete changes.
  v1.7
    19980202   Richard Gooch <rgooch@atnf.csiro.au>
               Bug fix in definition of <set_mtrr> for UP.
  v1.8
    19980319   Richard Gooch <rgooch@atnf.csiro.au>
               Fixups for kernel 2.1.90.
    19980323   Richard Gooch <rgooch@atnf.csiro.au>
               Move SMP BIOS fixup before secondary CPUs call <calibrate_delay>
  v1.9
    19980325   Richard Gooch <rgooch@atnf.csiro.au>
               Fixed test for overlapping regions: confused by adjacent regions
    19980326   Richard Gooch <rgooch@atnf.csiro.au>
               Added wbinvd in <set_mtrr_prepare>.
    19980401   Richard Gooch <rgooch@atnf.csiro.au>
               Bug fix for non-SMP compilation.
    19980418   David Wragg <dpw@doc.ic.ac.uk>
               Fixed-MTRR synchronisation for SMP and use atomic operations
	       instead of spinlocks.
    19980418   Richard Gooch <rgooch@atnf.csiro.au>
	       Differentiate different MTRR register classes for BIOS fixup.
  v1.10
    19980419   David Wragg <dpw@doc.ic.ac.uk>
	       Bug fix in variable MTRR synchronisation.
  v1.11
    19980419   Richard Gooch <rgooch@atnf.csiro.au>
	       Fixups for kernel 2.1.97.
  v1.12
    19980421   Richard Gooch <rgooch@atnf.csiro.au>
	       Safer synchronisation across CPUs when changing MTRRs.
  v1.13
    19980423   Richard Gooch <rgooch@atnf.csiro.au>
	       Bugfix for SMP systems without MTRR support.
  v1.14
    19980427   Richard Gooch <rgooch@atnf.csiro.au>
	       Trap calls to <mtrr_add> and <mtrr_del> on non-MTRR machines.
  v1.15
    19980427   Richard Gooch <rgooch@atnf.csiro.au>
	       Use atomic bitops for setting SMP change mask.
  v1.16
    19980428   Richard Gooch <rgooch@atnf.csiro.au>
	       Removed spurious diagnostic message.
  v1.17
    19980429   Richard Gooch <rgooch@atnf.csiro.au>
	       Moved register-setting macros into this file.
	       Moved setup code from init/main.c to i386-specific areas.
  v1.18
    19980502   Richard Gooch <rgooch@atnf.csiro.au>
	       Moved MTRR detection outside conditionals in <mtrr_init>.
  v1.19
    19980502   Richard Gooch <rgooch@atnf.csiro.au>
	       Documentation improvement: mention Pentium II and AGP.
  v1.20
    19980521   Richard Gooch <rgooch@atnf.csiro.au>
	       Only manipulate interrupt enable flag on local CPU.
	       Allow enclosed uncachable regions.
  v1.21
    19980611   Richard Gooch <rgooch@atnf.csiro.au>
	       Always define <main_lock>.
  v1.22
    19980901   Richard Gooch <rgooch@atnf.csiro.au>
	       Removed module support in order to tidy up code.
	       Added sanity check for <mtrr_add>/<mtrr_del> before <mtrr_init>.
	       Created addition queue for prior to SMP commence.
  v1.23
    19980902   Richard Gooch <rgooch@atnf.csiro.au>
	       Ported patch to kernel 2.1.120-pre3.
  v1.24
    19980910   Richard Gooch <rgooch@atnf.csiro.au>
	       Removed sanity checks and addition queue: Linus prefers an OOPS.
  v1.25
    19981001   Richard Gooch <rgooch@atnf.csiro.au>
	       Fixed harmless compiler warning in include/asm-i386/mtrr.h
	       Fixed version numbering and history for v1.23 -> v1.24.
  v1.26
    19990118   Richard Gooch <rgooch@atnf.csiro.au>
	       PLACEHOLDER.
  v1.27
    19990123   Richard Gooch <rgooch@atnf.csiro.au>
	       Changed locking to spin with reschedule.
	       Made use of new <smp_call_function>.
  v1.28
    19990201   Zoltan Boszormenyi <zboszor@mol.hu>
	       Extended the driver to be able to use Cyrix style ARRs.
    19990204   Richard Gooch <rgooch@atnf.csiro.au>
	       Restructured Cyrix support.
  v1.29
    19990204   Zoltan Boszormenyi <zboszor@mol.hu>
	       Refined ARR support: enable MAPEN in set_mtrr_prepare()
	       and disable MAPEN in set_mtrr_done().
    19990205   Richard Gooch <rgooch@atnf.csiro.au>
	       Minor cleanups.
  v1.30
    19990208   Zoltan Boszormenyi <zboszor@mol.hu>
               Protect plain 6x86s (and other processors without the
               Page Global Enable feature) against accessing CR4 in
               set_mtrr_prepare() and set_mtrr_done().
    19990210   Richard Gooch <rgooch@atnf.csiro.au>
	       Turned <set_mtrr_up> and <get_mtrr> into function pointers.
  v1.31
    19990212   Zoltan Boszormenyi <zboszor@mol.hu>
               Major rewrite of cyrix_arr_init(): do not touch ARRs,
               leave them as the BIOS have set them up.
               Enable usage of all 8 ARRs.
               Avoid multiplications by 3 everywhere and other
               code clean ups/speed ups.
    19990213   Zoltan Boszormenyi <zboszor@mol.hu>
               Set up other Cyrix processors identical to the boot cpu.
               Since Cyrix don't support Intel APIC, this is l'art pour l'art.
               Weigh ARRs by size:
               If size <= 32M is given, set up ARR# we were given.
               If size >  32M is given, set up ARR7 only if it is free,
               fail otherwise.
    19990214   Zoltan Boszormenyi <zboszor@mol.hu>
               Also check for size >= 256K if we are to set up ARR7,
               mtrr_add() returns the value it gets from set_mtrr()
    19990218   Zoltan Boszormenyi <zboszor@mol.hu>
               Remove Cyrix "coma bug" workaround from here.
               Moved to linux/arch/i386/kernel/setup.c and
               linux/include/asm-i386/bugs.h
    19990228   Richard Gooch <rgooch@atnf.csiro.au>
	       Added #ifdef CONFIG_DEVFS_FS
	       Added MTRRIOC_KILL_ENTRY ioctl(2)
	       Trap for counter underflow in <mtrr_file_del>.
	       Trap for 4 MiB aligned regions for PPro, stepping <= 7.
    19990301   Richard Gooch <rgooch@atnf.csiro.au>
	       Created <get_free_region> hook.
    19990305   Richard Gooch <rgooch@atnf.csiro.au>
	       Temporarily disable AMD support now MTRR capability flag is set.
  v1.32
    19990308   Zoltan Boszormenyi <zboszor@mol.hu>
	       Adjust my changes (19990212-19990218) to Richard Gooch's
	       latest changes. (19990228-19990305)
  v1.33
    19990309   Richard Gooch <rgooch@atnf.csiro.au>
	       Fixed typo in <printk> message.
    19990310   Richard Gooch <rgooch@atnf.csiro.au>
	       Support K6-II/III based on Alan Cox's <alan@redhat.com> patches.
  v1.34
    19990511   Bart Hartgers <bart@etpmod.phys.tue.nl>
	       Support Centaur C6 MCR's.
    19990512   Richard Gooch <rgooch@atnf.csiro.au>
	       Minor cleanups.
  v1.35
    19990707   Zoltan Boszormenyi <zboszor@mol.hu>
               Check whether ARR3 is protected in cyrix_get_free_region()
               and mtrr_del(). The code won't attempt to delete or change it
               from now on if the BIOS protected ARR3. It silently skips ARR3
               in cyrix_get_free_region() or returns with an error code from
               mtrr_del().
    19990711   Zoltan Boszormenyi <zboszor@mol.hu>
               Reset some bits in the CCRs in cyrix_arr_init() to disable SMM
               if ARR3 isn't protected. This is needed because if SMM is active
               and ARR3 isn't protected then deleting and setting ARR3 again
               may lock up the processor. With SMM entirely disabled, it does
               not happen.
    19990812   Zoltan Boszormenyi <zboszor@mol.hu>
               Rearrange switch() statements so the driver accomodates to
               the fact that the AMD Athlon handles its MTRRs the same way
               as Intel does.
    19990814   Zoltan Boszormenyi <zboszor@mol.hu>
	       Double check for Intel in mtrr_add()'s big switch() because
	       that revision check is only valid for Intel CPUs.
    19990819   Alan Cox <alan@redhat.com>
               Tested Zoltan's changes on a pre production Athlon - 100%
               success.
    19991008   Manfred Spraul <manfreds@colorfullife.com>
    	       replaced spin_lock_reschedule() with a normal semaphore.
*/
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/timer.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/module.h>
#define MTRR_NEED_STRINGS
#include <asm/mtrr.h>
#include <linux/init.h>
#include <linux/smp.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/segment.h>
#include <asm/bitops.h>
#include <asm/atomic.h>
#include <asm/msr.h>

#include <asm/hardirq.h>
#include <linux/irq.h>

#define MTRR_VERSION            "1.35 (19990512)"

#define TRUE  1
#define FALSE 0

#define MTRRcap_MSR     0x0fe
#define MTRRdefType_MSR 0x2ff

#define MTRRphysBase_MSR(reg) (0x200 + 2 * (reg))
#define MTRRphysMask_MSR(reg) (0x200 + 2 * (reg) + 1)

#define NUM_FIXED_RANGES 88
#define MTRRfix64K_00000_MSR 0x250
#define MTRRfix16K_80000_MSR 0x258
#define MTRRfix16K_A0000_MSR 0x259
#define MTRRfix4K_C0000_MSR 0x268
#define MTRRfix4K_C8000_MSR 0x269
#define MTRRfix4K_D0000_MSR 0x26a
#define MTRRfix4K_D8000_MSR 0x26b
#define MTRRfix4K_E0000_MSR 0x26c
#define MTRRfix4K_E8000_MSR 0x26d
#define MTRRfix4K_F0000_MSR 0x26e
#define MTRRfix4K_F8000_MSR 0x26f

#ifdef __SMP__
#  define MTRR_CHANGE_MASK_FIXED     0x01
#  define MTRR_CHANGE_MASK_VARIABLE  0x02
#  define MTRR_CHANGE_MASK_DEFTYPE   0x04
#endif

/* In the Intel processor's MTRR interface, the MTRR type is always held in
   an 8 bit field: */
typedef u8 mtrr_type;

#define LINE_SIZE      80
#define JIFFIE_TIMEOUT 100

#ifdef __SMP__
#  define set_mtrr(reg,base,size,type) set_mtrr_smp (reg, base, size, type)
#else
#  define set_mtrr(reg,base,size,type) (*set_mtrr_up) (reg, base, size, type, \
						       TRUE)
#endif

#ifndef CONFIG_PROC_FS
#  define compute_ascii() while (0)
#endif

#ifdef CONFIG_PROC_FS
static char *ascii_buffer = NULL;
static unsigned int ascii_buf_bytes = 0;
#endif
static unsigned int *usage_table = NULL;
static DECLARE_MUTEX(main_lock);

/*  Private functions  */
#ifdef CONFIG_PROC_FS
static void compute_ascii (void);
#endif


struct set_mtrr_context
{
    unsigned long flags;
    unsigned long deftype_lo;
    unsigned long deftype_hi;
    unsigned long cr4val;
    unsigned long ccr3;
};

static int arr3_protected;

/*  Put the processor into a state where MTRRs can be safely set  */
static void set_mtrr_prepare (struct set_mtrr_context *ctxt)
{
    unsigned long tmp;

    /*  Disable interrupts locally  */
    __save_flags (ctxt->flags); __cli ();

    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 >= 6) break; /* Athlon and post-Athlon CPUs */
	/* else fall through */
      case X86_VENDOR_CENTAUR:
	return;
	/*break;*/
    }
    /*  Save value of CR4 and clear Page Global Enable (bit 7)  */
    if (boot_cpu_data.x86_capability & X86_FEATURE_PGE)
	asm volatile ("movl  %%cr4, %0\n\t"
		      "movl  %0, %1\n\t"
		      "andb  $0x7f, %b1\n\t"
		      "movl  %1, %%cr4\n\t"
		      : "=r" (ctxt->cr4val), "=q" (tmp) : : "memory");

    /*  Disable and flush caches. Note that wbinvd flushes the TLBs as
	a side-effect  */
    asm volatile ("movl  %%cr0, %0\n\t"
		  "orl   $0x40000000, %0\n\t"
		  "wbinvd\n\t"
		  "movl  %0, %%cr0\n\t"
		  "wbinvd\n\t"
		  : "=r" (tmp) : : "memory");

    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
      case X86_VENDOR_INTEL:
	/*  Disable MTRRs, and set the default type to uncached  */
	rdmsr (MTRRdefType_MSR, ctxt->deftype_lo, ctxt->deftype_hi);
	wrmsr (MTRRdefType_MSR, ctxt->deftype_lo & 0xf300UL, ctxt->deftype_hi);
	break;
      case X86_VENDOR_CYRIX:
	tmp = getCx86 (CX86_CCR3);
	setCx86 (CX86_CCR3, (tmp & 0x0f) | 0x10);
	ctxt->ccr3 = tmp;
	break;
    }
}   /*  End Function set_mtrr_prepare  */

/*  Restore the processor after a set_mtrr_prepare  */
static void set_mtrr_done (struct set_mtrr_context *ctxt)
{
    unsigned long tmp;

    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 >= 6) break; /* Athlon and post-Athlon CPUs */
	/* else fall through */
      case X86_VENDOR_CENTAUR:
	__restore_flags (ctxt->flags);
	return;
	/*break;*/
    }
    /*  Flush caches and TLBs  */
    asm volatile ("wbinvd" : : : "memory" );

    /*  Restore MTRRdefType  */
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
      case X86_VENDOR_INTEL:
	wrmsr (MTRRdefType_MSR, ctxt->deftype_lo, ctxt->deftype_hi);
	break;
      case X86_VENDOR_CYRIX:
	setCx86 (CX86_CCR3, ctxt->ccr3);
	break;
    }

    /*  Enable caches  */
    asm volatile ("movl  %%cr0, %0\n\t"
		  "andl  $0xbfffffff, %0\n\t"
		  "movl  %0, %%cr0\n\t"
		  : "=r" (tmp) : : "memory");

    /*  Restore value of CR4  */
    if (boot_cpu_data.x86_capability & X86_FEATURE_PGE)
	asm volatile ("movl  %0, %%cr4"
		      : : "r" (ctxt->cr4val) : "memory");

    /*  Re-enable interrupts locally (if enabled previously)  */
    __restore_flags (ctxt->flags);
}   /*  End Function set_mtrr_done  */

/*  This function returns the number of variable MTRRs  */
static unsigned int get_num_var_ranges (void)
{
    unsigned long config, dummy;

    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 < 6) return 2; /* pre-Athlon CPUs */
	/* else fall through */
      case X86_VENDOR_INTEL:
	rdmsr (MTRRcap_MSR, config, dummy);
	return (config & 0xff);
	/*break;*/
      case X86_VENDOR_CYRIX:
	/*  Cyrix have 8 ARRs  */
      case X86_VENDOR_CENTAUR:
        /*  and Centaur has 8 MCR's  */
	return 8;
	/*break;*/
    }
    return 0;
}   /*  End Function get_num_var_ranges  */

/*  Returns non-zero if we have the write-combining memory type  */
static int have_wrcomb (void)
{
    unsigned long config, dummy;

    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 < 6) return 1; /* pre-Athlon CPUs */
	/* else fall through */
      case X86_VENDOR_INTEL:
	rdmsr (MTRRcap_MSR, config, dummy);
	return (config & (1<<10));
	/*break;*/
      case X86_VENDOR_CYRIX:
      case X86_VENDOR_CENTAUR:
	return 1;
	/*break;*/
    }
    return 0;
}   /*  End Function have_wrcomb  */

static void intel_get_mtrr (unsigned int reg, unsigned long *base,
			    unsigned long *size, mtrr_type *type)
{
    unsigned long dummy, mask_lo, base_lo;

    rdmsr (MTRRphysMask_MSR(reg), mask_lo, dummy);
    if ((mask_lo & 0x800) == 0) {
	/* Invalid (i.e. free) range. */
	*base = 0;
	*size = 0;
	*type = 0;
	return;
    }

    rdmsr(MTRRphysBase_MSR(reg), base_lo, dummy);

    /* We ignore the extra address bits (32-35). If someone wants to
       run x86 Linux on a machine with >4GB memory, this will be the
       least of their problems. */

    /* Clean up mask_lo so it gives the real address mask. */
    mask_lo = (mask_lo & 0xfffff000UL);
    /* This works correctly if size is a power of two, i.e. a
       contiguous range. */
    *size = ~(mask_lo - 1);

    *base = (base_lo & 0xfffff000UL);
    *type = (base_lo & 0xff);
}   /*  End Function intel_get_mtrr  */

static void cyrix_get_arr (unsigned int reg, unsigned long *base,
			   unsigned long *size, mtrr_type *type)
{
    unsigned long flags;
    unsigned char arr, ccr3, rcr, shift;

    arr = CX86_ARR_BASE + (reg << 1) + reg; /* avoid multiplication by 3 */

    /* Save flags and disable interrupts */
    __save_flags (flags); __cli ();

    ccr3 = getCx86 (CX86_CCR3);
    setCx86 (CX86_CCR3, (ccr3 & 0x0f) | 0x10);		/* enable MAPEN */
    ((unsigned char *) base)[3]  = getCx86 (arr);
    ((unsigned char *) base)[2]  = getCx86 (arr+1);
    ((unsigned char *) base)[1]  = getCx86 (arr+2);
    rcr = getCx86(CX86_RCR_BASE + reg);
    setCx86 (CX86_CCR3, ccr3);				/* disable MAPEN */

    /* Enable interrupts if it was enabled previously */
    __restore_flags (flags);
    shift = ((unsigned char *) base)[1] & 0x0f;
    *base &= 0xfffff000UL;

    /* Power of two, at least 4K on ARR0-ARR6, 256K on ARR7
     * Note: shift==0xf means 4G, this is unsupported.
     */
    if (shift)
      *size = (reg < 7 ? 0x800UL : 0x20000UL) << shift;
    else
      *size = 0;

    /* Bit 0 is Cache Enable on ARR7, Cache Disable on ARR0-ARR6 */
    if (reg < 7) {
      switch (rcr) {
	case  1: *type = MTRR_TYPE_UNCACHABLE; break;
	case  8: *type = MTRR_TYPE_WRBACK;     break;
	case  9: *type = MTRR_TYPE_WRCOMB;     break;
	case 24:
	default: *type = MTRR_TYPE_WRTHROUGH;  break;
      }
    } else {
      switch (rcr) {
	case  0: *type = MTRR_TYPE_UNCACHABLE; break;
	case  8: *type = MTRR_TYPE_WRCOMB;     break;
	case  9: *type = MTRR_TYPE_WRBACK;     break;
	case 25:
	default: *type = MTRR_TYPE_WRTHROUGH;  break;
      }
    }
}   /*  End Function cyrix_get_arr  */

static void amd_get_mtrr (unsigned int reg, unsigned long *base,
			  unsigned long *size, mtrr_type *type)
{
    unsigned long low, high;

    rdmsr (0xC0000085, low, high);
    /*  Upper dword is region 1, lower is region 0  */
    if (reg == 1) low = high;
    /*  The base masks off on the right alignment  */
    *base = low & 0xFFFE0000;
    *type = 0;
    if (low & 1) *type = MTRR_TYPE_UNCACHABLE;
    if (low & 2) *type = MTRR_TYPE_WRCOMB;
    if ( !(low & 3) )
    {
	*size = 0;
	return;
    }
    /*
     *	This needs a little explaining. The size is stored as an
     *	inverted mask of bits of 128K granularity 15 bits long offset
     *	2 bits
     *
     *	So to get a size we do invert the mask and add 1 to the lowest
     *	mask bit (4 as its 2 bits in). This gives us a size we then shift
     *	to turn into 128K blocks
     *
     *	eg		111 1111 1111 1100      is 512K
     *
     *	invert		000 0000 0000 0011
     *	+1		000 0000 0000 0100
     *	*128K	...
     */
    low = (~low) & 0x1FFFC;
    *size = (low + 4) << 15;
    return;
}   /*  End Function amd_get_mtrr  */

static struct
{
    unsigned long high;
    unsigned long low;
} centaur_mcr[8];

static void centaur_get_mcr (unsigned int reg, unsigned long *base,
			     unsigned long *size, mtrr_type *type)
{
    *base = centaur_mcr[reg].high & 0xfffff000;
    *size = (~(centaur_mcr[reg].low & 0xfffff000))+1;
    *type = MTRR_TYPE_WRCOMB;	/*  If it is there, it is write-combining  */
}   /*  End Function centaur_get_mcr  */

static void (*get_mtrr) (unsigned int reg, unsigned long *base,
			 unsigned long *size, mtrr_type *type) = NULL;

static void intel_set_mtrr_up (unsigned int reg, unsigned long base,
			       unsigned long size, mtrr_type type, int do_safe)
/*  [SUMMARY] Set variable MTRR register on the local CPU.
    <reg> The register to set.
    <base> The base address of the region.
    <size> The size of the region. If this is 0 the region is disabled.
    <type> The type of the region.
    <do_safe> If TRUE, do the change safely. If FALSE, safety measures should
    be done externally.
    [RETURNS] Nothing.
*/
{
    struct set_mtrr_context ctxt;

    if (do_safe) set_mtrr_prepare (&ctxt);
    if (size == 0)
    {
	/* The invalid bit is kept in the mask, so we simply clear the
	   relevant mask register to disable a range. */
	wrmsr (MTRRphysMask_MSR (reg), 0, 0);
    }
    else
    {
	wrmsr (MTRRphysBase_MSR (reg), base | type, 0);
	wrmsr (MTRRphysMask_MSR (reg), ~(size - 1) | 0x800, 0);
    }
    if (do_safe) set_mtrr_done (&ctxt);
}   /*  End Function intel_set_mtrr_up  */

static void cyrix_set_arr_up (unsigned int reg, unsigned long base,
			      unsigned long size, mtrr_type type, int do_safe)
{
    struct set_mtrr_context ctxt;
    unsigned char arr, arr_type, arr_size;

    arr = CX86_ARR_BASE + (reg << 1) + reg; /* avoid multiplication by 3 */

    /* count down from 32M (ARR0-ARR6) or from 2G (ARR7) */
    size >>= (reg < 7 ? 12 : 18);
    size &= 0x7fff; /* make sure arr_size <= 14 */
    for(arr_size = 0; size; arr_size++, size >>= 1);

    if (reg<7) {
      switch (type) {
	case MTRR_TYPE_UNCACHABLE:	arr_type =  1; break;
	case MTRR_TYPE_WRCOMB:		arr_type =  9; break;
	case MTRR_TYPE_WRTHROUGH:	arr_type = 24; break;
	default:			arr_type =  8; break;
      }
    } else {
      switch (type) {
	case MTRR_TYPE_UNCACHABLE:	arr_type =  0; break;
	case MTRR_TYPE_WRCOMB:		arr_type =  8; break;
	case MTRR_TYPE_WRTHROUGH:	arr_type = 25; break;
	default:			arr_type =  9; break;
      }
    }

    if (do_safe) set_mtrr_prepare (&ctxt);
    setCx86(arr,    ((unsigned char *) &base)[3]);
    setCx86(arr+1,  ((unsigned char *) &base)[2]);
    setCx86(arr+2, (((unsigned char *) &base)[1]) | arr_size);
    setCx86(CX86_RCR_BASE + reg, arr_type);
    if (do_safe) set_mtrr_done (&ctxt);
}   /*  End Function cyrix_set_arr_up  */

static void amd_set_mtrr_up (unsigned int reg, unsigned long base,
			     unsigned long size, mtrr_type type, int do_safe)
/*  [SUMMARY] Set variable MTRR register on the local CPU.
    <reg> The register to set.
    <base> The base address of the region.
    <size> The size of the region. If this is 0 the region is disabled.
    <type> The type of the region.
    <do_safe> If TRUE, do the change safely. If FALSE, safety measures should
    be done externally.
    [RETURNS] Nothing.
*/
{
    u32 low, high;
    struct set_mtrr_context ctxt;

    if (do_safe) set_mtrr_prepare (&ctxt);
    /*
     *	Low is MTRR0 , High MTRR 1
     */
    rdmsr (0xC0000085, low, high);
    /*
     *	Blank to disable
     */
    if (size == 0)
	*(reg ? &high : &low) = 0;
    else
	/* Set the register to the base (already shifted for us), the
	   type (off by one) and an inverted bitmask of the size
	   The size is the only odd bit. We are fed say 512K
	   We invert this and we get 111 1111 1111 1011 but
	   if you subtract one and invert you get the desired
	   111 1111 1111 1100 mask
	   */
	*(reg ? &high : &low)=(((~(size-1))>>15)&0x0001FFFC)|base|(type+1);
    /*
     *	The writeback rule is quite specific. See the manual. Its
     *	disable local interrupts, write back the cache, set the mtrr
     */
    __asm__ __volatile__ ("wbinvd" : : : "memory");
    wrmsr (0xC0000085, low, high);
    if (do_safe) set_mtrr_done (&ctxt);
}   /*  End Function amd_set_mtrr_up  */


static void centaur_set_mcr_up (unsigned int reg, unsigned long base,
				unsigned long size, mtrr_type type,
				int do_safe)
{
    struct set_mtrr_context ctxt;
    unsigned long low, high;

    if (do_safe) set_mtrr_prepare( &ctxt );
    if (size == 0)
    {
        /*  Disable  */
        high = low = 0;
    }
    else
    {
        high = base & 0xfffff000; /* base works on 4K pages... */
        low = ((~(size-1))&0xfffff000);
        low |= 0x1f;		  /* only support write-combining... */
    }
    centaur_mcr[reg].high = high;
    centaur_mcr[reg].low = low;
    wrmsr (0x110 + reg, low, high);
    if (do_safe) set_mtrr_done( &ctxt );
}   /*  End Function centaur_set_mtrr_up  */

static void (*set_mtrr_up) (unsigned int reg, unsigned long base,
			    unsigned long size, mtrr_type type,
			    int do_safe) = NULL;

#ifdef __SMP__

struct mtrr_var_range
{
    unsigned long base_lo;
    unsigned long base_hi;
    unsigned long mask_lo;
    unsigned long mask_hi;
};


/*  Get the MSR pair relating to a var range  */
static void __init get_mtrr_var_range (unsigned int index,
					   struct mtrr_var_range *vr)
{
    rdmsr (MTRRphysBase_MSR (index), vr->base_lo, vr->base_hi);
    rdmsr (MTRRphysMask_MSR (index), vr->mask_lo, vr->mask_hi);
}   /*  End Function get_mtrr_var_range  */


/*  Set the MSR pair relating to a var range. Returns TRUE if
    changes are made  */
static int __init set_mtrr_var_range_testing (unsigned int index,
						  struct mtrr_var_range *vr)
{
    unsigned int lo, hi;
    int changed = FALSE;

    rdmsr(MTRRphysBase_MSR(index), lo, hi);
    if ((vr->base_lo & 0xfffff0ffUL) != (lo & 0xfffff0ffUL)
	|| (vr->base_hi & 0xfUL) != (hi & 0xfUL)) {
	wrmsr(MTRRphysBase_MSR(index), vr->base_lo, vr->base_hi);
	changed = TRUE;
    }

    rdmsr(MTRRphysMask_MSR(index), lo, hi);

    if ((vr->mask_lo & 0xfffff800UL) != (lo & 0xfffff800UL)
	|| (vr->mask_hi & 0xfUL) != (hi & 0xfUL)) {
	wrmsr(MTRRphysMask_MSR(index), vr->mask_lo, vr->mask_hi);
	changed = TRUE;
    }
    return changed;
}   /*  End Function set_mtrr_var_range_testing  */

static void __init get_fixed_ranges(mtrr_type *frs)
{
    unsigned long *p = (unsigned long *)frs;
    int i;

    rdmsr(MTRRfix64K_00000_MSR, p[0], p[1]);

    for (i = 0; i < 2; i++)
	rdmsr(MTRRfix16K_80000_MSR + i, p[2 + i*2], p[3 + i*2]);
    for (i = 0; i < 8; i++)
	rdmsr(MTRRfix4K_C0000_MSR + i, p[6 + i*2], p[7 + i*2]);
}   /*  End Function get_fixed_ranges  */

static int __init set_fixed_ranges_testing(mtrr_type *frs)
{
    unsigned long *p = (unsigned long *)frs;
    int changed = FALSE;
    int i;
    unsigned long lo, hi;

    rdmsr(MTRRfix64K_00000_MSR, lo, hi);
    if (p[0] != lo || p[1] != hi) {
	wrmsr(MTRRfix64K_00000_MSR, p[0], p[1]);
	changed = TRUE;
    }

    for (i = 0; i < 2; i++) {
	rdmsr(MTRRfix16K_80000_MSR + i, lo, hi);
	if (p[2 + i*2] != lo || p[3 + i*2] != hi) {
	    wrmsr(MTRRfix16K_80000_MSR + i, p[2 + i*2], p[3 + i*2]);
	    changed = TRUE;
	}
    }

    for (i = 0; i < 8; i++) {
	rdmsr(MTRRfix4K_C0000_MSR + i, lo, hi);
	if (p[6 + i*2] != lo || p[7 + i*2] != hi) {
	    wrmsr(MTRRfix4K_C0000_MSR + i, p[6 + i*2], p[7 + i*2]);
	    changed = TRUE;
	}
    }
    return changed;
}   /*  End Function set_fixed_ranges_testing  */

struct mtrr_state
{
    unsigned int num_var_ranges;
    struct mtrr_var_range *var_ranges;
    mtrr_type fixed_ranges[NUM_FIXED_RANGES];
    unsigned char enabled;
    mtrr_type def_type;
};


/*  Grab all of the MTRR state for this CPU into *state  */
static void __init get_mtrr_state(struct mtrr_state *state)
{
    unsigned int nvrs, i;
    struct mtrr_var_range *vrs;
    unsigned long lo, dummy;

    nvrs = state->num_var_ranges = get_num_var_ranges();
    vrs = state->var_ranges
              = kmalloc (nvrs * sizeof (struct mtrr_var_range), GFP_KERNEL);
    if (vrs == NULL)
	nvrs = state->num_var_ranges = 0;

    for (i = 0; i < nvrs; i++)
	get_mtrr_var_range (i, &vrs[i]);
    get_fixed_ranges (state->fixed_ranges);

    rdmsr (MTRRdefType_MSR, lo, dummy);
    state->def_type = (lo & 0xff);
    state->enabled = (lo & 0xc00) >> 10;
}   /*  End Function get_mtrr_state  */


/*  Free resources associated with a struct mtrr_state  */
static void __init finalize_mtrr_state(struct mtrr_state *state)
{
    if (state->var_ranges) kfree (state->var_ranges);
}   /*  End Function finalize_mtrr_state  */


static unsigned long __init set_mtrr_state (struct mtrr_state *state,
						struct set_mtrr_context *ctxt)
/*  [SUMMARY] Set the MTRR state for this CPU.
    <state> The MTRR state information to read.
    <ctxt> Some relevant CPU context.
    [NOTE] The CPU must already be in a safe state for MTRR changes.
    [RETURNS] 0 if no changes made, else a mask indication what was changed.
*/
{
    unsigned int i;
    unsigned long change_mask = 0;

    for (i = 0; i < state->num_var_ranges; i++)
	if ( set_mtrr_var_range_testing (i, &state->var_ranges[i]) )
	    change_mask |= MTRR_CHANGE_MASK_VARIABLE;

    if ( set_fixed_ranges_testing(state->fixed_ranges) )
	change_mask |= MTRR_CHANGE_MASK_FIXED;
    /*  Set_mtrr_restore restores the old value of MTRRdefType,
	so to set it we fiddle with the saved value  */
    if ((ctxt->deftype_lo & 0xff) != state->def_type
	|| ((ctxt->deftype_lo & 0xc00) >> 10) != state->enabled)
    {
	ctxt->deftype_lo |= (state->def_type | state->enabled << 10);
	change_mask |= MTRR_CHANGE_MASK_DEFTYPE;
    }

    return change_mask;
}   /*  End Function set_mtrr_state  */


static atomic_t undone_count;
static volatile int wait_barrier_execute = FALSE;
static volatile int wait_barrier_cache_enable = FALSE;

struct set_mtrr_data
{
    unsigned long smp_base;
    unsigned long smp_size;
    unsigned int smp_reg;
    mtrr_type smp_type;
};

static void ipi_handler (void *info)
/*  [SUMMARY] Synchronisation handler. Executed by "other" CPUs.
    [RETURNS] Nothing.
*/
{
    struct set_mtrr_data *data = info;
    struct set_mtrr_context ctxt;

    set_mtrr_prepare (&ctxt);
    /*  Notify master that I've flushed and disabled my cache  */
    atomic_dec (&undone_count);
    while (wait_barrier_execute) barrier ();
    /*  The master has cleared me to execute  */
    (*set_mtrr_up) (data->smp_reg, data->smp_base, data->smp_size,
		    data->smp_type, FALSE);
    /*  Notify master CPU that I've executed the function  */
    atomic_dec (&undone_count);
    /*  Wait for master to clear me to enable cache and return  */
    while (wait_barrier_cache_enable) barrier ();
    set_mtrr_done (&ctxt);
}   /*  End Function ipi_handler  */

static void set_mtrr_smp (unsigned int reg, unsigned long base,
			  unsigned long size, mtrr_type type)
{
    struct set_mtrr_data data;
    struct set_mtrr_context ctxt;

    data.smp_reg = reg;
    data.smp_base = base;
    data.smp_size = size;
    data.smp_type = type;
    wait_barrier_execute = TRUE;
    wait_barrier_cache_enable = TRUE;
    atomic_set (&undone_count, smp_num_cpus - 1);
    /*  Flush and disable the local CPU's cache	and start the ball rolling on
	other CPUs  */
    set_mtrr_prepare (&ctxt);
    if (smp_call_function (ipi_handler, &data, 1, 0) != 0)
	panic ("mtrr: timed out waiting for other CPUs\n");
    /*  Wait for all other CPUs to flush and disable their caches  */
    while (atomic_read (&undone_count) > 0) barrier ();
    /* Set up for completion wait and then release other CPUs to change MTRRs*/
    atomic_set (&undone_count, smp_num_cpus - 1);
    wait_barrier_execute = FALSE;
    (*set_mtrr_up) (reg, base, size, type, FALSE);
    /*  Now wait for other CPUs to complete the function  */
    while (atomic_read (&undone_count) > 0) barrier ();
    /*  Now all CPUs should have finished the function. Release the barrier to
	allow them to re-enable their caches and return from their interrupt,
	then enable the local cache and return  */
    wait_barrier_cache_enable = FALSE;
    set_mtrr_done (&ctxt);
}   /*  End Function set_mtrr_smp  */


/*  Some BIOS's are fucked and don't set all MTRRs the same!  */
static void __init mtrr_state_warn(unsigned long mask)
{
    if (!mask) return;
    if (mask & MTRR_CHANGE_MASK_FIXED)
	printk ("mtrr: your CPUs had inconsistent fixed MTRR settings\n");
    if (mask & MTRR_CHANGE_MASK_VARIABLE)
	printk ("mtrr: your CPUs had inconsistent variable MTRR settings\n");
    if (mask & MTRR_CHANGE_MASK_DEFTYPE)
	printk ("mtrr: your CPUs had inconsistent MTRRdefType settings\n");
    printk ("mtrr: probably your BIOS does not setup all CPUs\n");
}   /*  End Function mtrr_state_warn  */

#endif  /*  __SMP__  */

static char *attrib_to_str (int x)
{
    return (x <= 6) ? mtrr_strings[x] : "?";
}   /*  End Function attrib_to_str  */

static void init_table (void)
{
    int i, max;

    max = get_num_var_ranges ();
    if ( ( usage_table = kmalloc (max * sizeof *usage_table, GFP_KERNEL) )
	 == NULL )
    {
	printk ("mtrr: could not allocate\n");
	return;
    }
    for (i = 0; i < max; i++) usage_table[i] = 1;
#ifdef CONFIG_PROC_FS
    if ( ( ascii_buffer = kmalloc (max * LINE_SIZE, GFP_KERNEL) ) == NULL )
    {
	printk ("mtrr: could not allocate\n");
	return;
    }
    ascii_buf_bytes = 0;
    compute_ascii ();
#endif
}   /*  End Function init_table  */

static int generic_get_free_region (unsigned long base, unsigned long size)
/*  [SUMMARY] Get a free MTRR.
    <base> The starting (base) address of the region.
    <size> The size (in bytes) of the region.
    [RETURNS] The index of the region on success, else -1 on error.
*/
{
    int i, max;
    mtrr_type ltype;
    unsigned long lbase, lsize;

    max = get_num_var_ranges ();
    for (i = 0; i < max; ++i)
    {
	(*get_mtrr) (i, &lbase, &lsize, &ltype);
	if (lsize < 1) return i;
    }
    return -ENOSPC;
}   /*  End Function generic_get_free_region  */

static int cyrix_get_free_region (unsigned long base, unsigned long size)
/*  [SUMMARY] Get a free ARR.
    <base> The starting (base) address of the region.
    <size> The size (in bytes) of the region.
    [RETURNS] The index of the region on success, else -1 on error.
*/
{
    int i;
    mtrr_type ltype;
    unsigned long lbase, lsize;

    /* If we are to set up a region >32M then look at ARR7 immediately */
    if (size > 0x2000000UL) {
	cyrix_get_arr (7, &lbase, &lsize, &ltype);
	if (lsize < 1) return 7;
    /* else try ARR0-ARR6 first */
    } else {
	for (i = 0; i < 7; i++)
	{
	    cyrix_get_arr (i, &lbase, &lsize, &ltype);
	    if ((i == 3) && arr3_protected) continue;
	    if (lsize < 1) return i;
	}
	/* ARR0-ARR6 isn't free, try ARR7 but its size must be at least 256K */
	cyrix_get_arr (i, &lbase, &lsize, &ltype);
	if ((lsize < 1) && (size >= 0x40000)) return i;
    }
    return -ENOSPC;
}   /*  End Function cyrix_get_free_region  */

static int (*get_free_region) (unsigned long base,
			       unsigned long size) = generic_get_free_region;

int mtrr_add (unsigned long base, unsigned long size, unsigned int type,
	      char increment)
/*  [SUMMARY] Add an MTRR entry.
    <base> The starting (base) address of the region.
    <size> The size (in bytes) of the region.
    <type> The type of the new region.
    <increment> If true and the region already exists, the usage count will be
    incremented.
    [RETURNS] The MTRR register on success, else a negative number indicating
    the error code.
    [NOTE] This routine uses a spinlock.
*/
{
    int i, max;
    mtrr_type ltype;
    unsigned long lbase, lsize, last;

    if ( !(boot_cpu_data.x86_capability & X86_FEATURE_MTRR) ) return -ENODEV;
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 < 6) { /* pre-Athlon CPUs */
	  /* Apply the K6 block alignment and size rules
	     In order
		o Uncached or gathering only
		o 128K or bigger block
		o Power of 2 block
		o base suitably aligned to the power
	    */
	  if (type > MTRR_TYPE_WRCOMB || size < (1 << 17) ||
	      (size & ~(size-1))-size || (base & (size-1)))
	      return -EINVAL;
	  break;
	} /* else fall through */
      case X86_VENDOR_INTEL:
	/*  Double check for Intel, we may run on Athlon. */
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL) {
	  /*  For Intel PPro stepping <= 7, must be 4 MiB aligned  */
	  if ( (boot_cpu_data.x86 == 6) && (boot_cpu_data.x86_model == 1) &&
	       (boot_cpu_data.x86_mask <= 7) && ( base & ( (1 << 22) - 1 ) ) )
	  {
	      printk ("mtrr: base(0x%lx) is not 4 MiB aligned\n", base);
	      return -EINVAL;
	  }
	}
	/*  Fall through  */
      case X86_VENDOR_CYRIX:
      case X86_VENDOR_CENTAUR:
	if ( (base & 0xfff) || (size & 0xfff) )
	{
	    printk ("mtrr: size and base must be multiples of 4 kiB\n");
	    printk ("mtrr: size: %lx  base: %lx\n", size, base);
	    return -EINVAL;
	}
        if (boot_cpu_data.x86_vendor == X86_VENDOR_CENTAUR)
	{
	    if (type != MTRR_TYPE_WRCOMB)
	    {
		printk ("mtrr: only write-combining is supported\n");
		return -EINVAL;
	    }
	}
	else if (base + size < 0x100000)
	{
	    printk ("mtrr: cannot set region below 1 MiB (0x%lx,0x%lx)\n",
		    base, size);
	    return -EINVAL;
	}
	/*  Check upper bits of base and last are equal and lower bits are 0
	    for base and 1 for last  */
	last = base + size - 1;
	for (lbase = base; !(lbase & 1) && (last & 1);
	     lbase = lbase >> 1, last = last >> 1);
	if (lbase != last)
	{
	    printk ("mtrr: base(0x%lx) is not aligned on a size(0x%lx) boundary\n",
		    base, size);
	    return -EINVAL;
	}
	break;
      default:
	return -EINVAL;
	/*break;*/
    }
    if (type >= MTRR_NUM_TYPES)
    {
	printk ("mtrr: type: %u illegal\n", type);
	return -EINVAL;
    }
    /*  If the type is WC, check that this processor supports it  */
    if ( (type == MTRR_TYPE_WRCOMB) && !have_wrcomb () )
    {
        printk ("mtrr: your processor doesn't support write-combining\n");
        return -ENOSYS;
    }
    increment = increment ? 1 : 0;
    max = get_num_var_ranges ();
    /*  Search for existing MTRR  */
    down(&main_lock);
    for (i = 0; i < max; ++i)
    {
	(*get_mtrr) (i, &lbase, &lsize, &ltype);
	if (base >= lbase + lsize) continue;
	if ( (base < lbase) && (base + size <= lbase) ) continue;
	/*  At this point we know there is some kind of overlap/enclosure  */
	if ( (base < lbase) || (base + size > lbase + lsize) )
	{
	    up(&main_lock);
	    printk ("mtrr: 0x%lx,0x%lx overlaps existing 0x%lx,0x%lx\n",
		    base, size, lbase, lsize);
	    return -EINVAL;
	}
	/*  New region is enclosed by an existing region  */
	if (ltype != type)
	{
	    if (type == MTRR_TYPE_UNCACHABLE) continue;
	    up(&main_lock);
	    printk ( "mtrr: type mismatch for %lx,%lx old: %s new: %s\n",
		     base, size, attrib_to_str (ltype), attrib_to_str (type) );
	    return -EINVAL;
	}
	if (increment) ++usage_table[i];
	compute_ascii ();
	up(&main_lock);
	return i;
    }
    /*  Search for an empty MTRR  */
    i = (*get_free_region) (base, size);
    if (i < 0)
    {
	up(&main_lock);
	printk ("mtrr: no more MTRRs available\n");
	return i;
    }
    set_mtrr (i, base, size, type);
    usage_table[i] = 1;
    compute_ascii ();
    up(&main_lock);
    return i;
}   /*  End Function mtrr_add  */

int mtrr_del (int reg, unsigned long base, unsigned long size)
/*  [SUMMARY] Delete MTRR/decrement usage count.
    <reg> The register. If this is less than 0 then <<base>> and <<size>> must
    be supplied.
    <base> The base address of the region. This is ignored if <<reg>> is >= 0.
    <size> The size of the region. This is ignored if <<reg>> is >= 0.
    [RETURNS] The register on success, else a negative number indicating
    the error code.
    [NOTE] This routine uses a spinlock.
*/
{
    int i, max;
    mtrr_type ltype;
    unsigned long lbase, lsize;

    if ( !(boot_cpu_data.x86_capability & X86_FEATURE_MTRR) ) return -ENODEV;
    max = get_num_var_ranges ();
    down(&main_lock);
    if (reg < 0)
    {
	/*  Search for existing MTRR  */
	for (i = 0; i < max; ++i)
	{
	    (*get_mtrr) (i, &lbase, &lsize, &ltype);
	    if ( (lbase == base) && (lsize == size) )
	    {
		reg = i;
		break;
	    }
	}
	if (reg < 0)
	{
	    up(&main_lock);
	    printk ("mtrr: no MTRR for %lx,%lx found\n", base, size);
	    return -EINVAL;
	}
    }
    if (reg >= max)
    {
	up(&main_lock);
	printk ("mtrr: register: %d too big\n", reg);
	return -EINVAL;
    }
    if (boot_cpu_data.x86_vendor == X86_VENDOR_CYRIX)
    {
	if ((reg == 3) && arr3_protected)
	{
	    up(&main_lock);
	    printk ("mtrr: ARR3 cannot be changed\n");
	    return -EINVAL;
	}
    }
    (*get_mtrr) (reg, &lbase, &lsize, &ltype);
    if (lsize < 1)
    {
	up(&main_lock);
	printk ("mtrr: MTRR %d not used\n", reg);
	return -EINVAL;
    }
    if (usage_table[reg] < 1)
    {
	up(&main_lock);
	printk ("mtrr: reg: %d has count=0\n", reg);
	return -EINVAL;
    }
    if (--usage_table[reg] < 1) set_mtrr (reg, 0, 0, 0);
    compute_ascii ();
    up(&main_lock);
    return reg;
}   /*  End Function mtrr_del  */

#ifdef CONFIG_PROC_FS

static int mtrr_file_add (unsigned long base, unsigned long size,
			  unsigned int type, char increment, struct file *file)
{
    int reg, max;
    unsigned int *fcount = file->private_data;

    max = get_num_var_ranges ();
    if (fcount == NULL)
    {
	if ( ( fcount = kmalloc (max * sizeof *fcount, GFP_KERNEL) ) == NULL )
	{
	    printk ("mtrr: could not allocate\n");
	    return -ENOMEM;
	}
	memset (fcount, 0, max * sizeof *fcount);
	file->private_data = fcount;
    }
    reg = mtrr_add (base, size, type, 1);
    if (reg >= 0) ++fcount[reg];
    return reg;
}   /*  End Function mtrr_file_add  */

static int mtrr_file_del (unsigned long base, unsigned long size,
			  struct file *file)
{
    int reg;
    unsigned int *fcount = file->private_data;

    reg = mtrr_del (-1, base, size);
    if (reg < 0) return reg;
    if (fcount == NULL) return reg;
    if (fcount[reg] < 1) return -EINVAL;
    --fcount[reg];
    return reg;
}   /*  End Function mtrr_file_del  */

static ssize_t mtrr_read (struct file *file, char *buf, size_t len,
			  loff_t *ppos)
{
    if (*ppos >= ascii_buf_bytes) return 0;
    if (*ppos + len > ascii_buf_bytes) len = ascii_buf_bytes - *ppos;
    if ( copy_to_user (buf, ascii_buffer + *ppos, len) ) return -EFAULT;
    *ppos += len;
    return len;
}   /*  End Function mtrr_read  */

static ssize_t mtrr_write (struct file *file, const char *buf, size_t len,
			   loff_t *ppos)
/*  Format of control line:
    "base=%lx size=%lx type=%s"     OR:
    "disable=%d"
*/
{
    int i, err;
    unsigned long reg, base, size;
    char *ptr;
    char line[LINE_SIZE];

    if ( !suser () ) return -EPERM;
    /*  Can't seek (pwrite) on this device  */
    if (ppos != &file->f_pos) return -ESPIPE;
    memset (line, 0, LINE_SIZE);
    if (len > LINE_SIZE) len = LINE_SIZE;
    if ( copy_from_user (line, buf, len - 1) ) return -EFAULT;
    ptr = line + strlen (line) - 1;
    if (*ptr == '\n') *ptr = '\0';
    if ( !strncmp (line, "disable=", 8) )
    {
	reg = simple_strtoul (line + 8, &ptr, 0);
	err = mtrr_del (reg, 0, 0);
	if (err < 0) return err;
	return len;
    }
    if ( strncmp (line, "base=", 5) )
    {
	printk ("mtrr: no \"base=\" in line: \"%s\"\n", line);
	return -EINVAL;
    }
    base = simple_strtoul (line + 5, &ptr, 0);
    for (; isspace (*ptr); ++ptr);
    if ( strncmp (ptr, "size=", 5) )
    {
	printk ("mtrr: no \"size=\" in line: \"%s\"\n", line);
	return -EINVAL;
    }
    size = simple_strtoul (ptr + 5, &ptr, 0);
    for (; isspace (*ptr); ++ptr);
    if ( strncmp (ptr, "type=", 5) )
    {
	printk ("mtrr: no \"type=\" in line: \"%s\"\n", line);
	return -EINVAL;
    }
    ptr += 5;
    for (; isspace (*ptr); ++ptr);
    for (i = 0; i < MTRR_NUM_TYPES; ++i)
    {
	if ( strcmp (ptr, mtrr_strings[i]) ) continue;
	err = mtrr_add (base, size, i, 1);
	if (err < 0) return err;
	return len;
    }
    printk ("mtrr: illegal type: \"%s\"\n", ptr);
    return -EINVAL;
}   /*  End Function mtrr_write  */

static int mtrr_ioctl (struct inode *inode, struct file *file,
		       unsigned int cmd, unsigned long arg)
{
    int err;
    mtrr_type type;
    struct mtrr_sentry sentry;
    struct mtrr_gentry gentry;

    switch (cmd)
    {
      default:
	return -ENOIOCTLCMD;
      case MTRRIOC_ADD_ENTRY:
	if ( !suser () ) return -EPERM;
	if ( copy_from_user (&sentry, (void *) arg, sizeof sentry) )
	    return -EFAULT;
	err = mtrr_file_add (sentry.base, sentry.size, sentry.type, 1, file);
	if (err < 0) return err;
	break;
      case MTRRIOC_SET_ENTRY:
	if ( !suser () ) return -EPERM;
	if ( copy_from_user (&sentry, (void *) arg, sizeof sentry) )
	    return -EFAULT;
	err = mtrr_add (sentry.base, sentry.size, sentry.type, 0);
	if (err < 0) return err;
	break;
      case MTRRIOC_DEL_ENTRY:
	if ( !suser () ) return -EPERM;
	if ( copy_from_user (&sentry, (void *) arg, sizeof sentry) )
	    return -EFAULT;
	err = mtrr_file_del (sentry.base, sentry.size, file);
	if (err < 0) return err;
	break;
      case MTRRIOC_KILL_ENTRY:
	if ( !suser () ) return -EPERM;
	if ( copy_from_user (&sentry, (void *) arg, sizeof sentry) )
	    return -EFAULT;
	err = mtrr_del (-1, sentry.base, sentry.size);
	if (err < 0) return err;
	break;
      case MTRRIOC_GET_ENTRY:
	if ( copy_from_user (&gentry, (void *) arg, sizeof gentry) )
	    return -EFAULT;
	if ( gentry.regnum >= get_num_var_ranges () ) return -EINVAL;
	(*get_mtrr) (gentry.regnum, &gentry.base, &gentry.size, &type);
	gentry.type = type;
	if ( copy_to_user ( (void *) arg, &gentry, sizeof gentry) )
	     return -EFAULT;
	break;
    }
    return 0;
}   /*  End Function mtrr_ioctl  */

static int mtrr_close (struct inode *ino, struct file *file)
{
    int i, max;
    unsigned int *fcount = file->private_data;

    MOD_DEC_USE_COUNT;
    if (fcount == NULL) return 0;
    max = get_num_var_ranges ();
    for (i = 0; i < max; ++i)
    {
	while (fcount[i] > 0)
	{
	    if (mtrr_del (i, 0, 0) < 0) printk ("mtrr: reg %d not used\n", i);
	    --fcount[i];
	}
    }
    kfree (fcount);
    file->private_data = NULL;
    return 0;
}   /*  End Function mtrr_close  */

static struct file_operations mtrr_fops =
{
    NULL,        /*  Seek              */
    mtrr_read,   /*  Read              */
    mtrr_write,  /*  Write             */
    NULL,        /*  Readdir           */
    NULL,        /*  Poll              */
    mtrr_ioctl,  /*  IOctl             */
    NULL,        /*  MMAP              */
    NULL,	 /*  Open              */
    NULL,        /*  Flush             */
    mtrr_close,  /*  Release           */
    NULL,        /*  Fsync             */
    NULL,        /*  Fasync            */
    NULL,        /*  CheckMediaChange  */
    NULL,        /*  Revalidate        */
    NULL,        /*  Lock              */
};

static struct inode_operations proc_mtrr_inode_operations = {
	&mtrr_fops,             /* default property file-ops */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* get_block */
	NULL,			/* readpage */
	NULL,			/* writepage */
	NULL,			/* flushpage */
	NULL,			/* truncate */
	NULL,			/* permission */
	NULL,			/* smap */
	NULL			/* revalidate */
};

static struct proc_dir_entry *proc_root_mtrr;

static void compute_ascii (void)
{
    char factor;
    int i, max;
    mtrr_type type;
    unsigned long base, size;

    ascii_buf_bytes = 0;
    max = get_num_var_ranges ();
    for (i = 0; i < max; i++)
    {
	(*get_mtrr) (i, &base, &size, &type);
	if (size < 1) usage_table[i] = 0;
	else
	{
	    if (size < 0x100000)
	    {
		/* 1MB */
		factor = 'k';
		size >>= 10;
	    }
	    else
	    {
		factor = 'M';
		size >>= 20;
	    }
	    sprintf
		(ascii_buffer + ascii_buf_bytes,
		 "reg%02i: base=0x%08lx (%4liMB), size=%4li%cB: %s, count=%d\n",
		 i, base, base>>20, size, factor,
		 attrib_to_str (type), usage_table[i]);
	    ascii_buf_bytes += strlen (ascii_buffer + ascii_buf_bytes);
	}
    }
    proc_root_mtrr->size = ascii_buf_bytes;
}   /*  End Function compute_ascii  */

#endif  /*  CONFIG_PROC_FS  */

EXPORT_SYMBOL(mtrr_add);
EXPORT_SYMBOL(mtrr_del);

#ifdef __SMP__

typedef struct {
  unsigned long base;
  unsigned long size;
  mtrr_type type;
} arr_state_t;

arr_state_t arr_state[8] __initdata = {
  {0UL,0UL,0UL}, {0UL,0UL,0UL}, {0UL,0UL,0UL}, {0UL,0UL,0UL},
  {0UL,0UL,0UL}, {0UL,0UL,0UL}, {0UL,0UL,0UL}, {0UL,0UL,0UL}
};

unsigned char ccr_state[7] __initdata = { 0, 0, 0, 0, 0, 0, 0 };

static void __init cyrix_arr_init_secondary(void)
{
    struct set_mtrr_context ctxt;
    int i;

    set_mtrr_prepare (&ctxt); /* flush cache and enable MAPEN */

     /* the CCRs are not contiguous */
    for(i=0; i<4; i++) setCx86(CX86_CCR0 + i, ccr_state[i]);
    for(   ; i<7; i++) setCx86(CX86_CCR4 + i, ccr_state[i]);
    for(i=0; i<8; i++)
      cyrix_set_arr_up(i,
        arr_state[i].base, arr_state[i].size, arr_state[i].type, FALSE);

    set_mtrr_done (&ctxt); /* flush cache and disable MAPEN */
}   /*  End Function cyrix_arr_init_secondary  */

#endif

/*
 * On Cyrix 6x86(MX) and M II the ARR3 is special: it has connection
 * with the SMM (System Management Mode) mode. So we need the following:
 * Check whether SMI_LOCK (CCR3 bit 0) is set
 *   if it is set, write a warning message: ARR3 cannot be changed!
 *     (it cannot be changed until the next processor reset)
 *   if it is reset, then we can change it, set all the needed bits:
 *   - disable access to SMM memory through ARR3 range (CCR1 bit 7 reset)
 *   - disable access to SMM memory (CCR1 bit 2 reset)
 *   - disable SMM mode (CCR1 bit 1 reset)
 *   - disable write protection of ARR3 (CCR6 bit 1 reset)
 *   - (maybe) disable ARR3
 * Just to be sure, we enable ARR usage by the processor (CCR5 bit 5 set)
 */
static void __init cyrix_arr_init(void)
{
    struct set_mtrr_context ctxt;
    unsigned char ccr[7];
    int ccrc[7] = { 0, 0, 0, 0, 0, 0, 0 };
#ifdef __SMP__
    int i;
#endif

    set_mtrr_prepare (&ctxt); /* flush cache and enable MAPEN */

    /* Save all CCRs locally */
    ccr[0] = getCx86 (CX86_CCR0);
    ccr[1] = getCx86 (CX86_CCR1);
    ccr[2] = getCx86 (CX86_CCR2);
    ccr[3] = ctxt.ccr3;
    ccr[4] = getCx86 (CX86_CCR4);
    ccr[5] = getCx86 (CX86_CCR5);
    ccr[6] = getCx86 (CX86_CCR6);

    if (ccr[3] & 1) {
      ccrc[3] = 1;
      arr3_protected = 1;
    } else {
      /* Disable SMM mode (bit 1), access to SMM memory (bit 2) and
       * access to SMM memory through ARR3 (bit 7).
       */
      if (ccr[1] & 0x80) { ccr[1] &= 0x7f; ccrc[1] |= 0x80; }
      if (ccr[1] & 0x04) { ccr[1] &= 0xfb; ccrc[1] |= 0x04; }
      if (ccr[1] & 0x02) { ccr[1] &= 0xfd; ccrc[1] |= 0x02; }
      arr3_protected = 0;
      if (ccr[6] & 0x02) {
        ccr[6] &= 0xfd; ccrc[6] = 1; /* Disable write protection of ARR3. */
        setCx86 (CX86_CCR6, ccr[6]);
      }
      /* Disable ARR3. This is safe now that we disabled SMM. */
      /* cyrix_set_arr_up (3, 0, 0, 0, FALSE); */
    }
    /* If we changed CCR1 in memory, change it in the processor, too. */
    if (ccrc[1]) setCx86 (CX86_CCR1, ccr[1]);

    /* Enable ARR usage by the processor */
    if (!(ccr[5] & 0x20)) {
      ccr[5] |= 0x20; ccrc[5] = 1;
      setCx86 (CX86_CCR5, ccr[5]);
    }

#ifdef __SMP__
    for(i=0; i<7; i++) ccr_state[i] = ccr[i];
    for(i=0; i<8; i++)
      cyrix_get_arr(i,
        &arr_state[i].base, &arr_state[i].size, &arr_state[i].type);
#endif

    set_mtrr_done (&ctxt); /* flush cache and disable MAPEN */

    if ( ccrc[5] ) printk ("mtrr: ARR usage was not enabled, enabled manually\n");
    if ( ccrc[3] ) printk ("mtrr: ARR3 cannot be changed\n");
/*
    if ( ccrc[1] & 0x80) printk ("mtrr: SMM memory access through ARR3 disabled\n");
    if ( ccrc[1] & 0x04) printk ("mtrr: SMM memory access disabled\n");
    if ( ccrc[1] & 0x02) printk ("mtrr: SMM mode disabled\n");
*/
    if ( ccrc[6] ) printk ("mtrr: ARR3 was write protected, unprotected\n");
}   /*  End Function cyrix_arr_init  */

static void __init centaur_mcr_init(void)
{
    unsigned i;
    struct set_mtrr_context ctxt;

    set_mtrr_prepare (&ctxt);
    /* Unfortunately, MCR's are read-only, so there is no way to
     * find out what the bios might have done.
     */
    /* Clear all MCR's.
     * This way we are sure that the centaur_mcr array contains the actual
     * values. The disadvantage is that any BIOS tweaks are thus undone.
     */
    for (i = 0; i < 8; ++i)
    {
        centaur_mcr[i].high = 0;
	centaur_mcr[i].low = 0;
	wrmsr (0x110 + i , 0, 0);
    }
    /*  Throw the main write-combining switch...  */
    wrmsr (0x120, 0x01f0001f, 0);
    set_mtrr_done (&ctxt);
}   /*  End Function centaur_mcr_init  */

static void __init mtrr_setup(void)
{
    printk ("mtrr: v%s Richard Gooch (rgooch@atnf.csiro.au)\n", MTRR_VERSION);
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 < 6) { /* pre-Athlon CPUs */
	  get_mtrr = amd_get_mtrr;
	  set_mtrr_up = amd_set_mtrr_up;
	  break;
	} /* else fall through */
      case X86_VENDOR_INTEL:
	get_mtrr = intel_get_mtrr;
	set_mtrr_up = intel_set_mtrr_up;
	break;
      case X86_VENDOR_CYRIX:
	get_mtrr = cyrix_get_arr;
	set_mtrr_up = cyrix_set_arr_up;
	get_free_region = cyrix_get_free_region;
	break;
     case X86_VENDOR_CENTAUR:
        get_mtrr = centaur_get_mcr;
        set_mtrr_up = centaur_set_mcr_up;
        break;
    }
}   /*  End Function mtrr_setup  */

#ifdef __SMP__

static volatile unsigned long smp_changes_mask __initdata = 0;
static struct mtrr_state smp_mtrr_state __initdata = {0, 0};

void __init mtrr_init_boot_cpu(void)
{
    if ( !(boot_cpu_data.x86_capability & X86_FEATURE_MTRR) ) return;
    mtrr_setup ();
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 < 6) break; /* pre-Athlon CPUs */
      case X86_VENDOR_INTEL:
	get_mtrr_state (&smp_mtrr_state);
	break;
      case X86_VENDOR_CYRIX:
	cyrix_arr_init ();
	break;
      case X86_VENDOR_CENTAUR:
        centaur_mcr_init ();
        break;
    }
}   /*  End Function mtrr_init_boot_cpu  */

static void __init intel_mtrr_init_secondary_cpu(void)
{
    unsigned long mask, count;
    struct set_mtrr_context ctxt;

    /*  Note that this is not ideal, since the cache is only flushed/disabled
	for this CPU while the MTRRs are changed, but changing this requires
	more invasive changes to the way the kernel boots  */
    set_mtrr_prepare (&ctxt);
    mask = set_mtrr_state (&smp_mtrr_state, &ctxt);
    set_mtrr_done (&ctxt);
    /*  Use the atomic bitops to update the global mask  */
    for (count = 0; count < sizeof mask * 8; ++count)
    {
	if (mask & 0x01) set_bit (count, &smp_changes_mask);
	mask >>= 1;
    }
}   /*  End Function intel_mtrr_init_secondary_cpu  */

void __init mtrr_init_secondary_cpu(void)
{
    if ( !(boot_cpu_data.x86_capability & X86_FEATURE_MTRR) ) return;
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	/* Just for robustness: pre-Athlon CPUs cannot do SMP. */
	if (boot_cpu_data.x86 < 6) break;
      case X86_VENDOR_INTEL:
	intel_mtrr_init_secondary_cpu ();
	break;
      case X86_VENDOR_CYRIX:
	/* This is _completely theoretical_!
	 * I assume here that one day Cyrix will support Intel APIC.
	 * In reality on non-Intel CPUs we won't even get to this routine.
	 * Hopefully no one will plug two Cyrix processors in a dual P5 board.
	 *  :-)
	 */
	cyrix_arr_init_secondary ();
	break;
      default:
	printk ("mtrr: SMP support incomplete for this vendor\n");
	break;
    }
}   /*  End Function mtrr_init_secondary_cpu  */
#endif  /*  __SMP__  */

int __init mtrr_init(void)
{
    if ( !(boot_cpu_data.x86_capability & X86_FEATURE_MTRR) ) return 0;
#  ifdef __SMP__
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_AMD:
	if (boot_cpu_data.x86 < 6) break; /* pre-Athlon CPUs */
      case X86_VENDOR_INTEL:
	finalize_mtrr_state (&smp_mtrr_state);
	mtrr_state_warn (smp_changes_mask);
	break;
    }
#  else /* __SMP__ */
    mtrr_setup ();
    switch (boot_cpu_data.x86_vendor)
    {
      case X86_VENDOR_CYRIX:
	cyrix_arr_init ();
	break;
      case X86_VENDOR_CENTAUR:
        centaur_mcr_init ();
        break;
    }
#  endif  /*  !__SMP__  */

#  ifdef CONFIG_PROC_FS
    proc_root_mtrr = create_proc_entry("mtrr", S_IWUSR|S_IRUGO, &proc_root);
    proc_root_mtrr->ops = &proc_mtrr_inode_operations;
#endif    
    init_table ();
    return 0;
}   /*  End Function mtrr_init  */
