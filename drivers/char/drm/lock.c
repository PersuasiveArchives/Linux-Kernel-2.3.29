/* lock.c -- IOCTLs for locking -*- linux-c -*-
 * Created: Tue Feb  2 08:37:54 1999 by faith@precisioninsight.com
 * Revised: Fri Aug 20 09:27:01 1999 by faith@precisioninsight.com
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
 * $PI: xc/programs/Xserver/hw/xfree86/os-support/linux/drm/generic/lock.c,v 1.5 1999/08/30 13:05:00 faith Exp $
 * $XFree86$
 *
 */

#define __NO_VERSION__
#include "drmP.h"

int drm_block(struct inode *inode, struct file *filp, unsigned int cmd,
	      unsigned long arg)
{
	DRM_DEBUG("\n");
	return 0;
}

int drm_unblock(struct inode *inode, struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	DRM_DEBUG("\n");
	return 0;
}

int drm_lock_take(__volatile__ unsigned int *lock, unsigned int context)
{
	unsigned int old;
	unsigned int new;
	char	     failed;

	DRM_DEBUG("%d attempts\n", context);
	do {
		old = *lock;
		if (old & _DRM_LOCK_HELD) new = old | _DRM_LOCK_CONT;
		else			  new = context | _DRM_LOCK_HELD;
		_DRM_CAS(lock, old, new, failed);
	} while (failed);
	if (_DRM_LOCKING_CONTEXT(old) == context) {
		if (old & _DRM_LOCK_HELD) {
			if (context != DRM_KERNEL_CONTEXT) {
				DRM_ERROR("%d holds heavyweight lock\n",
					  context);
			}
			return 0;
		}
	}
	if (new == (context | _DRM_LOCK_HELD)) {
				/* Have lock */
		DRM_DEBUG("%d\n", context);
		return 1;
	}
	DRM_DEBUG("%d unable to get lock held by %d\n",
		  context, _DRM_LOCKING_CONTEXT(old));
	return 0;
}

/* This takes a lock forcibly and hands it to context.	Should ONLY be used
   inside *_unlock to give lock to kernel before calling *_dma_schedule. */
int drm_lock_transfer(__volatile__ unsigned int *lock, unsigned int context)
{
	unsigned int old;
	unsigned int new;
	char	     failed;

	do {
		old = *lock;
		new = context | _DRM_LOCK_HELD;
		_DRM_CAS(lock, old, new, failed);
	} while (failed);
	DRM_DEBUG("%d => %d\n", _DRM_LOCKING_CONTEXT(old), context);
	return 1;
}

int drm_lock_free(drm_device_t *dev,
		  __volatile__ unsigned int *lock, unsigned int context)
{
	unsigned int old;
	unsigned int new;
	char	     failed;

	DRM_DEBUG("%d\n", context);
	do {
		old = *lock;
		new = 0;
		_DRM_CAS(lock, old, new, failed);
	} while (failed);
	if (_DRM_LOCK_IS_HELD(old) && _DRM_LOCKING_CONTEXT(old) != context) {
		DRM_ERROR("%d freed heavyweight lock held by %d\n",
			  context,
			  _DRM_LOCKING_CONTEXT(old));
		return 1;
	}
	dev->lock.pid = 0;
	wake_up_interruptible(&dev->lock.lock_queue);
	return 0;
}

static int drm_flush_queue(drm_device_t *dev, int context)
{
	DECLARE_WAITQUEUE(entry, current);
	int		  ret	= 0;
	drm_queue_t	  *q	= dev->queuelist[context];
	
	DRM_DEBUG("\n");
	
	atomic_inc(&q->use_count);
	if (atomic_read(&q->use_count) > 1) {
		atomic_inc(&q->block_write);
		current->state = TASK_INTERRUPTIBLE;
		add_wait_queue(&q->flush_queue, &entry);
		atomic_inc(&q->block_count);
		for (;;) {
			if (!DRM_BUFCOUNT(&q->waitlist)) break;
			schedule();
			if (signal_pending(current)) {
				ret = -EINTR; /* Can't restart */
				break;
			}
		}
		atomic_dec(&q->block_count);
		current->state = TASK_RUNNING;
		remove_wait_queue(&q->flush_queue, &entry);
	}
	atomic_dec(&q->use_count);
	atomic_inc(&q->total_flushed);
		
				/* NOTE: block_write is still incremented!
				   Use drm_flush_unlock_queue to decrement. */
	return ret;
}

static int drm_flush_unblock_queue(drm_device_t *dev, int context)
{
	drm_queue_t	  *q	= dev->queuelist[context];
	
	DRM_DEBUG("\n");
	
	atomic_inc(&q->use_count);
	if (atomic_read(&q->use_count) > 1) {
		if (atomic_read(&q->block_write)) {
			atomic_dec(&q->block_write);
			wake_up_interruptible(&q->write_queue);
		}
	}
	atomic_dec(&q->use_count);
	return 0;
}

int drm_flush_block_and_flush(drm_device_t *dev, int context,
			      drm_lock_flags_t flags)
{
	int ret = 0;
	int i;
	
	DRM_DEBUG("\n");
	
	if (flags & _DRM_LOCK_FLUSH) {
		ret = drm_flush_queue(dev, DRM_KERNEL_CONTEXT);
		if (!ret) ret = drm_flush_queue(dev, context);
	}
	if (flags & _DRM_LOCK_FLUSH_ALL) {
		for (i = 0; !ret && i < dev->queue_count; i++) {
			ret = drm_flush_queue(dev, i);
		}
	}
	return ret;
}

int drm_flush_unblock(drm_device_t *dev, int context, drm_lock_flags_t flags)
{
	int ret = 0;
	int i;
	
	DRM_DEBUG("\n");
	
	if (flags & _DRM_LOCK_FLUSH) {
		ret = drm_flush_unblock_queue(dev, DRM_KERNEL_CONTEXT);
		if (!ret) ret = drm_flush_unblock_queue(dev, context);
	}
	if (flags & _DRM_LOCK_FLUSH_ALL) {
		for (i = 0; !ret && i < dev->queue_count; i++) {
			ret = drm_flush_unblock_queue(dev, i);
		}
	}
		
	return ret;
}

int drm_finish(struct inode *inode, struct file *filp, unsigned int cmd,
	       unsigned long arg)
{
	drm_file_t	  *priv	  = filp->private_data;
	drm_device_t	  *dev	  = priv->dev;
	int		  ret	  = 0;
	drm_lock_t	  lock;

	DRM_DEBUG("\n");

	copy_from_user_ret(&lock, (drm_lock_t *)arg, sizeof(lock), -EFAULT);
	ret = drm_flush_block_and_flush(dev, lock.context, lock.flags);
	drm_flush_unblock(dev, lock.context, lock.flags);
	return ret;
}
