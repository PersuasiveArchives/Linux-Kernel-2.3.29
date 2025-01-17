/*
 *  linux/fs/buffer.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting an interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it.
 */

/* Start bdflush() with kernel_thread not syscall - Paul Gortmaker, 12/95 */

/* Removed a lot of unnecessary code and simplified things now that
 * the buffer cache isn't our primary cache - Andrew Tridgell 12/96
 */

/* Speed up hash, lru, and free list operations.  Use gfp() for allocating
 * hash table, use SLAB cache for buffer heads. -DaveM
 */

/* Added 32k buffer block sizes - these are required older ARM systems.
 * - RMK
 */

/* Thread it... -DaveM */

/* async buffer flushing, 1999 Andrea Arcangeli <andrea@suse.de> */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/malloc.h>
#include <linux/locks.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <linux/sysrq.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/quotaops.h>
#include <linux/iobuf.h>
#include <linux/highmem.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/bitops.h>
#include <asm/mmu_context.h>

#define NR_SIZES 7
static char buffersize_index[65] =
{-1,  0,  1, -1,  2, -1, -1, -1, 3, -1, -1, -1, -1, -1, -1, -1,
  4, -1, -1, -1, -1, -1, -1, -1, -1,-1, -1, -1, -1, -1, -1, -1,
  5, -1, -1, -1, -1, -1, -1, -1, -1,-1, -1, -1, -1, -1, -1, -1,
 -1, -1, -1, -1, -1, -1, -1, -1, -1,-1, -1, -1, -1, -1, -1, -1,
  6};

#define BUFSIZE_INDEX(X) ((int) buffersize_index[(X)>>9])
#define MAX_BUF_PER_PAGE (PAGE_CACHE_SIZE / 512)
#define NR_RESERVED (2*MAX_BUF_PER_PAGE)
#define MAX_UNUSED_BUFFERS NR_RESERVED+20 /* don't ever have more than this 
					     number of unused buffer heads */

/* Anti-deadlock ordering:
 *	lru_list_lock > hash_table_lock > free_list_lock > unused_list_lock
 */

/*
 * Hash table gook..
 */
static unsigned int bh_hash_mask = 0;
static unsigned int bh_hash_shift = 0;
static struct buffer_head **hash_table;
static rwlock_t hash_table_lock = RW_LOCK_UNLOCKED;

static struct buffer_head *lru_list[NR_LIST];
static spinlock_t lru_list_lock = SPIN_LOCK_UNLOCKED;
static int nr_buffers_type[NR_LIST] = {0,};
static unsigned long size_buffers_type[NR_LIST] = {0,};

static struct buffer_head * unused_list = NULL;
static int nr_unused_buffer_heads = 0;
static spinlock_t unused_list_lock = SPIN_LOCK_UNLOCKED;
static DECLARE_WAIT_QUEUE_HEAD(buffer_wait);

struct bh_free_head {
	struct buffer_head *list;
	spinlock_t lock;
};
static struct bh_free_head free_list[NR_SIZES];

kmem_cache_t *bh_cachep;

static int grow_buffers(int size);

/* This is used by some architectures to estimate available memory. */
atomic_t buffermem_pages = ATOMIC_INIT(0);

/* Here is the parameter block for the bdflush process. If you add or
 * remove any of the parameters, make sure to update kernel/sysctl.c.
 */

#define N_PARAM 9

/* The dummy values in this structure are left in there for compatibility
 * with old programs that play with the /proc entries.
 */
union bdflush_param {
	struct {
		int nfract;  /* Percentage of buffer cache dirty to 
				activate bdflush */
		int ndirty;  /* Maximum number of dirty blocks to write out per
				wake-cycle */
		int nrefill; /* Number of clean buffers to try to obtain
				each time we call refill */
		int nref_dirt; /* Dirty buffer threshold for activating bdflush
				  when trying to refill buffers. */
		int interval; /* jiffies delay between kupdate flushes */
		int age_buffer;  /* Time for normal buffer to age before we flush it */
		int age_super;  /* Time for superblock to age before we flush it */
		int dummy2;    /* unused */
		int dummy3;    /* unused */
	} b_un;
	unsigned int data[N_PARAM];
} bdf_prm = {{40, 500, 64, 256, 5*HZ, 30*HZ, 5*HZ, 1884, 2}};

/* These are the min and max parameter values that we will allow to be assigned */
int bdflush_min[N_PARAM] = {  0,  10,    5,   25,  0,   1*HZ,   1*HZ, 1, 1};
int bdflush_max[N_PARAM] = {100,50000, 20000, 20000,600*HZ, 6000*HZ, 6000*HZ, 2047, 5};

/*
 * Rewrote the wait-routines to use the "new" wait-queue functionality,
 * and getting rid of the cli-sti pairs. The wait-queue routines still
 * need cli-sti, but now it's just a couple of 386 instructions or so.
 *
 * Note that the real wait_on_buffer() is an inline function that checks
 * if 'b_wait' is set before calling this, so that the queues aren't set
 * up unnecessarily.
 */
void __wait_on_buffer(struct buffer_head * bh)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	atomic_inc(&bh->b_count);
	add_wait_queue(&bh->b_wait, &wait);
repeat:
	run_task_queue(&tq_disk);
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	if (buffer_locked(bh)) {
		schedule();
		goto repeat;
	}
	tsk->state = TASK_RUNNING;
	remove_wait_queue(&bh->b_wait, &wait);
	atomic_dec(&bh->b_count);
}

/* Call sync_buffers with wait!=0 to ensure that the call does not
 * return until all buffer writes have completed.  Sync() may return
 * before the writes have finished; fsync() may not.
 */

/* Godamity-damn.  Some buffers (bitmaps for filesystems)
 * spontaneously dirty themselves without ever brelse being called.
 * We will ultimately want to put these in a separate list, but for
 * now we search all of the lists for dirty buffers.
 */
static int sync_buffers(kdev_t dev, int wait)
{
	int i, retry, pass = 0, err = 0;
	struct buffer_head * bh, *next;

	/* One pass for no-wait, three for wait:
	 * 0) write out all dirty, unlocked buffers;
	 * 1) write out all dirty buffers, waiting if locked;
	 * 2) wait for completion by waiting for all buffers to unlock.
	 */
	do {
		retry = 0;

		/* We search all lists as a failsafe mechanism, not because we expect
		 * there to be dirty buffers on any of the other lists.
		 */
repeat:
		spin_lock(&lru_list_lock);
		bh = lru_list[BUF_DIRTY];
		if (!bh)
			goto repeat2;

		for (i = nr_buffers_type[BUF_DIRTY]*2 ; i-- > 0 ; bh = next) {
			next = bh->b_next_free;

			if (!lru_list[BUF_DIRTY])
				break;
			if (dev && bh->b_dev != dev)
				continue;
			if (buffer_locked(bh)) {
				/* Buffer is locked; skip it unless wait is
				 * requested AND pass > 0.
				 */
				if (!wait || !pass) {
					retry = 1;
					continue;
				}
				atomic_inc(&bh->b_count);
				spin_unlock(&lru_list_lock);
				wait_on_buffer (bh);
				atomic_dec(&bh->b_count);
				goto repeat;
			}

			/* If an unlocked buffer is not uptodate, there has
			 * been an IO error. Skip it.
			 */
			if (wait && buffer_req(bh) && !buffer_locked(bh) &&
			    !buffer_dirty(bh) && !buffer_uptodate(bh)) {
				err = -EIO;
				continue;
			}

			/* Don't write clean buffers.  Don't write ANY buffers
			 * on the third pass.
			 */
			if (!buffer_dirty(bh) || pass >= 2)
				continue;

			atomic_inc(&bh->b_count);
			spin_unlock(&lru_list_lock);
			ll_rw_block(WRITE, 1, &bh);
			atomic_dec(&bh->b_count);
			retry = 1;
			goto repeat;
		}

    repeat2:
		bh = lru_list[BUF_LOCKED];
		if (!bh) {
			spin_unlock(&lru_list_lock);
			break;
		}
		for (i = nr_buffers_type[BUF_LOCKED]*2 ; i-- > 0 ; bh = next) {
			next = bh->b_next_free;

			if (!lru_list[BUF_LOCKED])
				break;
			if (dev && bh->b_dev != dev)
				continue;
			if (buffer_locked(bh)) {
				/* Buffer is locked; skip it unless wait is
				 * requested AND pass > 0.
				 */
				if (!wait || !pass) {
					retry = 1;
					continue;
				}
				atomic_inc(&bh->b_count);
				spin_unlock(&lru_list_lock);
				wait_on_buffer (bh);
				spin_lock(&lru_list_lock);
				atomic_dec(&bh->b_count);
				goto repeat2;
			}
		}
		spin_unlock(&lru_list_lock);

		/* If we are waiting for the sync to succeed, and if any dirty
		 * blocks were written, then repeat; on the second pass, only
		 * wait for buffers being written (do not pass to write any
		 * more buffers on the second pass).
		 */
	} while (wait && retry && ++pass<=2);
	return err;
}

void sync_dev(kdev_t dev)
{
	sync_buffers(dev, 0);
	sync_supers(dev);
	sync_inodes(dev);
	sync_buffers(dev, 0);
	DQUOT_SYNC(dev);
	/*
	 * FIXME(eric) we need to sync the physical devices here.
	 * This is because some (scsi) controllers have huge amounts of
	 * cache onboard (hundreds of Mb), and we need to instruct
	 * them to commit all of the dirty memory to disk, and we should
	 * not return until this has happened.
	 *
	 * This would need to get implemented by going through the assorted
	 * layers so that each block major number can be synced, and this
	 * would call down into the upper and mid-layer scsi.
	 */
}

int fsync_dev(kdev_t dev)
{
	sync_buffers(dev, 0);

	lock_kernel();
	sync_supers(dev);
	sync_inodes(dev);
	DQUOT_SYNC(dev);
	unlock_kernel();

	return sync_buffers(dev, 1);
}

asmlinkage long sys_sync(void)
{
	fsync_dev(0);
	return 0;
}

/*
 *	filp may be NULL if called via the msync of a vma.
 */
 
int file_fsync(struct file *filp, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	struct super_block * sb;
	kdev_t dev;

	/* sync the inode to buffers */
	write_inode_now(inode);

	/* sync the superblock to buffers */
	sb = inode->i_sb;
	wait_on_super(sb);
	if (sb->s_op && sb->s_op->write_super)
		sb->s_op->write_super(sb);

	/* .. finally sync the buffers to disk */
	dev = inode->i_dev;
	return sync_buffers(dev, 1);
}

asmlinkage long sys_fsync(unsigned int fd)
{
	struct file * file;
	struct dentry * dentry;
	struct inode * inode;
	int err;

	lock_kernel();
	err = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	if (!dentry)
		goto out_putf;

	inode = dentry->d_inode;
	if (!inode)
		goto out_putf;

	err = -EINVAL;
	if (!file->f_op || !file->f_op->fsync)
		goto out_putf;

	/* We need to protect against concurrent writers.. */
	down(&inode->i_sem);
	err = file->f_op->fsync(file, dentry);
	up(&inode->i_sem);

out_putf:
	fput(file);
out:
	unlock_kernel();
	return err;
}

asmlinkage long sys_fdatasync(unsigned int fd)
{
	struct file * file;
	struct dentry * dentry;
	struct inode * inode;
	int err;

	lock_kernel();
	err = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	if (!dentry)
		goto out_putf;

	inode = dentry->d_inode;
	if (!inode)
		goto out_putf;

	err = -EINVAL;
	if (!file->f_op || !file->f_op->fsync)
		goto out_putf;

	/* this needs further work, at the moment it is identical to fsync() */
	down(&inode->i_sem);
	err = file->f_op->fsync(file, dentry);
	up(&inode->i_sem);

out_putf:
	fput(file);
out:
	unlock_kernel();
	return err;
}

void invalidate_buffers(kdev_t dev)
{
	int nlist;

	spin_lock(&lru_list_lock);
	for(nlist = 0; nlist < NR_LIST; nlist++) {
		struct buffer_head * bh;
		int i;
	retry:
		bh = lru_list[nlist];
		if (!bh)
			continue;
		for (i = nr_buffers_type[nlist]*2 ; --i > 0 ; bh = bh->b_next_free) {
			if (bh->b_dev != dev)
				continue;
			if (buffer_locked(bh)) {
				atomic_inc(&bh->b_count);
				spin_unlock(&lru_list_lock);
				wait_on_buffer(bh);
				spin_lock(&lru_list_lock);
				atomic_dec(&bh->b_count);
				goto retry;
			}
			if (atomic_read(&bh->b_count))
				continue;
			clear_bit(BH_Protected, &bh->b_state);
			clear_bit(BH_Uptodate, &bh->b_state);
			clear_bit(BH_Dirty, &bh->b_state);
			clear_bit(BH_Req, &bh->b_state);
		}
	}
	spin_unlock(&lru_list_lock);
}

/* After several hours of tedious analysis, the following hash
 * function won.  Do not mess with it... -DaveM
 */
#define _hashfn(dev,block)	\
	((((dev)<<(bh_hash_shift - 6)) ^ ((dev)<<(bh_hash_shift - 9))) ^ \
	 (((block)<<(bh_hash_shift - 6)) ^ ((block) >> 13) ^ ((block) << (bh_hash_shift - 12))))
#define hash(dev,block) hash_table[(_hashfn(dev,block) & bh_hash_mask)]

static __inline__ void __hash_link(struct buffer_head *bh, struct buffer_head **head)
{
	if ((bh->b_next = *head) != NULL)
		bh->b_next->b_pprev = &bh->b_next;
	*head = bh;
	bh->b_pprev = head;
}

static __inline__ void __hash_unlink(struct buffer_head *bh)
{
	if (bh->b_next)
		bh->b_next->b_pprev = bh->b_pprev;
	*(bh->b_pprev) = bh->b_next;
	bh->b_pprev = NULL;
}

static void __insert_into_lru_list(struct buffer_head * bh, int blist)
{
	struct buffer_head **bhp = &lru_list[blist];

	if(!*bhp) {
		*bhp = bh;
		bh->b_prev_free = bh;
	}
	bh->b_next_free = *bhp;
	bh->b_prev_free = (*bhp)->b_prev_free;
	(*bhp)->b_prev_free->b_next_free = bh;
	(*bhp)->b_prev_free = bh;
	nr_buffers_type[blist]++;
	size_buffers_type[blist] += bh->b_size;
}

static void __remove_from_lru_list(struct buffer_head * bh, int blist)
{
	if (bh->b_prev_free || bh->b_next_free) {
		bh->b_prev_free->b_next_free = bh->b_next_free;
		bh->b_next_free->b_prev_free = bh->b_prev_free;
		if (lru_list[blist] == bh)
			lru_list[blist] = bh->b_next_free;
		if (lru_list[blist] == bh)
			lru_list[blist] = NULL;
		bh->b_next_free = bh->b_prev_free = NULL;
		nr_buffers_type[blist]--;
		size_buffers_type[blist] -= bh->b_size;
	}
}

static void __remove_from_free_list(struct buffer_head * bh, int index)
{
	if(bh->b_next_free == bh)
		 free_list[index].list = NULL;
	else {
		bh->b_prev_free->b_next_free = bh->b_next_free;
		bh->b_next_free->b_prev_free = bh->b_prev_free;
		if (free_list[index].list == bh)
			 free_list[index].list = bh->b_next_free;
	}
	bh->b_next_free = bh->b_prev_free = NULL;
}

/* The following two functions must operate atomically
 * because they control the visibility of a buffer head
 * to the rest of the kernel.
 */
static __inline__ void __remove_from_queues(struct buffer_head *bh)
{
	write_lock(&hash_table_lock);
	if (bh->b_pprev)
		__hash_unlink(bh);
	__remove_from_lru_list(bh, bh->b_list);
	write_unlock(&hash_table_lock);
}

static void insert_into_queues(struct buffer_head *bh)
{
	struct buffer_head **head = &hash(bh->b_dev, bh->b_blocknr);

	spin_lock(&lru_list_lock);
	write_lock(&hash_table_lock);
	__hash_link(bh, head);
	__insert_into_lru_list(bh, bh->b_list);
	write_unlock(&hash_table_lock);
	spin_unlock(&lru_list_lock);
}

/* This function must only run if there are no other
 * references _anywhere_ to this buffer head.
 */
static void put_last_free(struct buffer_head * bh)
{
	struct bh_free_head *head = &free_list[BUFSIZE_INDEX(bh->b_size)];
	struct buffer_head **bhp = &head->list;

	spin_lock(&head->lock);
	bh->b_dev = B_FREE;
	if(!*bhp) {
		*bhp = bh;
		bh->b_prev_free = bh;
	}
	bh->b_next_free = *bhp;
	bh->b_prev_free = (*bhp)->b_prev_free;
	(*bhp)->b_prev_free->b_next_free = bh;
	(*bhp)->b_prev_free = bh;
	spin_unlock(&head->lock);
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are reading them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
struct buffer_head * get_hash_table(kdev_t dev, int block, int size)
{
	struct buffer_head **head = &hash(dev, block);
	struct buffer_head *bh;

	read_lock(&hash_table_lock);
	for(bh = *head; bh; bh = bh->b_next)
		if (bh->b_blocknr == block	&&
		    bh->b_size    == size	&&
		    bh->b_dev     == dev)
			break;
	if (bh)
		atomic_inc(&bh->b_count);
	read_unlock(&hash_table_lock);

	return bh;
}

unsigned int get_hardblocksize(kdev_t dev)
{
	/*
	 * Get the hard sector size for the given device.  If we don't know
	 * what it is, return 0.
	 */
	if (hardsect_size[MAJOR(dev)] != NULL) {
		int blksize = hardsect_size[MAJOR(dev)][MINOR(dev)];
		if (blksize != 0)
			return blksize;
	}

	/*
	 * We don't know what the hardware sector size for this device is.
	 * Return 0 indicating that we don't know.
	 */
	return 0;
}

void set_blocksize(kdev_t dev, int size)
{
	extern int *blksize_size[];
	int i, nlist;
	struct buffer_head * bh, *bhnext;

	if (!blksize_size[MAJOR(dev)])
		return;

	/* Size must be a power of two, and between 512 and PAGE_SIZE */
	if (size > PAGE_SIZE || size < 512 || (size & (size-1)))
		panic("Invalid blocksize passed to set_blocksize");

	if (blksize_size[MAJOR(dev)][MINOR(dev)] == 0 && size == BLOCK_SIZE) {
		blksize_size[MAJOR(dev)][MINOR(dev)] = size;
		return;
	}
	if (blksize_size[MAJOR(dev)][MINOR(dev)] == size)
		return;
	sync_buffers(dev, 2);
	blksize_size[MAJOR(dev)][MINOR(dev)] = size;

	/* We need to be quite careful how we do this - we are moving entries
	 * around on the free list, and we can get in a loop if we are not careful.
	 */
	for(nlist = 0; nlist < NR_LIST; nlist++) {
	repeat:
		spin_lock(&lru_list_lock);
		bh = lru_list[nlist];
		for (i = nr_buffers_type[nlist]*2 ; --i > 0 ; bh = bhnext) {
			if(!bh)
				break;

			bhnext = bh->b_next_free; 
			if (bh->b_dev != dev)
				 continue;
			if (bh->b_size == size)
				 continue;
			if (buffer_locked(bh)) {
				atomic_inc(&bh->b_count);
				spin_unlock(&lru_list_lock);
				wait_on_buffer(bh);
				atomic_dec(&bh->b_count);
				goto repeat;
			}
			if (bh->b_dev == dev && bh->b_size != size) {
				clear_bit(BH_Dirty, &bh->b_state);
				clear_bit(BH_Uptodate, &bh->b_state);
				clear_bit(BH_Req, &bh->b_state);
			}
			if (atomic_read(&bh->b_count) == 0) {
				__remove_from_queues(bh);
				put_last_free(bh);
			}
		}
		spin_unlock(&lru_list_lock);
	}
}

/*
 * We used to try various strange things. Let's not.
 */
static void refill_freelist(int size)
{
	if (!grow_buffers(size)) {
		wakeup_bdflush(1);
		current->policy |= SCHED_YIELD;
		schedule();
	}
}

void init_buffer(struct buffer_head *bh, bh_end_io_t *handler, void *dev_id)
{
	bh->b_list = BUF_CLEAN;
	bh->b_end_io = handler;
	bh->b_dev_id = dev_id;
}

static void end_buffer_io_sync(struct buffer_head *bh, int uptodate)
{
	mark_buffer_uptodate(bh, uptodate);
	unlock_buffer(bh);
}

static void end_buffer_io_bad(struct buffer_head *bh, int uptodate)
{
	mark_buffer_uptodate(bh, uptodate);
	unlock_buffer(bh);
	BUG();
}

static void end_buffer_io_async(struct buffer_head * bh, int uptodate)
{
	static spinlock_t page_uptodate_lock = SPIN_LOCK_UNLOCKED;
	unsigned long flags;
	struct buffer_head *tmp;
	struct page *page;

	mark_buffer_uptodate(bh, uptodate);

	/* This is a temporary buffer used for page I/O. */
	page = bh->b_page;

	if (!uptodate)
		SetPageError(page);

	/*
	 * Be _very_ careful from here on. Bad things can happen if
	 * two buffer heads end IO at almost the same time and both
	 * decide that the page is now completely done.
	 *
	 * Async buffer_heads are here only as labels for IO, and get
	 * thrown away once the IO for this page is complete.  IO is
	 * deemed complete once all buffers have been visited
	 * (b_count==0) and are now unlocked. We must make sure that
	 * only the _last_ buffer that decrements its count is the one
	 * that unlock the page..
	 */
	spin_lock_irqsave(&page_uptodate_lock, flags);
	unlock_buffer(bh);
	atomic_dec(&bh->b_count);
	tmp = bh->b_this_page;
	while (tmp != bh) {
		if (atomic_read(&tmp->b_count) &&
		    (tmp->b_end_io == end_buffer_io_async))
			goto still_busy;
		tmp = tmp->b_this_page;
	}

	/* OK, the async IO on this page is complete. */
	spin_unlock_irqrestore(&page_uptodate_lock, flags);

	/*
	 * if none of the buffers had errors then we can set the
	 * page uptodate:
	 */
	if (!PageError(page))
		SetPageUptodate(page);

	/*
	 * Run the hooks that have to be done when a page I/O has completed.
	 */
	if (test_and_clear_bit(PG_decr_after, &page->flags))
		atomic_dec(&nr_async_pages);

	UnlockPage(page);

	return;

still_busy:
	spin_unlock_irqrestore(&page_uptodate_lock, flags);
	return;
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algorithm is changed: hopefully better, and an elusive bug removed.
 *
 * 14.02.92: changed it to sync dirty buffers a bit: better performance
 * when the filesystem starts to get full of dirty blocks (I hope).
 */
struct buffer_head * getblk(kdev_t dev, int block, int size)
{
	struct buffer_head * bh;
	int isize;

repeat:
	bh = get_hash_table(dev, block, size);
	if (bh)
		goto out;

	isize = BUFSIZE_INDEX(size);
	spin_lock(&free_list[isize].lock);
	bh = free_list[isize].list;
	if (bh) {
		__remove_from_free_list(bh, isize);
		atomic_set(&bh->b_count, 1);
	}
	spin_unlock(&free_list[isize].lock);
	if (!bh)
		goto refill;

	/* OK, FINALLY we know that this buffer is the only one of its kind,
	 * we hold a reference (b_count>0), it is unlocked, and it is clean.
	 */
	init_buffer(bh, end_buffer_io_sync, NULL);
	bh->b_dev = dev;
	bh->b_blocknr = block;
	bh->b_state = 1 << BH_Mapped;

	/* Insert the buffer into the regular lists */
	insert_into_queues(bh);
	goto out;

	/*
	 * If we block while refilling the free list, somebody may
	 * create the buffer first ... search the hashes again.
	 */
refill:
	refill_freelist(size);
	goto repeat;
out:
	return bh;
}

/* -1 -> no need to flush
    0 -> async flush
    1 -> sync flush (wait for I/O completation) */
static int balance_dirty_state(kdev_t dev)
{
	unsigned long dirty, tot, hard_dirty_limit, soft_dirty_limit;

	dirty = size_buffers_type[BUF_DIRTY] >> PAGE_SHIFT;
	tot = nr_free_buffer_pages();
	hard_dirty_limit = tot * bdf_prm.b_un.nfract / 100;
	soft_dirty_limit = hard_dirty_limit >> 1;

	if (dirty > soft_dirty_limit)
	{
		if (dirty > hard_dirty_limit)
			return 1;
		return 0;
	}
	return -1;
}

/*
 * if a new dirty buffer is created we need to balance bdflush.
 *
 * in the future we might want to make bdflush aware of different
 * pressures on different devices - thus the (currently unused)
 * 'dev' parameter.
 */
void balance_dirty(kdev_t dev)
{
	int state = balance_dirty_state(dev);

	if (state < 0)
		return;
	wakeup_bdflush(state);
}

static inline void __mark_dirty(struct buffer_head *bh, int flag)
{
	bh->b_flushtime = jiffies + (flag ? bdf_prm.b_un.age_super : bdf_prm.b_un.age_buffer);
	clear_bit(BH_New, &bh->b_state);
	refile_buffer(bh);
}

void __mark_buffer_dirty(struct buffer_head *bh, int flag)
{
	__mark_dirty(bh, flag);
}

/*
 * A buffer may need to be moved from one buffer list to another
 * (e.g. in case it is not shared any more). Handle this.
 */
static __inline__ void __refile_buffer(struct buffer_head *bh)
{
	int dispose = BUF_CLEAN;
	if (buffer_locked(bh))
		dispose = BUF_LOCKED;
	if (buffer_dirty(bh))
		dispose = BUF_DIRTY;
	if (dispose != bh->b_list) {
		__remove_from_lru_list(bh, bh->b_list);
		bh->b_list = dispose;
		__insert_into_lru_list(bh, dispose);
	}
}

void refile_buffer(struct buffer_head *bh)
{
	spin_lock(&lru_list_lock);
	__refile_buffer(bh);
	spin_unlock(&lru_list_lock);
}

/*
 * Release a buffer head
 */
void __brelse(struct buffer_head * buf)
{
	touch_buffer(buf);

	if (atomic_read(&buf->b_count)) {
		atomic_dec(&buf->b_count);
		return;
	}
	printk("VFS: brelse: Trying to free free buffer\n");
}

/*
 * bforget() is like brelse(), except it puts the buffer on the
 * free list if it can.. We can NOT free the buffer if:
 *  - there are other users of it
 *  - it is locked and thus can have active IO
 */
void __bforget(struct buffer_head * buf)
{
	/* grab the lru lock here to block bdflush. */
	spin_lock(&lru_list_lock);
	write_lock(&hash_table_lock);
	if (!atomic_dec_and_test(&buf->b_count) || buffer_locked(buf))
		goto in_use;
	if (buf->b_pprev)
		__hash_unlink(buf);
	write_unlock(&hash_table_lock);
	__remove_from_lru_list(buf, buf->b_list);
	spin_unlock(&lru_list_lock);
	buf->b_state = 0;
	put_last_free(buf);
	return;

 in_use:
	write_unlock(&hash_table_lock);
	spin_unlock(&lru_list_lock);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(kdev_t dev, int block, int size)
{
	struct buffer_head * bh;

	bh = getblk(dev, block, size);
	if (buffer_uptodate(bh))
		return bh;
	ll_rw_block(READ, 1, &bh);
	wait_on_buffer(bh);
	if (buffer_uptodate(bh))
		return bh;
	brelse(bh);
	return NULL;
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */

#define NBUF 16

struct buffer_head * breada(kdev_t dev, int block, int bufsize,
	unsigned int pos, unsigned int filesize)
{
	struct buffer_head * bhlist[NBUF];
	unsigned int blocks;
	struct buffer_head * bh;
	int index;
	int i, j;

	if (pos >= filesize)
		return NULL;

	if (block < 0)
		return NULL;

	bh = getblk(dev, block, bufsize);
	index = BUFSIZE_INDEX(bh->b_size);

	if (buffer_uptodate(bh))
		return(bh);   
	else ll_rw_block(READ, 1, &bh);

	blocks = (filesize - pos) >> (9+index);

	if (blocks < (read_ahead[MAJOR(dev)] >> index))
		blocks = read_ahead[MAJOR(dev)] >> index;
	if (blocks > NBUF) 
		blocks = NBUF;

/*	if (blocks) printk("breada (new) %d blocks\n",blocks); */

	bhlist[0] = bh;
	j = 1;
	for(i=1; i<blocks; i++) {
		bh = getblk(dev,block+i,bufsize);
		if (buffer_uptodate(bh)) {
			brelse(bh);
			break;
		}
		else bhlist[j++] = bh;
	}

	/* Request the read for these buffers, and then release them. */
	if (j>1)  
		ll_rw_block(READA, (j-1), bhlist+1); 
	for(i=1; i<j; i++)
		brelse(bhlist[i]);

	/* Wait for this buffer, and then continue on. */
	bh = bhlist[0];
	wait_on_buffer(bh);
	if (buffer_uptodate(bh))
		return bh;
	brelse(bh);
	return NULL;
}

/*
 * Note: the caller should wake up the buffer_wait list if needed.
 */
static __inline__ void __put_unused_buffer_head(struct buffer_head * bh)
{
	if (nr_unused_buffer_heads >= MAX_UNUSED_BUFFERS) {
		kmem_cache_free(bh_cachep, bh);
	} else {
		bh->b_blocknr = -1;
		init_waitqueue_head(&bh->b_wait);
		nr_unused_buffer_heads++;
		bh->b_next_free = unused_list;
		bh->b_this_page = NULL;
		unused_list = bh;
	}
}

/*
 * Reserve NR_RESERVED buffer heads for async IO requests to avoid
 * no-buffer-head deadlock.  Return NULL on failure; waiting for
 * buffer heads is now handled in create_buffers().
 */ 
static struct buffer_head * get_unused_buffer_head(int async)
{
	struct buffer_head * bh;

	spin_lock(&unused_list_lock);
	if (nr_unused_buffer_heads > NR_RESERVED) {
		bh = unused_list;
		unused_list = bh->b_next_free;
		nr_unused_buffer_heads--;
		spin_unlock(&unused_list_lock);
		return bh;
	}
	spin_unlock(&unused_list_lock);

	/* This is critical.  We can't swap out pages to get
	 * more buffer heads, because the swap-out may need
	 * more buffer-heads itself.  Thus SLAB_BUFFER.
	 */
	if((bh = kmem_cache_alloc(bh_cachep, SLAB_BUFFER)) != NULL) {
		memset(bh, 0, sizeof(*bh));
		init_waitqueue_head(&bh->b_wait);
		return bh;
	}

	/*
	 * If we need an async buffer, use the reserved buffer heads.
	 */
	if (async) {
		spin_lock(&unused_list_lock);
		if (unused_list) {
			bh = unused_list;
			unused_list = bh->b_next_free;
			nr_unused_buffer_heads--;
			spin_unlock(&unused_list_lock);
			return bh;
		}
		spin_unlock(&unused_list_lock);
	}
#if 0
	/*
	 * (Pending further analysis ...)
	 * Ordinary (non-async) requests can use a different memory priority
	 * to free up pages. Any swapping thus generated will use async
	 * buffer heads.
	 */
	if(!async &&
	   (bh = kmem_cache_alloc(bh_cachep, SLAB_KERNEL)) != NULL) {
		memset(bh, 0, sizeof(*bh));
		init_waitqueue_head(&bh->b_wait);
		return bh;
	}
#endif

	return NULL;
}

void set_bh_page (struct buffer_head *bh, struct page *page, unsigned int offset)
{
	bh->b_page = page;
	if (offset >= PAGE_SIZE)
		BUG();
	if (PageHighMem(page))
		/*
		 * This catches illegal uses and preserves the offset:
		 */
		bh->b_data = (char *)(0 + offset);
	else
		bh->b_data = (char *)(page_address(page) + offset);
}

/*
 * Create the appropriate buffers when given a page for data area and
 * the size of each buffer.. Use the bh->b_this_page linked list to
 * follow the buffers created.  Return NULL if unable to create more
 * buffers.
 * The async flag is used to differentiate async IO (paging, swapping)
 * from ordinary buffer allocations, and only async requests are allowed
 * to sleep waiting for buffer heads. 
 */
static struct buffer_head * create_buffers(struct page * page, unsigned long size, int async)
{
	struct buffer_head *bh, *head;
	long offset;

try_again:
	head = NULL;
	offset = PAGE_SIZE;
	while ((offset -= size) >= 0) {
		bh = get_unused_buffer_head(async);
		if (!bh)
			goto no_grow;

		bh->b_dev = B_FREE;  /* Flag as unused */
		bh->b_this_page = head;
		head = bh;

		bh->b_state = 0;
		bh->b_next_free = NULL;
		bh->b_pprev = NULL;
		atomic_set(&bh->b_count, 0);
		bh->b_size = size;

		set_bh_page(bh, page, offset);

		bh->b_list = BUF_CLEAN;
		bh->b_end_io = end_buffer_io_bad;
	}
	return head;
/*
 * In case anything failed, we just free everything we got.
 */
no_grow:
	if (head) {
		spin_lock(&unused_list_lock);
		do {
			bh = head;
			head = head->b_this_page;
			__put_unused_buffer_head(bh);
		} while (head);
		spin_unlock(&unused_list_lock);

		/* Wake up any waiters ... */
		wake_up(&buffer_wait);
	}

	/*
	 * Return failure for non-async IO requests.  Async IO requests
	 * are not allowed to fail, so we have to wait until buffer heads
	 * become available.  But we don't want tasks sleeping with 
	 * partially complete buffers, so all were released above.
	 */
	if (!async)
		return NULL;

	/* We're _really_ low on memory. Now we just
	 * wait for old buffer heads to become free due to
	 * finishing IO.  Since this is an async request and
	 * the reserve list is empty, we're sure there are 
	 * async buffer heads in use.
	 */
	run_task_queue(&tq_disk);

	/* 
	 * Set our state for sleeping, then check again for buffer heads.
	 * This ensures we won't miss a wake_up from an interrupt.
	 */
	wait_event(buffer_wait, nr_unused_buffer_heads >= MAX_BUF_PER_PAGE);
	goto try_again;
}

static int create_page_buffers(int rw, struct page *page, kdev_t dev, int b[], int size)
{
	struct buffer_head *head, *bh, *tail;
	int block;

	if (!PageLocked(page))
		BUG();
	/*
	 * Allocate async buffer heads pointing to this page, just for I/O.
	 * They don't show up in the buffer hash table, but they *are*
	 * registered in page->buffers.
	 */
	head = create_buffers(page, size, 1);
	if (page->buffers)
		BUG();
	if (!head)
		BUG();
	tail = head;
	for (bh = head; bh; bh = bh->b_this_page) {
		block = *(b++);

		tail = bh;
		init_buffer(bh, end_buffer_io_async, NULL);
		bh->b_dev = dev;
		bh->b_blocknr = block;

		set_bit(BH_Mapped, &bh->b_state);
	}
	tail->b_this_page = head;
	get_page(page);
	page->buffers = head;
	return 0;
}

static void unmap_buffer(struct buffer_head * bh)
{
	if (buffer_mapped(bh))
	{
		mark_buffer_clean(bh);
		wait_on_buffer(bh);
		clear_bit(BH_Uptodate, &bh->b_state);
		clear_bit(BH_Mapped, &bh->b_state);
		clear_bit(BH_Req, &bh->b_state);
	}
}

/*
 * We don't have to release all buffers here, but
 * we have to be sure that no dirty buffer is left
 * and no IO is going on (no buffer is locked), because
 * we have truncated the file and are going to free the
 * blocks on-disk..
 */
int block_flushpage(struct inode *inode, struct page *page, unsigned long offset)
{
	struct buffer_head *head, *bh, *next;
	unsigned int curr_off = 0;

	if (!PageLocked(page))
		BUG();
	if (!page->buffers)
		return 1;

	head = page->buffers;
	bh = head;
	do {
		unsigned int next_off = curr_off + bh->b_size;
		next = bh->b_this_page;

		/*
		 * is this block fully flushed?
		 */
		if (offset <= curr_off)
			unmap_buffer(bh);
		curr_off = next_off;
		bh = next;
	} while (bh != head);

	/*
	 * subtle. We release buffer-heads only if this is
	 * the 'final' flushpage. We have invalidated the get_block
	 * cached value unconditionally, so real IO is not
	 * possible anymore.
	 *
	 * If the free doesn't work out, the buffers can be
	 * left around - they just turn into anonymous buffers
	 * instead.
	 */
	if (!offset) {
		if (!try_to_free_buffers(page)) {
			atomic_inc(&buffermem_pages);
			return 0;
		}
	}

	return 1;
}

static void create_empty_buffers(struct page *page, struct inode *inode, unsigned long blocksize)
{
	struct buffer_head *bh, *head, *tail;

	head = create_buffers(page, blocksize, 1);
	if (page->buffers)
		BUG();

	bh = head;
	do {
		bh->b_dev = inode->i_dev;
		bh->b_blocknr = 0;
		bh->b_end_io = end_buffer_io_bad;
		tail = bh;
		bh = bh->b_this_page;
	} while (bh);
	tail->b_this_page = head;
	page->buffers = head;
	get_page(page);
}

static void unmap_underlying_metadata(struct buffer_head * bh)
{
#if 0
	if (buffer_new(bh)) {
		struct buffer_head *old_bh;

		old_bh = get_hash_table(bh->b_dev, bh->b_blocknr, bh->b_size);
		if (old_bh) {
			unmap_buffer(old_bh);
			/* Here we could run brelse or bforget. We use
			   bforget because it will try to put the buffer
			   in the freelist. */
			__bforget(old_bh);
		}
	}
#endif
}

/*
 * block_write_full_page() is SMP-safe - currently it's still
 * being called with the kernel lock held, but the code is ready.
 */
int block_write_full_page(struct file *file, struct page *page)
{
	struct dentry *dentry = file->f_dentry;
	struct inode *inode = dentry->d_inode;
	int err, i;
	unsigned long block;
	struct buffer_head *bh, *head;

	if (!PageLocked(page))
		BUG();

	if (!page->buffers)
		create_empty_buffers(page, inode, inode->i_sb->s_blocksize);
	head = page->buffers;

	/* The page cache is now PAGE_CACHE_SIZE aligned, period.  We handle old a.out
	 * and others via unaligned private mappings.
	 */
	block = page->index << (PAGE_CACHE_SHIFT - inode->i_sb->s_blocksize_bits);

	bh = head;
	i = 0;
	do {
		if (!bh)
			BUG();

		/*
		 * If the buffer isn't up-to-date, we can't be sure
		 * that the buffer has been initialized with the proper
		 * block number information etc..
		 *
		 * Leave it to the low-level FS to make all those
		 * decisions (block #0 may actually be a valid block)
		 */
		bh->b_end_io = end_buffer_io_sync;
		if (!buffer_mapped(bh)) {
			err = inode->i_op->get_block(inode, block, bh, 1);
			if (err)
				goto out;
			unmap_underlying_metadata(bh);
		}
		set_bit(BH_Uptodate, &bh->b_state);
		mark_buffer_dirty(bh,0);

		bh = bh->b_this_page;
		block++;
	} while (bh != head);

	SetPageUptodate(page);
	return 0;
out:
	ClearPageUptodate(page);
	return err;
}

int block_write_partial_page(struct file *file, struct page *page, unsigned long offset, unsigned long bytes, const char * buf)
{
	struct dentry *dentry = file->f_dentry;
	struct inode *inode = dentry->d_inode;
	unsigned long block;
	int err, partial;
	unsigned long blocksize, start_block, end_block;
	unsigned long start_offset, start_bytes, end_bytes;
	unsigned long bbits, blocks, i, len;
	struct buffer_head *bh, *head;
	char *target_buf, *kaddr;
	int need_balance_dirty;

	kaddr = (char *)kmap(page);
	target_buf = kaddr + offset;

	if (!PageLocked(page))
		BUG();

	blocksize = inode->i_sb->s_blocksize;
	if (!page->buffers)
		create_empty_buffers(page, inode, blocksize);
	head = page->buffers;

	bbits = inode->i_sb->s_blocksize_bits;
	block = page->index << (PAGE_CACHE_SHIFT - bbits);
	blocks = PAGE_CACHE_SIZE >> bbits;
	start_block = offset >> bbits;
	end_block = (offset + bytes - 1) >> bbits;
	start_offset = offset & (blocksize - 1);
	start_bytes = blocksize - start_offset;
	if (start_bytes > bytes)
		start_bytes = bytes;
	end_bytes = (offset+bytes) & (blocksize - 1);
	if (end_bytes > bytes)
		end_bytes = bytes;

	if (offset < 0 || offset >= PAGE_SIZE)
		BUG();
	if (bytes+offset < 0 || bytes+offset > PAGE_SIZE)
		BUG();
	if (start_block < 0 || start_block >= blocks)
		BUG();
	if (end_block < 0 || end_block >= blocks)
		BUG();

	i = 0;
	bh = head;
	partial = 0;
	need_balance_dirty = 0;
	do {
		if (!bh)
			BUG();

		if ((i < start_block) || (i > end_block)) {
			if (!buffer_uptodate(bh))
				partial = 1;
			goto skip;
		}

		/*
		 * If the buffer is not up-to-date, we need to ask the low-level
		 * FS to do something for us (we used to have assumptions about
		 * the meaning of b_blocknr etc, that's bad).
		 *
		 * If "update" is set, that means that the low-level FS should
		 * try to make sure that the block is up-to-date because we're
		 * not going to fill it completely.
		 */
		bh->b_end_io = end_buffer_io_sync;
		if (!buffer_mapped(bh)) {
			err = inode->i_op->get_block(inode, block, bh, 1);
			if (err)
				goto out;
			unmap_underlying_metadata(bh);
		}

		if (!buffer_uptodate(bh) && (start_offset || (end_bytes && (i == end_block)))) {
			if (buffer_new(bh)) {
				memset(kaddr + i*blocksize, 0, blocksize);
			} else {
				ll_rw_block(READ, 1, &bh);
				wait_on_buffer(bh);
				err = -EIO;
				if (!buffer_uptodate(bh))
					goto out;
			}
		}

		len = blocksize;
		if (start_offset) {
			len = start_bytes;
			start_offset = 0;
		} else if (end_bytes && (i == end_block)) {
			len = end_bytes;
			end_bytes = 0;
		}
		if (target_buf >= kaddr + PAGE_SIZE)
			BUG();
		if (target_buf+len-1 >= kaddr + PAGE_SIZE)
			BUG();
		err = copy_from_user(target_buf, buf, len);
		target_buf += len;
		buf += len;

		/*
		 * we dirty buffers only after copying the data into
		 * the page - this way we can dirty the buffer even if
		 * the bh is still doing IO.
		 *
		 * NOTE! This also does a direct dirty balace check,
		 * rather than relying on bdflush just waking up every
		 * once in a while. This is to catch (and slow down)
		 * the processes that write tons of buffer..
		 *
		 * Note how we do NOT want to do this in the full block
		 * case: full pages are flushed not by the people who
		 * dirtied them, but by people who need memory. And we
		 * should not penalize them for somebody else writing
		 * lots of dirty pages.
		 */
		set_bit(BH_Uptodate, &bh->b_state);
		if (!test_and_set_bit(BH_Dirty, &bh->b_state)) {
			__mark_dirty(bh, 0);
			need_balance_dirty = 1;
		}

		if (err) {
			err = -EFAULT;
			goto out;
		}

skip:
		i++;
		block++;
		bh = bh->b_this_page;
	} while (bh != head);

	if (need_balance_dirty)
		balance_dirty(bh->b_dev);

	/*
	 * is this a partial write that happened to make all buffers
	 * uptodate then we can optimize away a bogus readpage() for
	 * the next read(). Here we 'discover' wether the page went
	 * uptodate as a result of this (potentially partial) write.
	 */
	if (!partial)
		SetPageUptodate(page);
	kunmap(page);
	return bytes;
out:
	ClearPageUptodate(page);
	kunmap(page);
	return err;
}

/*
 * For moronic filesystems that do not allow holes in file.
 * we allow offset==PAGE_SIZE, bytes==0
 */

int block_write_cont_page(struct file *file, struct page *page, unsigned long offset, unsigned long bytes, const char * buf)
{
	struct dentry *dentry = file->f_dentry;
	struct inode *inode = dentry->d_inode;
	unsigned long block;
	int err, partial;
	unsigned long blocksize, start_block, end_block;
	unsigned long start_offset, start_bytes, end_bytes;
	unsigned long bbits, blocks, i, len;
	struct buffer_head *bh, *head;
	char * target_buf, *target_data;
	unsigned long data_offset = offset;
	int need_balance_dirty;

	offset = inode->i_size - (page->index << PAGE_CACHE_SHIFT);
	if (page->index > (inode->i_size >> PAGE_CACHE_SHIFT))
		offset = 0;
	else if (offset >= data_offset)
		offset = data_offset;
	bytes += data_offset - offset;

	target_buf = (char *)page_address(page) + offset;
	target_data = (char *)page_address(page) + data_offset;

	if (!PageLocked(page))
		BUG();

	blocksize = inode->i_sb->s_blocksize;
	if (!page->buffers)
		create_empty_buffers(page, inode, blocksize);
	head = page->buffers;

	bbits = inode->i_sb->s_blocksize_bits;
	block = page->index << (PAGE_CACHE_SHIFT - bbits);
	blocks = PAGE_CACHE_SIZE >> bbits;
	start_block = offset >> bbits;
	end_block = (offset + bytes - 1) >> bbits;
	start_offset = offset & (blocksize - 1);
	start_bytes = blocksize - start_offset;
	if (start_bytes > bytes)
		start_bytes = bytes;
	end_bytes = (offset+bytes) & (blocksize - 1);
	if (end_bytes > bytes)
		end_bytes = bytes;

	if (offset < 0 || offset > PAGE_SIZE)
		BUG();
	if (bytes+offset < 0 || bytes+offset > PAGE_SIZE)
		BUG();
	if (start_block < 0 || start_block > blocks)
		BUG();
	if (end_block < 0 || end_block >= blocks)
		BUG();

	i = 0;
	bh = head;
	partial = 0;
	need_balance_dirty = 0;
	do {
		if (!bh)
			BUG();

		if ((i < start_block) || (i > end_block)) {
			if (!buffer_uptodate(bh))
				partial = 1;
			goto skip;
		}

		/*
		 * If the buffer is not up-to-date, we need to ask the low-level
		 * FS to do something for us (we used to have assumptions about
		 * the meaning of b_blocknr etc, that's bad).
		 *
		 * If "update" is set, that means that the low-level FS should
		 * try to make sure that the block is up-to-date because we're
		 * not going to fill it completely.
		 */
		bh->b_end_io = end_buffer_io_sync;
		if (!buffer_mapped(bh)) {
			err = inode->i_op->get_block(inode, block, bh, 1);
			if (err)
				goto out;
			unmap_underlying_metadata(bh);
		}

		if (!buffer_uptodate(bh) && (start_offset || (end_bytes && (i == end_block)))) {
			if (buffer_new(bh)) {
				memset(bh->b_data, 0, bh->b_size);
			} else {
				ll_rw_block(READ, 1, &bh);
				wait_on_buffer(bh);
				err = -EIO;
				if (!buffer_uptodate(bh))
					goto out;
			}
		}

		len = blocksize;
		if (start_offset) {
			len = start_bytes;
			start_offset = 0;
		} else if (end_bytes && (i == end_block)) {
			len = end_bytes;
			end_bytes = 0;
		}
		err = 0;
		if (target_buf+len<=target_data)
			memset(target_buf, 0, len);
		else if (target_buf<target_data) {
			memset(target_buf, 0, target_data-target_buf);
			copy_from_user(target_data, buf,
					len+target_buf-target_data);
		} else
			err = copy_from_user(target_buf, buf, len);
		target_buf += len;
		buf += len;

		/*
		 * we dirty buffers only after copying the data into
		 * the page - this way we can dirty the buffer even if
		 * the bh is still doing IO.
		 *
		 * NOTE! This also does a direct dirty balace check,
		 * rather than relying on bdflush just waking up every
		 * once in a while. This is to catch (and slow down)
		 * the processes that write tons of buffer..
		 *
		 * Note how we do NOT want to do this in the full block
		 * case: full pages are flushed not by the people who
		 * dirtied them, but by people who need memory. And we
		 * should not penalize them for somebody else writing
		 * lots of dirty pages.
		 */
		set_bit(BH_Uptodate, &bh->b_state);
		if (!test_and_set_bit(BH_Dirty, &bh->b_state)) {
			__mark_dirty(bh, 0);
			need_balance_dirty = 1;
		}

		if (err) {
			err = -EFAULT;
			goto out;
		}

skip:
		i++;
		block++;
		bh = bh->b_this_page;
	} while (bh != head);

	if (need_balance_dirty)
		balance_dirty(bh->b_dev);

	/*
	 * is this a partial write that happened to make all buffers
	 * uptodate then we can optimize away a bogus readpage() for
	 * the next read(). Here we 'discover' wether the page went
	 * uptodate as a result of this (potentially partial) write.
	 */
	if (!partial)
		SetPageUptodate(page);
	return bytes;
out:
	ClearPageUptodate(page);
	return err;
}


/*
 * IO completion routine for a buffer_head being used for kiobuf IO: we
 * can't dispatch the kiobuf callback until io_count reaches 0.  
 */

static void end_buffer_io_kiobuf(struct buffer_head *bh, int uptodate)
{
	struct kiobuf *kiobuf;
	
	mark_buffer_uptodate(bh, uptodate);

	kiobuf = bh->b_kiobuf;
	if (atomic_dec_and_test(&kiobuf->io_count))
		kiobuf->end_io(kiobuf);
	if (!uptodate)
		kiobuf->errno = -EIO;
}


/*
 * For brw_kiovec: submit a set of buffer_head temporary IOs and wait
 * for them to complete.  Clean up the buffer_heads afterwards.  
 */

static int do_kio(struct kiobuf *kiobuf,
		  int rw, int nr, struct buffer_head *bh[], int size)
{
	int iosize;
	int i;
	struct buffer_head *tmp;

	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	if (rw == WRITE)
		rw = WRITERAW;
	atomic_add(nr, &kiobuf->io_count);
	kiobuf->errno = 0;
	ll_rw_block(rw, nr, bh);

	kiobuf_wait_for_io(kiobuf);
	
	spin_lock(&unused_list_lock);
	
	iosize = 0;
	for (i = nr; --i >= 0; ) {
		iosize += size;
		tmp = bh[i];
		if (!buffer_uptodate(tmp)) {
			/* We are traversing bh'es in reverse order so
                           clearing iosize on error calculates the
                           amount of IO before the first error. */
			iosize = 0;
		}
		__put_unused_buffer_head(tmp);
	}
	
	spin_unlock(&unused_list_lock);

	if (iosize)
		return iosize;
	if (kiobuf->errno)
		return kiobuf->errno;
	return -EIO;
}

/*
 * Start I/O on a physical range of kernel memory, defined by a vector
 * of kiobuf structs (much like a user-space iovec list).
 *
 * The kiobuf must already be locked for IO.  IO is submitted
 * asynchronously: you need to check page->locked, page->uptodate, and
 * maybe wait on page->wait.
 *
 * It is up to the caller to make sure that there are enough blocks
 * passed in to completely map the iobufs to disk.
 */

int brw_kiovec(int rw, int nr, struct kiobuf *iovec[], 
	       kdev_t dev, unsigned long b[], int size)
{
	int		err;
	int		length;
	int		transferred;
	int		i;
	int		bufind;
	int		pageind;
	int		bhind;
	int		offset;
	unsigned long	blocknr;
	struct kiobuf *	iobuf = NULL;
	unsigned long	page;
	struct page *	map;
	struct buffer_head *tmp, *bh[KIO_MAX_SECTORS];

	if (!nr)
		return 0;
	
	/* 
	 * First, do some alignment and validity checks 
	 */
	for (i = 0; i < nr; i++) {
		iobuf = iovec[i];
		if ((iobuf->offset & (size-1)) ||
		    (iobuf->length & (size-1)))
			return -EINVAL;
		if (!iobuf->locked)
			panic("brw_kiovec: iobuf not locked for I/O");
		if (!iobuf->nr_pages)
			panic("brw_kiovec: iobuf not initialised");
	}

	/* 
	 * OK to walk down the iovec doing page IO on each page we find. 
	 */
	bufind = bhind = transferred = err = 0;
	for (i = 0; i < nr; i++) {
		iobuf = iovec[i];
		offset = iobuf->offset;
		length = iobuf->length;

		for (pageind = 0; pageind < iobuf->nr_pages; pageind++) {
			map  = iobuf->maplist[pageind];
			if (map && PageHighMem(map)) {
				err = -EIO;
				goto error;
			}
			page = page_address(map);

			while (length > 0) {
				blocknr = b[bufind++];
				tmp = get_unused_buffer_head(0);
				if (!tmp) {
					err = -ENOMEM;
					goto error;
				}
				
				tmp->b_dev = B_FREE;
				tmp->b_size = size;
				set_bh_page(tmp, map, offset);
				tmp->b_this_page = tmp;

				init_buffer(tmp, end_buffer_io_kiobuf, NULL);
				tmp->b_dev = dev;
				tmp->b_blocknr = blocknr;
				tmp->b_state = 1 << BH_Mapped;
				tmp->b_kiobuf = iobuf;

				if (rw == WRITE) {
					set_bit(BH_Uptodate, &tmp->b_state);
					set_bit(BH_Dirty, &tmp->b_state);
				}

				bh[bhind++] = tmp;
				length -= size;
				offset += size;

				/* 
				 * Start the IO if we have got too much 
				 */
				if (bhind >= KIO_MAX_SECTORS) {
					err = do_kio(iobuf, rw, bhind, bh, size);
					if (err >= 0)
						transferred += err;
					else
						goto finished;
					bhind = 0;
				}
				
				if (offset >= PAGE_SIZE) {
					offset = 0;
					break;
				}
			} /* End of block loop */
		} /* End of page loop */		
	} /* End of iovec loop */

	/* Is there any IO still left to submit? */
	if (bhind) {
		err = do_kio(iobuf, rw, bhind, bh, size);
		if (err >= 0)
			transferred += err;
		else
			goto finished;
	}

 finished:
	if (transferred)
		return transferred;
	return err;

 error:
	/* We got an error allocating the bh'es.  Just free the current
           buffer_heads and exit. */
	spin_lock(&unused_list_lock);
	for (i = bhind; --i >= 0; ) {
		__put_unused_buffer_head(bh[bhind]);
	}
	spin_unlock(&unused_list_lock);
	goto finished;
}

/*
 * Start I/O on a page.
 * This function expects the page to be locked and may return
 * before I/O is complete. You then have to check page->locked,
 * page->uptodate, and maybe wait on page->wait.
 *
 * brw_page() is SMP-safe, although it's being called with the
 * kernel lock held - but the code is ready.
 *
 * FIXME: we need a swapper_inode->get_block function to remove
 *        some of the bmap kludges and interface ugliness here.
 */
int brw_page(int rw, struct page *page, kdev_t dev, int b[], int size)
{
	struct buffer_head *head, *bh, *arr[MAX_BUF_PER_PAGE];
	int nr, fresh /* temporary debugging flag */, block;

	if (!PageLocked(page))
		panic("brw_page: page not locked for I/O");
//	clear_bit(PG_error, &page->flags);
	/*
	 * We pretty much rely on the page lock for this, because
	 * create_page_buffers() might sleep.
	 */
	fresh = 0;
	if (!page->buffers) {
		create_page_buffers(rw, page, dev, b, size);
		fresh = 1;
	}
	if (!page->buffers)
		BUG();

	head = page->buffers;
	bh = head;
	nr = 0;
	do {
		block = *(b++);

		if (fresh && (atomic_read(&bh->b_count) != 0))
			BUG();
		if (rw == READ) {
			if (!fresh)
				BUG();
			if (!buffer_uptodate(bh)) {
				arr[nr++] = bh;
				atomic_inc(&bh->b_count);
			}
		} else { /* WRITE */
			if (!bh->b_blocknr) {
				if (!block)
					BUG();
				bh->b_blocknr = block;
			} else {
				if (!block)
					BUG();
			}
			set_bit(BH_Uptodate, &bh->b_state);
			set_bit(BH_Dirty, &bh->b_state);
			arr[nr++] = bh;
			atomic_inc(&bh->b_count);
		}
		bh = bh->b_this_page;
	} while (bh != head);
	if (rw == READ)
		++current->maj_flt;
	if ((rw == READ) && nr) {
		if (Page_Uptodate(page))
			BUG();
		ll_rw_block(rw, nr, arr);
	} else {
		if (!nr && rw == READ) {
			SetPageUptodate(page);
			UnlockPage(page);
		}
		if (nr && (rw == WRITE))
			ll_rw_block(rw, nr, arr);
	}
	return 0;
}

/*
 * Generic "read page" function for block devices that have the normal
 * get_block functionality. This is most of the block device filesystems.
 * Reads the page asynchronously --- the unlock_buffer() and
 * mark_buffer_uptodate() functions propagate buffer state into the
 * page struct once IO has completed.
 */
int block_read_full_page(struct file * file, struct page * page)
{
	struct dentry *dentry = file->f_dentry;
	struct inode *inode = dentry->d_inode;
	unsigned long iblock;
	struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
	unsigned int blocksize, blocks;
	unsigned long kaddr = 0;
	int nr, i;

	if (!PageLocked(page))
		PAGE_BUG(page);
	blocksize = inode->i_sb->s_blocksize;
	if (!page->buffers)
		create_empty_buffers(page, inode, blocksize);
	head = page->buffers;

	blocks = PAGE_CACHE_SIZE >> inode->i_sb->s_blocksize_bits;
	iblock = page->index << (PAGE_CACHE_SHIFT - inode->i_sb->s_blocksize_bits);
	bh = head;
	nr = 0;
	i = 0;

	do {
		if (buffer_uptodate(bh))
			continue;

		if (!buffer_mapped(bh)) {
			inode->i_op->get_block(inode, iblock, bh, 0);
			if (!buffer_mapped(bh)) {
				if (!kaddr)
					kaddr = kmap(page);
				memset((char *)(kaddr + i*blocksize), 0, blocksize);
				set_bit(BH_Uptodate, &bh->b_state);
				continue;
			}
		}

		init_buffer(bh, end_buffer_io_async, NULL);
		atomic_inc(&bh->b_count);
		arr[nr] = bh;
		nr++;
	} while (i++, iblock++, (bh = bh->b_this_page) != head);

	++current->maj_flt;
	if (nr) {
		if (Page_Uptodate(page))
			BUG();
		ll_rw_block(READ, nr, arr);
	} else {
		/*
		 * all buffers are uptodate - we can set the page
		 * uptodate as well.
		 */
		SetPageUptodate(page);
		UnlockPage(page);
	}
	if (kaddr)
		kunmap(page);
	return 0;
}

/*
 * Try to increase the number of buffers available: the size argument
 * is used to determine what kind of buffers we want.
 */
static int grow_buffers(int size)
{
	struct page * page;
	struct buffer_head *bh, *tmp;
	struct buffer_head * insert_point;
	int isize;

	if ((size & 511) || (size > PAGE_SIZE)) {
		printk("VFS: grow_buffers: size = %d\n",size);
		return 0;
	}

	page = alloc_page(GFP_BUFFER);
	if (!page)
		goto out;
	bh = create_buffers(page, size, 0);
	if (!bh)
		goto no_buffer_head;

	isize = BUFSIZE_INDEX(size);

	spin_lock(&free_list[isize].lock);
	insert_point = free_list[isize].list;
	tmp = bh;
	while (1) {
		if (insert_point) {
			tmp->b_next_free = insert_point->b_next_free;
			tmp->b_prev_free = insert_point;
			insert_point->b_next_free->b_prev_free = tmp;
			insert_point->b_next_free = tmp;
		} else {
			tmp->b_prev_free = tmp;
			tmp->b_next_free = tmp;
		}
		insert_point = tmp;
		if (tmp->b_this_page)
			tmp = tmp->b_this_page;
		else
			break;
	}
	tmp->b_this_page = bh;
	free_list[isize].list = bh;
	spin_unlock(&free_list[isize].lock);

	page->buffers = bh;
	lru_cache_add(page);
	atomic_inc(&buffermem_pages);
	return 1;

no_buffer_head:
	__free_page(page);
out:
	return 0;
}

/*
 * Can the buffer be thrown out?
 */
#define BUFFER_BUSY_BITS	((1<<BH_Dirty) | (1<<BH_Lock) | (1<<BH_Protected))
#define buffer_busy(bh)		(atomic_read(&(bh)->b_count) | ((bh)->b_state & BUFFER_BUSY_BITS))

/*
 * try_to_free_buffers() checks if all the buffers on this particular page
 * are unused, and free's the page if so.
 *
 * Wake up bdflush() if this fails - if we're running low on memory due
 * to dirty buffers, we need to flush them out as quickly as possible.
 *
 * NOTE: There are quite a number of ways that threads of control can
 *       obtain a reference to a buffer head within a page.  So we must
 *	 lock out all of these paths to cleanly toss the page.
 */
int try_to_free_buffers(struct page * page)
{
	struct buffer_head * tmp, * bh = page->buffers;
	int index = BUFSIZE_INDEX(bh->b_size);
	int ret;

	spin_lock(&lru_list_lock);
	write_lock(&hash_table_lock);
	spin_lock(&free_list[index].lock);
	tmp = bh;
	do {
		struct buffer_head * p = tmp;

		tmp = tmp->b_this_page;
		if (buffer_busy(p))
			goto busy_buffer_page;
	} while (tmp != bh);

	spin_lock(&unused_list_lock);
	tmp = bh;
	do {
		struct buffer_head * p = tmp;
		tmp = tmp->b_this_page;

		/* The buffer can be either on the regular
		 * queues or on the free list..
		 */
		if (p->b_dev == B_FREE) {
			__remove_from_free_list(p, index);
		} else {
			if (p->b_pprev)
				__hash_unlink(p);
			__remove_from_lru_list(p, p->b_list);
		}
		__put_unused_buffer_head(p);
	} while (tmp != bh);
	spin_unlock(&unused_list_lock);

	/* Wake up anyone waiting for buffer heads */
	wake_up(&buffer_wait);

	/* And free the page */
	page->buffers = NULL;
	__free_page(page);
	ret = 1;
out:
	spin_unlock(&free_list[index].lock);
	write_unlock(&hash_table_lock);
	spin_unlock(&lru_list_lock);
	return ret;

busy_buffer_page:
	/* Uhhuh, start writeback so that we don't end up with all dirty pages */
	wakeup_bdflush(0);
	ret = 0;
	goto out;
}

/* ================== Debugging =================== */

void show_buffers(void)
{
#ifdef __SMP__
	struct buffer_head * bh;
	int found = 0, locked = 0, dirty = 0, used = 0, lastused = 0;
	int protected = 0;
	int nlist;
	static char *buf_types[NR_LIST] = { "CLEAN", "LOCKED", "DIRTY" };
#endif

	printk("Buffer memory:   %6dkB\n",
			atomic_read(&buffermem_pages) << (PAGE_SHIFT-10));

#ifdef __SMP__ /* trylock does nothing on UP and so we could deadlock */
	if (!spin_trylock(&lru_list_lock))
		return;
	for(nlist = 0; nlist < NR_LIST; nlist++) {
		found = locked = dirty = used = lastused = protected = 0;
		bh = lru_list[nlist];
		if(!bh) continue;

		do {
			found++;
			if (buffer_locked(bh))
				locked++;
			if (buffer_protected(bh))
				protected++;
			if (buffer_dirty(bh))
				dirty++;
			if (atomic_read(&bh->b_count))
				used++, lastused = found;
			bh = bh->b_next_free;
		} while (bh != lru_list[nlist]);
		printk("%8s: %d buffers, %d used (last=%d), "
		       "%d locked, %d protected, %d dirty\n",
		       buf_types[nlist], found, used, lastused,
		       locked, protected, dirty);
	}
	spin_unlock(&lru_list_lock);
#endif
}

/* ===================== Init ======================= */

/*
 * allocate the hash table and init the free list
 * Use gfp() for the hash table to decrease TLB misses, use
 * SLAB cache for buffer heads.
 */
void __init buffer_init(unsigned long mempages)
{
	int order, i;
	unsigned int nr_hash;

	/* The buffer cache hash table is less important these days,
	 * trim it a bit.
	 */
	mempages >>= 14;

	mempages *= sizeof(struct buffer_head *);

	for (order = 0; (1 << order) < mempages; order++)
		;

	/* try to allocate something until we get it or we're asking
	   for something that is really too small */

	do {
		unsigned long tmp;

		nr_hash = (PAGE_SIZE << order) / sizeof(struct buffer_head *);
		bh_hash_mask = (nr_hash - 1);

		tmp = nr_hash;
		bh_hash_shift = 0;
		while((tmp >>= 1UL) != 0UL)
			bh_hash_shift++;

		hash_table = (struct buffer_head **)
		    __get_free_pages(GFP_ATOMIC, order);
	} while (hash_table == NULL && --order > 0);
	printk("Buffer-cache hash table entries: %d (order: %d, %ld bytes)\n",
	       nr_hash, order, (1UL<<order) * PAGE_SIZE);

	if (!hash_table)
		panic("Failed to allocate buffer hash table\n");

	/* Setup hash chains. */
	for(i = 0; i < nr_hash; i++)
		hash_table[i] = NULL;

	/* Setup free lists. */
	for(i = 0; i < NR_SIZES; i++) {
		free_list[i].list = NULL;
		free_list[i].lock = SPIN_LOCK_UNLOCKED;
	}

	/* Setup lru lists. */
	for(i = 0; i < NR_LIST; i++)
		lru_list[i] = NULL;

	bh_cachep = kmem_cache_create("buffer_head",
				      sizeof(struct buffer_head),
				      0,
				      SLAB_HWCACHE_ALIGN, NULL, NULL);
	if(!bh_cachep)
		panic("Cannot create buffer head SLAB cache\n");
}


/* ====================== bdflush support =================== */

/* This is a simple kernel daemon, whose job it is to provide a dynamic
 * response to dirty buffers.  Once this process is activated, we write back
 * a limited number of buffers to the disks and then go back to sleep again.
 */
static DECLARE_WAIT_QUEUE_HEAD(bdflush_done);
struct task_struct *bdflush_tsk = 0;

void wakeup_bdflush(int block)
{
	DECLARE_WAITQUEUE(wait, current);

	if (current == bdflush_tsk)
		return;

	if (!block)
	{
		wake_up_process(bdflush_tsk);
		return;
	}

	/* kflushd can wakeup us before we have a chance to
	   go to sleep so we must be smart in handling
	   this wakeup event from kflushd to avoid deadlocking in SMP
	   (we are not holding any lock anymore in these two paths). */
	__set_current_state(TASK_UNINTERRUPTIBLE);
	add_wait_queue(&bdflush_done, &wait);

	wake_up_process(bdflush_tsk);
	schedule();

	remove_wait_queue(&bdflush_done, &wait);
	__set_current_state(TASK_RUNNING);
}

/* This is the _only_ function that deals with flushing async writes
   to disk.
   NOTENOTENOTENOTE: we _only_ need to browse the DIRTY lru list
   as all dirty buffers lives _only_ in the DIRTY lru list.
   As we never browse the LOCKED and CLEAN lru lists they are infact
   completly useless. */
static void flush_dirty_buffers(int check_flushtime)
{
	struct buffer_head * bh, *next;
	int flushed = 0, i;

 restart:
	spin_lock(&lru_list_lock);
	bh = lru_list[BUF_DIRTY];
	if (!bh)
		goto out_unlock;
	for (i = nr_buffers_type[BUF_DIRTY]; i-- > 0; bh = next)
	{
		next = bh->b_next_free;

		if (!buffer_dirty(bh))
		{
			__refile_buffer(bh);
			continue;
		}
		if (buffer_locked(bh))
			continue;

		if (check_flushtime)
		{
			/* The dirty lru list is chronogical ordered so
			   if the current bh is not yet timed out,
			   then also all the following bhs
			   will be too young. */
			if (time_before(jiffies, bh->b_flushtime))
				goto out_unlock;
		}
		else
		{
			if (++flushed > bdf_prm.b_un.ndirty)
				goto out_unlock;
		}

		/* OK, now we are committed to write it out. */
		atomic_inc(&bh->b_count);
		spin_unlock(&lru_list_lock);
		ll_rw_block(WRITE, 1, &bh);
		atomic_dec(&bh->b_count);

		if (current->need_resched)
			schedule();
		goto restart;
	}
 out_unlock:
	spin_unlock(&lru_list_lock);
}

/* 
 * Here we attempt to write back old buffers.  We also try to flush inodes 
 * and supers as well, since this function is essentially "update", and 
 * otherwise there would be no way of ensuring that these quantities ever 
 * get written back.  Ideally, we would have a timestamp on the inodes
 * and superblocks so that we could write back only the old ones as well
 */

static int sync_old_buffers(void)
{
	lock_kernel();
	sync_supers(0);
	sync_inodes(0);
	unlock_kernel();

	flush_dirty_buffers(1);
	/* must really sync all the active I/O request to disk here */
	run_task_queue(&tq_disk);
	return 0;
}

/* This is the interface to bdflush.  As we get more sophisticated, we can
 * pass tuning parameters to this "process", to adjust how it behaves. 
 * We would want to verify each parameter, however, to make sure that it 
 * is reasonable. */

asmlinkage long sys_bdflush(int func, long data)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (func == 1) {
		/* do_exit directly and let kupdate to do its work alone. */
		do_exit(0);
#if 0 /* left here as it's the only example of lazy-mm-stuff used from
	 a syscall that doesn't care about the current mm context. */
		int error;
		struct mm_struct *user_mm;

		/*
		 * bdflush will spend all of it's time in kernel-space,
		 * without touching user-space, so we can switch it into
		 * 'lazy TLB mode' to reduce the cost of context-switches
		 * to and from bdflush.
		 */
		user_mm = start_lazy_tlb();
		error = sync_old_buffers();
		end_lazy_tlb(user_mm);
		return error;
#endif
	}

	/* Basically func 1 means read param 1, 2 means write param 1, etc */
	if (func >= 2) {
		int i = (func-2) >> 1;
		if (i >= 0 && i < N_PARAM) {
			if ((func & 1) == 0)
				return put_user(bdf_prm.data[i], (int*)data);

			if (data >= bdflush_min[i] && data <= bdflush_max[i]) {
				bdf_prm.data[i] = data;
				return 0;
			}
		}
		return -EINVAL;
	}

	/* Having func 0 used to launch the actual bdflush and then never
	 * return (unless explicitly killed). We return zero here to 
	 * remain semi-compatible with present update(8) programs.
	 */
	return 0;
}

/*
 * This is the actual bdflush daemon itself. It used to be started from
 * the syscall above, but now we launch it ourselves internally with
 * kernel_thread(...)  directly after the first thread in init/main.c
 */
int bdflush(void * unused) 
{
	/*
	 *	We have a bare-bones task_struct, and really should fill
	 *	in a few more things so "top" and /proc/2/{exe,root,cwd}
	 *	display semi-sane things. Not real crucial though...  
	 */

	current->session = 1;
	current->pgrp = 1;
	sprintf(current->comm, "kflushd");
	bdflush_tsk = current;

	/* avoid getting signals */
	spin_lock_irq(&current->sigmask_lock);
	flush_signals(current);
	sigfillset(&current->blocked);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	for (;;) {
		CHECK_EMERGENCY_SYNC

		flush_dirty_buffers(0);

		/* If wakeup_bdflush will wakeup us
		   after our bdflush_done wakeup, then
		   we must make sure to not sleep
		   in schedule_timeout otherwise
		   wakeup_bdflush may wait for our
		   bdflush_done wakeup that would never arrive
		   (as we would be sleeping) and so it would
		   deadlock in SMP. */
		__set_current_state(TASK_INTERRUPTIBLE);
		wake_up(&bdflush_done);
		/*
		 * If there are still a lot of dirty buffers around,
		 * skip the sleep and flush some more. Otherwise, we
		 * sleep for a while.
		 */
		if (balance_dirty_state(NODEV) < 0)
			schedule_timeout(5*HZ);
		/* Remember to mark us as running otherwise
		   the next schedule will block. */
		__set_current_state(TASK_RUNNING);
	}
}

/*
 * This is the kernel update daemon. It was used to live in userspace
 * but since it's need to run safely we want it unkillable by mistake.
 * You don't need to change your userspace configuration since
 * the userspace `update` will do_exit(0) at the first sys_bdflush().
 */
int kupdate(void * unused) 
{
	struct task_struct * tsk = current;
	int interval;

	tsk->session = 1;
	tsk->pgrp = 1;
	strcpy(tsk->comm, "kupdate");

	/* sigstop and sigcont will stop and wakeup kupdate */
	spin_lock_irq(&tsk->sigmask_lock);
	sigfillset(&tsk->blocked);
	siginitsetinv(&current->blocked, sigmask(SIGCONT) | sigmask(SIGSTOP));
	recalc_sigpending(tsk);
	spin_unlock_irq(&tsk->sigmask_lock);

	for (;;) {
		/* update interval */
		interval = bdf_prm.b_un.interval;
		if (interval)
		{
			tsk->state = TASK_INTERRUPTIBLE;
			schedule_timeout(interval);
		}
		else
		{
		stop_kupdate:
			tsk->state = TASK_STOPPED;
			schedule(); /* wait for SIGCONT */
		}
		/* check for sigstop */
		if (signal_pending(tsk))
		{
			int stopped = 0;
			spin_lock_irq(&tsk->sigmask_lock);
			if (sigismember(&tsk->signal, SIGSTOP))
			{
				sigdelset(&tsk->signal, SIGSTOP);
				stopped = 1;
			}
			recalc_sigpending(tsk);
			spin_unlock_irq(&tsk->sigmask_lock);
			if (stopped)
				goto stop_kupdate;
		}
#ifdef DEBUG
		printk("kupdate() activated...\n");
#endif
		sync_old_buffers();
	}
}

static int __init bdflush_init(void)
{
	kernel_thread(bdflush, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	kernel_thread(kupdate, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	return 0;
}

module_init(bdflush_init)

