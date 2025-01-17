/* fops.c -- File operations for DRM -*- linux-c -*-
 * Created: Mon Jan  4 08:58:31 1999 by faith@precisioninsight.com
 * Revised: Fri Aug 20 11:31:46 1999 by faith@precisioninsight.com
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
 * $PI: xc/programs/Xserver/hw/xfree86/os-support/linux/drm/generic/fops.c,v 1.3 1999/08/20 15:36:45 faith Exp $
 * $XFree86$
 *
 */

#define __NO_VERSION__
#include "drmP.h"

/* drm_open is called whenever a process opens /dev/drm. */

int drm_open_helper(struct inode *inode, struct file *filp, drm_device_t *dev)
{
	kdev_t	     minor = MINOR(inode->i_rdev);
	drm_file_t   *priv;

	if (filp->f_flags & O_EXCL)   return -EBUSY; /* No exclusive opens */

	DRM_DEBUG("pid = %d, minor = %d\n", current->pid, minor);

	priv		    = drm_alloc(sizeof(*priv), DRM_MEM_FILES);
	memset(priv, 0, sizeof(*priv));
	filp->private_data  = priv;
	priv->uid	    = current->euid;
	priv->pid	    = current->pid;
	priv->minor	    = minor;
	priv->dev	    = dev;
	priv->ioctl_count   = 0;
	priv->authenticated = capable(CAP_SYS_ADMIN);

	down(&dev->struct_sem);
	if (!dev->file_last) {
		priv->next	= NULL;
		priv->prev	= NULL;
		dev->file_first = priv;
		dev->file_last	= priv;
	} else {
		priv->next	     = NULL;
		priv->prev	     = dev->file_last;
		dev->file_last->next = priv;
		dev->file_last	     = priv;
	}
	up(&dev->struct_sem);
	
	return 0;
}

int drm_flush(struct file *filp)
{
	drm_file_t    *priv   = filp->private_data;
	drm_device_t  *dev    = priv->dev;

	DRM_DEBUG("pid = %d, device = 0x%x, open_count = %d, f_count = %d\n",
		  current->pid, dev->device, dev->open_count, atomic_read(&filp->f_count));
	return 0;
}

/* drm_release is called whenever a process closes /dev/drm*.  Linux calls
   this only if any mappings have been closed. */

int drm_release(struct inode *inode, struct file *filp)
{
	drm_file_t    *priv   = filp->private_data;
	drm_device_t  *dev    = priv->dev;

	DRM_DEBUG("pid = %d, device = 0x%x, open_count = %d\n",
		  current->pid, dev->device, dev->open_count);

	drm_reclaim_buffers(dev, priv->pid);

	drm_fasync(-1, filp, 0);

	down(&dev->struct_sem);
	if (priv->prev) priv->prev->next = priv->next;
	else		dev->file_first	 = priv->next;
	if (priv->next) priv->next->prev = priv->prev;
	else		dev->file_last	 = priv->prev;
	up(&dev->struct_sem);
	
	drm_free(priv, sizeof(*priv), DRM_MEM_FILES);
	
	return 0;
}

int drm_fasync(int fd, struct file *filp, int on)
{
	drm_file_t    *priv   = filp->private_data;
	drm_device_t  *dev    = priv->dev;
	int	      retcode;
	
	DRM_DEBUG("fd = %d, device = 0x%x\n", fd, dev->device);
	retcode = fasync_helper(fd, filp, on, &dev->buf_async);
	if (retcode < 0) return retcode;
	return 0;
}


/* The drm_read and drm_write_string code (especially that which manages
   the circular buffer), is based on Alessandro Rubini's LINUX DEVICE
   DRIVERS (Cambridge: O'Reilly, 1998), pages 111-113. */

ssize_t drm_read(struct file *filp, char *buf, size_t count, loff_t *off)
{
	drm_file_t    *priv   = filp->private_data;
	drm_device_t  *dev    = priv->dev;
	int	      left;
	int	      avail;
	int	      send;
	int	      cur;

	DRM_DEBUG("%p, %p\n", dev->buf_rp, dev->buf_wp);
	
	while (dev->buf_rp == dev->buf_wp) {
		DRM_DEBUG("  sleeping\n");
		if (filp->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}
		interruptible_sleep_on(&dev->buf_readers);
		if (signal_pending(current)) {
			DRM_DEBUG("  interrupted\n");
			return -ERESTARTSYS;
		}
		DRM_DEBUG("  awake\n");
	}

	left  = (dev->buf_rp + DRM_BSZ - dev->buf_wp) % DRM_BSZ;
	avail = DRM_BSZ - left;
	send  = DRM_MIN(avail, count);

	while (send) {
		if (dev->buf_wp > dev->buf_rp) {
			cur = DRM_MIN(send, dev->buf_wp - dev->buf_rp);
		} else {
			cur = DRM_MIN(send, dev->buf_end - dev->buf_rp);
		}
		copy_to_user_ret(buf, dev->buf_rp, cur, -EINVAL);
		dev->buf_rp += cur;
		if (dev->buf_rp == dev->buf_end) dev->buf_rp = dev->buf;
		send -= cur;
	}
	
	wake_up_interruptible(&dev->buf_writers);
	return DRM_MIN(avail, count);;
}

int drm_write_string(drm_device_t *dev, const char *s)
{
	int left   = (dev->buf_rp + DRM_BSZ - dev->buf_wp) % DRM_BSZ;
	int send   = strlen(s);
	int count;

	DRM_DEBUG("%d left, %d to send (%p, %p)\n",
		  left, send, dev->buf_rp, dev->buf_wp);
	
	if (left == 1 || dev->buf_wp != dev->buf_rp) {
		DRM_ERROR("Buffer not empty (%d left, wp = %p, rp = %p)\n",
			  left,
			  dev->buf_wp,
			  dev->buf_rp);
	}

	while (send) {
		if (dev->buf_wp >= dev->buf_rp) {
			count = DRM_MIN(send, dev->buf_end - dev->buf_wp);
			if (count == left) --count; /* Leave a hole */
		} else {
			count = DRM_MIN(send, dev->buf_rp - dev->buf_wp - 1);
		}
		strncpy(dev->buf_wp, s, count);
		dev->buf_wp += count;
		if (dev->buf_wp == dev->buf_end) dev->buf_wp = dev->buf;
		send -= count;
	}

	if (dev->buf_async) kill_fasync(dev->buf_async, SIGIO, POLL_OUT);
	DRM_DEBUG("waking\n");
	wake_up_interruptible(&dev->buf_readers);
	return 0;
}
