/* drawable.c -- IOCTLs for drawables -*- linux-c -*-
 * Created: Tue Feb  2 08:37:54 1999 by faith@precisioninsight.com
 * Revised: Fri Aug 20 09:27:03 1999 by faith@precisioninsight.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 * 
 * $PI: xc/programs/Xserver/hw/xfree86/os-support/linux/drm/generic/drawable.c,v 1.3 1999/08/30 13:05:00 faith Exp $
 * $XFree86$
 *
 */

#define __NO_VERSION__
#include "drmP.h"

int drm_adddraw(struct inode *inode, struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	drm_draw_t draw;

	draw.handle = 0;	/* NOOP */
	DRM_DEBUG("%d\n", draw.handle);
	copy_to_user_ret((drm_draw_t *)arg, &draw, sizeof(draw), -EFAULT);
	return 0;
}

int drm_rmdraw(struct inode *inode, struct file *filp, unsigned int cmd,
	       unsigned long arg)
{
	return 0;		/* NOOP */
}
