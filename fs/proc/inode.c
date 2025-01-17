/*
 *  linux/fs/proc/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/file.h>
#include <linux/locks.h>
#include <linux/limits.h>
#define __NO_VERSION__
#include <linux/module.h>

#include <asm/system.h>
#include <asm/uaccess.h>

extern void free_proc_entry(struct proc_dir_entry *);

struct proc_dir_entry * de_get(struct proc_dir_entry *de)
{
	if (de)
		de->count++;
	return de;
}

/*
 * Decrements the use count and checks for deferred deletion.
 */
void de_put(struct proc_dir_entry *de)
{
	if (de) {
		if (!de->count) {
			printk("de_put: entry %s already free!\n", de->name);
			return;
		}

		if (!--de->count) {
			if (de->deleted) {
				printk("de_put: deferred delete of %s\n",
					de->name);
				free_proc_entry(de);
			}
		}
	}
}

static void proc_put_inode(struct inode *inode)
{
	/*
	 * Kill off unused inodes ... VFS will unhash and
	 * delete the inode if we set i_nlink to zero.
	 */
	if (inode->i_count == 1)
		inode->i_nlink = 0;
}

/*
 * Decrement the use count of the proc_dir_entry.
 */
static void proc_delete_inode(struct inode *inode)
{
	struct proc_dir_entry *de = inode->u.generic_ip;

	if (PROC_INODE_PROPER(inode)) {
		proc_pid_delete_inode(inode);
		return;
	}
	if (de) {
		if (de->owner)
			__MOD_DEC_USE_COUNT(de->owner);
		de_put(de);
	}
}

struct super_block *proc_super_blocks = NULL;

static void proc_put_super(struct super_block *sb)
{
	struct super_block **p = &proc_super_blocks;
	while (*p != sb) {
		if (!*p)	/* should never happen */
			return;
		p = (struct super_block **)&(*p)->u.generic_sbp;
	}
	*p = (struct super_block *)(*p)->u.generic_sbp;
}

static void proc_write_inode(struct inode * inode)
{
}

static void proc_read_inode(struct inode * inode)
{
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
}

static int proc_statfs(struct super_block *sb, struct statfs *buf, int bufsiz)
{
	struct statfs tmp;

	tmp.f_type = PROC_SUPER_MAGIC;
	tmp.f_bsize = PAGE_SIZE/sizeof(long);
	tmp.f_blocks = 0;
	tmp.f_bfree = 0;
	tmp.f_bavail = 0;
	tmp.f_files = 0;
	tmp.f_ffree = 0;
	tmp.f_namelen = NAME_MAX;
	return copy_to_user(buf, &tmp, bufsiz) ? -EFAULT : 0;
}

static struct super_operations proc_sops = { 
	proc_read_inode,
	proc_write_inode,
	proc_put_inode,
	proc_delete_inode,	/* delete_inode(struct inode *) */
	NULL,
	proc_put_super,
	NULL,
	proc_statfs,
	NULL
};


static int parse_options(char *options,uid_t *uid,gid_t *gid)
{
	char *this_char,*value;

	*uid = current->uid;
	*gid = current->gid;
	if (!options) return 1;
	for (this_char = strtok(options,","); this_char; this_char = strtok(NULL,",")) {
		if ((value = strchr(this_char,'=')) != NULL)
			*value++ = 0;
		if (!strcmp(this_char,"uid")) {
			if (!value || !*value)
				return 0;
			*uid = simple_strtoul(value,&value,0);
			if (*value)
				return 0;
		}
		else if (!strcmp(this_char,"gid")) {
			if (!value || !*value)
				return 0;
			*gid = simple_strtoul(value,&value,0);
			if (*value)
				return 0;
		}
		else return 1;
	}
	return 1;
}

struct inode * proc_get_inode(struct super_block * sb, int ino,
				struct proc_dir_entry * de)
{
	struct inode * inode;

	/*
	 * Increment the use count so the dir entry can't disappear.
	 */
	de_get(de);
#if 1
/* shouldn't ever happen */
if (de && de->deleted)
printk("proc_iget: using deleted entry %s, count=%d\n", de->name, de->count);
#endif

	inode = iget(sb, ino);
	if (!inode)
		goto out_fail;
	
	inode->u.generic_ip = (void *) de;
	if (de) {
		if (de->mode) {
			inode->i_mode = de->mode;
			inode->i_uid = de->uid;
			inode->i_gid = de->gid;
		}
		if (de->size)
			inode->i_size = de->size;
		if (de->nlink)
			inode->i_nlink = de->nlink;
		if (de->owner)
			__MOD_INC_USE_COUNT(de->owner);
		if (S_ISBLK(de->mode)||S_ISCHR(de->mode)||S_ISFIFO(de->mode))
			init_special_inode(inode,de->mode,kdev_t_to_nr(de->rdev));
		else if (de->ops)
			inode->i_op = de->ops;
	}

out:
	return inode;

out_fail:
	de_put(de);
	goto out;
}			

struct super_block *proc_read_super(struct super_block *s,void *data, 
				    int silent)
{
	struct inode * root_inode;
	struct task_struct *p;

	lock_super(s);
	s->s_blocksize = 1024;
	s->s_blocksize_bits = 10;
	s->s_magic = PROC_SUPER_MAGIC;
	s->s_op = &proc_sops;
	root_inode = proc_get_inode(s, PROC_ROOT_INO, &proc_root);
	if (!root_inode)
		goto out_no_root;
	/*
	 * Fixup the root inode's nlink value
	 */
	read_lock(&tasklist_lock);
	for_each_task(p) if (p->pid) root_inode->i_nlink++;
	read_unlock(&tasklist_lock);
	s->s_root = d_alloc_root(root_inode);
	if (!s->s_root)
		goto out_no_root;
	parse_options(data, &root_inode->i_uid, &root_inode->i_gid);
	s->u.generic_sbp = (void*) proc_super_blocks;
	proc_super_blocks = s;
	unlock_super(s);
	return s;

out_no_root:
	printk("proc_read_super: get root inode failed\n");
	iput(root_inode);
	s->s_dev = 0;
	unlock_super(s);
	return NULL;
}
