#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

#include <asm/param.h>	/* for HZ */

extern unsigned long event;

#include <linux/binfmts.h>
#include <linux/personality.h>
#include <linux/threads.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/times.h>
#include <linux/timex.h>

#include <asm/system.h>
#include <asm/semaphore.h>
#include <asm/page.h>
#include <asm/ptrace.h>

#include <linux/smp.h>
#include <linux/tty.h>
#include <linux/sem.h>
#include <linux/signal.h>
#include <linux/securebits.h>

/*
 * cloning flags:
 */
#define CSIGNAL		0x000000ff	/* signal mask to be sent at exit */
#define CLONE_VM	0x00000100	/* set if VM shared between processes */
#define CLONE_FS	0x00000200	/* set if fs info shared between processes */
#define CLONE_FILES	0x00000400	/* set if open files shared between processes */
#define CLONE_SIGHAND	0x00000800	/* set if signal handlers shared */
#define CLONE_PID	0x00001000	/* set if pid shared */
#define CLONE_PTRACE	0x00002000	/* set if we want to let tracing continue on the child too */
#define CLONE_VFORK	0x00004000	/* set if the parent wants the child to wake it up on mm_release */
#define CLONE_PARENT	0x00008000	/* set if we want to have the same parent as the cloner */

/*
 * These are the constant used to fake the fixed-point load-average
 * counting. Some notes:
 *  - 11 bit fractions expand to 22 bits by the multiplies: this gives
 *    a load-average precision of 10 bits integer + 11 bits fractional
 *  - if you want to count load-averages more often, you need more
 *    precision, or rounding will get you. With 2-second counting freq,
 *    the EXP_n values would be 1981, 2034 and 2043 if still using only
 *    11 bit fractions.
 */
extern unsigned long avenrun[];		/* Load averages */

#define FSHIFT		11		/* nr of bits of precision */
#define FIXED_1		(1<<FSHIFT)	/* 1.0 as fixed-point */
#define LOAD_FREQ	(5*HZ)		/* 5 sec intervals */
#define EXP_1		1884		/* 1/exp(5sec/1min) as fixed-point */
#define EXP_5		2014		/* 1/exp(5sec/5min) */
#define EXP_15		2037		/* 1/exp(5sec/15min) */

#define CALC_LOAD(load,exp,n) \
	load *= exp; \
	load += n*(FIXED_1-exp); \
	load >>= FSHIFT;

#define CT_TO_SECS(x)	((x) / HZ)
#define CT_TO_USECS(x)	(((x) % HZ) * 1000000/HZ)

extern int nr_running, nr_threads;
extern int last_pid;

#include <linux/fs.h>
#include <linux/time.h>
#include <linux/param.h>
#include <linux/resource.h>
#include <linux/timer.h>

#include <asm/processor.h>

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define TASK_ZOMBIE		4
#define TASK_STOPPED		8
#define TASK_SWAPPING		16
#define TASK_EXCLUSIVE		32

#define __set_task_state(tsk, state_value)		\
	do { tsk->state = state_value; } while (0)
#ifdef __SMP__
#define set_task_state(tsk, state_value)		\
	set_mb(tsk->state, state_value)
#else
#define set_task_state(tsk, state_value)		\
	__set_task_state(tsk, state_value)
#endif

#define __set_current_state(state_value)			\
	do { current->state = state_value; } while (0)
#ifdef __SMP__
#define set_current_state(state_value)		\
	set_mb(current->state, state_value)
#else
#define set_current_state(state_value)		\
	__set_current_state(state_value)
#endif

/*
 * Scheduling policies
 */
#define SCHED_OTHER		0
#define SCHED_FIFO		1
#define SCHED_RR		2

/*
 * This is an additional bit set when we want to
 * yield the CPU for one re-schedule..
 */
#define SCHED_YIELD		0x10

struct sched_param {
	int sched_priority;
};

#ifndef NULL
#define NULL ((void *) 0)
#endif

#ifdef __KERNEL__

#include <linux/spinlock.h>

/*
 * This serializes "schedule()" and also protects
 * the run-queue from deletions/modifications (but
 * _adding_ to the beginning of the run-queue has
 * a separate lock).
 */
extern rwlock_t tasklist_lock;
extern spinlock_t runqueue_lock;

extern void sched_init(void);
extern void init_idle(void);
extern void show_state(void);
extern void cpu_init (void);
extern void trap_init(void);
extern void update_one_process( struct task_struct *p,
	unsigned long ticks, unsigned long user, unsigned long system, int cpu);

#define	MAX_SCHEDULE_TIMEOUT	LONG_MAX
extern signed long FASTCALL(schedule_timeout(signed long timeout));
asmlinkage void schedule(void);

/*
 * The default fd array needs to be at least BITS_PER_LONG,
 * as this is the granularity returned by copy_fdset().
 */
#define NR_OPEN_DEFAULT BITS_PER_LONG

/*
 * Open file table structure
 */
struct files_struct {
	atomic_t count;
	rwlock_t file_lock;
	int max_fds;
	int max_fdset;
	int next_fd;
	struct file ** fd;	/* current fd array */
	fd_set *close_on_exec;
	fd_set *open_fds;
	fd_set close_on_exec_init;
	fd_set open_fds_init;
	struct file * fd_array[NR_OPEN_DEFAULT];
};

#define INIT_FILES { \
	ATOMIC_INIT(1), \
	RW_LOCK_UNLOCKED, \
	NR_OPEN_DEFAULT, \
	__FD_SETSIZE, \
	0, \
	&init_files.fd_array[0], \
	&init_files.close_on_exec_init, \
	&init_files.open_fds_init, \
	{ { 0, } }, \
	{ { 0, } }, \
	{ NULL, } \
}

struct fs_struct {
	atomic_t count;
	int umask;
	struct dentry * root, * pwd;
};

#define INIT_FS { \
	ATOMIC_INIT(1), \
	0022, \
	NULL, NULL \
}

/* Maximum number of active map areas.. This is a random (large) number */
#define MAX_MAP_COUNT	(65536)

/* Number of map areas at which the AVL tree is activated. This is arbitrary. */
#define AVL_MIN_MAP_COUNT	32

struct mm_struct {
	struct vm_area_struct * mmap;		/* list of VMAs */
	struct vm_area_struct * mmap_avl;	/* tree of VMAs */
	struct vm_area_struct * mmap_cache;	/* last find_vma result */
	pgd_t * pgd;
	atomic_t mm_users;			/* How many users with user space? */
	atomic_t mm_count;			/* How many references to "struct mm_struct" (users count as 1) */
	int map_count;				/* number of VMAs */
	struct semaphore mmap_sem;
	spinlock_t page_table_lock;
	unsigned long context;
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;
	unsigned long rss, total_vm, locked_vm;
	unsigned long def_flags;
	unsigned long cpu_vm_mask;
	unsigned long swap_cnt;	/* number of pages to swap on next pass */
	unsigned long swap_address;
	/*
	 * This is an architecture-specific pointer: the portable
	 * part of Linux does not know about any segments.
	 */
	void * segments;
};

#define INIT_MM(name) {					\
		&init_mmap, NULL, NULL,			\
		swapper_pg_dir, 			\
		ATOMIC_INIT(2), ATOMIC_INIT(1), 1,	\
		__MUTEX_INITIALIZER(name.mmap_sem),	\
		SPIN_LOCK_UNLOCKED,			\
		0,					\
		0, 0, 0, 0,				\
		0, 0, 0, 				\
		0, 0, 0, 0,				\
		0, 0, 0,				\
		0, 0, 0, 0, NULL }

struct signal_struct {
	atomic_t		count;
	struct k_sigaction	action[_NSIG];
	spinlock_t		siglock;
};


#define INIT_SIGNALS { \
		ATOMIC_INIT(1), \
		{ {{0,}}, }, \
		SPIN_LOCK_UNLOCKED }

/*
 * Some day this will be a full-fledged user tracking system..
 * Right now it is only used to track how many processes a
 * user has, but it has the potential to track memory usage etc.
 */
struct user_struct;

struct task_struct {
/* these are hardcoded - don't touch */
	volatile long state;	/* -1 unrunnable, 0 runnable, >0 stopped */
	unsigned long flags;	/* per process flags, defined below */
	int sigpending;
	mm_segment_t addr_limit;	/* thread address space:
					 	0-0xBFFFFFFF for user-thead
						0-0xFFFFFFFF for kernel-thread
					 */
	struct exec_domain *exec_domain;
	volatile long need_resched;

/* various fields */
	long counter;
	long priority;
	cycles_t avg_slice;
/* SMP and runqueue state */
	int has_cpu;
	int processor;
	int last_processor;
	int lock_depth;		/* Lock depth. We can context switch in and out of holding a syscall kernel lock... */	
	struct task_struct *next_task, *prev_task;
	struct list_head run_list;

/* task state */
	struct linux_binfmt *binfmt;
	int exit_code, exit_signal;
	int pdeath_signal;  /*  The signal sent when the parent dies  */
	/* ??? */
	unsigned long personality;
	int dumpable:1;
	int did_exec:1;
	pid_t pid;
	pid_t pgrp;
	pid_t tty_old_pgrp;
	pid_t session;
	/* boolean value for session group leader */
	int leader;
	/* 
	 * pointers to (original) parent process, youngest child, younger sibling,
	 * older sibling, respectively.  (p->father can be replaced with 
	 * p->p_pptr->pid)
	 */
	struct task_struct *p_opptr, *p_pptr, *p_cptr, *p_ysptr, *p_osptr;

	/* PID hash table linkage. */
	struct task_struct *pidhash_next;
	struct task_struct **pidhash_pprev;

	wait_queue_head_t wait_chldexit;	/* for wait4() */
	struct semaphore *vfork_sem;		/* for vfork() */
	unsigned long policy, rt_priority;
	unsigned long it_real_value, it_prof_value, it_virt_value;
	unsigned long it_real_incr, it_prof_incr, it_virt_incr;
	struct timer_list real_timer;
	struct tms times;
	unsigned long start_time;
	long per_cpu_utime[NR_CPUS], per_cpu_stime[NR_CPUS];
/* mm fault and swap info: this can arguably be seen as either mm-specific or thread-specific */
	unsigned long min_flt, maj_flt, nswap, cmin_flt, cmaj_flt, cnswap;
	int swappable:1;
/* process credentials */
	uid_t uid,euid,suid,fsuid;
	gid_t gid,egid,sgid,fsgid;
	int ngroups;
	gid_t	groups[NGROUPS];
	kernel_cap_t   cap_effective, cap_inheritable, cap_permitted;
	struct user_struct *user;
/* limits */
	struct rlimit rlim[RLIM_NLIMITS];
	unsigned short used_math;
	char comm[16];
/* file system info */
	int link_count;
	struct tty_struct *tty; /* NULL if no tty */
/* ipc stuff */
	struct sem_undo *semundo;
	struct sem_queue *semsleeping;
/* CPU-specific state of this task */
	struct thread_struct thread;
/* filesystem information */
	struct fs_struct *fs;
/* open file information */
	struct files_struct *files;

/* memory management info */
	struct mm_struct *mm, *active_mm;

/* signal handlers */
	spinlock_t sigmask_lock;	/* Protects signal and blocked */
	struct signal_struct *sig;
	sigset_t signal, blocked;
	struct signal_queue *sigqueue, **sigqueue_tail;
	unsigned long sas_ss_sp;
	size_t sas_ss_size;
	
/* Thread group tracking */
   	u32 parent_exec_id;
   	u32 self_exec_id;
/* Protection of fields allocatio/deallocation */
	struct semaphore exit_sem;
};

/*
 * Per process flags
 */
#define PF_ALIGNWARN	0x00000001	/* Print alignment warning msgs */
					/* Not implemented yet, only for 486*/
#define PF_STARTING	0x00000002	/* being created */
#define PF_EXITING	0x00000004	/* getting shut down */
#define PF_PTRACED	0x00000010	/* set if ptrace (0) has been called */
#define PF_TRACESYS	0x00000020	/* tracing system calls */
#define PF_FORKNOEXEC	0x00000040	/* forked but didn't exec */
#define PF_SUPERPRIV	0x00000100	/* used super-user privileges */
#define PF_DUMPCORE	0x00000200	/* dumped core */
#define PF_SIGNALED	0x00000400	/* killed by a signal */
#define PF_MEMALLOC	0x00000800	/* Allocating memory */
#define PF_VFORK	0x00001000	/* Wake up parent in mm_release */

#define PF_USEDFPU	0x00100000	/* task used FPU this quantum (SMP) */
#define PF_DTRACE	0x00200000	/* delayed trace (used on m68k, i386) */

/*
 * Limit the stack by to some sane default: root can always
 * increase this limit if needed..  8MB seems reasonable.
 */
#define _STK_LIM	(8*1024*1024)

#define DEF_PRIORITY	(20*HZ/100)	/* 200 ms time slices */

/*
 *  INIT_TASK is used to set up the first task table, touch at
 * your own risk!. Base=0, limit=0x1fffff (=2MB)
 */
#define INIT_TASK(name) \
/* state etc */	{ 0,0,0,KERNEL_DS,&default_exec_domain,0, \
/* counter */	DEF_PRIORITY,DEF_PRIORITY,0, \
/* SMP */	0,0,0,-1, \
/* schedlink */	&init_task,&init_task, LIST_HEAD_INIT(init_task.run_list), \
/* binfmt */	NULL, \
/* ec,brk... */	0,0,0,0,0,0, \
/* pid etc.. */	0,0,0,0,0, \
/* proc links*/ &init_task,&init_task,NULL,NULL,NULL, \
/* pidhash */	NULL, NULL, \
/* chld wait */	__WAIT_QUEUE_HEAD_INITIALIZER(name.wait_chldexit), NULL, \
/* timeout */	SCHED_OTHER,0,0,0,0,0,0,0, \
/* timer */	{ NULL, NULL, 0, 0, it_real_fn }, \
/* utime */	{0,0,0,0},0, \
/* per CPU times */ {0, }, {0, }, \
/* flt */	0,0,0,0,0,0, \
/* swp */	0, \
/* process credentials */					\
/* uid etc */	0,0,0,0,0,0,0,0,				\
/* suppl grps*/ 0, {0,},					\
/* caps */      CAP_INIT_EFF_SET,CAP_INIT_INH_SET,CAP_FULL_SET, \
/* user */	NULL,						\
/* rlimits */   INIT_RLIMITS, \
/* math */	0, \
/* comm */	"swapper", \
/* fs info */	0,NULL, \
/* ipc */	NULL, NULL, \
/* thread */	INIT_THREAD, \
/* fs */	&init_fs, \
/* files */	&init_files, \
/* mm */	NULL, &init_mm, \
/* signals */	SPIN_LOCK_UNLOCKED, &init_signals, {{0}}, {{0}}, NULL, &init_task.sigqueue, 0, 0, \
/* exec cts */	0,0, \
/* exit_sem */	__MUTEX_INITIALIZER(name.exit_sem),	\
}

#ifndef INIT_TASK_SIZE
# define INIT_TASK_SIZE	2048*sizeof(long)
#endif

union task_union {
	struct task_struct task;
	unsigned long stack[INIT_TASK_SIZE/sizeof(long)];
};

extern union task_union init_task_union;

extern struct   mm_struct init_mm;
extern struct task_struct *init_tasks[NR_CPUS];

/* PID hashing. (shouldnt this be dynamic?) */
#define PIDHASH_SZ (4096 >> 2)
extern struct task_struct *pidhash[PIDHASH_SZ];

#define pid_hashfn(x)	((((x) >> 8) ^ (x)) & (PIDHASH_SZ - 1))

extern __inline__ void hash_pid(struct task_struct *p)
{
	struct task_struct **htable = &pidhash[pid_hashfn(p->pid)];

	if((p->pidhash_next = *htable) != NULL)
		(*htable)->pidhash_pprev = &p->pidhash_next;
	*htable = p;
	p->pidhash_pprev = htable;
}

extern __inline__ void unhash_pid(struct task_struct *p)
{
	if(p->pidhash_next)
		p->pidhash_next->pidhash_pprev = p->pidhash_pprev;
	*p->pidhash_pprev = p->pidhash_next;
}

extern __inline__ struct task_struct *find_task_by_pid(int pid)
{
	struct task_struct *p, **htable = &pidhash[pid_hashfn(pid)];

	for(p = *htable; p && p->pid != pid; p = p->pidhash_next)
		;

	return p;
}

/* per-UID process charging. */
extern int alloc_uid(struct task_struct *);
void free_uid(struct task_struct *);

#include <asm/current.h>

extern unsigned long volatile jiffies;
extern unsigned long itimer_ticks;
extern unsigned long itimer_next;
extern struct timeval xtime;
extern void do_timer(struct pt_regs *);

extern unsigned int * prof_buffer;
extern unsigned long prof_len;
extern unsigned long prof_shift;

#define CURRENT_TIME (xtime.tv_sec)

extern void FASTCALL(__wake_up(wait_queue_head_t *q, unsigned int mode));
extern void FASTCALL(__wake_up_sync(wait_queue_head_t *q, unsigned int mode));
extern void FASTCALL(sleep_on(wait_queue_head_t *q));
extern long FASTCALL(sleep_on_timeout(wait_queue_head_t *q,
				      signed long timeout));
extern void FASTCALL(interruptible_sleep_on(wait_queue_head_t *q));
extern long FASTCALL(interruptible_sleep_on_timeout(wait_queue_head_t *q,
						    signed long timeout));
extern void FASTCALL(wake_up_process(struct task_struct * tsk));

#define wake_up(x)			__wake_up((x),TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE)
#define wake_up_sync(x)			__wake_up_sync((x),TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE)
#define wake_up_interruptible(x)	__wake_up((x),TASK_INTERRUPTIBLE)
#define wake_up_interruptible_sync(x)	__wake_up_sync((x),TASK_INTERRUPTIBLE)

extern int in_group_p(gid_t);

extern void flush_signals(struct task_struct *);
extern void flush_signal_handlers(struct task_struct *);
extern int dequeue_signal(sigset_t *, siginfo_t *);
extern int send_sig_info(int, struct siginfo *, struct task_struct *);
extern int force_sig_info(int, struct siginfo *, struct task_struct *);
extern int kill_pg_info(int, struct siginfo *, pid_t);
extern int kill_sl_info(int, struct siginfo *, pid_t);
extern int kill_proc_info(int, struct siginfo *, pid_t);
extern int kill_something_info(int, struct siginfo *, int);
extern void notify_parent(struct task_struct *, int);
extern void force_sig(int, struct task_struct *);
extern int send_sig(int, struct task_struct *, int);
extern int kill_pg(pid_t, int, int);
extern int kill_sl(pid_t, int, int);
extern int kill_proc(pid_t, int, int);
extern int do_sigaction(int, const struct k_sigaction *, struct k_sigaction *);
extern int do_sigaltstack(const stack_t *, stack_t *, unsigned long);

extern inline int signal_pending(struct task_struct *p)
{
	return (p->sigpending != 0);
}

/* Reevaluate whether the task has signals pending delivery.
   This is required every time the blocked sigset_t changes.
   All callers should have t->sigmask_lock.  */

static inline void recalc_sigpending(struct task_struct *t)
{
	unsigned long ready;
	long i;

	switch (_NSIG_WORDS) {
	default:
		for (i = _NSIG_WORDS, ready = 0; --i >= 0 ;)
			ready |= t->signal.sig[i] &~ t->blocked.sig[i];
		break;

	case 4: ready  = t->signal.sig[3] &~ t->blocked.sig[3];
		ready |= t->signal.sig[2] &~ t->blocked.sig[2];
		ready |= t->signal.sig[1] &~ t->blocked.sig[1];
		ready |= t->signal.sig[0] &~ t->blocked.sig[0];
		break;

	case 2: ready  = t->signal.sig[1] &~ t->blocked.sig[1];
		ready |= t->signal.sig[0] &~ t->blocked.sig[0];
		break;

	case 1: ready  = t->signal.sig[0] &~ t->blocked.sig[0];
	}

	t->sigpending = (ready != 0);
}

/* True if we are on the alternate signal stack.  */

static inline int on_sig_stack(unsigned long sp)
{
	return (sp - current->sas_ss_sp < current->sas_ss_size);
}

static inline int sas_ss_flags(unsigned long sp)
{
	return (current->sas_ss_size == 0 ? SS_DISABLE
		: on_sig_stack(sp) ? SS_ONSTACK : 0);
}

extern int request_irq(unsigned int,
		       void (*handler)(int, void *, struct pt_regs *),
		       unsigned long, const char *, void *);
extern void free_irq(unsigned int, void *);

/*
 * This has now become a routine instead of a macro, it sets a flag if
 * it returns true (to do BSD-style accounting where the process is flagged
 * if it uses root privs). The implication of this is that you should do
 * normal permissions checks first, and check suser() last.
 *
 * [Dec 1997 -- Chris Evans]
 * For correctness, the above considerations need to be extended to
 * fsuser(). This is done, along with moving fsuser() checks to be
 * last.
 *
 * These will be removed, but in the mean time, when the SECURE_NOROOT 
 * flag is set, uids don't grant privilege.
 */
extern inline int suser(void)
{
	if (!issecure(SECURE_NOROOT) && current->euid == 0) { 
		current->flags |= PF_SUPERPRIV;
		return 1;
	}
	return 0;
}

extern inline int fsuser(void)
{
	if (!issecure(SECURE_NOROOT) && current->fsuid == 0) {
		current->flags |= PF_SUPERPRIV;
		return 1;
	}
	return 0;
}

/*
 * capable() checks for a particular capability.  
 * New privilege checks should use this interface, rather than suser() or
 * fsuser(). See include/linux/capability.h for defined capabilities.
 */

extern inline int capable(int cap)
{
#if 1 /* ok now */
	if (cap_raised(current->cap_effective, cap))
#else
	if (cap_is_fs_cap(cap) ? current->fsuid == 0 : current->euid == 0)
#endif
	{
		current->flags |= PF_SUPERPRIV;
		return 1;
	}
	return 0;
}

/*
 * Routines for handling mm_structs
 */
extern struct mm_struct * mm_alloc(void);

extern struct mm_struct * start_lazy_tlb(void);
extern void end_lazy_tlb(struct mm_struct *mm);

/* mmdrop drops the mm and the page tables */
extern inline void FASTCALL(__mmdrop(struct mm_struct *));
static inline void mmdrop(struct mm_struct * mm)
{
	if (atomic_dec_and_test(&mm->mm_count))
		__mmdrop(mm);
}

/* mmput gets rid of the mappings and all user-space */
extern void mmput(struct mm_struct *);
/* Remove the current tasks stale references to the old mm_struct */
extern void mm_release(void);

/*
 * Routines for handling the fd arrays
 */
extern struct file ** alloc_fd_array(int);
extern int expand_fd_array(struct files_struct *, int nr);
extern void free_fd_array(struct file **, int);

extern fd_set *alloc_fdset(int);
extern int expand_fdset(struct files_struct *, int nr);
extern void free_fdset(fd_set *, int);

/* Expand files.  Return <0 on error; 0 nothing done; 1 files expanded,
 * we may have blocked. 
 *
 * Should be called with the files->file_lock spinlock held for write.
 */
static inline int expand_files(struct files_struct *files, int nr)
{
	int err, expand = 0;
#ifdef FDSET_DEBUG	
	printk (KERN_ERR __FUNCTION__ " %d: nr = %d\n", current->pid, nr);
#endif
	
	if (nr >= files->max_fdset) {
		expand = 1;
		if ((err = expand_fdset(files, nr)))
			goto out;
	}
	if (nr >= files->max_fds) {
		expand = 1;
		if ((err = expand_fd_array(files, nr)))
			goto out;
	}
	err = expand;
 out:
#ifdef FDSET_DEBUG	
	if (err)
		printk (KERN_ERR __FUNCTION__ " %d: return %d\n", current->pid, err);
#endif
	return err;
}

extern int  copy_thread(int, unsigned long, unsigned long, struct task_struct *, struct pt_regs *);
extern void flush_thread(void);
extern void exit_thread(void);

extern void exit_mm(struct task_struct *);
extern void exit_fs(struct task_struct *);
extern void exit_files(struct task_struct *);
extern void exit_sighand(struct task_struct *);

extern void daemonize(void);

extern int do_execve(char *, char **, char **, struct pt_regs *);
extern int do_fork(unsigned long, unsigned long, struct pt_regs *);

extern inline void add_wait_queue(wait_queue_head_t *q, wait_queue_t * wait)
{
	unsigned long flags;

	wq_write_lock_irqsave(&q->lock, flags);
	__add_wait_queue(q, wait);
	wq_write_unlock_irqrestore(&q->lock, flags);
}

extern inline void add_wait_queue_exclusive(wait_queue_head_t *q,
							wait_queue_t * wait)
{
	unsigned long flags;

	wq_write_lock_irqsave(&q->lock, flags);
	__add_wait_queue_tail(q, wait);
	wq_write_unlock_irqrestore(&q->lock, flags);
}

extern inline void remove_wait_queue(wait_queue_head_t *q, wait_queue_t * wait)
{
	unsigned long flags;

	wq_write_lock_irqsave(&q->lock, flags);
	__remove_wait_queue(q, wait);
	wq_write_unlock_irqrestore(&q->lock, flags);
}

#define __wait_event(wq, condition) 					\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_UNINTERRUPTIBLE);		\
		if (condition)						\
			break;						\
		schedule();						\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)

#define wait_event(wq, condition) 					\
do {									\
	if (condition)	 						\
		break;							\
	__wait_event(wq, condition);					\
} while (0)

#define __wait_event_interruptible(wq, condition, ret)			\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_INTERRUPTIBLE);			\
		if (condition)						\
			break;						\
		if (!signal_pending(current)) {				\
			schedule();					\
			continue;					\
		}							\
		ret = -ERESTARTSYS;					\
		break;							\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)
	
#define wait_event_interruptible(wq, condition)				\
({									\
	int __ret = 0;							\
	if (!(condition))						\
		__wait_event_interruptible(wq, condition, __ret);	\
	__ret;								\
})

#define REMOVE_LINKS(p) do { \
	(p)->next_task->prev_task = (p)->prev_task; \
	(p)->prev_task->next_task = (p)->next_task; \
	if ((p)->p_osptr) \
		(p)->p_osptr->p_ysptr = (p)->p_ysptr; \
	if ((p)->p_ysptr) \
		(p)->p_ysptr->p_osptr = (p)->p_osptr; \
	else \
		(p)->p_pptr->p_cptr = (p)->p_osptr; \
	} while (0)

#define SET_LINKS(p) do { \
	(p)->next_task = &init_task; \
	(p)->prev_task = init_task.prev_task; \
	init_task.prev_task->next_task = (p); \
	init_task.prev_task = (p); \
	(p)->p_ysptr = NULL; \
	if (((p)->p_osptr = (p)->p_pptr->p_cptr) != NULL) \
		(p)->p_osptr->p_ysptr = p; \
	(p)->p_pptr->p_cptr = p; \
	} while (0)

#define for_each_task(p) \
	for (p = &init_task ; (p = p->next_task) != &init_task ; )


static inline void del_from_runqueue(struct task_struct * p)
{
	nr_running--;
	list_del(&p->run_list);
	p->run_list.next = NULL;
}

extern inline int task_on_runqueue(struct task_struct *p)
{
	return (p->run_list.next != NULL);
}

extern inline void unhash_process(struct task_struct *p)
{
	if (task_on_runqueue(p)) BUG();
	write_lock_irq(&tasklist_lock);
	nr_threads--;
	unhash_pid(p);
	REMOVE_LINKS(p);
	write_unlock_irq(&tasklist_lock);
}

static inline int task_lock(struct task_struct *p)
{
	down(&p->exit_sem);
	if (p->p_pptr)
		return 1;
	/* He's dead, Jim. You take his wallet, I'll take the tricorder... */
	up(&p->exit_sem);
	return 0;
}

static inline void task_unlock(struct task_struct *p)
{
	up(&p->exit_sem);
}

#endif /* __KERNEL__ */

#endif
