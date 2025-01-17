/*
 *  linux/fs/ufs/util.h
 *
 * Copyright (C) 1998 
 * Daniel Pirkl <daniel.pirkl@email.cz>
 * Charles University, Faculty of Mathematics and Physics
 */

#include <linux/fs.h>
#include "swab.h"


/*
 * some useful macros
 */
#define in_range(b,first,len)	((b)>=(first)&&(b)<(first)+(len))
#define min(x,y)		((x)<(y)?(x):(y))
#define max(x,y)		((x)>(y)?(x):(y))



/*
 * macros used for retyping
 */
#define UCPI_UBH ((struct ufs_buffer_head *)ucpi)
#define USPI_UBH ((struct ufs_buffer_head *)uspi)



/*
 * macros used for accesing structures
 */
#define ufs_get_fs_state(usb1,usb3) _ufs_get_fs_state_(usb1,usb3,flags,swab)
static inline __s32 _ufs_get_fs_state_(struct ufs_super_block_first * usb1,
	struct ufs_super_block_third * usb3, unsigned flags, unsigned swab)
{
	switch (flags & UFS_ST_MASK) {
		case UFS_ST_SUN:
			return SWAB32((usb3)->fs_u2.fs_sun.fs_state);
		case UFS_ST_SUNx86:
			return SWAB32((usb1)->fs_u1.fs_sunx86.fs_state);
		case UFS_ST_44BSD:
		default:
			return SWAB32((usb3)->fs_u2.fs_44.fs_state);
	}
}

#define ufs_set_fs_state(usb1,usb3,value) _ufs_set_fs_state_(usb1,usb3,value,flags,swab)
static inline void _ufs_set_fs_state_(struct ufs_super_block_first * usb1,
	struct ufs_super_block_third * usb3, __s32 value, unsigned flags, unsigned swab)
{
	switch (flags & UFS_ST_MASK) {
		case UFS_ST_SUN:
			(usb3)->fs_u2.fs_sun.fs_state = SWAB32(value);
			break;
		case UFS_ST_SUNx86:
			(usb1)->fs_u1.fs_sunx86.fs_state = SWAB32(value);
			break;
		case UFS_ST_44BSD:
			(usb3)->fs_u2.fs_44.fs_state = SWAB32(value);
			break;
	}
}

#define ufs_get_fs_npsect(usb1,usb3) _ufs_get_fs_npsect_(usb1,usb3,flags,swab)
static inline __u32 _ufs_get_fs_npsect_(struct ufs_super_block_first * usb1,
	struct ufs_super_block_third * usb3, unsigned flags, unsigned swab)
{
	if ((flags & UFS_ST_MASK) == UFS_ST_SUNx86)
		return SWAB32((usb3)->fs_u2.fs_sunx86.fs_npsect);
	else
		return SWAB32((usb1)->fs_u1.fs_sun.fs_npsect);
}

#define ufs_get_fs_qbmask(usb3) _ufs_get_fs_qbmask_(usb3,flags,swab)
static inline __u64 _ufs_get_fs_qbmask_(struct ufs_super_block_third * usb3,
	unsigned flags, unsigned swab)
{
	__u64 tmp;
	switch (flags & UFS_ST_MASK) {
		case UFS_ST_SUN:
			((u32 *)&tmp)[0] = usb3->fs_u2.fs_sun.fs_qbmask[0];
			((u32 *)&tmp)[1] = usb3->fs_u2.fs_sun.fs_qbmask[1];
			break;
		case UFS_ST_SUNx86:
			((u32 *)&tmp)[0] = usb3->fs_u2.fs_sunx86.fs_qbmask[0];
			((u32 *)&tmp)[1] = usb3->fs_u2.fs_sunx86.fs_qbmask[1];
			break;
		case UFS_ST_44BSD:
			((u32 *)&tmp)[0] = usb3->fs_u2.fs_44.fs_qbmask[0];
			((u32 *)&tmp)[1] = usb3->fs_u2.fs_44.fs_qbmask[1];
			break;
	}
	return SWAB64(tmp);
}

#define ufs_get_fs_qfmask(usb3) _ufs_get_fs_qfmask_(usb3,flags,swab)
static inline __u64 _ufs_get_fs_qfmask_(struct ufs_super_block_third * usb3,
	unsigned flags, unsigned swab)
{
	__u64 tmp;
	switch (flags & UFS_ST_MASK) {
		case UFS_ST_SUN:
			((u32 *)&tmp)[0] = usb3->fs_u2.fs_sun.fs_qfmask[0];
			((u32 *)&tmp)[1] = usb3->fs_u2.fs_sun.fs_qfmask[1];
			break;
		case UFS_ST_SUNx86:
			((u32 *)&tmp)[0] = usb3->fs_u2.fs_sunx86.fs_qfmask[0];
			((u32 *)&tmp)[1] = usb3->fs_u2.fs_sunx86.fs_qfmask[1];
			break;
		case UFS_ST_44BSD:
			((u32 *)&tmp)[0] = usb3->fs_u2.fs_44.fs_qfmask[0];
			((u32 *)&tmp)[1] = usb3->fs_u2.fs_44.fs_qfmask[1];
			break;
	}
	return SWAB64(tmp);
}

#define ufs_get_de_namlen(de) \
	(((flags & UFS_DE_MASK) == UFS_DE_OLD) \
	? SWAB16(de->d_u.d_namlen) \
	: de->d_u.d_44.d_namlen)

#define ufs_set_de_namlen(de,value) \
	(((flags & UFS_DE_MASK) == UFS_DE_OLD) \
	? (de->d_u.d_namlen = SWAB16(value)) \
	: (de->d_u.d_44.d_namlen = value))

#define ufs_set_de_type(de,mode) _ufs_set_de_type_(de,mode,flags,swab)
static inline void _ufs_set_de_type_(struct ufs_dir_entry * de, int mode, 
	unsigned flags, unsigned swab)
{
	if ((flags & UFS_DE_MASK) == UFS_DE_44BSD) {
		switch (mode & S_IFMT) {
			case S_IFSOCK: de->d_u.d_44.d_type = DT_SOCK; break;
			case S_IFLNK: de->d_u.d_44.d_type = DT_LNK; break;
			case S_IFREG: de->d_u.d_44.d_type = DT_REG; break;
			case S_IFBLK: de->d_u.d_44.d_type = DT_BLK; break;
			case S_IFDIR: de->d_u.d_44.d_type = DT_DIR; break;
			case S_IFCHR: de->d_u.d_44.d_type = DT_CHR; break;
			case S_IFIFO: de->d_u.d_44.d_type = DT_FIFO; break;
			default: de->d_u.d_44.d_type = DT_UNKNOWN;
		}
	}
}

#define ufs_get_inode_uid(inode) _ufs_get_inode_uid_(inode,flags,swab)
static inline __u32 _ufs_get_inode_uid_(struct ufs_inode * inode,
	unsigned flags, unsigned swab)
{
	switch (flags & UFS_UID_MASK) {
		case UFS_UID_EFT:
			return SWAB32(inode->ui_u3.ui_sun.ui_uid);
		case UFS_UID_44BSD:
			return SWAB32(inode->ui_u3.ui_44.ui_uid);
		default:
			return SWAB16(inode->ui_u1.oldids.ui_suid);
	}
}

#define ufs_set_inode_uid(inode,value) _ufs_set_inode_uid_(inode,value,flags,swab)
static inline void _ufs_set_inode_uid_(struct ufs_inode * inode, __u32 value,
	unsigned flags, unsigned swab)
{
	inode->ui_u1.oldids.ui_suid = SWAB16(value); 
	switch (flags & UFS_UID_MASK) {
		case UFS_UID_EFT:
			inode->ui_u3.ui_sun.ui_uid = SWAB32(value);
			break;
		case UFS_UID_44BSD:
			inode->ui_u3.ui_44.ui_uid = SWAB32(value);
			break;
	}
}

#define ufs_get_inode_gid(inode) _ufs_get_inode_gid_(inode,flags,swab)
static inline __u32 _ufs_get_inode_gid_(struct ufs_inode * inode, 
	unsigned flags, unsigned swab)
{
	switch (flags & UFS_UID_MASK) {
		case UFS_UID_EFT:
			return SWAB32(inode->ui_u3.ui_sun.ui_gid);
		case UFS_UID_44BSD:
			return SWAB32(inode->ui_u3.ui_44.ui_gid);
		default:
			return SWAB16(inode->ui_u1.oldids.ui_sgid);
	}
}

#define ufs_set_inode_gid(inode,value) _ufs_set_inode_gid_(inode,value,flags,swab)
static inline void _ufs_set_inode_gid_(struct ufs_inode * inode, __u32 value, 
	unsigned flags, unsigned swab)
{
	inode->ui_u1.oldids.ui_sgid =  SWAB16(value);
	switch (flags & UFS_UID_MASK) {
		case UFS_UID_EFT:
			inode->ui_u3.ui_sun.ui_gid = SWAB32(value);
			break;
		case UFS_UID_44BSD:
			inode->ui_u3.ui_44.ui_gid = SWAB32(value);
			break;
	}
}



/*
 * These functions manipulate ufs buffers
 */
#define ubh_bread(dev,fragment,size) _ubh_bread_(uspi,dev,fragment,size)  
extern struct ufs_buffer_head * _ubh_bread_(struct ufs_sb_private_info *, kdev_t, unsigned, unsigned);
extern struct ufs_buffer_head * ubh_bread_uspi(struct ufs_sb_private_info *, kdev_t, unsigned, unsigned);
extern void ubh_brelse (struct ufs_buffer_head *);
extern void ubh_brelse_uspi (struct ufs_sb_private_info *);
extern void ubh_mark_buffer_dirty (struct ufs_buffer_head *, int);
extern void ubh_mark_buffer_uptodate (struct ufs_buffer_head *, int);
extern void ubh_ll_rw_block (int, unsigned, struct ufs_buffer_head **);
extern void ubh_wait_on_buffer (struct ufs_buffer_head *);
extern unsigned ubh_max_bcount (struct ufs_buffer_head *);
extern void ubh_bforget (struct ufs_buffer_head *);
extern int  ubh_buffer_dirty (struct ufs_buffer_head *);
#define ubh_ubhcpymem(mem,ubh,size) _ubh_ubhcpymem_(uspi,mem,ubh,size)
extern void _ubh_ubhcpymem_(struct ufs_sb_private_info *, unsigned char *, struct ufs_buffer_head *, unsigned);
#define ubh_memcpyubh(ubh,mem,size) _ubh_memcpyubh_(uspi,ubh,mem,size)
extern void _ubh_memcpyubh_(struct ufs_sb_private_info *, struct ufs_buffer_head *, unsigned char *, unsigned);



/*
 * macros to get important structures from ufs_buffer_head
 */
#define ubh_get_usb_first(ubh) \
	((struct ufs_super_block_first *)((ubh)->bh[0]->b_data))

#define ubh_get_usb_second(ubh) \
	((struct ufs_super_block_second *)(ubh)-> \
	bh[UFS_SECTOR_SIZE >> uspi->s_fshift]->b_data + (UFS_SECTOR_SIZE & ~uspi->s_fmask))

#define ubh_get_usb_third(ubh) \
	((struct ufs_super_block_third *)((ubh)-> \
	bh[UFS_SECTOR_SIZE*2 >> uspi->s_fshift]->b_data + (UFS_SECTOR_SIZE*2 & ~uspi->s_fmask)))

#define ubh_get_ucg(ubh) \
	((struct ufs_cylinder_group *)((ubh)->bh[0]->b_data))


/*
 * Extract byte from ufs_buffer_head
 * Extract the bits for a block from a map inside ufs_buffer_head
 */
#define ubh_get_addr8(ubh,begin) \
	((u8*)(ubh)->bh[(begin) >> uspi->s_fshift]->b_data + \
	((begin) & ~uspi->s_fmask))

#define ubh_get_addr16(ubh,begin) \
	(((u16*)((ubh)->bh[(begin) >> (uspi->s_fshift-1)]->b_data)) + \
	((begin) & (uspi->fsize>>1) - 1)))

#define ubh_get_addr32(ubh,begin) \
	(((u32*)((ubh)->bh[(begin) >> (uspi->s_fshift-2)]->b_data)) + \
	((begin) & ((uspi->s_fsize>>2) - 1)))

#define ubh_get_addr ubh_get_addr8

#define ubh_blkmap(ubh,begin,bit) \
	((*ubh_get_addr(ubh, (begin) + ((bit) >> 3)) >> ((bit) & 7)) & (0xff >> (UFS_MAXFRAG - uspi->s_fpb)))


/*
 * Macros for access to superblock array structures
 */
#define ubh_postbl(ubh,cylno,i) \
	((uspi->s_postblformat != UFS_DYNAMICPOSTBLFMT) \
	? (*(__s16*)(ubh_get_addr(ubh, \
	(unsigned)(&((struct ufs_super_block *)0)->fs_opostbl) \
	+ (((cylno) * 16 + (i)) << 1) ) )) \
	: (*(__s16*)(ubh_get_addr(ubh, \
	uspi->s_postbloff + (((cylno) * uspi->s_nrpos + (i)) << 1) ))))

#define ubh_rotbl(ubh,i) \
	((uspi->s_postblformat != UFS_DYNAMICPOSTBLFMT) \
	? (*(__u8*)(ubh_get_addr(ubh, \
	(unsigned)(&((struct ufs_super_block *)0)->fs_space) + (i)))) \
	: (*(__u8*)(ubh_get_addr(ubh, uspi->s_rotbloff + (i)))))

/*
 * Determine the number of available frags given a
 * percentage to hold in reserve.
 */
#define ufs_freespace(usb, percentreserved) \
	(ufs_blkstofrags(SWAB32((usb)->fs_cstotal.cs_nbfree)) + \
	SWAB32((usb)->fs_cstotal.cs_nffree) - (uspi->s_dsize * (percentreserved) / 100))

/*
 * Macros to access cylinder group array structures
 */
#define ubh_cg_blktot(ucpi,cylno) \
	(*((__u32*)ubh_get_addr(UCPI_UBH, (ucpi)->c_btotoff + ((cylno) << 2))))

#define ubh_cg_blks(ucpi,cylno,rpos) \
	(*((__u16*)ubh_get_addr(UCPI_UBH, \
	(ucpi)->c_boff + (((cylno) * uspi->s_nrpos + (rpos)) << 1 ))))

/*
 * Bitmap operations
 * These functions work like classical bitmap operations.
 * The difference is that we don't have the whole bitmap
 * in one contiguous chunk of memory, but in several buffers.
 * The parameters of each function are super_block, ufs_buffer_head and
 * position of the beginning of the bitmap.
 */
#define ubh_setbit(ubh,begin,bit) \
	(*ubh_get_addr(ubh, (begin) + ((bit) >> 3)) |= (1 << ((bit) & 7)))

#define ubh_clrbit(ubh,begin,bit) \
	(*ubh_get_addr (ubh, (begin) + ((bit) >> 3)) &= ~(1 << ((bit) & 7)))

#define ubh_isset(ubh,begin,bit) \
	(*ubh_get_addr (ubh, (begin) + ((bit) >> 3)) & (1 << ((bit) & 7)))

#define ubh_isclr(ubh,begin,bit) (!ubh_isset(ubh,begin,bit))

#define ubh_find_first_zero_bit(ubh,begin,size) _ubh_find_next_zero_bit_(uspi,ubh,begin,size,0)

#define ubh_find_next_zero_bit(ubh,begin,size,offset) _ubh_find_next_zero_bit_(uspi,ubh,begin,size,offset)
static inline unsigned _ubh_find_next_zero_bit_(
	struct ufs_sb_private_info * uspi, struct ufs_buffer_head * ubh,
	unsigned begin, unsigned size, unsigned offset)
{
	unsigned base, count, pos;

	size -= offset;
	begin <<= 3;
	offset += begin;
	base = offset >> uspi->s_bpfshift;
	offset &= uspi->s_bpfmask;
	for (;;) {
		count = min (size + offset, uspi->s_bpf);
		size -= count - offset;
		pos = ext2_find_next_zero_bit (ubh->bh[base]->b_data, count, offset);
		if (pos < count || !size)
			break;
		base++;
		offset = 0;
	}
	return (base << uspi->s_bpfshift) + pos - begin;
} 	

static inline unsigned find_last_zero_bit (unsigned char * bitmap,
	unsigned size, unsigned offset)
{
	unsigned bit, i;
	unsigned char * mapp;
	unsigned char map;

	mapp = bitmap + (size >> 3);
	map = *mapp--;
	bit = 1 << (size & 7);
	for (i = size; i > offset; i--) {
		if ((map & bit) == 0)
			break;
		if ((i & 7) != 0) {
			bit >>= 1;
		} else {
			map = *mapp--;
			bit = 1 << 7;
		}
	}
	return i;
}

#define ubh_find_last_zero_bit(ubh,begin,size,offset) _ubh_find_last_zero_bit_(uspi,ubh,begin,size,offset)
static inline unsigned _ubh_find_last_zero_bit_(
	struct ufs_sb_private_info * uspi, struct ufs_buffer_head * ubh,
	unsigned begin, unsigned start, unsigned end)
{
	unsigned base, count, pos, size;

	size = start - end;
	begin <<= 3;
	start += begin;
	base = start >> uspi->s_bpfshift;
	start &= uspi->s_bpfmask;
	for (;;) {
		count = min (size + (uspi->s_bpf - start), uspi->s_bpf)
			- (uspi->s_bpf - start);
		size -= count;
		pos = find_last_zero_bit (ubh->bh[base]->b_data,
			start, start - count);
		if (pos > start - count || !size)
			break;
		base--;
		start = uspi->s_bpf;
	}
	return (base << uspi->s_bpfshift) + pos - begin;
} 	

#define ubh_isblockclear(ubh,begin,block) (!_ubh_isblockset_(uspi,ubh,begin,block))

#define ubh_isblockset(ubh,begin,block) _ubh_isblockset_(uspi,ubh,begin,block)
static inline int _ubh_isblockset_(struct ufs_sb_private_info * uspi,
	struct ufs_buffer_head * ubh, unsigned begin, unsigned block)
{
	switch (uspi->s_fpb) {
	case 8:
	    	return (*ubh_get_addr (ubh, begin + block) == 0xff);
	case 4:
		return (*ubh_get_addr (ubh, begin + (block >> 1)) == (0x0f << ((block & 0x01) << 2)));
	case 2:
		return (*ubh_get_addr (ubh, begin + (block >> 2)) == (0x03 << ((block & 0x03) << 1)));
	case 1:
		return (*ubh_get_addr (ubh, begin + (block >> 3)) == (0x01 << (block & 0x07)));
	}
	return 0;	
}

#define ubh_clrblock(ubh,begin,block) _ubh_clrblock_(uspi,ubh,begin,block)
static inline void _ubh_clrblock_(struct ufs_sb_private_info * uspi,
	struct ufs_buffer_head * ubh, unsigned begin, unsigned block)
{
	switch (uspi->s_fpb) {
	case 8:
	    	*ubh_get_addr (ubh, begin + block) = 0x00;
	    	return; 
	case 4:
		*ubh_get_addr (ubh, begin + (block >> 1)) &= ~(0x0f << ((block & 0x01) << 2));
		return;
	case 2:
		*ubh_get_addr (ubh, begin + (block >> 2)) &= ~(0x03 << ((block & 0x03) << 1));
		return;
	case 1:
		*ubh_get_addr (ubh, begin + (block >> 3)) &= ~(0x01 << ((block & 0x07)));
		return;
	}
}

#define ubh_setblock(ubh,begin,block) _ubh_setblock_(uspi,ubh,begin,block)
static inline void _ubh_setblock_(struct ufs_sb_private_info * uspi,
	struct ufs_buffer_head * ubh, unsigned begin, unsigned block)
{
	switch (uspi->s_fpb) {
	case 8:
	    	*ubh_get_addr(ubh, begin + block) = 0xff;
	    	return;
	case 4:
		*ubh_get_addr(ubh, begin + (block >> 1)) |= (0x0f << ((block & 0x01) << 2));
		return;
	case 2:
		*ubh_get_addr(ubh, begin + (block >> 2)) |= (0x03 << ((block & 0x03) << 1));
		return;
	case 1:
		*ubh_get_addr(ubh, begin + (block >> 3)) |= (0x01 << ((block & 0x07)));
		return;
	}
}

static inline void ufs_fragacct (struct super_block * sb, unsigned blockmap,
	unsigned * fraglist, int cnt)
{
	struct ufs_sb_private_info * uspi;
	unsigned fragsize, pos;
	unsigned swab;
	
	swab = sb->u.ufs_sb.s_swab;
	uspi = sb->u.ufs_sb.s_uspi;
	
	fragsize = 0;
	for (pos = 0; pos < uspi->s_fpb; pos++) {
		if (blockmap & (1 << pos)) {
			fragsize++;
		}
		else if (fragsize > 0) {
			ADD_SWAB32(fraglist[fragsize], cnt);
			fragsize = 0;
		}
	}
	if (fragsize > 0 && fragsize < uspi->s_fpb)
		ADD_SWAB32(fraglist[fragsize], cnt);
}

#define ubh_scanc(ubh,begin,size,table,mask) _ubh_scanc_(uspi,ubh,begin,size,table,mask)
static inline unsigned _ubh_scanc_(struct ufs_sb_private_info * uspi, struct ufs_buffer_head * ubh, 
	unsigned begin, unsigned size, unsigned char * table, unsigned char mask)
{
	unsigned rest, offset;
	unsigned char * cp;
	

	offset = begin & ~uspi->s_fmask;
	begin >>= uspi->s_fshift;
	for (;;) {
		if ((offset + size) < uspi->s_fsize)
			rest = size;
		else
			rest = uspi->s_fsize - offset;
		size -= rest;
		cp = ubh->bh[begin]->b_data + offset;
		while ((table[*cp++] & mask) == 0 && --rest);
		if (rest || !size)
			break;
		begin++;
		offset = 0;
	}
	return (size + rest);
}
