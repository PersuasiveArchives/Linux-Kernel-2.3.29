/*
 *  file.c
 *
 *  Copyright (C) 1995, 1996, 1997 by Paal-Kr. Engstad and Volker Lendecke
 *  Copyright (C) 1997 by Volker Lendecke
 *
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/smbno.h>
#include <linux/smb_fs.h>

#define SMBFS_PARANOIA 1
/* #define SMBFS_DEBUG_VERBOSE 1 */
/* #define pr_debug printk */

static inline int
min(int a, int b)
{
	return a < b ? a : b;
}

static int
smb_fsync(struct file *file, struct dentry * dentry)
{
#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_fsync: sync file %s/%s\n", 
dentry->d_parent->d_name.name, dentry->d_name.name);
#endif
	return 0;
}

/*
 * Read a page synchronously.
 */
static int
smb_readpage_sync(struct dentry *dentry, struct page *page)
{
	char *buffer = (char *) page_address(page);
	unsigned long offset = page->index << PAGE_CACHE_SHIFT;
	int rsize = smb_get_rsize(server_from_dentry(dentry));
	int count = PAGE_SIZE;
	int result;

	/* We can't replace this with ClearPageError. why? is it a problem? 
	   fs/buffer.c:brw_page does the same. */
	/* clear_bit(PG_error, &page->flags); */

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_readpage_sync: file %s/%s, count=%d@%ld, rsize=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, count, offset, rsize);
#endif
	result = smb_open(dentry, SMB_O_RDONLY);
	if (result < 0)
	{
#ifdef SMBFS_PARANOIA
printk("smb_readpage_sync: %s/%s open failed, error=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, result);
#endif
		goto io_error;
	}

	do {
		if (count < rsize)
			rsize = count;

		result = smb_proc_read(dentry, offset, rsize, buffer);
		if (result < 0)
			goto io_error;

		count -= result;
		offset += result;
		buffer += result;
		dentry->d_inode->i_atime = CURRENT_TIME;
		if (result < rsize)
			break;
	} while (count);

	memset(buffer, 0, count);
	SetPageUptodate(page);
	result = 0;

io_error:
	UnlockPage(page);
	return result;
}

int
smb_readpage(struct file *file, struct page *page)
{
	struct dentry *dentry = file->f_dentry;
	int		error;

	pr_debug("SMB: smb_readpage %08lx\n", page_address(page));
#ifdef SMBFS_PARANOIA
	if (!PageLocked(page))
		printk("smb_readpage: page not already locked!\n");
#endif

	get_page(page);
	error = smb_readpage_sync(dentry, page);
	put_page(page);
	return error;
}

/*
 * Write a page synchronously.
 * Offset is the data offset within the page.
 */
static int
smb_writepage_sync(struct dentry *dentry, struct page *page,
		   unsigned long offset, unsigned int count)
{
	struct inode *inode = dentry->d_inode;
	u8 *buffer = (u8 *) page_address(page) + offset;
	int wsize = smb_get_wsize(server_from_dentry(dentry));
	int result, written = 0;

	offset += page->index << PAGE_CACHE_SHIFT;
#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_writepage_sync: file %s/%s, count=%d@%ld, wsize=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, count, offset, wsize);
#endif

	do {
		if (count < wsize)
			wsize = count;

		result = smb_proc_write(dentry, offset, wsize, buffer);
		if (result < 0)
			break;
		/* N.B. what if result < wsize?? */
#ifdef SMBFS_PARANOIA
if (result < wsize)
printk("smb_writepage_sync: short write, wsize=%d, result=%d\n", wsize, result);
#endif
		buffer += wsize;
		offset += wsize;
		written += wsize;
		count -= wsize;
		/*
		 * Update the inode now rather than waiting for a refresh.
		 */
		inode->i_mtime = inode->i_atime = CURRENT_TIME;
		if (offset > inode->i_size)
			inode->i_size = offset;
		inode->u.smbfs_i.cache_valid |= SMB_F_LOCALWRITE;
	} while (count);
	return written ? written : result;
}

/*
 * Write a page to the server. This will be used for NFS swapping only
 * (for now), and we currently do this synchronously only.
 *
 * We are called with the page locked and the caller unlocks.
 */
static int
smb_writepage(struct file *file, struct page *page)
{
	struct dentry *dentry = file->f_dentry;
	int 	result;

#ifdef SMBFS_PARANOIA
	if (!PageLocked(page))
		printk("smb_writepage: page not already locked!\n");
#endif
	get_page(page);
	result = smb_writepage_sync(dentry, page, 0, PAGE_SIZE);
	SetPageUptodate(page);
	put_page(page);
	return result;
}

static int
smb_updatepage(struct file *file, struct page *page, unsigned long offset, unsigned int count)
{
	struct dentry *dentry = file->f_dentry;

	pr_debug("SMBFS: smb_updatepage(%s/%s %d@%ld)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
	 	count, (page->index << PAGE_CACHE_SHIFT)+offset);

	return smb_writepage_sync(dentry, page, offset, count);
}

static ssize_t
smb_file_read(struct file * file, char * buf, size_t count, loff_t *ppos)
{
	struct dentry * dentry = file->f_dentry;
	ssize_t	status;

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_read: file %s/%s, count=%lu@%lu\n",
dentry->d_parent->d_name.name, dentry->d_name.name,
(unsigned long) count, (unsigned long) *ppos);
#endif

	status = smb_revalidate_inode(dentry);
	if (status)
	{
#ifdef SMBFS_PARANOIA
printk("smb_file_read: %s/%s validation failed, error=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, status);
#endif
		goto out;
	}

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_read: before read, size=%ld, pages=%ld, flags=%x, atime=%ld\n",
dentry->d_inode->i_size, dentry->d_inode->i_nrpages, dentry->d_inode->i_flags,
dentry->d_inode->i_atime);
#endif
	status = generic_file_read(file, buf, count, ppos);
out:
	return status;
}

static int
smb_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct dentry * dentry = file->f_dentry;
	int	status;

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_mmap: file %s/%s, address %lu - %lu\n",
dentry->d_parent->d_name.name, dentry->d_name.name, vma->vm_start, vma->vm_end);
#endif

	status = smb_revalidate_inode(dentry);
	if (status)
	{
#ifdef SMBFS_PARANOIA
printk("smb_file_mmap: %s/%s validation failed, error=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, status);
#endif
		goto out;
	}
	status = generic_file_mmap(file, vma);
out:
	return status;
}

/*
 * This does the "real" work of the write. The generic routine has
 * allocated the page, locked it, done all the page alignment stuff
 * calculations etc. Now we should just copy the data from user
 * space and write it back to the real medium..
 *
 * If the writer ends up delaying the write, the writer needs to
 * increment the page use counts until he is done with the page.
 */
static int smb_write_one_page(struct file *file, struct page *page, unsigned long offset, unsigned long bytes, const char * buf)
{
	int status;

	bytes -= copy_from_user((u8*)page_address(page) + offset, buf, bytes);
	status = -EFAULT;
	if (bytes) {
		lock_kernel();
		status = smb_updatepage(file, page, offset, bytes);
		unlock_kernel();
	}
	return status;
}

/* 
 * Write to a file (through the page cache).
 */
static ssize_t
smb_file_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	struct dentry * dentry = file->f_dentry;
	ssize_t	result;

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_write: file %s/%s, count=%lu@%lu, pages=%ld\n",
dentry->d_parent->d_name.name, dentry->d_name.name,
(unsigned long) count, (unsigned long) *ppos, dentry->d_inode->i_nrpages);
#endif

	result = smb_revalidate_inode(dentry);
	if (result)
	{
#ifdef SMBFS_PARANOIA
printk("smb_file_write: %s/%s validation failed, error=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, result);
#endif
			goto out;
	}

	result = smb_open(dentry, SMB_O_WRONLY);
	if (result)
		goto out;

	if (count > 0)
	{
		result = generic_file_write(file, buf, count, ppos, smb_write_one_page);
#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_write: pos=%ld, size=%ld, mtime=%ld, atime=%ld\n",
(long) file->f_pos, dentry->d_inode->i_size, dentry->d_inode->i_mtime,
dentry->d_inode->i_atime);
#endif
	}
out:
	return result;
}

static int
smb_file_open(struct inode *inode, struct file * file)
{
#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_open: opening %s/%s, d_count=%d\n",
file->f_dentry->d_parent->d_name.name, file->f_dentry->d_name.name,
file->f_dentry->d_count);
#endif
	return 0;
}

static int
smb_file_release(struct inode *inode, struct file * file)
{
	struct dentry * dentry = file->f_dentry;

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_release: closing %s/%s, d_count=%d\n",
dentry->d_parent->d_name.name, dentry->d_name.name, dentry->d_count);
#endif
	
	if (dentry->d_count == 1)
	{
		smb_close(inode);
	}
	return 0;
}

/*
 * Check whether the required access is compatible with
 * an inode's permission. SMB doesn't recognize superuser
 * privileges, so we need our own check for this.
 */
static int
smb_file_permission(struct inode *inode, int mask)
{
	int mode = inode->i_mode;
	int error = 0;

#ifdef SMBFS_DEBUG_VERBOSE
printk("smb_file_permission: mode=%x, mask=%x\n", mode, mask);
#endif
	/* Look at user permissions */
	mode >>= 6;
	if ((mode & 7 & mask) != mask)
		error = -EACCES;
	return error;
}

static struct file_operations smb_file_operations =
{
	NULL,			/* lseek - default */
	smb_file_read,		/* read */
	smb_file_write,		/* write */
	NULL,			/* readdir - bad */
	NULL,			/* poll - default */
	smb_ioctl,		/* ioctl */
	smb_file_mmap,		/* mmap(struct file*, struct vm_area_struct*) */
	smb_file_open,		/* open(struct inode*, struct file*) */
	NULL,			/* flush */
	smb_file_release,	/* release(struct inode*, struct file*) */
	smb_fsync,		/* fsync(struct file*, struct dentry*) */
	NULL,			/* fasync(struct file*, int) */
	NULL,			/* check_media_change(kdev_t dev) */
	NULL,			/* revalidate(kdev_t dev) */
	NULL			/* lock(struct file*, int, struct file_lock*) */
};

struct inode_operations smb_file_inode_operations =
{
	&smb_file_operations,	/* default file operations */
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
	smb_readpage,		/* readpage */
	smb_writepage,		/* writepage */
	NULL,			/* flushpage */
	NULL,			/* truncate */
	smb_file_permission,	/* permission */
	NULL,			/* smap */
	smb_revalidate_inode,	/* revalidate */
};
