/*
 *  linux/arch/alpha/kernel/signal.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *
 *  1997-11-02  Modified for POSIX.1b signals by Richard Henderson
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/stddef.h>

#include <asm/bitops.h>
#include <asm/uaccess.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>

#include "proto.h"


#define DEBUG_SIG 0

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

asmlinkage int sys_wait4(int, int *, int, struct rusage *);
asmlinkage void ret_from_sys_call(void);
asmlinkage int do_signal(sigset_t *, struct pt_regs *,
			 struct switch_stack *, unsigned long, unsigned long);


/*
 * The OSF/1 sigprocmask calling sequence is different from the
 * C sigprocmask() sequence..
 *
 * how:
 * 1 - SIG_BLOCK
 * 2 - SIG_UNBLOCK
 * 3 - SIG_SETMASK
 *
 * We change the range to -1 .. 1 in order to let gcc easily
 * use the conditional move instructions.
 *
 * Note that we don't need to acquire the kernel lock for SMP
 * operation, as all of this is local to this thread.
 */
asmlinkage unsigned long
osf_sigprocmask(int how, unsigned long newmask, long a2, long a3,
		long a4, long a5, struct pt_regs regs)
{
	unsigned long oldmask = -EINVAL;

	if ((unsigned long)how-1 <= 2) {
		long sign = how-2;		/* -1 .. 1 */
		unsigned long block, unblock;

		newmask &= _BLOCKABLE;
		spin_lock_irq(&current->sigmask_lock);
		oldmask = current->blocked.sig[0];

		unblock = oldmask & ~newmask;
		block = oldmask | newmask;
		if (!sign)
			block = unblock;
		if (sign <= 0)
			newmask = block;
		if (_NSIG_WORDS > 1 && sign > 0)
			sigemptyset(&current->blocked);
		current->blocked.sig[0] = newmask;
		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);

		(&regs)->r0 = 0;		/* special no error return */
	}
	return oldmask;
}

asmlinkage int 
osf_sigaction(int sig, const struct osf_sigaction *act,
	      struct osf_sigaction *oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;
		if (verify_area(VERIFY_READ, act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_flags, &act->sa_flags))
			return -EFAULT;
		__get_user(mask, &act->sa_mask);
		siginitset(&new_ka.sa.sa_mask, mask);
		new_ka.ka_restorer = NULL;
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (verify_area(VERIFY_WRITE, oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_flags, &oact->sa_flags))
			return -EFAULT;
		__put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask);
	}

	return ret;
}

asmlinkage int 
sys_rt_sigaction(int sig, const struct sigaction *act, struct sigaction *oact,
		 size_t sigsetsize, void *restorer)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (act) {
		new_ka.ka_restorer = restorer;
		if (copy_from_user(&new_ka.sa, act, sizeof(*act)))
			return -EFAULT;
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (copy_to_user(oact, &old_ka.sa, sizeof(*oact)))
			return -EFAULT;
	}

	return ret;
}

asmlinkage int
osf_sigpending(old_sigset_t *set)
{
        sigset_t pending;

        spin_lock_irq(&current->sigmask_lock);
        sigandsets(&pending, &current->blocked, &current->signal);
        spin_unlock_irq(&current->sigmask_lock);

        return copy_to_user(set, &pending, sizeof(*set));
}

/*
 * Atomically swap in the new signal mask, and wait for a signal.
 */
asmlinkage int
do_sigsuspend(old_sigset_t mask, struct pt_regs *reg, struct switch_stack *sw)
{
	sigset_t oldset;

	mask &= _BLOCKABLE;
	spin_lock_irq(&current->sigmask_lock);
	oldset = current->blocked;
	siginitset(&current->blocked, mask);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&oldset, reg, sw, 0, 0))
			return -EINTR;
	}
}

asmlinkage int
do_rt_sigsuspend(sigset_t *uset, size_t sigsetsize,
		 struct pt_regs *reg, struct switch_stack *sw)
{
	sigset_t oldset, set;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;
	if (copy_from_user(&set, uset, sizeof(set)))
		return -EFAULT;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	oldset = current->blocked;
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&oldset, reg, sw, 0, 0))
			return -EINTR;
	}
}

asmlinkage int
sys_sigaltstack(const stack_t *uss, stack_t *uoss)
{
	return do_sigaltstack(uss, uoss, rdusp());
}

/*
 * Do a signal return; undo the signal stack.
 */

struct sigframe
{
	struct sigcontext sc;
	unsigned long extramask[_NSIG_WORDS-1];
	unsigned int retcode[3];
};

struct rt_sigframe
{
	struct siginfo info;
	struct ucontext uc;
	unsigned int retcode[3];
};

#define INSN_MOV_R30_R16	0x47fe0410
#define INSN_LDI_R0		0x201f0000
#define INSN_CALLSYS		0x00000083

static long
restore_sigcontext(struct sigcontext *sc, struct pt_regs *regs,
		   struct switch_stack *sw)
{
	unsigned long usp;
	long i, err = 0;

	err |= __get_user(regs->pc, &sc->sc_pc);
	sw->r26 = (unsigned long) ret_from_sys_call;

	err |= __get_user(regs->r0, sc->sc_regs+0);
	err |= __get_user(regs->r1, sc->sc_regs+1);
	err |= __get_user(regs->r2, sc->sc_regs+2);
	err |= __get_user(regs->r3, sc->sc_regs+3);
	err |= __get_user(regs->r4, sc->sc_regs+4);
	err |= __get_user(regs->r5, sc->sc_regs+5);
	err |= __get_user(regs->r6, sc->sc_regs+6);
	err |= __get_user(regs->r7, sc->sc_regs+7);
	err |= __get_user(regs->r8, sc->sc_regs+8);
	err |= __get_user(sw->r9, sc->sc_regs+9);
	err |= __get_user(sw->r10, sc->sc_regs+10);
	err |= __get_user(sw->r11, sc->sc_regs+11);
	err |= __get_user(sw->r12, sc->sc_regs+12);
	err |= __get_user(sw->r13, sc->sc_regs+13);
	err |= __get_user(sw->r14, sc->sc_regs+14);
	err |= __get_user(sw->r15, sc->sc_regs+15);
	err |= __get_user(regs->r16, sc->sc_regs+16);
	err |= __get_user(regs->r17, sc->sc_regs+17);
	err |= __get_user(regs->r18, sc->sc_regs+18);
	err |= __get_user(regs->r19, sc->sc_regs+19);
	err |= __get_user(regs->r20, sc->sc_regs+20);
	err |= __get_user(regs->r21, sc->sc_regs+21);
	err |= __get_user(regs->r22, sc->sc_regs+22);
	err |= __get_user(regs->r23, sc->sc_regs+23);
	err |= __get_user(regs->r24, sc->sc_regs+24);
	err |= __get_user(regs->r25, sc->sc_regs+25);
	err |= __get_user(regs->r26, sc->sc_regs+26);
	err |= __get_user(regs->r27, sc->sc_regs+27);
	err |= __get_user(regs->r28, sc->sc_regs+28);
	err |= __get_user(regs->gp, sc->sc_regs+29);
	err |= __get_user(usp, sc->sc_regs+30);
	wrusp(usp);

	for (i = 0; i < 31; i++)
		err |= __get_user(sw->fp[i], sc->sc_fpregs+i);
	err |= __get_user(sw->fp[31], &sc->sc_fpcr);

	return err;
}

asmlinkage void
do_sigreturn(struct sigframe *frame, struct pt_regs *regs,
	     struct switch_stack *sw)
{
	sigset_t set;

	/* Verify that it's a good sigcontext before using it */
	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto give_sigsegv;
	if (__get_user(set.sig[0], &frame->sc.sc_mask)
	    || (_NSIG_WORDS > 1
		&& __copy_from_user(&set.sig[1], &frame->extramask,
				    sizeof(frame->extramask))))
		goto give_sigsegv;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	if (restore_sigcontext(&frame->sc, regs, sw))
		goto give_sigsegv;

	/* Send SIGTRAP if we're single-stepping: */
	if (ptrace_cancel_bpt (current))
		send_sig(SIGTRAP, current, 1);
	return;

give_sigsegv:
	force_sig(SIGSEGV, current);
}

asmlinkage void
do_rt_sigreturn(struct rt_sigframe *frame, struct pt_regs *regs,
		struct switch_stack *sw)
{
	sigset_t set;
	stack_t st;

	/* Verify that it's a good sigcontext before using it */
	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto give_sigsegv;
	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto give_sigsegv;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	if (restore_sigcontext(&frame->uc.uc_mcontext, regs, sw))
		goto give_sigsegv;

	if (__copy_from_user(&st, &frame->uc.uc_stack, sizeof(st)))
		goto give_sigsegv;
	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&st, NULL, rdusp());

	/* Send SIGTRAP if we're single-stepping: */
	if (ptrace_cancel_bpt (current))
		send_sig(SIGTRAP, current, 1);
	return;

give_sigsegv:
	force_sig(SIGSEGV, current);
}


/*
 * Set up a signal frame.
 */

static inline void *
get_sigframe(struct k_sigaction *ka, unsigned long sp, size_t frame_size)
{
	if ((ka->sa.sa_flags & SA_ONSTACK) != 0 && ! on_sig_stack(sp))
		sp = current->sas_ss_sp + current->sas_ss_size;

	return (void *)((sp - frame_size) & -32ul);
}

static long
setup_sigcontext(struct sigcontext *sc, struct pt_regs *regs, 
		 struct switch_stack *sw, unsigned long mask, unsigned long sp)
{
	long i, err = 0;

	err |= __put_user(on_sig_stack((unsigned long)sc), &sc->sc_onstack);
	err |= __put_user(mask, &sc->sc_mask);
	err |= __put_user(regs->pc, &sc->sc_pc);
	err |= __put_user(8, &sc->sc_ps);

	err |= __put_user(regs->r0 , sc->sc_regs+0);
	err |= __put_user(regs->r1 , sc->sc_regs+1);
	err |= __put_user(regs->r2 , sc->sc_regs+2);
	err |= __put_user(regs->r3 , sc->sc_regs+3);
	err |= __put_user(regs->r4 , sc->sc_regs+4);
	err |= __put_user(regs->r5 , sc->sc_regs+5);
	err |= __put_user(regs->r6 , sc->sc_regs+6);
	err |= __put_user(regs->r7 , sc->sc_regs+7);
	err |= __put_user(regs->r8 , sc->sc_regs+8);
	err |= __put_user(sw->r9   , sc->sc_regs+9);
	err |= __put_user(sw->r10  , sc->sc_regs+10);
	err |= __put_user(sw->r11  , sc->sc_regs+11);
	err |= __put_user(sw->r12  , sc->sc_regs+12);
	err |= __put_user(sw->r13  , sc->sc_regs+13);
	err |= __put_user(sw->r14  , sc->sc_regs+14);
	err |= __put_user(sw->r15  , sc->sc_regs+15);
	err |= __put_user(regs->r16, sc->sc_regs+16);
	err |= __put_user(regs->r17, sc->sc_regs+17);
	err |= __put_user(regs->r18, sc->sc_regs+18);
	err |= __put_user(regs->r19, sc->sc_regs+19);
	err |= __put_user(regs->r20, sc->sc_regs+20);
	err |= __put_user(regs->r21, sc->sc_regs+21);
	err |= __put_user(regs->r22, sc->sc_regs+22);
	err |= __put_user(regs->r23, sc->sc_regs+23);
	err |= __put_user(regs->r24, sc->sc_regs+24);
	err |= __put_user(regs->r25, sc->sc_regs+25);
	err |= __put_user(regs->r26, sc->sc_regs+26);
	err |= __put_user(regs->r27, sc->sc_regs+27);
	err |= __put_user(regs->r28, sc->sc_regs+28);
	err |= __put_user(regs->gp , sc->sc_regs+29);
	err |= __put_user(sp, sc->sc_regs+30);
	err |= __put_user(0, sc->sc_regs+31);

	for (i = 0; i < 31; i++)
		err |= __put_user(sw->fp[i], sc->sc_fpregs+i);
	err |= __put_user(0, sc->sc_fpregs+31);
	err |= __put_user(sw->fp[31], &sc->sc_fpcr);

	err |= __put_user(regs->trap_a0, &sc->sc_traparg_a0);
	err |= __put_user(regs->trap_a1, &sc->sc_traparg_a1);
	err |= __put_user(regs->trap_a2, &sc->sc_traparg_a2);

	return err;
}

static void
setup_frame(int sig, struct k_sigaction *ka, sigset_t *set,
	    struct pt_regs *regs, struct switch_stack * sw)
{
	unsigned long oldsp, r26, err = 0;
	struct sigframe *frame;

	oldsp = rdusp();
	frame = get_sigframe(ka, oldsp, sizeof(*frame));
	if (verify_area(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= setup_sigcontext(&frame->sc, regs, sw, set->sig[0], oldsp);
	if (_NSIG_WORDS > 1) {
		err |= __copy_to_user(frame->extramask, &set->sig[1], 
				      sizeof(frame->extramask));
	}

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->ka_restorer) {
		r26 = (unsigned long) ka->ka_restorer;
	} else {
		err |= __put_user(INSN_MOV_R30_R16, frame->retcode+0);
		err |= __put_user(INSN_LDI_R0+__NR_sigreturn, frame->retcode+1);
		err |= __put_user(INSN_CALLSYS, frame->retcode+2);
		imb();
		r26 = (unsigned long) frame->retcode;
	}

	/* Check that everything was written properly.  */
	if (err)
		goto give_sigsegv;

	/* "Return" to the handler */
	regs->r26 = r26;
	regs->r27 = regs->pc = (unsigned long) ka->sa.sa_handler;
	regs->r16 = sig;			/* a0: signal number */
	regs->r17 = 0;				/* a1: exception code */
	regs->r18 = (unsigned long) &frame->sc;	/* a2: sigcontext pointer */
	wrusp((unsigned long) frame);
	
#if DEBUG_SIG
	printk("SIG deliver (%s:%d): sp=%p pc=%p ra=%p\n",
		current->comm, current->pid, frame, regs->pc, regs->r26);
#endif

	return;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}

static void
setup_rt_frame(int sig, struct k_sigaction *ka, siginfo_t *info,
	       sigset_t *set, struct pt_regs *regs, struct switch_stack * sw)
{
	unsigned long oldsp, r26, err = 0;
	struct rt_sigframe *frame;

	oldsp = rdusp();
	frame = get_sigframe(ka, oldsp, sizeof(*frame));
	if (verify_area(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= __copy_to_user(&frame->info, info, sizeof(siginfo_t));

	/* Create the ucontext.  */
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(0, &frame->uc.uc_link);
	err |= __put_user(set->sig[0], &frame->uc.uc_osf_sigmask);
	err |= __put_user(current->sas_ss_sp, &frame->uc.uc_stack.ss_sp);
	err |= __put_user(sas_ss_flags(oldsp), &frame->uc.uc_stack.ss_flags);
	err |= __put_user(current->sas_ss_size, &frame->uc.uc_stack.ss_size);
	err |= setup_sigcontext(&frame->uc.uc_mcontext, regs, sw,
				set->sig[0], oldsp);
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->ka_restorer) {
		r26 = (unsigned long) ka->ka_restorer;
	} else {
		err |= __put_user(INSN_MOV_R30_R16, frame->retcode+0);
		err |= __put_user(INSN_LDI_R0+__NR_rt_sigreturn,
				  frame->retcode+1);
		err |= __put_user(INSN_CALLSYS, frame->retcode+2);
		imb();
		r26 = (unsigned long) frame->retcode;
	}

	if (err)
		goto give_sigsegv;

	/* "Return" to the handler */
	regs->r26 = r26;
	regs->r27 = regs->pc = (unsigned long) ka->sa.sa_handler;
	regs->r16 = sig;			  /* a0: signal number */
	regs->r17 = (unsigned long) &frame->info; /* a1: siginfo pointer */
	regs->r18 = (unsigned long) &frame->uc;	  /* a2: ucontext pointer */
	wrusp((unsigned long) frame);

#if DEBUG_SIG
	printk("SIG deliver (%s:%d): sp=%p pc=%p ra=%p\n",
		current->comm, current->pid, frame, regs->pc, regs->r26);
#endif

	return;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}


/*
 * OK, we're invoking a handler.
 */
static inline void
handle_signal(int sig, struct k_sigaction *ka, siginfo_t *info,
	      sigset_t *oldset, struct pt_regs * regs, struct switch_stack *sw)
{
	if (ka->sa.sa_flags & SA_SIGINFO)
		setup_rt_frame(sig, ka, info, oldset, regs, sw);
	else
		setup_frame(sig, ka, oldset, regs, sw);

	if (ka->sa.sa_flags & SA_RESETHAND)
		ka->sa.sa_handler = SIG_DFL;

	if (!(ka->sa.sa_flags & SA_NODEFER)) {
		spin_lock_irq(&current->sigmask_lock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked,sig);
		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);
	}
}

static inline void
syscall_restart(unsigned long r0, unsigned long r19,
		struct pt_regs *regs, struct k_sigaction *ka)
{
	switch (regs->r0) {
	case ERESTARTSYS:
		if (!(ka->sa.sa_flags & SA_RESTART)) {
		case ERESTARTNOHAND:
			regs->r0 = EINTR;
			break;
		}
		/* fallthrough */
	case ERESTARTNOINTR:
		regs->r0 = r0;	/* reset v0 and a3 and replay syscall */
		regs->r19 = r19;
		regs->pc -= 4;
		break;
	}
}


/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 *
 * Note that we go through the signals twice: once to check the signals that
 * the kernel can handle, and then we build all the user-level signal handling
 * stack-frames in one go after that.
 *
 * "r0" and "r19" are the registers we need to restore for system call
 * restart. "r0" is also used as an indicator whether we can restart at
 * all (if we get here from anything but a syscall return, it will be 0)
 */
asmlinkage int
do_signal(sigset_t *oldset, struct pt_regs * regs, struct switch_stack * sw,
	  unsigned long r0, unsigned long r19)
{
	unsigned long single_stepping = ptrace_cancel_bpt(current);

	if (!oldset)
		oldset = &current->blocked;

	while (1) {
		unsigned long signr;
		struct k_sigaction *ka;
		siginfo_t info;

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
			single_stepping |= ptrace_cancel_bpt(current);

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
			int exit_code = signr & 0x7f;

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
				if (!(current->p_pptr->sig->action[SIGCHLD-1]
				      .sa.sa_flags & SA_NOCLDSTOP))
					notify_parent(current, SIGCHLD);
				schedule();
				single_stepping |= ptrace_cancel_bpt(current);
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
				current->flags |= PF_SIGNALED;
				do_exit(exit_code);
				/* NOTREACHED */
			}
			continue;
		}

		/* Whee!  Actually deliver the signal.  */
		if (r0) syscall_restart(r0, r19, regs, ka);
		handle_signal(signr, ka, &info, oldset, regs, sw);
		if (single_stepping) 
			ptrace_set_bpt(current); /* re-set bpt */
		return 1;
	}

	if (r0 &&
	    (regs->r0 == ERESTARTNOHAND ||
	     regs->r0 == ERESTARTSYS ||
	     regs->r0 == ERESTARTNOINTR)) {
		regs->r0 = r0;	/* reset v0 and a3 and replay syscall */
		regs->r19 = r19;
		regs->pc -= 4;
	}
	if (single_stepping)
		ptrace_set_bpt(current);	/* re-set breakpoint */

	return 0;
}
