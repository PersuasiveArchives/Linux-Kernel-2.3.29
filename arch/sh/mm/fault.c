/* $Id: fault.c,v 1.5 1999/10/31 13:17:31 gniibe Exp $
 *
 *  linux/arch/sh/mm/fault.c
 *  Copyright (C) 1999  Niibe Yutaka
 *
 *  Based on linux/arch/i386/mm/fault.c:
 *   Copyright (C) 1995  Linus Torvalds
 */

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/hardirq.h>
#include <asm/mmu_context.h>

extern void die(const char *,struct pt_regs *,long);

/*
 * Ugly, ugly, but the goto's result in better assembly..
 */
int __verify_write(const void * addr, unsigned long size)
{
	struct vm_area_struct * vma;
	unsigned long start = (unsigned long) addr;

	if (!size)
		return 1;

	vma = find_vma(current->mm, start);
	if (!vma)
		goto bad_area;
	if (vma->vm_start > start)
		goto check_stack;

good_area:
	if (!(vma->vm_flags & VM_WRITE))
		goto bad_area;
	size--;
	size += start & ~PAGE_MASK;
	size >>= PAGE_SHIFT;
	start &= PAGE_MASK;

	for (;;) {
		if (handle_mm_fault(current, vma, start, 1) <= 0)
			goto bad_area;
		if (!size)
			break;
		size--;
		start += PAGE_SIZE;
		if (start < vma->vm_end)
			continue;
		vma = vma->vm_next;
		if (!vma || vma->vm_start != start)
			goto bad_area;
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;;
	}
	return 1;

check_stack:
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (expand_stack(vma, start) == 0)
		goto good_area;

bad_area:
	return 0;
}

static void handle_vmalloc_fault(struct task_struct *tsk, unsigned long address)
{
	pgd_t *dir;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;

	dir = pgd_offset_k(address);
	pmd = pmd_offset(dir, address);
	if (pmd_none(*pmd)) {
		printk(KERN_ERR "vmalloced area %08lx bad\n", address);
		return;
	}
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return;
	}
	pte = pte_offset(pmd, address);
	entry = *pte;
	if (pte_none(entry) || !pte_present(entry) || !pte_write(entry)) {
		printk(KERN_ERR "vmalloced area %08lx bad\n", address);
		return;
	}

	update_mmu_cache(NULL, address, entry);
}

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 */
asmlinkage void do_page_fault(struct pt_regs *regs, unsigned long writeaccess,
			      unsigned long address)
{
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct * vma;
	unsigned long page;
	unsigned long fixup;

	tsk = current;
	mm = tsk->mm;

	if (address >= VMALLOC_START && address < VMALLOC_END) {
		handle_vmalloc_fault(tsk, address);
		return;
	}

	/*
	 * If we're in an interrupt or have no user
	 * context, we must not take the fault..
	 */
	if (in_interrupt() || !mm)
		goto no_context;

	down(&mm->mmap_sem);

	vma = find_vma(mm, address);
	if (!vma)
		goto bad_area;
	if (vma->vm_start <= address)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (expand_stack(vma, address))
		goto bad_area;
/*
 * Ok, we have a good vm_area for this memory access, so
 * we can handle it..
 */
good_area:
	if (writeaccess) {
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
	} else {
		if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
			goto bad_area;
	}

	/*
	 * If for any reason at all we couldn't handle the fault,
	 * make sure we exit gracefully rather than endlessly redo
	 * the fault.
	 */
	{
		int fault = handle_mm_fault(tsk, vma, address, writeaccess);
		if (fault < 0)
			goto out_of_memory;
		if (!fault)
			goto do_sigbus;
	}

	up(&mm->mmap_sem);
	return;

/*
 * Something tried to access memory that isn't in our memory map..
 * Fix it, but check if it's kernel or user first..
 */
bad_area:
	up(&mm->mmap_sem);

	if (user_mode(regs)) {
		tsk->thread.address = address;
		tsk->thread.error_code = writeaccess;
		force_sig(SIGSEGV, tsk);
		return;
	}

no_context:
	/* Are we prepared to handle this kernel fault?  */
	fixup = search_exception_table(regs->pc);
	if (fixup != 0) {
		regs->pc = fixup;
		return;
	}

/*
 * Oops. The kernel tried to access some bad page. We'll have to
 * terminate things with extreme prejudice.
 *
 */
	if (address < PAGE_SIZE)
		printk(KERN_ALERT "Unable to handle kernel NULL pointer dereference");
	else
		printk(KERN_ALERT "Unable to handle kernel paging request");
	printk(" at virtual address %08lx\n",address);
	printk(KERN_ALERT "pc = %08lx\n", regs->pc);
	page = (unsigned long)mm->pgd;
	page = ((unsigned long *) __va(page))[address >> 22];
	printk(KERN_ALERT "*pde = %08lx\n", page);
	if (page & 1) {
		page &= PAGE_MASK;
		address &= 0x003ff000;
		page = ((unsigned long *) __va(page))[address >> PAGE_SHIFT];
		printk(KERN_ALERT "*pte = %08lx\n", page);
	}
	die("Oops", regs, writeaccess);
	do_exit(SIGKILL);

/*
 * We ran out of memory, or some other thing happened to us that made
 * us unable to handle the page fault gracefully.
 */
out_of_memory:
	up(&mm->mmap_sem);
	printk("VM: killing process %s\n", tsk->comm);
	if (user_mode(regs))
		do_exit(SIGKILL);
	goto no_context;

do_sigbus:
	up(&mm->mmap_sem);

	/*
	 * Send a sigbus, regardless of whether we were in kernel
	 * or user mode.
	 */
	tsk->thread.address = address;
	tsk->thread.error_code = writeaccess;
	tsk->thread.trap_no = 14;
	force_sig(SIGBUS, tsk);

	/* Kernel mode? Handle exceptions or die */
	if (!user_mode(regs))
		goto no_context;
}

void update_mmu_cache(struct vm_area_struct * vma,
		      unsigned long address, pte_t pte)
{
	unsigned long flags;
	unsigned long pteval;

	save_and_cli(flags);
	/*
	 *  We don't need to set PTEH register.
	 *  It's automatically set by the hardware.
	 */
	pteval = pte_val(pte);
	pteval &= _PAGE_FLAGS_HARDWARE_MASK; /* drop software flags */
	pteval |= _PAGE_FLAGS_HARDWARE_DEFAULT; /* add default flags */
	/* Set PTEL register */
	ctrl_outl(pteval, MMU_PTEL);

	/* Load the TLB */
	asm volatile("ldtlb": /* no output */ : /* no input */ : "memory");
	restore_flags(flags);
}

static void __flush_tlb_page(struct mm_struct *mm, unsigned long page)
{
	unsigned long addr, data, asid;
	unsigned long saved_asid = MMU_NO_ASID;

	if (mm->context == NO_CONTEXT)
		return;

	asid = mm->context & MMU_CONTEXT_ASID_MASK;
	if (mm != current->mm) {
		saved_asid = get_asid();
		/*
		 * We need to set ASID of the target entry to flush,
		 * because TLB is indexed by (ASID and PAGE).
		 */
		set_asid(asid);
	}

#if defined(__sh3__)
	addr = MMU_TLB_ADDRESS_ARRAY |(page & 0x1F000)| MMU_PAGE_ASSOC_BIT;
	data = (page & 0xfffe0000) | asid; /* VALID bit is off */
	ctrl_outl(data, addr);
#elif defined(__SH4__)
	int i;

	addr = MMU_UTLB_ADDRESS_ARRAY | MMU_PAGE_ASSOC_BIT;
	data = page | asid; /* VALID bit is off */
	ctrl_outl(data, addr);

	for (i=0; i<4; i++) {
		addr = MMU_ITLB_ADDRESS_ARRAY | (i<<8);
		data = ctrl_inl(addr);
		data &= ~0x30;
		if (data == (page | asid)) {
			ctrl_outl(data, addr);
			break;
		}
	}
#endif
	if (saved_asid != MMU_NO_ASID)
		set_asid(saved_asid);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long page)
{
	unsigned long flags;

	if (vma->vm_mm) {
		page &= PAGE_MASK;
		save_and_cli(flags);
		__flush_tlb_page(vma->vm_mm, page);
		restore_flags(flags);
	}
}

void flush_tlb_range(struct mm_struct *mm, unsigned long start,
		     unsigned long end)
{
	if (mm->context != NO_CONTEXT) {
		unsigned long flags;
		int size;

		save_and_cli(flags);
		size = (end - start + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
		if (size > (MMU_NTLB_ENTRIES/4)) { /* Too many TLB to flush */
			mm->context = NO_CONTEXT;
			if (mm == current->mm)
				activate_context(mm);
		} else {
			start &= PAGE_MASK;
			end += (PAGE_SIZE - 1);
			end &= PAGE_MASK;
			while (start < end) {
				__flush_tlb_page(mm, start);
				start += PAGE_SIZE;
			}
		}
		restore_flags(flags);
	}
}

void flush_tlb_mm(struct mm_struct *mm)
{
	/* Invalidate all TLB of this process. */
	/* Instead of invalidating each TLB, we get new MMU context. */
	if (mm->context != NO_CONTEXT) {
		unsigned long flags;

		save_and_cli(flags);
		mm->context = NO_CONTEXT;
		if (mm == current->mm)
			activate_context(mm);
		restore_flags(flags);
	}
}

void flush_tlb_all(void)
{
	unsigned long flags, status;

	save_and_cli(flags);
	status = ctrl_inl(MMUCR);
	status |= 0x04;		/* Set TF-bit to flush */
	ctrl_outl(status,MMUCR);
	restore_flags(flags);
}
