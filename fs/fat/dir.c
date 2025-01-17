/*
 *  linux/fs/fat/dir.c
 *
 *  directory handling functions for fat-based filesystems
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  Hidden files 1995 by Albert Cahalan <albert@ccs.neu.edu> <adc@coe.neu.edu>
 *
 *  VFAT extensions by Gordon Chaffee <chaffee@plateau.cs.berkeley.edu>
 *  Merged with msdos fs by Henrik Storner <storner@osiris.ping.dk>
 *  Rewritten for constant inumbers. Plugged buffer overrun in readdir(). AV
 */

#define ASC_LINUX_VERSION(V, P, S)	(((V) * 65536) + ((P) * 256) + (S))

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/msdos_fs.h>
#include <linux/nls.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/dirent.h>
#include <linux/mm.h>
#include <linux/ctype.h>

#include <asm/uaccess.h>

#include "msbuffer.h"

#define PRINTK(X)

static ssize_t fat_dir_read(struct file * filp, char * buf,
			    size_t count, loff_t *ppos)
{
	return -EISDIR;
}

struct file_operations fat_dir_operations = {
	NULL,			/* lseek - default */
	fat_dir_read,		/* read */
	NULL,			/* write - bad */
	fat_readdir,		/* readdir */
	NULL,			/* select v2.0.x/poll v2.1.x - default */
	fat_dir_ioctl,		/* ioctl - default */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* flush */
	NULL,			/* no special release code */
	file_fsync		/* fsync */
};

/*
 * Convert Unicode 16 to UTF8, translated Unicode, or ASCII.
 * If uni_xlate is enabled and we
 * can't get a 1:1 conversion, use a colon as an escape character since
 * it is normally invalid on the vfat filesystem.  The following three
 * characters are a sort of uuencoded 16 bit Unicode value.  This lets
 * us do a full dump and restore of Unicode filenames.  We could get
 * into some trouble with long Unicode names, but ignore that right now.
 * Ahem... Stack smashing in ring 0 isn't fun. Fixed.
 */
static int
uni16_to_x8(unsigned char *ascii, unsigned char *uni, int uni_xlate,
	    struct nls_table *nls)
{
	unsigned char *ip, *op;
	unsigned char ch, cl;
	unsigned char *uni_page;
	unsigned short val;

	ip = uni;
	op = ascii;

	while (*ip || ip[1]) {
		cl = *ip++;
		ch = *ip++;

		uni_page = nls->page_uni2charset[ch];
		if (uni_page && uni_page[cl]) {
			*op++ = uni_page[cl];
		} else {
			if (uni_xlate == 1) {
				*op++ = ':';
				val = (cl << 8) + ch;
				op[2] = fat_uni2esc[val & 0x3f];
				val >>= 6;
				op[1] = fat_uni2esc[val & 0x3f];
				val >>= 6;
				*op = fat_uni2esc[val & 0x3f];
				op += 3;
			} else {
				*op++ = '?';
			}
		}
		/* We have some slack there, so it's OK */
		if (op>ascii+256) {
			op = ascii + 256;
			break;
		}
	}
	*op = 0;
	return (op - ascii);
}

#if 0
static void dump_de(struct msdos_dir_entry *de)
{
	int i;
	unsigned char *p = (unsigned char *) de;
	printk("[");

	for (i = 0; i < 32; i++, p++) {
		printk("%02x ", *p);
	}
	printk("]\n");
}
#endif
static int memicmp(const char *s1, const char *s2, int len) {
	while(len--) if (tolower(*s1++)!=tolower(*s2++)) return 1;
	return 0;
}

/*
 * Return values: negative -> error, 0 -> not found, positive -> found,
 * value is the total amount of slots, including the shortname entry.
 */
int fat_search_long(
	struct inode *inode, const char *name, int name_len, int anycase,
	loff_t *spos, loff_t *lpos)
{
	struct super_block *sb = inode->i_sb;
	int ino,i,i2,last;
	char c;
	struct buffer_head *bh = NULL;
	struct msdos_dir_entry *de;
	loff_t cpos = 0;
	char bufname[14];
	unsigned char long_slots;
	int uni_xlate = MSDOS_SB(sb)->options.unicode_xlate;
	int utf8 = MSDOS_SB(sb)->options.utf8;
	unsigned char *unicode = NULL;
	struct nls_table *nls = MSDOS_SB(sb)->nls_io;
	int res = 0;

	while(1) {
		if (fat_get_entry(inode,&cpos,&bh,&de,&ino) == -1)
			goto EODir;
parse_record:
		long_slots = 0;
		if (de->name[0] == (__s8) DELETED_FLAG)
			continue;
		if (de->attr != ATTR_EXT && (de->attr & ATTR_VOLUME))
			continue;
		if (de->attr != ATTR_EXT && IS_FREE(de->name))
			continue;
		if (de->attr ==  ATTR_EXT) {
			struct msdos_dir_slot *ds;
			int offset;
			unsigned char id;
			unsigned char slot;
			unsigned char slots;
			unsigned char sum;
			unsigned char alias_checksum;

			if (!unicode) {
				unicode = (unsigned char *)
					__get_free_page(GFP_KERNEL);
				if (!unicode) {
					fat_brelse(sb, bh);
					return -ENOMEM;
				}
			}
parse_long:
			slots = 0;
			offset = 0;
			ds = (struct msdos_dir_slot *) de;
			id = ds->id;
			if (!(id & 0x40))
				continue;
			slots = id & ~0x40;
			if (slots > 20 || !slots)	/* ceil(256 * 2 / 26) */
				continue;
			long_slots = slots;
			alias_checksum = ds->alias_checksum;

			slot = slots;
			while (1) {
				slot--;
				offset = slot * 26;
				memcpy(&unicode[offset], ds->name0_4, 10);
				memcpy(&unicode[offset+10], ds->name5_10, 12);
				memcpy(&unicode[offset+22], ds->name11_12, 4);
				offset += 26;

				if (ds->id & 0x40) {
					unicode[offset] = 0;
					unicode[offset+1] = 0;
				}
				if (fat_get_entry(inode,&cpos,&bh,&de,&ino)<0)
					goto EODir;
				if (slot == 0)
					break;
				ds = (struct msdos_dir_slot *) de;
				if (ds->attr !=  ATTR_EXT)
					goto parse_record;
				if ((ds->id & ~0x40) != slot)
					goto parse_long;
				if (ds->alias_checksum != alias_checksum)
					goto parse_long;
			}
			if (de->name[0] == (__s8) DELETED_FLAG)
				continue;
			if (de->attr ==  ATTR_EXT)
				goto parse_long;
			if (IS_FREE(de->name) || (de->attr & ATTR_VOLUME))
				continue;
			for (sum = 0, i = 0; i < 11; i++)
				sum = (((sum&1)<<7)|((sum&0xfe)>>1)) + de->name[i];
			if (sum != alias_checksum)
				long_slots = 0;
		}

		for (i = 0, last = 0; i < 8;) {
			if (!(c = de->name[i])) break;
			if (c >= 'A' && c <= 'Z') c += 32;
			if (c == 0x05) c = 0xE5;
			if ((bufname[i++] = c) != ' ')
				last = i;
		}
		i = last;
		bufname[i++] = '.';
		for (i2 = 0; i2 < 3; i2++) {
			if (!(c = de->ext[i2])) break;
			if (c >= 'A' && c <= 'Z') c += 32;
			if ((bufname[i++] = c) != ' ')
				last = i;
		}
		if (!last)
			continue;

		if (last==name_len)
			if ((!anycase && !memcmp(name, bufname, last)) ||
			    (anycase && !memicmp(name, bufname, last)))
			goto Found;
		if (long_slots) {
			char longname[260];	/* 256 + 4 */
			unsigned char long_len;
			long_len = utf8
				?utf8_wcstombs(longname, (__u16 *) unicode, 260)
				:uni16_to_x8(longname, unicode, uni_xlate, nls);
			if (long_len != name_len)
				continue;
			if ((!anycase && !memcmp(name, longname, long_len)) ||
			    (anycase && !memicmp(name, longname, long_len)))
				goto Found;
		}
	}

Found:
	fat_brelse(sb, bh);
	res = long_slots + 1;
	*spos = cpos - sizeof(struct msdos_dir_entry);
	*lpos = cpos - res*sizeof(struct msdos_dir_entry);
EODir:
	if (unicode) {
		free_page((unsigned long) unicode);
	}
	return res;
}

static int fat_readdirx(
	struct inode *inode,
	struct file *filp,
	void *dirent,
	filldir_t filldir,
	int shortnames,
	int both)
{
	struct super_block *sb = inode->i_sb;
	int ino,inum,i,i2,last;
	char c;
	struct buffer_head *bh;
	struct msdos_dir_entry *de;
	unsigned long lpos;
	loff_t cpos;
	unsigned char long_slots;
	int uni_xlate = MSDOS_SB(sb)->options.unicode_xlate;
	int utf8 = MSDOS_SB(sb)->options.utf8;
	unsigned char *unicode = NULL;
	struct nls_table *nls = MSDOS_SB(sb)->nls_io;
	char bufname[14];
	char *ptname = bufname;
	int dotoffset = 0;
	unsigned long *furrfu = &lpos;
	unsigned long dummy;

	cpos = filp->f_pos;
/* Fake . and .. for the root directory. */
	if (inode->i_ino == MSDOS_ROOT_INO) {
		while (cpos < 2) {
			if (filldir(dirent, "..", cpos+1, cpos, MSDOS_ROOT_INO) < 0)
				return 0;
			cpos++;
			filp->f_pos++;
		}
		if (cpos == 2) {
			dummy = 2;
			furrfu = &dummy;
			cpos = 0;
		}
	}
	if (cpos & (sizeof(struct msdos_dir_entry)-1))
		return -ENOENT;

 	bh = NULL;
GetNew:
	long_slots = 0;
	if (fat_get_entry(inode,&cpos,&bh,&de,&ino) == -1)
		goto EODir;
	/* Check for long filename entry */
	if (MSDOS_SB(sb)->options.isvfat) {
		if (de->name[0] == (__s8) DELETED_FLAG)
			goto RecEnd;
		if (de->attr != ATTR_EXT && (de->attr & ATTR_VOLUME))
			goto RecEnd;
		if (de->attr != ATTR_EXT && IS_FREE(de->name))
			goto RecEnd;
	} else {
		if ((de->attr & ATTR_VOLUME) || IS_FREE(de->name))
			goto RecEnd;
	}

	if (MSDOS_SB(sb)->options.isvfat && de->attr ==  ATTR_EXT) {
		struct msdos_dir_slot *ds;
		int offset;
		unsigned char id;
		unsigned char slot;
		unsigned char slots;
		unsigned char sum;
		unsigned char alias_checksum;

		if (!unicode) {
			unicode = (unsigned char *)
				__get_free_page(GFP_KERNEL);
			if (!unicode) {
				filp->f_pos = cpos;
				fat_brelse(sb, bh);
				return -ENOMEM;
			}
		}
ParseLong:
		slots = 0;
		offset = 0;
		ds = (struct msdos_dir_slot *) de;
		id = ds->id;
		if (!(id & 0x40))
			goto RecEnd;
		slots = id & ~0x40;
		if (slots > 20 || !slots)	/* ceil(256 * 2 / 26) */
			goto RecEnd;
		long_slots = slots;
		alias_checksum = ds->alias_checksum;

		slot = slots;
		while (1) {
			slot--;
			offset = slot * 26;
			memcpy(&unicode[offset], ds->name0_4, 10);
			memcpy(&unicode[offset+10], ds->name5_10, 12);
			memcpy(&unicode[offset+22], ds->name11_12, 4);
			offset += 26;

			if (ds->id & 0x40) {
				unicode[offset] = 0;
				unicode[offset+1] = 0;
			}
			if (fat_get_entry(inode,&cpos,&bh,&de,&ino) == -1)
				goto EODir;
			if (slot == 0)
				break;
			ds = (struct msdos_dir_slot *) de;
			if (ds->attr !=  ATTR_EXT)
				goto RecEnd;	/* XXX */
			if ((ds->id & ~0x40) != slot)
				goto ParseLong;
			if (ds->alias_checksum != alias_checksum)
				goto ParseLong;
		}
		if (de->name[0] == (__s8) DELETED_FLAG)
			goto RecEnd;
		if (de->attr ==  ATTR_EXT)
			goto ParseLong;
		if (IS_FREE(de->name) || (de->attr & ATTR_VOLUME))
			goto RecEnd;
		for (sum = 0, i = 0; i < 11; i++)
			sum = (((sum&1)<<7)|((sum&0xfe)>>1)) + de->name[i];
		if (sum != alias_checksum)
			long_slots = 0;
	}

	if ((de->attr & ATTR_HIDDEN) && MSDOS_SB(sb)->options.dotsOK) {
		*ptname++ = '.';
		dotoffset = 1;
	}
	for (i = 0, last = 0; i < 8;) {
		if (!(c = de->name[i])) break;
		if (c >= 'A' && c <= 'Z') c += 32;
		/* see namei.c, msdos_format_name */
		if (c == 0x05) c = 0xE5;
		if ((ptname[i++] = c) != ' ')
			last = i;
	}
	i = last;
	ptname[i++] = '.';
	for (i2 = 0; i2 < 3; i2++) {
		if (!(c = de->ext[i2])) break;
		if (c >= 'A' && c <= 'Z') c += 32;
		if ((ptname[i++] = c) != ' ')
			last = i;
	}
	if (!last)
		goto RecEnd;

	i = last + dotoffset;

	lpos = cpos - (long_slots+1)*sizeof(struct msdos_dir_entry);
	if (!memcmp(de->name,MSDOS_DOT,11))
		inum = inode->i_ino;
	else if (!memcmp(de->name,MSDOS_DOTDOT,11)) {
/*		inum = fat_parent_ino(inode,0); */
		inum = filp->f_dentry->d_parent->d_inode->i_ino;
	} else {
		struct inode *tmp = fat_iget(sb, ino);
		if (tmp) {
			inum = tmp->i_ino;
			iput(tmp);
		} else
			inum = iunique(sb, MSDOS_ROOT_INO);
	}

	if (!long_slots||shortnames) {
		if (both)
			bufname[i] = '\0';
		if (filldir(dirent, bufname, i, *furrfu, inum) < 0)
			goto FillFailed;
	} else {
		char longname[275];
		unsigned char long_len = utf8
			? utf8_wcstombs(longname, (__u16 *) unicode, 275)
			: uni16_to_x8(longname, unicode, uni_xlate, nls);
		if (both) {
			memcpy(&longname[long_len+1], bufname, i);
			long_len += i;
		}
		if (filldir(dirent, longname, long_len, *furrfu, inum) < 0)
			goto FillFailed;
	}

RecEnd:
	furrfu = &lpos;
	filp->f_pos = cpos;
	goto GetNew;
EODir:
	filp->f_pos = cpos;
FillFailed:
	if (bh)
		fat_brelse(sb, bh);
	if (unicode) {
		free_page((unsigned long) unicode);
	}
	return 0;
}

int fat_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	return fat_readdirx(inode, filp, dirent, filldir, 0, 0);
}

static int vfat_ioctl_fill(
	void * buf,
	const char * name,
	int name_len,
	off_t offset,
	ino_t ino)
{
	struct dirent *d1 = (struct dirent *)buf;
	struct dirent *d2 = d1 + 1;
	int len, slen;
	int dotdir;

	get_user(len, &d1->d_reclen);
	if (len != 0) {
		return -1;
	}

	if ((name_len == 1 && name[0] == '.') ||
	    (name_len == 2 && name[0] == '.' && name[1] == '.')) {
		dotdir = 1;
		len = name_len;
	} else {
		dotdir = 0;
		len = strlen(name);
	}
	if (len != name_len) {
		copy_to_user(d2->d_name, name, len);
		put_user(0, d2->d_name + len);
		put_user(len, &d2->d_reclen);
		put_user(ino, &d2->d_ino);
		put_user(offset, &d2->d_off);
		slen = name_len - len;
		copy_to_user(d1->d_name, name+len+1, slen);
		put_user(0, d1->d_name+slen);
		put_user(slen, &d1->d_reclen);
	} else {
		put_user(0, d2->d_name);
		put_user(0, &d2->d_reclen);
		copy_to_user(d1->d_name, name, len);
		put_user(0, d1->d_name+len);
		put_user(len, &d1->d_reclen);
	}
	PRINTK(("FAT d1=%p d2=%p len=%d, name_len=%d\n",
		d1, d2, len, name_len));

	return 0;
}

int fat_dir_ioctl(struct inode * inode, struct file * filp,
		  unsigned int cmd, unsigned long arg)
{
	int err;
	/*
	 * We want to provide an interface for Samba to be able
	 * to get the short filename for a given long filename.
	 * Samba should use this ioctl instead of readdir() to
	 * get the information it needs.
	 */
	switch (cmd) {
	case VFAT_IOCTL_READDIR_BOTH: {
		struct dirent *d1 = (struct dirent *)arg;
		err = verify_area(VERIFY_WRITE, d1, sizeof(struct dirent[2]));
		if (err)
			return err;
		put_user(0, &d1->d_reclen);
		return fat_readdirx(inode,filp,(void *)arg,
				    vfat_ioctl_fill, 0, 1);
	}
	case VFAT_IOCTL_READDIR_SHORT: {
		struct dirent *d1 = (struct dirent *)arg;
		put_user(0, &d1->d_reclen);
		err = verify_area(VERIFY_WRITE, d1, sizeof(struct dirent[2]));
		if (err)
			return err;
		return fat_readdirx(inode,filp,(void *)arg,
				    vfat_ioctl_fill, 1, 1);
	}
	default:
		/* forward ioctl to CVF extension */
	       if (MSDOS_SB(inode->i_sb)->cvf_format &&
		   MSDOS_SB(inode->i_sb)->cvf_format->cvf_dir_ioctl)
		       return MSDOS_SB(inode->i_sb)->cvf_format
			       ->cvf_dir_ioctl(inode,filp,cmd,arg);
		return -EINVAL;
	}

	return 0;
}

/***** See if directory is empty */
int fat_dir_empty(struct inode *dir)
{
	loff_t pos;
	struct buffer_head *bh;
	struct msdos_dir_entry *de;
	int ino,result = 0;

	pos = 0;
	bh = NULL;
	while (fat_get_entry(dir,&pos,&bh,&de,&ino) > -1) {
		/* Ignore vfat longname entries */
		if (de->attr == ATTR_EXT)
			continue;
		if (!IS_FREE(de->name) && 
		    strncmp(de->name,MSDOS_DOT   , MSDOS_NAME) &&
		    strncmp(de->name,MSDOS_DOTDOT, MSDOS_NAME)) {
			result = -ENOTEMPTY;
			break;
		}
	}
	if (bh)
		fat_brelse(dir->i_sb, bh);

	return result;
}

/* This assumes that size of cluster is above the 32*slots */

int fat_add_entries(struct inode *dir,int slots, struct buffer_head **bh,
		  struct msdos_dir_entry **de, int *ino)
{
	struct super_block *sb = dir->i_sb;
	loff_t offset, curr;
	int row;
	struct buffer_head *new_bh;

	offset = curr = 0;
	*bh = NULL;
	row = 0;
	while (fat_get_entry(dir,&curr,bh,de,ino) > -1) {
		if (IS_FREE((*de)->name)) {
			if (++row == slots)
				return offset;
		} else {
			row = 0;
			offset = curr;
		}
	}
	if ((dir->i_ino == MSDOS_ROOT_INO) && (MSDOS_SB(sb)->fat_bits != 32)) 
		return -ENOSPC;
	new_bh = fat_extend_dir(dir);
	if (!new_bh)
		return -ENOSPC;
	fat_brelse(sb, new_bh);
	do fat_get_entry(dir,&curr,bh,de,ino); while (++row<slots);
	return offset;
}

int fat_new_dir(struct inode *dir, struct inode *parent, int is_vfat)
{
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh;
	struct msdos_dir_entry *de;
	__u16 date, time;

	if ((bh = fat_extend_dir(dir)) == NULL) return -ENOSPC;
	/* zeroed out, so... */
	fat_date_unix2dos(dir->i_mtime,&time,&date);
	de = (struct msdos_dir_entry*)&bh->b_data[0];
	memcpy(de[0].name,MSDOS_DOT,MSDOS_NAME);
	memcpy(de[1].name,MSDOS_DOTDOT,MSDOS_NAME);
	de[0].attr = de[1].attr = ATTR_DIR;
	de[0].time = de[1].time = CT_LE_W(time);
	de[0].date = de[1].date = CT_LE_W(date);
	if (is_vfat) {	/* extra timestamps */
		de[0].ctime = de[1].ctime = CT_LE_W(time);
		de[0].adate = de[0].cdate =
			de[1].adate = de[1].cdate = CT_LE_W(date);
	}
	de[0].start = CT_LE_W(MSDOS_I(dir)->i_logstart);
	de[0].starthi = CT_LE_W(MSDOS_I(dir)->i_logstart>>16);
	de[1].start = CT_LE_W(MSDOS_I(parent)->i_logstart);
	de[1].starthi = CT_LE_W(MSDOS_I(parent)->i_logstart>>16);
	fat_mark_buffer_dirty(sb, bh, 1);
	fat_brelse(sb, bh);
	dir->i_atime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(dir);

	return 0;
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 8
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -8
 * c-argdecl-indent: 8
 * c-label-offset: -8
 * c-continued-statement-offset: 8
 * c-continued-brace-offset: 0
 * End:
 */
