/*
 *  linux/arch/ppc/kernel/signal.c
 *
 *  $Id: signal.c,v 1.27 1999/08/03 19:16:38 cort Exp $
 *
 *  PowerPC version 
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Derived from "arch/i386/kernel/signal.c"
 *    Copyright (C) 1991, 1992 Linus Torvalds
 *    1997-11-28  Modified for POSIX.1b signals by Richard Henderson
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/stddef.h>
#include <linux/elf.h>
#include <asm/ucontext.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>

#define DEBUG_SIG 0

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#define GP_REGS_SIZE	MIN(sizeof(elf_gregset_t), sizeof(struct pt_regs))

/* 
 * These are the flags in the MSR that the user is allowed to change
 * by modifying the saved value of the MSR on the stack.  SE and BE
 * should not be in this list since gdb may want to change these.  I.e,
 * you should be able to step out of a signal handler to see what
 * instruction executes next after the signal handler completes.
 * Alternately, if you stepped into a signal handler, you should be
 * able to continue 'til the next breakpoint from within the signal
 * handler, even if the handler returns.
 */
#define MSR_USERCHANGE	(MSR_FE0 | MSR_FE1)

int do_signal(sigset_t *oldset, struct pt_regs *regs);
extern int sys_wait4(pid_t pid, unsigned long *stat_addr,
		     int options, unsigned long *ru);

/*
 * Atomically swap in the new signal mask, and wait for a signal.
 */
int
sys_sigsuspend(old_sigset_t mask, int p2, int p3, int p4, int p6, int p7,
	       struct pt_regs *regs)
{
	sigset_t saveset;

	mask &= _BLOCKABLE;
	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	siginitset(&current->blocked, mask);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->gpr[3] = -EINTR;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&saveset, regs))
			/*
			 * If a signal handler needs to be called,
			 * do_signal() has set R3 to the signal number (the
			 * first argument of the signal handler), so don't
			 * overwrite that with EINTR !
			 * In the other cases, do_signal() doesn't touch 
			 * R3, so it's still set to -EINTR (see above).
			 */
			return regs->gpr[3];
	}
}

int
sys_rt_sigsuspend(sigset_t *unewset, size_t sigsetsize, int p3, int p4, int p6,
		  int p7, struct pt_regs *regs)
{
	sigset_t saveset, newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	sigdelsetmask(&newset, ~_BLOCKABLE);

	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	current->blocked = newset;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->gpr[3] = -EINTR;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&saveset, regs))
			return regs->gpr[3];
	}
}


asmlinkage int sys_rt_sigreturn(unsigned long __unused)
{
	printk("sys_rt_sigreturn(): %s/%d not yet implemented.\n",
	       current->comm,current->pid);
	do_exit(SIGSEGV);
}

asmlinkage int
sys_sigaltstack(const stack_t *uss, stack_t *uoss)
{
	struct pt_regs *regs = (struct pt_regs *) &uss;
	return do_sigaltstack(uss, uoss, regs->gpr[1]);
}

int 
sys_sigaction(int sig, const struct old_sigaction *act,
	      struct old_sigaction *oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;
		if (verify_area(VERIFY_READ, act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_restorer, &act->sa_restorer))
			return -EFAULT;
		__get_user(new_ka.sa.sa_flags, &act->sa_flags);
		__get_user(mask, &act->sa_mask);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (verify_area(VERIFY_WRITE, oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_restorer, &oact->sa_restorer))
			return -EFAULT;
		__put_user(old_ka.sa.sa_flags, &oact->sa_flags);
		__put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask);
	}

	return ret;
}

/*
 * When we have signals to deliver, we set up on the
 * user stack, going down from the original stack pointer:
 *	a sigregs struct
 *	one or more sigcontext structs
 *	a gap of __SIGNAL_FRAMESIZE bytes
 *
 * Each of these things must be a multiple of 16 bytes in size.
 *
 * XXX ultimately we will have to stack up a siginfo and ucontext
 * for each rt signal.
 */
struct sigregs {
	elf_gregset_t	gp_regs;
	double		fp_regs[ELF_NFPREG];
	unsigned long	tramp[2];
	/* Programs using the rs6000/xcoff abi can save up to 19 gp regs
	   and 18 fp regs below sp before decrementing it. */
	int		abigap[56];
};

/*
 * Do a signal return; undo the signal stack.
 */
int sys_sigreturn(struct pt_regs *regs)
{
	struct sigcontext_struct *sc, sigctx;
	struct sigregs *sr;
	int ret;
	elf_gregset_t saved_regs;  /* an array of ELF_NGREG unsigned longs */
	sigset_t set;
	unsigned long prevsp;

	sc = (struct sigcontext_struct *)(regs->gpr[1] + __SIGNAL_FRAMESIZE);
	if (copy_from_user(&sigctx, sc, sizeof(sigctx)))
		goto badframe;

	set.sig[0] = sigctx.oldmask;
#if _NSIG_WORDS > 1
	set.sig[1] = sigctx._unused[3];
#endif
	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	sc++;			/* Look at next sigcontext */
	if (sc == (struct sigcontext_struct *)(sigctx.regs)) {
		/* Last stacked signal - restore registers */
		sr = (struct sigregs *) sigctx.regs;
		if (regs->msr & MSR_FP )
			giveup_fpu(current);
		if (copy_from_user(saved_regs, &sr->gp_regs,
				   sizeof(sr->gp_regs)))
			goto badframe;
		saved_regs[PT_MSR] = (regs->msr & ~MSR_USERCHANGE)
			| (saved_regs[PT_MSR] & MSR_USERCHANGE);
		memcpy(regs, saved_regs, GP_REGS_SIZE);

		if (copy_from_user(current->thread.fpr, &sr->fp_regs,
				   sizeof(sr->fp_regs)))
			goto badframe;

		ret = regs->result;

	} else {
		/* More signals to go */
		regs->gpr[1] = (unsigned long)sc - __SIGNAL_FRAMESIZE;
		if (copy_from_user(&sigctx, sc, sizeof(sigctx)))
			goto badframe;
		sr = (struct sigregs *) sigctx.regs;
		regs->gpr[3] = ret = sigctx.signal;
		regs->gpr[4] = (unsigned long) sc;
		regs->link = (unsigned long) &sr->tramp;
		regs->nip = sigctx.handler;

		if (get_user(prevsp, &sr->gp_regs[PT_R1])
		    || put_user(prevsp, (unsigned long *) regs->gpr[1]))
			goto badframe;
	}
	return ret;

badframe:
	lock_kernel();
	do_exit(SIGSEGV);
}	

/*
 * Set up a signal frame.
 */
static void
setup_frame(struct pt_regs *regs, struct sigregs *frame,
	    unsigned long newsp)
{
	struct sigcontext_struct *sc = (struct sigcontext_struct *) newsp;

	if (verify_area(VERIFY_WRITE, frame, sizeof(*frame)))
		goto badframe;
		if (regs->msr & MSR_FP)
			giveup_fpu(current);
	if (__copy_to_user(&frame->gp_regs, regs, GP_REGS_SIZE)
	    || __copy_to_user(&frame->fp_regs, current->thread.fpr,
			      ELF_NFPREG * sizeof(double))
	    || __put_user(0x38007777UL, &frame->tramp[0])    /* li r0,0x7777 */
	    || __put_user(0x44000002UL, &frame->tramp[1]))   /* sc */
		goto badframe;
	flush_icache_range((unsigned long) &frame->tramp[0],
			   (unsigned long) &frame->tramp[2]);

	newsp -= __SIGNAL_FRAMESIZE;
	if (put_user(regs->gpr[1], (unsigned long *)newsp)
	    || get_user(regs->nip, &sc->handler)
	    || get_user(regs->gpr[3], &sc->signal))
		goto badframe;
	regs->gpr[1] = newsp;
	regs->gpr[4] = (unsigned long) sc;
	regs->link = (unsigned long) frame->tramp;

	return;

badframe:
#if DEBUG_SIG
	printk("badframe in setup_frame, regs=%p frame=%p newsp=%lx\n",
	       regs, frame, newsp);
#endif
	lock_kernel();
	do_exit(SIGSEGV);
}

/*
 * OK, we're invoking a handler
 */
static void
handle_signal(unsigned long sig, struct k_sigaction *ka,
	      siginfo_t *info, sigset_t *oldset, struct pt_regs * regs,
	      unsigned long *newspp, unsigned long frame)
{
	struct sigcontext_struct *sc;

	if (regs->trap == 0x0C00 /* System Call! */
	    && ((int)regs->result == -ERESTARTNOHAND ||
		((int)regs->result == -ERESTARTSYS &&
		 !(ka->sa.sa_flags & SA_RESTART))))
		regs->result = -EINTR;

	/* Put another sigcontext on the stack */
	*newspp -= sizeof(*sc);
	sc = (struct sigcontext_struct *) *newspp;
	if (verify_area(VERIFY_WRITE, sc, sizeof(*sc)))
		goto badframe;

	if (__put_user((unsigned long) ka->sa.sa_handler, &sc->handler)
	    || __put_user(oldset->sig[0], &sc->oldmask)
#if _NSIG_WORDS > 1
	    || __put_user(oldset->sig[1], &sc->_unused[3])
#endif
	    || __put_user((struct pt_regs *)frame, &sc->regs)
	    || __put_user(sig, &sc->signal))
		goto badframe;

	if (ka->sa.sa_flags & SA_ONESHOT)
		ka->sa.sa_handler = SIG_DFL;

	if (!(ka->sa.sa_flags & SA_NODEFER)) {
		spin_lock_irq(&current->sigmask_lock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked,sig);
		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);
	}
	return;

badframe:
#if DEBUG_SIG
	printk("badframe in handle_signal, regs=%p frame=%lx newsp=%lx\n",
	       regs, frame, *newspp);
	printk("sc=%p sig=%d ka=%p info=%p oldset=%p\n", sc, sig, ka, info, oldset);
#endif
	lock_kernel();
	do_exit(SIGSEGV);
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 */
int do_signal(sigset_t *oldset, struct pt_regs *regs)
{
	siginfo_t info;
	struct k_sigaction *ka;
	unsigned long frame, newsp;

	if (!oldset)
		oldset = &current->blocked;

	newsp = frame = 0;

	for (;;) {
		unsigned long signr;

		spin_lock_irq(&current->sigmask_lock);
		signr = dequeue_signal(&current->blocked, &info);
		spin_unlock_irq(&current->sigmask_lock);

		if (!signr)
			break;

		if ((current->flags & PF_PTRACED) && signr != SIGKILL) {
			/* Let the debugger run.  */
			current->exit_code = signr;
			current->state = TASK_STOPPED;
			notify_parent(current, SIGCHLD);
			schedule();

			/* We're back.  Did the debugger cancel the sig?  */
			if (!(signr = current->exit_code))
				continue;
			current->exit_code = 0;

			/* The debugger continued.  Ignore SIGSTOP.  */
			if (signr == SIGSTOP)
				continue;

			/* Update the siginfo structure.  Is this good?  */
			if (signr != info.si_signo) {
				info.si_signo = signr;
				info.si_errno = 0;
				info.si_code = SI_USER;
				info.si_pid = current->p_pptr->pid;
				info.si_uid = current->p_pptr->uid;
			}

			/* If the (new) signal is now blocked, requeue it.  */
			if (sigismember(&current->blocked, signr)) {
				send_sig_info(signr, &info, current);
				continue;
			}
		}

		ka = &current->sig->action[signr-1];
		if (ka->sa.sa_handler == SIG_IGN) {
			if (signr != SIGCHLD)
				continue;
			/* Check for SIGCHLD: it's special.  */
			while (sys_wait4(-1, NULL, WNOHANG, NULL) > 0)
				/* nothing */;
			continue;
		}

		if (ka->sa.sa_handler == SIG_DFL) {
			int exit_code = signr;

			/* Init gets no signals it doesn't want.  */
			if (current->pid == 1)
				continue;

			switch (signr) {
			case SIGCONT: case SIGCHLD: case SIGWINCH:
				continue;

			case SIGTSTP: case SIGTTIN: case SIGTTOU:
				if (is_orphaned_pgrp(current->pgrp))
					continue;
				/* FALLTHRU */

			case SIGSTOP:
				current->state = TASK_STOPPED;
				current->exit_code = signr;
				if (!(current->p_pptr->sig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDSTOP))
					notify_parent(current, SIGCHLD);
				schedule();
				continue;

			case SIGQUIT: case SIGILL: case SIGTRAP:
			case SIGABRT: case SIGFPE: case SIGSEGV:
			case SIGBUS: case SIGSYS: case SIGXCPU: case SIGXFSZ:
				if (do_coredump(signr, regs))
					exit_code |= 0x80;
				/* FALLTHRU */

			default:
				lock_kernel();
				sigaddset(&current->signal, signr);
				recalc_sigpending(current);
				current->flags |= PF_SIGNALED;
				do_exit(exit_code);
				/* NOTREACHED */
			}
		}

		if ( (ka->sa.sa_flags & SA_ONSTACK)
		     && (! on_sig_stack(regs->gpr[1])))
			newsp = (current->sas_ss_sp + current->sas_ss_size);
		else
			newsp = regs->gpr[1];
		newsp = frame = newsp - sizeof(struct sigregs);

		/* Whee!  Actually deliver the signal.  */
		handle_signal(signr, ka, &info, oldset, regs, &newsp, frame);
	}

	if (regs->trap == 0x0C00 /* System Call! */ &&
	    ((int)regs->result == -ERESTARTNOHAND ||
	     (int)regs->result == -ERESTARTSYS ||
	     (int)regs->result == -ERESTARTNOINTR)) {
		regs->gpr[3] = regs->orig_gpr3;
		regs->nip -= 4;		/* Back up & retry system call */
		regs->result = 0;
	}

	if (newsp == frame)
		return 0;		/* no signals delivered */

	setup_frame(regs, (struct sigregs *) frame, newsp);
	return 1;
}

