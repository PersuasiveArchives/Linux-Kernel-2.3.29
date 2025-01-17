/*
 *  ISA Plug & Play support
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef LINUX_ISAPNP_H
#define LINUX_ISAPNP_H

#include <linux/config.h>
#include <linux/errno.h>

/*
 *  Configuration registers (TODO: change by specification)
 */ 

#define ISAPNP_CFG_ACTIVATE		0x30	/* byte */
#define ISAPNP_CFG_MEM			0x40	/* 4 * dword */
#define ISAPNP_CFG_PORT			0x60	/* 8 * word */
#define ISAPNP_CFG_IRQ			0x70	/* 2 * word */
#define ISAPNP_CFG_DMA			0x74	/* 2 * byte */

/*
 *
 */

#define ISAPNP_VENDOR(a,b,c)	(((((a)-'A'+1)&0x3f)<<2)|\
				((((b)-'A'+1)&0x18)>>3)|((((b)-'A'+1)&7)<<13)|\
				((((c)-'A'+1)&0x1f)<<8))
#define ISAPNP_DEVICE(x)	((((x)&0xf000)>>8)|\
				 (((x)&0x0f00)>>8)|\
				 (((x)&0x00f0)<<8)|\
				 (((x)&0x000f)<<8))
#define ISAPNP_FUNCTION(x)	ISAPNP_DEVICE(x)

/*
 *
 */

#ifdef __KERNEL__

#include <linux/pci.h>

#define ISAPNP_PORT_FLAG_16BITADDR	(1<<0)
#define ISAPNP_PORT_FLAG_FIXED		(1<<1)

struct isapnp_port {
	unsigned short min;		/* min base number */
	unsigned short max;		/* max base number */
	unsigned char align;		/* align boundary */
	unsigned char size;		/* size of range */
	unsigned char flags;		/* port flags */
	unsigned char pad;		/* pad */
	struct isapnp_resources *res;	/* parent */
	struct isapnp_port *next;	/* next port */
};

struct isapnp_irq {
	unsigned short map;		/* bitmaks for IRQ lines */
	unsigned char flags;		/* IRQ flags */
	unsigned char pad;		/* pad */
	struct isapnp_resources *res;	/* parent */
	struct isapnp_irq *next;	/* next IRQ */
};

struct isapnp_dma {
	unsigned char map;		/* bitmask for DMA channels */
	unsigned char flags;		/* DMA flags */
	struct isapnp_resources *res;	/* parent */
	struct isapnp_dma *next;	/* next port */
};

struct isapnp_mem {
	unsigned int min;		/* min base number */
	unsigned int max;		/* max base number */
	unsigned int align;		/* align boundary */
	unsigned int size;		/* size of range */
	unsigned char flags;		/* memory flags */
	unsigned char pad;		/* pad */
	struct isapnp_resources *res;	/* parent */
	struct isapnp_mem *next;	/* next memory resource */
};

struct isapnp_mem32 {
	/* TODO */
	unsigned char data[17];
	struct isapnp_resources *res;	/* parent */
	struct isapnp_mem32 *next;	/* next 32-bit memory resource */
};

#define ISAPNP_RES_PRIORITY_PREFERRED	0
#define ISAPNP_RES_PRIORITY_ACCEPTABLE	1
#define ISAPNP_RES_PRIORITY_FUNCTIONAL	2
#define ISAPNP_RES_PRIORITY_INVALID	65535

struct isapnp_resources {
	unsigned short priority;	/* priority */
	unsigned short dependent;	/* dependent resources */
	struct isapnp_port *port;	/* first port */
	struct isapnp_irq *irq;		/* first IRQ */
	struct isapnp_dma *dma;		/* first DMA */
	struct isapnp_mem *mem;		/* first memory resource */
	struct isapnp_mem32 *mem32;	/* first 32-bit memory */
	struct pci_dev *dev;		/* parent */
	struct isapnp_resources *alt;	/* alternative resource (aka dependent resources) */
	struct isapnp_resources *next;	/* next resource */
};

#if defined(CONFIG_ISAPNP) || (defined(CONFIG_ISAPNP_MODULE) && defined(MODULE))

/* lowlevel configuration */
int isapnp_present(void);
int isapnp_cfg_begin(int csn, int device);
int isapnp_cfg_end(void);
unsigned char isapnp_read_byte(unsigned char idx);
unsigned short isapnp_read_word(unsigned char idx);
unsigned int isapnp_read_dword(unsigned char idx);
void isapnp_write_byte(unsigned char idx, unsigned char val);
void isapnp_write_word(unsigned char idx, unsigned short val);
void isapnp_write_dword(unsigned char idx, unsigned int val);
void isapnp_wake(unsigned char csn);
void isapnp_device(unsigned char device);
void isapnp_activate(unsigned char device);
void isapnp_deactivate(unsigned char device);
/* manager */
struct pci_bus *isapnp_find_card(unsigned short vendor,
				 unsigned short device,
				 struct pci_bus *from);
struct pci_dev *isapnp_find_dev(struct pci_bus *card,
				unsigned short vendor,
				unsigned short function,
				struct pci_dev *from);
/* misc */
void isapnp_resource_change(struct resource *resource,
			    unsigned long start,
			    unsigned long size);
/* init/main.c */
int isapnp_init(void);

#else /* !CONFIG_ISAPNP */

/* lowlevel configuration */
extern inline int isapnp_present(void) { return 0; }
extern inline int isapnp_cfg_begin(int csn, int device) { return -ENODEV; }
extern inline int isapnp_cfg_end(void) { return -ENODEV; }
extern inline unsigned char isapnp_read_byte(unsigned char idx) { return 0xff; }
extern inline unsigned short isapnp_read_word(unsigned char idx) { return 0xffff; }
extern inline unsigned int isapnp_read_dword(unsigned char idx) { return 0xffffffff; }
extern inline void isapnp_write_byte(unsigned char idx, unsigned char val) { ; }
extern inline void isapnp_write_word(unsigned char idx, unsigned short val) { ; }
extern inline void isapnp_write_dword(unsigned char idx, unsigned int val) { ; }
extern void isapnp_wake(unsigned char csn) { ; }
extern void isapnp_device(unsigned char device) { ; }
extern void isapnp_activate(unsigned char device) { ; }
extern void isapnp_deactivate(unsigned char device) { ; }
/* manager */
extern struct pci_bus *isapnp_find_card(unsigned short vendor,
				        unsigned short device,
				        struct pci_bus *from) { return NULL; }
extern struct pci_dev *isapnp_find_dev(struct pci_bus *card,
				       unsigned short vendor,
				       unsigned short function,
				       struct pci_dev *from) { return NULL; }
extern void isapnp_resource_change(struct resource *resource,
				   unsigned long start,
				   unsigned long size) { ; }

#endif /* CONFIG_ISAPNP */

#endif /* __KERNEL__ */
#endif /* LINUX_ISAPNP_H */
