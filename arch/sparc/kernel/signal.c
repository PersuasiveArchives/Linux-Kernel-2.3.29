/*  $Id: signal.c,v 1.95 1999/08/14 03:51:22 anton Exp $
 *  linux/arch/sparc/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 *  Copyright (C) 1996 Miguel de Icaza (miguel@nuclecu.unam.mx)
 *  Copyright (C) 1997 Eddie C. Dost   (ecd@skynet.be)
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

#include <asm/uaccess.h>
#include <asm/bitops.h>
#include <asm/ptrace.h>
#include <asm/svr4.h>
#include <asm/pgtable.h>

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

asmlinkage int sys_wait4(pid_t pid, unsigned long *stat_addr,
			 int options, unsigned long *ru);

extern void fpsave(unsigned long *fpregs, unsigned long *fsr,
		   void *fpqueue, unsigned long *fpqdepth);
extern void fpload(unsigned long *fpregs, unsigned long *fsr);

asmlinkage int do_signal(sigset_t *oldset, struct pt_regs * regs,
			 unsigned long orig_o0, int ret_from_syscall);

/* This turned off for production... */
/* #define DEBUG_SIGNALS 1 */
/* #define DEBUG_SIGNALS_TRACE 1 */
/* #define DEBUG_SIGNALS_MAPS 1 */

/* Signal frames: the original one (compatible with SunOS):
 *
 * Set up a signal frame... Make the stack look the way SunOS
 * expects it to look which is basically:
 *
 * ---------------------------------- <-- %sp at signal time
 * Struct sigcontext
 * Signal address
 * Ptr to sigcontext area above
 * Signal code
 * The signal number itself
 * One register window
 * ---------------------------------- <-- New %sp
 */
struct signal_sframe {
	struct reg_window	sig_window;
	int			sig_num;
	int			sig_code;
	struct sigcontext	*sig_scptr;
	int			sig_address;
	struct sigcontext	sig_context;
	unsigned int		extramask[_NSIG_WORDS - 1];
};

/* 
 * And the new one, intended to be used for Linux applications only
 * (we have enough in there to work with clone).
 * All the interesting bits are in the info field.
 */

struct new_signal_frame {
	struct sparc_stackf	ss;
	__siginfo_t		info;
	__siginfo_fpu_t		*fpu_save;
	unsigned long		insns [2] __attribute__ ((aligned (8)));
	unsigned int		extramask[_NSIG_WORDS - 1];
	unsigned int		extra_size; /* Should be 0 */
	__siginfo_fpu_t		fpu_state;
};

struct rt_signal_frame {
	struct sparc_stackf	ss;
	siginfo_t		info;
	struct pt_regs		regs;
	sigset_t		mask;
	__siginfo_fpu_t		*fpu_save;
	unsigned int		insns [2];
	stack_t			stack;
	unsigned int		extra_size; /* Should be 0 */
	__siginfo_fpu_t		fpu_state;
};

/* Align macros */
#define SF_ALIGNEDSZ  (((sizeof(struct signal_sframe) + 7) & (~7)))
#define NF_ALIGNEDSZ  (((sizeof(struct new_signal_frame) + 7) & (~7)))
#define RT_ALIGNEDSZ  (((sizeof(struct rt_signal_frame) + 7) & (~7)))

/*
 * atomically swap in the new signal mask, and wait for a signal.
 * This is really tricky on the Sparc, watch out...
 */
asmlinkage void _sigpause_common(old_sigset_t set, struct pt_regs *regs)
{
	sigset_t saveset;

	set &= _BLOCKABLE;
	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	siginitset(&current->blocked, set);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->pc = regs->npc;
	regs->npc += 4;

	/* Condition codes and return value where set here for sigpause,
	 * and so got used by setup_frame, which again causes sigreturn()
	 * to return -EINTR.
	 */
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		/*
		 * Return -EINTR and set condition code here,
		 * so the interrupted system call actually returns
		 * these.
		 */
		regs->psr |= PSR_C;
		regs->u_regs[UREG_I0] = EINTR;
		if (do_signal(&saveset, regs, 0, 0))
			return;
	}
}

asmlinkage void do_sigpause(unsigned int set, struct pt_regs *regs)
{
	_sigpause_common(set, regs);
}

asmlinkage void do_sigsuspend (struct pt_regs *regs)
{
	_sigpause_common(regs->u_regs[UREG_I0], regs);
}

asmlinkage void do_rt_sigsuspend(sigset_t *uset, size_t sigsetsize,
				 struct pt_regs *regs)
{
	sigset_t oldset, set;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t)) {
		regs->psr |= PSR_C;
		regs->u_regs[UREG_I0] = EINVAL;
		return;
	}

	if (copy_from_user(&set, uset, sizeof(set))) {
		regs->psr |= PSR_C;
		regs->u_regs[UREG_I0] = EFAULT;
		return;
	}

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	oldset = current->blocked;
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->pc = regs->npc;
	regs->npc += 4;

	/* Condition codes and return value where set here for sigpause,
	 * and so got used by setup_frame, which again causes sigreturn()
	 * to return -EINTR.
	 */
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		/*
		 * Return -EINTR and set condition code here,
		 * so the interrupted system call actually returns
		 * these.
		 */
		regs->psr |= PSR_C;
		regs->u_regs[UREG_I0] = EINTR;
		if (do_signal(&oldset, regs, 0, 0))
			return;
	}
}

static inline int
restore_fpu_state(struct pt_regs *regs, __siginfo_fpu_t *fpu)
{
	int err;
#ifdef __SMP__
	if (current->flags & PF_USEDFPU)
		regs->psr &= ~PSR_EF;
#else
	if (current == last_task_used_math) {
		last_task_used_math = 0;
		regs->psr &= ~PSR_EF;
	}
#endif
	current->used_math = 1;
	current->flags &= ~PF_USEDFPU;
	
	if (verify_area (VERIFY_READ, fpu, sizeof(*fpu)))
		return -EFAULT;

	err = __copy_from_user(&current->thread.float_regs[0], &fpu->si_float_regs[0],
			       (sizeof(unsigned long) * 32));
	err |= __get_user(current->thread.fsr, &fpu->si_fsr);
	err |= __get_user(current->thread.fpqdepth, &fpu->si_fpqdepth);
	if (current->thread.fpqdepth != 0)
		err |= __copy_from_user(&current->thread.fpqueue[0],
					&fpu->si_fpqueue[0],
					((sizeof(unsigned long) +
					(sizeof(unsigned long *)))*16));
	return err;
}

static inline void do_new_sigreturn (struct pt_regs *regs)
{
	struct new_signal_frame *sf;
	unsigned long up_psr, pc, npc;
	sigset_t set;
	__siginfo_fpu_t *fpu_save;
	int err;

	sf = (struct new_signal_frame *) regs->u_regs [UREG_FP];

	/* 1. Make sure we are not getting garbage from the user */
	if (verify_area (VERIFY_READ, sf, sizeof (*sf)))
		goto segv_and_exit;

	if (((uint) sf) & 3)
		goto segv_and_exit;

	err = __get_user(pc,  &sf->info.si_regs.pc);
	err |= __get_user(npc, &sf->info.si_regs.npc);

	if ((pc | npc) & 3)
		goto segv_and_exit;

	/* 2. Restore the state */
	up_psr = regs->psr;
	err |= __copy_from_user(regs, &sf->info.si_regs, sizeof (struct pt_regs));

	/* User can only change condition codes and FPU enabling in %psr. */
	regs->psr = (up_psr & ~(PSR_ICC | PSR_EF))
		  | (regs->psr & (PSR_ICC | PSR_EF));

	err |= __get_user(fpu_save, &sf->fpu_save);

	if (fpu_save)
		err |= restore_fpu_state(regs, fpu_save);

	/* This is pretty much atomic, no amount locking would prevent
	 * the races which exist anyways.
	 */
	err |= __get_user(set.sig[0], &sf->info.si_mask);
	err |= __copy_from_user(&set.sig[1], &sf->extramask,
			        (_NSIG_WORDS-1) * sizeof(unsigned int));
			   
	if (err)
		goto segv_and_exit;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);
	return;

segv_and_exit:
	do_exit(SIGSEGV);
}

asmlinkage void do_sigreturn(struct pt_regs *regs)
{
	struct sigcontext *scptr;
	unsigned long pc, npc, psr;
	sigset_t set;
	int err;

	synchronize_user_stack();

	if (current->thread.new_signal)
		return do_new_sigreturn (regs);

	scptr = (struct sigcontext *) regs->u_regs[UREG_I0];

	/* Check sanity of the user arg. */
	if(verify_area(VERIFY_READ, scptr, sizeof(struct sigcontext)) ||
	   (((unsigned long) scptr) & 3))
		goto segv_and_exit;

	err = __get_user(pc, &scptr->sigc_pc);
	err |= __get_user(npc, &scptr->sigc_npc);

	if((pc | npc) & 3)
		goto segv_and_exit;

	/* This is pretty much atomic, no amount locking would prevent
	 * the races which exist anyways.
	 */
	err |= __get_user(set.sig[0], &scptr->sigc_mask);
	/* Note that scptr + 1 points to extramask */
	err |= __copy_from_user(&set.sig[1], scptr + 1,
				(_NSIG_WORDS - 1) * sizeof(unsigned int));
	
	if (err)
		goto segv_and_exit;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	regs->pc = pc;
	regs->npc = npc;

	err = __get_user(regs->u_regs[UREG_FP], &scptr->sigc_sp);
	err |= __get_user(regs->u_regs[UREG_I0], &scptr->sigc_o0);
	err |= __get_user(regs->u_regs[UREG_G1], &scptr->sigc_g1);

	/* User can only change condition codes in %psr. */
	err |= __get_user(psr, &scptr->sigc_psr);
	if (err)
		goto segv_and_exit;
		
	regs->psr &= ~(PSR_ICC);
	regs->psr |= (psr & PSR_ICC);
	return;

segv_and_exit:
	send_sig(SIGSEGV, current, 1);
}

asmlinkage void do_rt_sigreturn(struct pt_regs *regs)
{
	struct rt_signal_frame *sf;
	unsigned int psr, pc, npc;
	__siginfo_fpu_t *fpu_save;
	sigset_t set;
	stack_t st;
	int err;

	synchronize_user_stack();
	sf = (struct rt_signal_frame *) regs->u_regs[UREG_FP];
	if(verify_area(VERIFY_READ, sf, sizeof(*sf)) ||
	   (((unsigned long) sf) & 0x03))
		goto segv;

	err = __get_user(pc, &sf->regs.pc);
	err |= __get_user(npc, &sf->regs.npc);
	err |= ((pc | npc) & 0x03);

	err |= __get_user(regs->y, &sf->regs.y);
	err |= __get_user(psr, &sf->regs.psr);

	err |= __copy_from_user(&regs->u_regs[UREG_G1], &sf->regs.u_regs[UREG_G1], 15*sizeof(u32));

	regs->psr = (regs->psr & ~PSR_ICC) | (psr & PSR_ICC);

	err |= __get_user(fpu_save, &sf->fpu_save);

	if(fpu_save)
		err |= restore_fpu_state(regs, fpu_save);
	err |= __copy_from_user(&set, &sf->mask, sizeof(sigset_t));
	
	err |= __copy_from_user(&st, &sf->stack, sizeof(stack_t));
	
	if (err)
		goto segv;
		
	regs->pc = pc;
	regs->npc = npc;
	
	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&st, NULL, (unsigned long)sf);

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);
	return;
segv:
	send_sig(SIGSEGV, current, 1);
}

/* Checks if the fp is valid */
static inline int invalid_frame_pointer (void *fp, int fplen)
{
	if ((((unsigned long) fp) & 7) ||
	    !__access_ok((unsigned long)fp, fplen) ||
	    ((sparc_cpu_model == sun4 || sparc_cpu_model == sun4c) &&
	     ((unsigned long) fp < 0xe0000000 && (unsigned long) fp >= 0x20000000)))
		return 1;
	
	return 0;
}

static inline void *get_sigframe(struct sigaction *sa, struct pt_regs *regs, unsigned long framesize)
{
	unsigned long sp;

	sp = regs->u_regs[UREG_FP];

	/* This is the X/Open sanctioned signal stack switching.  */
	if (sa->sa_flags & SA_ONSTACK) {
		if (!on_sig_stack(sp) && !((current->sas_ss_sp + current->sas_ss_size) & 7))
			sp = current->sas_ss_sp + current->sas_ss_size;
	}
	return (void *)(sp - framesize);
}

static inline void
setup_frame(struct sigaction *sa, unsigned long pc, unsigned long npc,
	    struct pt_regs *regs, int signr, sigset_t *oldset)
{
	struct signal_sframe *sframep;
	struct sigcontext *sc;
	int window = 0, err;

	synchronize_user_stack();
	sframep = (struct signal_sframe *)get_sigframe(sa, regs, SF_ALIGNEDSZ);
	if (invalid_frame_pointer (sframep, sizeof(*sframep))){
#ifdef DEBUG_SIGNALS /* fills up the console logs during crashme runs, yuck... */
		printk("%s [%d]: User has trashed signal stack\n",
		       current->comm, current->pid);
		printk("Sigstack ptr %p handler at pc<%08lx> for sig<%d>\n",
		       sframep, pc, signr);
#endif
		/* Don't change signal code and address, so that
		 * post mortem debuggers can have a look.
		 */
		goto sigill_and_return;
	}

	sc = &sframep->sig_context;

	/* We've already made sure frame pointer isn't in kernel space... */
	err  = __put_user((sas_ss_flags(regs->u_regs[UREG_FP]) == SS_ONSTACK),
			 &sc->sigc_onstack);
	err |= __put_user(oldset->sig[0], &sc->sigc_mask);
	err |= __copy_to_user(sframep->extramask, &oldset->sig[1],
			      (_NSIG_WORDS - 1) * sizeof(unsigned int));
	err |= __put_user(regs->u_regs[UREG_FP], &sc->sigc_sp);
	err |= __put_user(pc, &sc->sigc_pc);
	err |= __put_user(npc, &sc->sigc_npc);
	err |= __put_user(regs->psr, &sc->sigc_psr);
	err |= __put_user(regs->u_regs[UREG_G1], &sc->sigc_g1);
	err |= __put_user(regs->u_regs[UREG_I0], &sc->sigc_o0);
	err |= __put_user(current->thread.w_saved, &sc->sigc_oswins);
	if(current->thread.w_saved)
		for(window = 0; window < current->thread.w_saved; window++) {
			sc->sigc_spbuf[window] =
				(char *)current->thread.rwbuf_stkptrs[window];
			err |= __copy_to_user(&sc->sigc_wbuf[window],
					      &current->thread.reg_window[window],
					      sizeof(struct reg_window));
		}
	else
		err |= __copy_to_user(sframep, (char *)regs->u_regs[UREG_FP],
				      sizeof(struct reg_window));

	current->thread.w_saved = 0; /* So process is allowed to execute. */
	err |= __put_user(signr, &sframep->sig_num);
	if(signr == SIGSEGV ||
	   signr == SIGILL ||
	   signr == SIGFPE ||
	   signr == SIGBUS ||
	   signr == SIGEMT) {
		err |= __put_user(current->thread.sig_desc, &sframep->sig_code);
		err |= __put_user(current->thread.sig_address, &sframep->sig_address);
	} else {
		err |= __put_user(0, &sframep->sig_code);
		err |= __put_user(0, &sframep->sig_address);
	}
	err |= __put_user(sc, &sframep->sig_scptr);
	if (err)
		goto sigsegv;

	regs->u_regs[UREG_FP] = (unsigned long) sframep;
	regs->pc = (unsigned long) sa->sa_handler;
	regs->npc = (regs->pc + 4);
	return;

sigill_and_return:
	do_exit(SIGILL);
sigsegv:
	do_exit(SIGSEGV);
}


static inline int
save_fpu_state(struct pt_regs *regs, __siginfo_fpu_t *fpu)
{
	int err = 0;
#ifdef __SMP__
	if (current->flags & PF_USEDFPU) {
		put_psr(get_psr() | PSR_EF);
		fpsave(&current->thread.float_regs[0], &current->thread.fsr,
		       &current->thread.fpqueue[0], &current->thread.fpqdepth);
		regs->psr &= ~(PSR_EF);
		current->flags &= ~(PF_USEDFPU);
	}
#else
	if (current == last_task_used_math) {
		put_psr(get_psr() | PSR_EF);
		fpsave(&current->thread.float_regs[0], &current->thread.fsr,
		       &current->thread.fpqueue[0], &current->thread.fpqdepth);
		last_task_used_math = 0;
		regs->psr &= ~(PSR_EF);
	}
#endif
	err |= __copy_to_user(&fpu->si_float_regs[0], &current->thread.float_regs[0],
			      (sizeof(unsigned long) * 32));
	err |= __put_user(current->thread.fsr, &fpu->si_fsr);
	err |= __put_user(current->thread.fpqdepth, &fpu->si_fpqdepth);
	if (current->thread.fpqdepth != 0)
		err |= __copy_to_user(&fpu->si_fpqueue[0], &current->thread.fpqueue[0],
				      ((sizeof(unsigned long) +
				      (sizeof(unsigned long *)))*16));
	current->used_math = 0;
	return err;
}

static inline void
new_setup_frame(struct k_sigaction *ka, struct pt_regs *regs,
		int signo, sigset_t *oldset)
{
	struct new_signal_frame *sf;
	int sigframe_size, err;

	/* 1. Make sure everything is clean */
	synchronize_user_stack();

	sigframe_size = NF_ALIGNEDSZ;
	if (!current->used_math)
		sigframe_size -= sizeof(__siginfo_fpu_t);

	sf = (struct new_signal_frame *)get_sigframe(&ka->sa, regs, sigframe_size);

	if (invalid_frame_pointer (sf, sigframe_size))
		goto sigill_and_return;

	if (current->thread.w_saved != 0) {
#ifdef DEBUG_SIGNALS 
		printk ("%s [%d]: Invalid user stack frame for "
			"signal delivery.\n", current->comm, current->pid);
#endif
		goto sigill_and_return;
	}

	/* 2. Save the current process state */
	err = __copy_to_user(&sf->info.si_regs, regs, sizeof (struct pt_regs));
	
	err |= __put_user(0, &sf->extra_size);

	if (current->used_math) {
		err |= save_fpu_state(regs, &sf->fpu_state);
		err |= __put_user(&sf->fpu_state, &sf->fpu_save);
	} else {
		err |= __put_user(0, &sf->fpu_save);
	}

	err |= __put_user(oldset->sig[0], &sf->info.si_mask);
	err |= __copy_to_user(sf->extramask, &oldset->sig[1],
			      (_NSIG_WORDS - 1) * sizeof(unsigned int));
	err |= __copy_to_user(sf, (char *) regs->u_regs [UREG_FP],
			      sizeof (struct reg_window));
	if (err)
		goto sigsegv;
	
	/* 3. signal handler back-trampoline and parameters */
	regs->u_regs[UREG_FP] = (unsigned long) sf;
	regs->u_regs[UREG_I0] = signo;
	regs->u_regs[UREG_I1] = (unsigned long) &sf->info;

	/* 4. signal handler */
	regs->pc = (unsigned long) ka->sa.sa_handler;
	regs->npc = (regs->pc + 4);

	/* 5. return to kernel instructions */
	if (ka->ka_restorer)
		regs->u_regs[UREG_I7] = (unsigned long)ka->ka_restorer;
	else {
		regs->u_regs[UREG_I7] = (unsigned long)(&(sf->insns[0]) - 2);

		/* mov __NR_sigreturn, %g1 */
		err |= __put_user(0x821020d8, &sf->insns[0]);

		/* t 0x10 */
		err |= __put_user(0x91d02010, &sf->insns[1]);
		if (err)
			goto sigsegv;

		/* Flush instruction space. */
		flush_sig_insns(current->mm, (unsigned long) &(sf->insns[0]));
	}
	return;

sigill_and_return:
	do_exit(SIGILL);
sigsegv:
	do_exit(SIGSEGV);
}

static inline void
new_setup_rt_frame(struct k_sigaction *ka, struct pt_regs *regs,
		   int signo, sigset_t *oldset, siginfo_t *info)
{
	struct rt_signal_frame *sf;
	int sigframe_size;
	unsigned int psr;
	int err;

	synchronize_user_stack();
	sigframe_size = RT_ALIGNEDSZ;
	if(!current->used_math)
		sigframe_size -= sizeof(__siginfo_fpu_t);
	sf = (struct rt_signal_frame *)get_sigframe(&ka->sa, regs, sigframe_size);
	if(invalid_frame_pointer(sf, sigframe_size))
		goto sigill;
	if(current->thread.w_saved != 0)
		goto sigill;

	err  = __put_user(regs->pc, &sf->regs.pc);
	err |= __put_user(regs->npc, &sf->regs.npc);
	err |= __put_user(regs->y, &sf->regs.y);
	psr = regs->psr;
	if(current->used_math)
		psr |= PSR_EF;
	err |= __put_user(psr, &sf->regs.psr);
	err |= __copy_to_user(&sf->regs.u_regs, regs->u_regs, sizeof(regs->u_regs));
	err |= __put_user(0, &sf->extra_size);

	if(psr & PSR_EF) {
		err |= save_fpu_state(regs, &sf->fpu_state);
		err |= __put_user(&sf->fpu_state, &sf->fpu_save);
	} else {
		err |= __put_user(0, &sf->fpu_save);
	}
	err |= __copy_to_user(&sf->mask, &oldset->sig[0], sizeof(sigset_t));
	
	/* Setup sigaltstack */
	err |= __put_user(current->sas_ss_sp, &sf->stack.ss_sp);
	err |= __put_user(sas_ss_flags(regs->u_regs[UREG_FP]), &sf->stack.ss_flags);
	err |= __put_user(current->sas_ss_size, &sf->stack.ss_size);
	
	err |= __copy_to_user(sf, (char *) regs->u_regs [UREG_FP],
			      sizeof (struct reg_window));	

	err |= __copy_to_user(&sf->info, info, sizeof(siginfo_t));

	if (err)
		goto sigsegv;

	regs->u_regs[UREG_FP] = (unsigned long) sf;
	regs->u_regs[UREG_I0] = signo;
	regs->u_regs[UREG_I1] = (unsigned long) &sf->info;

	regs->pc = (unsigned long) ka->sa.sa_handler;
	regs->npc = (regs->pc + 4);

	if(ka->ka_restorer)
		regs->u_regs[UREG_I7] = (unsigned long)ka->ka_restorer;
	else {
		regs->u_regs[UREG_I7] = (unsigned long)(&(sf->insns[0]) - 2);

		/* mov __NR_sigreturn, %g1 */
		err |= __put_user(0x821020d8, &sf->insns[0]);

		/* t 0x10 */
		err |= __put_user(0x91d02010, &sf->insns[1]);
		if (err)
			goto sigsegv;

		/* Flush instruction space. */
		flush_sig_insns(current->mm, (unsigned long) &(sf->insns[0]));
	}
	return;

sigill:
	do_exit(SIGILL);
sigsegv:
	do_exit(SIGSEGV);
}

/* Setup a Solaris stack frame */
static inline void
setup_svr4_frame(struct sigaction *sa, unsigned long pc, unsigned long npc,
		 struct pt_regs *regs, int signr, sigset_t *oldset)
{
	svr4_signal_frame_t *sfp;
	svr4_gregset_t  *gr;
	svr4_siginfo_t  *si;
	svr4_mcontext_t *mc;
	svr4_gwindows_t *gw;
	svr4_ucontext_t *uc;
	svr4_sigset_t	setv;
	int window = 0, err;

	synchronize_user_stack();
	sfp = (svr4_signal_frame_t *) get_sigframe(sa, regs, SVR4_SF_ALIGNED + REGWIN_SZ);

	if (invalid_frame_pointer (sfp, sizeof (*sfp))){
#ifdef DEBUG_SIGNALS
		printk ("Invalid stack frame\n");
#endif
		goto sigill_and_return;
	}

	/* Start with a clean frame pointer and fill it */
	err = __clear_user(sfp, sizeof (*sfp));

	/* Setup convenience variables */
	si = &sfp->si;
	uc = &sfp->uc;
	gw = &sfp->gw;
	mc = &uc->mcontext;
	gr = &mc->greg;
	
	/* FIXME: where am I supposed to put this?
	 * sc->sigc_onstack = old_status;
	 * anyways, it does not look like it is used for anything at all.
	 */
	setv.sigbits[0] = oldset->sig[0];
	setv.sigbits[1] = oldset->sig[1];
	if (_NSIG_WORDS >= 4) {
		setv.sigbits[2] = oldset->sig[2];
		setv.sigbits[3] = oldset->sig[3];
		err |= __copy_to_user(&uc->sigmask, &setv, sizeof(svr4_sigset_t));
	} else
		err |= __copy_to_user(&uc->sigmask, &setv, 2 * sizeof(unsigned int));

	/* Store registers */
	err |= __put_user(regs->pc, &((*gr) [SVR4_PC]));
	err |= __put_user(regs->npc, &((*gr) [SVR4_NPC]));
	err |= __put_user(regs->psr, &((*gr) [SVR4_PSR]));
	err |= __put_user(regs->y, &((*gr) [SVR4_Y]));
	
	/* Copy g [1..7] and o [0..7] registers */
	err |= __copy_to_user(&(*gr)[SVR4_G1], &regs->u_regs [UREG_G1], sizeof (long) * 7);
	err |= __copy_to_user(&(*gr)[SVR4_O0], &regs->u_regs [UREG_I0], sizeof (long) * 8);

	/* Setup sigaltstack */
	err |= __put_user(current->sas_ss_sp, &uc->stack.sp);
	err |= __put_user(sas_ss_flags(regs->u_regs[UREG_FP]), &uc->stack.flags);
	err |= __put_user(current->sas_ss_size, &uc->stack.size);

	/* Save the currently window file: */

	/* 1. Link sfp->uc->gwins to our windows */
	err |= __put_user(gw, &mc->gwin);
	    
	/* 2. Number of windows to restore at setcontext (): */
	err |= __put_user(current->thread.w_saved, &gw->count);

	/* 3. Save each valid window
	 *    Currently, it makes a copy of the windows from the kernel copy.
	 *    David's code for SunOS, makes the copy but keeps the pointer to
	 *    the kernel.  My version makes the pointer point to a userland 
	 *    copy of those.  Mhm, I wonder if I shouldn't just ignore those
	 *    on setcontext and use those that are on the kernel, the signal
	 *    handler should not be modyfing those, mhm.
	 *
	 *    These windows are just used in case synchronize_user_stack failed
	 *    to flush the user windows.
	 */
	for(window = 0; window < current->thread.w_saved; window++) {
		err |= __put_user((int *) &(gw->win [window]), &gw->winptr [window]);
		err |= __copy_to_user(&gw->win [window],
				      &current->thread.reg_window [window],
				      sizeof (svr4_rwindow_t));
		err |= __put_user(0, gw->winptr [window]);
	}

	/* 4. We just pay attention to the gw->count field on setcontext */
	current->thread.w_saved = 0; /* So process is allowed to execute. */

	/* Setup the signal information.  Solaris expects a bunch of
	 * information to be passed to the signal handler, we don't provide
	 * that much currently, should use those that David already
	 * is providing with thread.sig_desc
	 */
	err |= __put_user(signr, &si->siginfo.signo);
	err |= __put_user(SVR4_SINOINFO, &si->siginfo.code);
	if (err)
		goto sigsegv;

	regs->u_regs[UREG_FP] = (unsigned long) sfp;
	regs->pc = (unsigned long) sa->sa_handler;
	regs->npc = (regs->pc + 4);

#ifdef DEBUG_SIGNALS
	printk ("Solaris-frame: %x %x\n", (int) regs->pc, (int) regs->npc);
#endif
	/* Arguments passed to signal handler */
	if (regs->u_regs [14]){
		struct reg_window *rw = (struct reg_window *) regs->u_regs [14];

		err |= __put_user(signr, &rw->ins [0]);
		err |= __put_user(si, &rw->ins [1]);
		err |= __put_user(uc, &rw->ins [2]);
		err |= __put_user(sfp, &rw->ins [6]);	/* frame pointer */
		if (err)
			goto sigsegv;

		regs->u_regs[UREG_I0] = signr;
		regs->u_regs[UREG_I1] = (uint) si;
		regs->u_regs[UREG_I2] = (uint) uc;
	}
	return;

sigill_and_return:
	do_exit(SIGILL);
sigsegv:
	do_exit(SIGSEGV);
}

asmlinkage int svr4_getcontext (svr4_ucontext_t *uc, struct pt_regs *regs)
{
	svr4_gregset_t  *gr;
	svr4_mcontext_t *mc;
	svr4_sigset_t	setv;
	int err = 0;

	synchronize_user_stack();

	if (current->thread.w_saved)
		goto sigsegv_and_return;

	err = clear_user(uc, sizeof (*uc));
	if (err)
		return -EFAULT;

	/* Setup convenience variables */
	mc = &uc->mcontext;
	gr = &mc->greg;

	setv.sigbits[0] = current->blocked.sig[0];
	setv.sigbits[1] = current->blocked.sig[1];
	if (_NSIG_WORDS >= 4) {
		setv.sigbits[2] = current->blocked.sig[2];
		setv.sigbits[3] = current->blocked.sig[3];
		err |= __copy_to_user(&uc->sigmask, &setv, sizeof(svr4_sigset_t));
	} else
		err |= __copy_to_user(&uc->sigmask, &setv, 2 * sizeof(unsigned int));

	/* Store registers */
	err |= __put_user(regs->pc, &uc->mcontext.greg [SVR4_PC]);
	err |= __put_user(regs->npc, &uc->mcontext.greg [SVR4_NPC]);
	err |= __put_user(regs->psr, &uc->mcontext.greg [SVR4_PSR]);
	err |= __put_user(regs->y, &uc->mcontext.greg [SVR4_Y]);
	
	/* Copy g [1..7] and o [0..7] registers */
	err |= __copy_to_user(&(*gr)[SVR4_G1], &regs->u_regs [UREG_G1], sizeof (uint) * 7);
	err |= __copy_to_user(&(*gr)[SVR4_O0], &regs->u_regs [UREG_I0], sizeof (uint) * 8);

	/* Setup sigaltstack */
	err |= __put_user(current->sas_ss_sp, &uc->stack.sp);
	err |= __put_user(sas_ss_flags(regs->u_regs[UREG_FP]), &uc->stack.flags);
	err |= __put_user(current->sas_ss_size, &uc->stack.size);

	/* The register file is not saved
	 * we have already stuffed all of it with sync_user_stack
	 */
	return (err ? -EFAULT : 0);

sigsegv_and_return:
	do_exit(SIGSEGV);
}

/* Set the context for a svr4 application, this is Solaris way to sigreturn */
asmlinkage int svr4_setcontext (svr4_ucontext_t *c, struct pt_regs *regs)
{
	struct thread_struct *tp = &current->thread;
	svr4_gregset_t  *gr;
	unsigned long pc, npc, psr;
	sigset_t set;
	svr4_sigset_t setv;
	int err;
	stack_t st;
	
	/* Fixme: restore windows, or is this already taken care of in
	 * svr4_setup_frame when sync_user_windows is done?
	 */
	flush_user_windows();
	
	if (tp->w_saved)
		goto sigsegv_and_return;

	if (((uint) c) & 3)
		goto sigsegv_and_return;

	if(!__access_ok((unsigned long)c, sizeof(*c)))
		goto sigsegv_and_return;

	/* Check for valid PC and nPC */
	gr = &c->mcontext.greg;
	err = __get_user(pc, &((*gr)[SVR4_PC]));
	err |= __get_user(npc, &((*gr)[SVR4_NPC]));

	if((pc | npc) & 3)
		goto sigsegv_and_return;

	/* Retrieve information from passed ucontext */
	/* note that nPC is ored a 1, this is used to inform entry.S */
	/* that we don't want it to mess with our PC and nPC */

	/* This is pretty much atomic, no amount locking would prevent
	 * the races which exist anyways.
	 */
	err |= __copy_from_user(&setv, &c->sigmask, sizeof(svr4_sigset_t));
	
	err |= __get_user(st.ss_sp, &c->stack.sp);
	err |= __get_user(st.ss_flags, &c->stack.flags);
	err |= __get_user(st.ss_size, &c->stack.size);
	
	if (err)
		goto sigsegv_and_return;
		
	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&st, NULL, regs->u_regs[UREG_I6]);
	
	set.sig[0] = setv.sigbits[0];
	set.sig[1] = setv.sigbits[1];
	if (_NSIG_WORDS >= 4) {
		set.sig[2] = setv.sigbits[2];
		set.sig[3] = setv.sigbits[3];
	}
	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);
	regs->pc = pc;
	regs->npc = npc | 1;
	err |= __get_user(regs->y, &((*gr) [SVR4_Y]));
	err |= __get_user(psr, &((*gr) [SVR4_PSR]));
	regs->psr &= ~(PSR_ICC);
	regs->psr |= (psr & PSR_ICC);

	/* Restore g[1..7] and o[0..7] registers */
	err |= __copy_from_user(&regs->u_regs [UREG_G1], &(*gr)[SVR4_G1],
			      sizeof (long) * 7);
	err |= __copy_from_user(&regs->u_regs [UREG_I0], &(*gr)[SVR4_O0],
			      sizeof (long) * 8);
	return (err ? -EFAULT : 0);

sigsegv_and_return:
	do_exit(SIGSEGV);
}

static inline void
handle_signal(unsigned long signr, struct k_sigaction *ka,
	      siginfo_t *info, sigset_t *oldset, struct pt_regs *regs,
	      int svr4_signal)
{
	if (svr4_signal)
		setup_svr4_frame(&ka->sa, regs->pc, regs->npc, regs, signr, oldset);
	else {
		if (ka->sa.sa_flags & SA_SIGINFO)
			new_setup_rt_frame(ka, regs, signr, oldset, info);
		else if (current->thread.new_signal)
			new_setup_frame (ka, regs, signr, oldset);
		else
			setup_frame(&ka->sa, regs->pc, regs->npc, regs, signr, oldset);
	}
	if(ka->sa.sa_flags & SA_ONESHOT)
		ka->sa.sa_handler = SIG_DFL;
	if(!(ka->sa.sa_flags & SA_NOMASK)) {
		spin_lock_irq(&current->sigmask_lock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked, signr);
		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);
	}
}

static inline void syscall_restart(unsigned long orig_i0, struct pt_regs *regs,
				   struct sigaction *sa)
{
	switch(regs->u_regs[UREG_I0]) {
		case ERESTARTNOHAND:
		no_system_call_restart:
			regs->u_regs[UREG_I0] = EINTR;
			regs->psr |= PSR_C;
			break;
		case ERESTARTSYS:
			if(!(sa->sa_flags & SA_RESTART))
				goto no_system_call_restart;
		/* fallthrough */
		case ERESTARTNOINTR:
			regs->u_regs[UREG_I0] = orig_i0;
			regs->pc -= 4;
			regs->npc -= 4;
	}
}

#ifdef DEBUG_SIGNALS_MAPS

#define MAPS_LINE_FORMAT	  "%08lx-%08lx %s %08lx %s %lu "

static inline void read_maps (void)
{
	struct vm_area_struct * map, * next;
	char * buffer;
	ssize_t i;

	buffer = (char*)__get_free_page(GFP_KERNEL);
	if (!buffer)
		return;

	for (map = current->mm->mmap ; map ; map = next ) {
		/* produce the next line */
		char *line;
		char str[5], *cp = str;
		int flags;
		kdev_t dev;
		unsigned long ino;

		/*
		 * Get the next vma now (but it won't be used if we sleep).
		 */
		next = map->vm_next;
		flags = map->vm_flags;

		*cp++ = flags & VM_READ ? 'r' : '-';
		*cp++ = flags & VM_WRITE ? 'w' : '-';
		*cp++ = flags & VM_EXEC ? 'x' : '-';
		*cp++ = flags & VM_MAYSHARE ? 's' : 'p';
		*cp++ = 0;

		dev = 0;
		ino = 0;
		if (map->vm_file != NULL) {
			dev = map->vm_file->f_dentry->d_inode->i_dev;
			ino = map->vm_file->f_dentry->d_inode->i_ino;
			line = d_path(map->vm_file->f_dentry, buffer, PAGE_SIZE);
		}
		printk(MAPS_LINE_FORMAT, map->vm_start, map->vm_end, str, map->vm_pgoff << PAGE_SHIFT,
			      kdevname(dev), ino);
		if (map->vm_file != NULL)
			printk("%s\n", line);
		else
			printk("\n");
	}
	free_page((unsigned long)buffer);
	return;
}
#endif

/* Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 */
asmlinkage int do_signal(sigset_t *oldset, struct pt_regs * regs,
			 unsigned long orig_i0, int restart_syscall)
{
	unsigned long signr;
	struct k_sigaction *ka;
	siginfo_t info;

	int svr4_signal = current->personality == PER_SVR4;

	if (!oldset)
		oldset = &current->blocked;

	for (;;) {
		spin_lock_irq(&current->sigmask_lock);
		signr = dequeue_signal(&current->blocked, &info);
		spin_unlock_irq(&current->sigmask_lock);

		if (!signr) break;

		if ((current->flags & PF_PTRACED) && signr != SIGKILL) {
			current->exit_code = signr;
			current->state = TASK_STOPPED;

			/* This happens to be SMP safe so no need to
			 * grab master kernel lock even in this case.
			 */
			notify_parent(current, SIGCHLD);
			schedule();
			if (!(signr = current->exit_code))
				continue;
			current->exit_code = 0;
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

		if(ka->sa.sa_handler == SIG_IGN) {
			if(signr != SIGCHLD)
				continue;

			/* sys_wait4() grabs the master kernel lock, so
			 * we need not do so, that sucker should be
			 * threaded and would not be that difficult to
			 * do anyways.
			 */
			while(sys_wait4(-1, NULL, WNOHANG, NULL) > 0)
				;
			continue;
		}
		if(ka->sa.sa_handler == SIG_DFL) {
			unsigned long exit_code = signr;

			if(current->pid == 1)
				continue;
			switch(signr) {
			case SIGCONT: case SIGCHLD: case SIGWINCH:
				continue;

			case SIGTSTP: case SIGTTIN: case SIGTTOU:
				/* The operations performed by
				 * is_orphaned_pgrp() are protected by
				 * the tasklist_lock.
				 */
				if (is_orphaned_pgrp(current->pgrp))
					continue;

			case SIGSTOP:
				if (current->flags & PF_PTRACED)
					continue;
				current->state = TASK_STOPPED;
				current->exit_code = signr;

				/* notify_parent() is SMP safe */
				if(!(current->p_pptr->sig->action[SIGCHLD-1].sa.sa_flags &
				     SA_NOCLDSTOP))
					notify_parent(current, SIGCHLD);
				schedule();
				continue;

			case SIGQUIT: case SIGILL: case SIGTRAP:
			case SIGABRT: case SIGFPE: case SIGSEGV:
			case SIGBUS: case SIGSYS: case SIGXCPU: case SIGXFSZ:
				if (do_coredump(signr, regs))
					exit_code |= 0x80;
#ifdef DEBUG_SIGNALS
				/* Very useful to debug dynamic linker problems */
				printk ("Sig %ld going for %s[%d]...\n", signr, current->comm, current->pid);
				show_regs (regs);
#ifdef DEBUG_SIGNALS_TRACE
				{
					struct reg_window *rw = (struct reg_window *)regs->u_regs[UREG_FP];
					unsigned int ins[8];

					while(rw &&
					      !(((unsigned long) rw) & 0x3)) {
						copy_from_user(ins, &rw->ins[0], sizeof(ins));
						printk("Caller[%08x](%08x,%08x,%08x,%08x,%08x,%08x)\n", ins[7], ins[0], ins[1], ins[2], ins[3], ins[4], ins[5]);
						rw = (struct reg_window *)(unsigned long)ins[6];
					}
				}
#endif
#ifdef DEBUG_SIGNALS_MAPS
				printk("Maps:\n");
				read_maps();
#endif
#endif
				/* fall through */
			default:
				lock_kernel();
				sigaddset(&current->signal, signr);
				recalc_sigpending(current);
				current->flags |= PF_SIGNALED;
				do_exit(exit_code);
				/* NOT REACHED */
			}
		}
		if(restart_syscall)
			syscall_restart(orig_i0, regs, &ka->sa);
		handle_signal(signr, ka, &info, oldset, regs, svr4_signal);
		return 1;
	}
	if(restart_syscall &&
	   (regs->u_regs[UREG_I0] == ERESTARTNOHAND ||
	    regs->u_regs[UREG_I0] == ERESTARTSYS ||
	    regs->u_regs[UREG_I0] == ERESTARTNOINTR)) {
		/* replay the system call when we are done */
		regs->u_regs[UREG_I0] = orig_i0;
		regs->pc -= 4;
		regs->npc -= 4;
	}
	return 0;
}

asmlinkage int
do_sys_sigstack(struct sigstack *ssptr, struct sigstack *ossptr, unsigned long sp)
{
	int ret = -EFAULT;

	/* First see if old state is wanted. */
	if (ossptr) {
		if (put_user(current->sas_ss_sp + current->sas_ss_size, &ossptr->the_stack) ||
		    __put_user(on_sig_stack(sp), &ossptr->cur_status))
			goto out;
	}

	/* Now see if we want to update the new state. */
	if (ssptr) {
		void *ss_sp;

		if (get_user((long)ss_sp, &ssptr->the_stack))
			goto out;
		/* If the current stack was set with sigaltstack, don't
		   swap stacks while we are on it.  */
		ret = -EPERM;
		if (current->sas_ss_sp && on_sig_stack(sp))
			goto out;

		/* Since we don't know the extent of the stack, and we don't
		   track onstack-ness, but rather calculate it, we must
		   presume a size.  Ho hum this interface is lossy.  */
		current->sas_ss_sp = (unsigned long)ss_sp - SIGSTKSZ;
		current->sas_ss_size = SIGSTKSZ;
	}
	ret = 0;
out:
	return ret;
}
