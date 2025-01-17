/*
 * ialloc.c
 *
 * PURPOSE
 *	Inode allocation handling routines for the OSTA-UDF(tm) filesystem.
 *
 * CONTACTS
 *	E-mail regarding any portion of the Linux UDF file system should be
 *	directed to the development team mailing list (run by majordomo):
 *		linux_udf@hootie.lvld.hp.com
 *
 * COPYRIGHT
 *	This file is distributed under the terms of the GNU General Public
 *	License (GPL). Copies of the GPL can be obtained from:
 *		ftp://prep.ai.mit.edu/pub/gnu/GPL
 *	Each contributing author retains all rights to their own work.
 *
 *  (C) 1998-1999 Ben Fennema
 *
 * HISTORY
 *
 *  02/24/99 blf  Created.
 *
 */

#include "udfdecl.h"
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/udf_fs.h>

#include "udf_i.h"
#include "udf_sb.h"

void udf_free_inode(struct inode * inode)
{
	struct super_block * sb = inode->i_sb;
	int is_directory;
	unsigned long ino;

	if (!inode->i_dev)
	{
		udf_debug("inode has no device\n");
		return;
	}
	if (inode->i_count > 1)
	{
		udf_debug("inode has count=%d\n", inode->i_count);
		return;
	}
	if (inode->i_nlink)
	{
		udf_debug("inode has nlink=%d\n", inode->i_nlink);
		return;
	}
	if (!sb)
	{
		udf_debug("inode on nonexistent device\n");
		return;
	}

	ino = inode->i_ino;

	lock_super(sb);

	is_directory = S_ISDIR(inode->i_mode);

	clear_inode(inode);

	if (UDF_SB_LVIDBH(sb))
	{
		if (is_directory)
			UDF_SB_LVIDIU(sb)->numDirs =
				cpu_to_le32(le32_to_cpu(UDF_SB_LVIDIU(sb)->numDirs) - 1);
		else
			UDF_SB_LVIDIU(sb)->numFiles =
				cpu_to_le32(le32_to_cpu(UDF_SB_LVIDIU(sb)->numFiles) - 1);
		
		mark_buffer_dirty(UDF_SB_LVIDBH(sb), 1);
	}

	unlock_super(sb);

	udf_free_blocks(inode, UDF_I_LOCATION(inode), 0, 1);
}

struct inode * udf_new_inode (const struct inode *dir, int mode, int * err)
{
	struct super_block *sb;
	struct inode * inode;
	int block;
	Uint32 start = UDF_I_LOCATION(dir).logicalBlockNum;

	inode = get_empty_inode();
	if (!inode)
	{
		*err = -ENOMEM;
		return NULL;
	}
	sb = dir->i_sb;
	inode->i_sb = sb;
	inode->i_flags = 0;
	*err = -ENOSPC;

	block = udf_new_block(dir, UDF_I_LOCATION(dir).partitionReferenceNum,
		start, err);
	if (*err)
	{
		iput(inode);
		return NULL;
	}
	lock_super(sb);

	if (UDF_SB_LVIDBH(sb))
	{
		struct LogicalVolHeaderDesc *lvhd;
		Uint64 uniqueID;
		lvhd = (struct LogicalVolHeaderDesc *)(UDF_SB_LVID(sb)->logicalVolContentsUse);
		if (S_ISDIR(mode))
			UDF_SB_LVIDIU(sb)->numDirs =
				cpu_to_le32(le32_to_cpu(UDF_SB_LVIDIU(sb)->numDirs) + 1);
		else
			UDF_SB_LVIDIU(sb)->numFiles =
				cpu_to_le32(le32_to_cpu(UDF_SB_LVIDIU(sb)->numFiles) + 1);
		UDF_I_UNIQUE(inode) = uniqueID = le64_to_cpu(lvhd->uniqueID);
		if (!(++uniqueID & 0x00000000FFFFFFFFUL))
			uniqueID += 16;
		lvhd->uniqueID = cpu_to_le64(uniqueID);
		mark_buffer_dirty(UDF_SB_LVIDBH(sb), 1);
	}
	inode->i_mode = mode;
	inode->i_sb = sb;
	inode->i_nlink = 1;
	inode->i_dev = sb->s_dev;
	inode->i_uid = current->fsuid;
	if (dir->i_mode & S_ISGID)
	{
		inode->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	}
	else
		inode->i_gid = current->fsgid;
	UDF_I_LOCATION(inode).logicalBlockNum = block;
	UDF_I_LOCATION(inode).partitionReferenceNum = UDF_I_LOCATION(dir).partitionReferenceNum;
	inode->i_ino = udf_get_lb_pblock(sb, UDF_I_LOCATION(inode), 0);
	inode->i_blksize = PAGE_SIZE;
	inode->i_blocks = 0;
	inode->i_size = 0;
	UDF_I_LENEATTR(inode) = 0;
	UDF_I_LENALLOC(inode) = 0;
	UDF_I_EXT0LOC(inode) = UDF_I_LOCATION(inode);
	UDF_I_EXT0LEN(inode) = 0;
#if 1
	UDF_I_EXT0OFFS(inode) = sizeof(struct FileEntry);
	UDF_I_ALLOCTYPE(inode) = ICB_FLAG_AD_IN_ICB;
#else
	UDF_I_EXT0OFFS(inode) = 0;
	UDF_I_ALLOCTYPE(inode) = ICB_FLAG_AD_LONG;
#endif

	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	UDF_I_UMTIME(inode) = UDF_I_UATIME(inode) = UDF_I_UCTIME(inode) = CURRENT_UTIME;
	inode->i_op = NULL;
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	unlock_super(sb);
	*err = 0;
	return inode;
}
