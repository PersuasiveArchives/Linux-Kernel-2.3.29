/* $Id: tcxfb.c,v 1.8 1999/08/10 15:56:26 davem Exp $
 * tcxfb.c: TCX 24/8bit frame buffer driver
 *
 * Copyright (C) 1996,1998 Jakub Jelinek (jj@ultra.linux.cz)
 * Copyright (C) 1996 Miguel de Icaza (miguel@nuclecu.unam.mx)
 * Copyright (C) 1996 Eddie C. Dost (ecd@skynet.be)
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/selection.h>

#include <video/sbusfb.h>
#include <asm/io.h>

#include <video/fbcon-cfb8.h>

/* THC definitions */
#define TCX_THC_MISC_REV_SHIFT       16
#define TCX_THC_MISC_REV_MASK        15
#define TCX_THC_MISC_VSYNC_DIS       (1 << 25)
#define TCX_THC_MISC_HSYNC_DIS       (1 << 24)
#define TCX_THC_MISC_RESET           (1 << 12)
#define TCX_THC_MISC_VIDEO           (1 << 10)
#define TCX_THC_MISC_SYNC            (1 << 9)
#define TCX_THC_MISC_VSYNC           (1 << 8)
#define TCX_THC_MISC_SYNC_ENAB       (1 << 7)
#define TCX_THC_MISC_CURS_RES        (1 << 6)
#define TCX_THC_MISC_INT_ENAB        (1 << 5)
#define TCX_THC_MISC_INT             (1 << 4)
#define TCX_THC_MISC_INIT            0x9f
#define TCX_THC_REV_REV_SHIFT        20
#define TCX_THC_REV_REV_MASK         15
#define TCX_THC_REV_MINREV_SHIFT     28
#define TCX_THC_REV_MINREV_MASK      15

/* The contents are unknown */
struct tcx_tec {
	volatile u32 tec_matrix;
	volatile u32 tec_clip;
	volatile u32 tec_vdc;
};

struct tcx_thc {
	volatile u32 thc_rev;
        u32 thc_pad0[511];
	volatile u32 thc_hs;		/* hsync timing */
	volatile u32 thc_hsdvs;
	volatile u32 thc_hd;
	volatile u32 thc_vs;		/* vsync timing */
	volatile u32 thc_vd;
	volatile u32 thc_refresh;
	volatile u32 thc_misc;
	u32 thc_pad1[56];
	volatile u32 thc_cursxy;	/* cursor x,y position (16 bits each) */
	volatile u32 thc_cursmask[32];	/* cursor mask bits */
	volatile u32 thc_cursbits[32];	/* what to show where mask enabled */
};

static struct sbus_mmap_map tcx_mmap_map[] = {
	{ TCX_RAM8BIT,		0,		SBUS_MMAP_FBSIZE(1) },
	{ TCX_RAM24BIT,		0,		SBUS_MMAP_FBSIZE(4) },
	{ TCX_UNK3,		0,		SBUS_MMAP_FBSIZE(8) },
	{ TCX_UNK4,		0,		SBUS_MMAP_FBSIZE(8) },
	{ TCX_CONTROLPLANE,	0,		SBUS_MMAP_FBSIZE(4) },
	{ TCX_UNK6,		0,		SBUS_MMAP_FBSIZE(8) },
	{ TCX_UNK7,		0,		SBUS_MMAP_FBSIZE(8) },
	{ TCX_TEC,		0,		PAGE_SIZE	    },
	{ TCX_BTREGS,		0,		PAGE_SIZE	    },
	{ TCX_THC,		0,		PAGE_SIZE	    },
	{ TCX_DHC,		0,		PAGE_SIZE	    },
	{ TCX_ALT,		0,		PAGE_SIZE	    },
	{ TCX_UNK2,		0,		0x20000		    },
	{ 0,			0,		0		    }
};

static void tcx_set_control_plane (struct fb_info_sbusfb *fb)
{
	u32 *p, *pend;
        
	p = fb->s.tcx.cplane;
	if (!p) return;
	for (pend = p + fb->type.fb_size; p < pend; p++)
		*p &= 0xffffff;
}
                                                
static void tcx_switch_from_graph (struct fb_info_sbusfb *fb)
{
	/* Reset control plane to 8bit mode if necessary */
	if (fb->open && fb->mmaped)
		tcx_set_control_plane (fb);
}

static void tcx_loadcmap (struct fb_info_sbusfb *fb, struct display *p, int index, int count)
{
	struct bt_regs *bt = fb->s.tcx.bt;
	int i;
                
	bt->addr = index << 24;
	for (i = index; count--; i++){
		bt->color_map = fb->color_map CM(i,0) << 24;
		bt->color_map = fb->color_map CM(i,1) << 24;
		bt->color_map = fb->color_map CM(i,2) << 24;
	}
	bt->addr = 0;
}

static void tcx_restore_palette (struct fb_info_sbusfb *fb)
{
	struct bt_regs *bt = fb->s.tcx.bt;
                
	bt->addr = 0;
	bt->color_map = 0xffffffff;
	bt->color_map = 0xffffffff;
	bt->color_map = 0xffffffff;
}

static void tcx_setcursormap (struct fb_info_sbusfb *fb, u8 *red, u8 *green, u8 *blue)
{
        struct bt_regs *bt = fb->s.tcx.bt;

	/* Note the 2 << 24 is different from cg6's 1 << 24 */
	bt->addr = 2 << 24;
	bt->cursor = red[0] << 24;
	bt->cursor = green[0] << 24;
	bt->cursor = blue[0] << 24;
	bt->addr = 3 << 24;
	bt->cursor = red[1] << 24;
	bt->cursor = green[1] << 24;
	bt->cursor = blue[1] << 24;
	bt->addr = 0;
}

/* Set cursor shape */
static void tcx_setcurshape (struct fb_info_sbusfb *fb)
{
	struct tcx_thc *thc = fb->s.tcx.thc;
	int i;

	for (i = 0; i < 32; i++){
		thc->thc_cursmask [i] = fb->cursor.bits[0][i];
		thc->thc_cursbits [i] = fb->cursor.bits[1][i];
	}
}

/* Load cursor information */
static void tcx_setcursor (struct fb_info_sbusfb *fb)
{
	unsigned int v;
	struct cg_cursor *c = &fb->cursor;

	if (c->enable)
		v = ((c->cpos.fbx - c->chot.fbx) << 16)
		    |((c->cpos.fby - c->chot.fby) & 0xffff);
	else
		/* Magic constant to turn off the cursor */
		v = ((65536-32) << 16) | (65536-32);
	fb->s.tcx.thc->thc_cursxy = v;
}

static void tcx_blank (struct fb_info_sbusfb *fb)
{
	fb->s.tcx.thc->thc_misc &= ~TCX_THC_MISC_VIDEO;
	/* This should put us in power-save */
	fb->s.tcx.thc->thc_misc |= TCX_THC_MISC_VSYNC_DIS;
        fb->s.tcx.thc->thc_misc |= TCX_THC_MISC_HSYNC_DIS;
}

static void tcx_unblank (struct fb_info_sbusfb *fb)
{
	fb->s.tcx.thc->thc_misc &= ~TCX_THC_MISC_VSYNC_DIS;
	fb->s.tcx.thc->thc_misc &= ~TCX_THC_MISC_HSYNC_DIS;
	fb->s.tcx.thc->thc_misc |= TCX_THC_MISC_VIDEO;
}

static void tcx_reset (struct fb_info_sbusfb *fb)
{
	if (fb->open && fb->mmaped)
		tcx_set_control_plane(fb);
	
	/* Turn off stuff in the Transform Engine. */
	fb->s.tcx.tec->tec_matrix = 0;
	fb->s.tcx.tec->tec_clip = 0;
	fb->s.tcx.tec->tec_vdc = 0;

	/* Enable cursor in Brooktree DAC. */
	fb->s.tcx.bt->addr = 0x06 << 24;
	fb->s.tcx.bt->control |= 0x03 << 24;
}

static void tcx_margins (struct fb_info_sbusfb *fb, struct display *p, int x_margin, int y_margin)
{
	p->screen_base += (y_margin - fb->y_margin) * p->line_length + (x_margin - fb->x_margin);
}

static char idstring[60] __initdata = { 0 };

char __init *tcxfb_init(struct fb_info_sbusfb *fb)
{
	struct fb_fix_screeninfo *fix = &fb->fix;
	struct display *disp = &fb->disp;
	struct fbtype *type = &fb->type;
	unsigned long phys = fb->sbdp->reg_addrs[0].phys_addr;
	int lowdepth, i, j;

#ifndef FBCON_HAS_CFB8
	return NULL;
#endif

	lowdepth = prom_getbool (fb->prom_node, "tcx-8-bit");
	
	if (lowdepth) {
		strcpy(fb->info.modename, "TCX8");
		strcpy(fix->id, "TCX8");
	} else {
		strcpy(fb->info.modename, "TCX24");
		strcpy(fix->id, "TCX24");
	}
	fix->line_length = fb->var.xres_virtual;
	fix->accel = FB_ACCEL_SUN_TCX;
	
	disp->scrollmode = SCROLL_YREDRAW;
	if (!disp->screen_base)
		disp->screen_base = (char *)sparc_alloc_io(phys, 0, 
			type->fb_size, "tcx_ram", fb->iospace, 0);
	disp->screen_base += fix->line_length * fb->y_margin + fb->x_margin;
	fb->s.tcx.tec = (struct tcx_tec *)sparc_alloc_io(fb->sbdp->reg_addrs[7].phys_addr, 0, 
			sizeof(struct tcx_tec), "tcx_tec", fb->iospace, 0);
	fb->s.tcx.thc = (struct tcx_thc *)sparc_alloc_io(fb->sbdp->reg_addrs[9].phys_addr, 0, 
			sizeof(struct tcx_thc), "tcx_thc", fb->iospace, 0);
	fb->s.tcx.bt = (struct bt_regs *)sparc_alloc_io(fb->sbdp->reg_addrs[8].phys_addr, 0, 
			sizeof(struct bt_regs), "tcx_dac", fb->iospace, 0);
	if (!lowdepth) {
		fb->s.tcx.cplane = (u32 *)sparc_alloc_io(fb->sbdp->reg_addrs[4].phys_addr, 0, 
				type->fb_size*4, "tcx_cplane", fb->iospace, 0);
		type->fb_depth = 24;
		fb->switch_from_graph = tcx_switch_from_graph;
	} else {
		/* As there can be one tcx in a machine only, we can write directly into
		   tcx_mmap_map */
		tcx_mmap_map[1].size = SBUS_MMAP_EMPTY;
		tcx_mmap_map[4].size = SBUS_MMAP_EMPTY;
		tcx_mmap_map[5].size = SBUS_MMAP_EMPTY;
		tcx_mmap_map[6].size = SBUS_MMAP_EMPTY;
	}
	fb->dispsw = fbcon_cfb8;

	fb->margins = tcx_margins;
	fb->loadcmap = tcx_loadcmap;
	if (prom_getbool (fb->prom_node, "hw-cursor")) {
		fb->setcursor = tcx_setcursor;
		fb->setcursormap = tcx_setcursormap;
		fb->setcurshape = tcx_setcurshape;
	}
	fb->restore_palette = tcx_restore_palette;
	fb->blank = tcx_blank;
	fb->unblank = tcx_unblank;
	fb->reset = tcx_reset;

	fb->physbase = 0;
	for (i = 0; i < 13; i++) {
		/* tcx_mmap_map has to be sorted by voff, while
		   order of phys registers from PROM differs a little
		   bit. Here is the correction */
		switch (i) {
		case 10: j = 12; break;
		case 11:
		case 12: j = i - 1; break;
		default: j = i; break;
		}
		tcx_mmap_map[i].poff = fb->sbdp->reg_addrs[j].phys_addr;
	}
	fb->mmap_map = tcx_mmap_map;

	/* Initialize Brooktree DAC */
	fb->s.tcx.bt->addr = 0x04 << 24;         /* color planes */
	fb->s.tcx.bt->control = 0xff << 24;
	fb->s.tcx.bt->addr = 0x05 << 24;
	fb->s.tcx.bt->control = 0x00 << 24;
	fb->s.tcx.bt->addr = 0x06 << 24;         /* overlay plane */
	fb->s.tcx.bt->control = 0x73 << 24;
	fb->s.tcx.bt->addr = 0x07 << 24;
	fb->s.tcx.bt->control = 0x00 << 24;

	sprintf(idstring, "tcx at %x.%08lx Rev %d.%d %s", fb->iospace, phys,
		    (fb->s.tcx.thc->thc_rev >> TCX_THC_REV_REV_SHIFT) & TCX_THC_REV_REV_MASK,
                    (fb->s.tcx.thc->thc_rev >> TCX_THC_REV_MINREV_SHIFT) & TCX_THC_REV_MINREV_MASK,
		    lowdepth ? "8-bit only" : "24-bit depth");
		    
	tcx_reset(fb);

	return idstring;
}
