/*
 * linux/drivers/char/ppdev.c
 *
 * This is the code behind /dev/parport* -- it allows a user-space
 * application to use the parport subsystem.
 *
 * Copyright (C) 1998-9 Tim Waugh <tim@cyberelk.demon.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * A /dev/parportx device node represents an arbitrary device
 * on port 'x'.  The following operations are possible:
 *
 * open		do nothing, set up default IEEE 1284 protocol to be COMPAT
 * close	release port and unregister device (if necessary)
 * ioctl
 *   EXCL	register device exclusively (may fail)
 *   CLAIM	(register device first time) parport_claim_or_block
 *   RELEASE	parport_release
 *   SETMODE	set the IEEE 1284 protocol to use for read/write
 *   SETPHASE	set the IEEE 1284 phase of a particular mode.  Not to be
 *              confused with ioctl(fd, SETPHASER, &stun). ;-)
 *   DATADIR	data_forward / data_reverse
 *   WDATA	write_data
 *   RDATA	read_data
 *   WCONTROL	write_control
 *   RCONTROL	read_control
 *   FCONTROL	frob_control
 *   RSTATUS	read_status
 *   NEGOT	parport_negotiate
 *   YIELD	parport_yield_blocking
 *   WCTLONIRQ	on interrupt, set control lines
 *   CLRIRQ	clear (and return) interrupt count
 *   SETTIME	sets device timeout (struct timeval)
 *   GETTIME    gets device timeout (struct timeval)
 * read/write	read or write in current IEEE 1284 protocol
 * select	wait for interrupt (in readfds)
 *
 * Added SETTIME/GETTIME ioctl, Fred Barnes 1999.
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/parport.h>
#include <linux/ctype.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#include "ppdev.h"

#define PP_VERSION "ppdev: user-space parallel port driver"
#define CHRDEV "ppdev"

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

struct pp_struct {
	struct pardevice * pdev;
	wait_queue_head_t irq_wait;
	atomic_t irqc;
	unsigned int flags;
	int irqresponse;
	unsigned char irqctl;
	struct ieee1284_info state;
	struct ieee1284_info saved_state;
};

/* pp_struct.flags bitfields */
#define PP_CLAIMED    (1<<0)
#define PP_EXCL       (1<<1)

/* Other constants */
#define PP_INTERRUPT_TIMEOUT (10 * HZ) /* 10s */
#define PP_BUFFER_SIZE 256
#define PARDEVICE_MAX 8

/* ROUND_UP macro from fs/select.c */
#define ROUND_UP(x,y) (((x)+(y)-1)/(y))

static inline void enable_irq (struct pp_struct *pp)
{
	struct parport *port = pp->pdev->port;
	port->ops->enable_irq (port);
}

static loff_t pp_lseek (struct file * file, long long offset, int origin)
{
	return -ESPIPE;
}

static ssize_t pp_read (struct file * file, char * buf, size_t count,
			loff_t * ppos)
{
	unsigned int minor = MINOR (file->f_dentry->d_inode->i_rdev);
	struct pp_struct *pp = file->private_data;
	char * kbuffer;
	ssize_t bytes_read = 0;
	ssize_t got = 0;

	if (!(pp->flags & PP_CLAIMED)) {
		/* Don't have the port claimed */
		printk (KERN_DEBUG CHRDEV "%x: claim the port first\n",
			minor);
		return -EINVAL;
	}

	kbuffer = kmalloc (min (count, PP_BUFFER_SIZE), GFP_KERNEL);
	if (!kbuffer)
		return -ENOMEM;

	while (bytes_read < count) {
		ssize_t need = min(count - bytes_read, PP_BUFFER_SIZE);

		got = parport_read (pp->pdev->port, kbuffer, need);

		if (got <= 0) {
			if (!bytes_read)
				bytes_read = got;

			break;
		}

		if (copy_to_user (buf + bytes_read, kbuffer, got)) {
			bytes_read = -EFAULT;
			break;
		}

		bytes_read += got;

		if (signal_pending (current)) {
			if (!bytes_read)
				bytes_read = -EINTR;
			break;
		}

		if (current->need_resched)
			schedule ();
	}

	kfree (kbuffer);
	enable_irq (pp);
	return bytes_read;
}

static ssize_t pp_write (struct file * file, const char * buf, size_t count,
			 loff_t * ppos)
{
	unsigned int minor = MINOR (file->f_dentry->d_inode->i_rdev);
	struct pp_struct *pp = file->private_data;
	char * kbuffer;
	ssize_t bytes_written = 0;
	ssize_t wrote;

	if (!(pp->flags & PP_CLAIMED)) {
		/* Don't have the port claimed */
		printk (KERN_DEBUG CHRDEV "%x: claim the port first\n",
			minor);
		return -EINVAL;
	}

	kbuffer = kmalloc (min (count, PP_BUFFER_SIZE), GFP_KERNEL);
	if (!kbuffer)
		return -ENOMEM;

	while (bytes_written < count) {
		ssize_t n = min(count - bytes_written, PP_BUFFER_SIZE);

		if (copy_from_user (kbuffer, buf + bytes_written, n)) {
			bytes_written = -EFAULT;
			break;
		}

		wrote = parport_write (pp->pdev->port, kbuffer, n);

		if (wrote < 0) {
			if (!bytes_written)
				bytes_written = wrote;
			break;
		}

		bytes_written += wrote;

		if (signal_pending (current)) {
			if (!bytes_written)
				bytes_written = -EINTR;
			break;
		}

		if (current->need_resched)
			schedule ();
	}

	kfree (kbuffer);
	enable_irq (pp);
	return bytes_written;
}

static void pp_irq (int irq, void * private, struct pt_regs * unused)
{
	struct pp_struct * pp = (struct pp_struct *) private;

	if (pp->irqresponse) {
		parport_write_control (pp->pdev->port, pp->irqctl);
		pp->irqresponse = 0;
	}

	atomic_inc (&pp->irqc);
	wake_up_interruptible (&pp->irq_wait);
}

static int register_device (int minor, struct pp_struct *pp)
{
	struct parport * port;
	struct pardevice * pdev = NULL;
	char *name;
	int fl;

	name = kmalloc (strlen (CHRDEV) + 3, GFP_KERNEL);
	if (name == NULL)
		return -ENOMEM;

	sprintf (name, CHRDEV "%x", minor);
	port = parport_enumerate (); /* FIXME: use attach/detach */

	while (port && port->number != minor)
		port = port->next;

	if (!port) {
		printk (KERN_WARNING "%s: no associated port!\n", name);
		kfree (name);
		return -ENXIO;
	}

	fl = (pp->flags & PP_EXCL) ? PARPORT_FLAG_EXCL : 0;
	pdev = parport_register_device (port, name, NULL, NULL, pp_irq, fl,
					pp);

	if (!pdev) {
		printk (KERN_WARNING "%s: failed to register device!\n", name);
		kfree (name);
		return -ENXIO;
	}

	pp->pdev = pdev;
	printk (KERN_DEBUG "%s: registered pardevice\n", name);
	return 0;
}

static enum ieee1284_phase init_phase (int mode)
{
	switch (mode & ~(IEEE1284_DEVICEID
			 | IEEE1284_ADDR)) {
	case IEEE1284_MODE_NIBBLE:
	case IEEE1284_MODE_BYTE:
		return IEEE1284_PH_REV_IDLE;
	}
	return IEEE1284_PH_FWD_IDLE;
}

static int pp_ioctl(struct inode *inode, struct file *file,
		    unsigned int cmd, unsigned long arg)
{
	unsigned int minor = MINOR(inode->i_rdev);
	struct pp_struct *pp = file->private_data;
	struct parport * port;

	/* First handle the cases that don't take arguments. */
	if (cmd == PPCLAIM) {
		struct ieee1284_info *info;

		if (pp->flags & PP_CLAIMED) {
			printk (KERN_DEBUG CHRDEV
				"%x: you've already got it!\n", minor);
			return -EINVAL;
		}

		/* Deferred device registration. */
		if (!pp->pdev) {
			int err = register_device (minor, pp);
			if (err)
				return err;
		}

		parport_claim_or_block (pp->pdev);
		pp->flags |= PP_CLAIMED;

		/* For interrupt-reporting to work, we need to be
		 * informed of each interrupt. */
		enable_irq (pp);

		/* We may need to fix up the state machine. */
		info = &pp->pdev->port->ieee1284;
		pp->saved_state.mode = info->mode;
		pp->saved_state.phase = info->phase;
		info->mode = pp->state.mode;
		info->phase = pp->state.phase;

		return 0;
	}

	if (cmd == PPEXCL) {
		if (pp->pdev) {
			printk (KERN_DEBUG CHRDEV "%x: too late for PPEXCL; "
				"already registered\n", minor);
			if (pp->flags & PP_EXCL)
				/* But it's not really an error. */
				return 0;
			/* There's no chance of making the driver happy. */
			return -EINVAL;
		}

		/* Just remember to register the device exclusively
		 * when we finally do the registration. */
		pp->flags |= PP_EXCL;
		return 0;
	}

	if (cmd == PPSETMODE) {
		int mode;
		if (copy_from_user (&mode, (int *) arg, sizeof (mode)))
			return -EFAULT;
		/* FIXME: validate mode */
		pp->state.mode = mode;
		pp->state.phase = init_phase (mode);

		if (pp->flags & PP_CLAIMED) {
			pp->pdev->port->ieee1284.mode = mode;
			pp->pdev->port->ieee1284.phase = pp->state.phase;
		}

		return 0;
	}

	if (cmd == PPSETPHASE) {
		int phase;
		if (copy_from_user (&phase, (int *) arg, sizeof (phase)))
			return -EFAULT;
		/* FIXME: validate phase */
		pp->state.phase = phase;

		if (pp->flags & PP_CLAIMED)
			pp->pdev->port->ieee1284.phase = phase;

		return 0;
	}

	/* Everything else requires the port to be claimed, so check
	 * that now. */
	if ((pp->flags & PP_CLAIMED) == 0) {
		printk (KERN_DEBUG CHRDEV "%x: claim the port first\n",
			minor);
		return -EINVAL;
	}

	port = pp->pdev->port;
	switch (cmd) {
		struct ieee1284_info *info;
		unsigned char reg;
		unsigned char mask;
		int mode;
		int ret;
		struct timeval par_timeout;
		long to_jiffies;

	case PPRSTATUS:
		reg = parport_read_status (port);
		return copy_to_user ((unsigned char *) arg, &reg,
				     sizeof (reg));

	case PPRDATA:
		reg = parport_read_data (port);
		return copy_to_user ((unsigned char *) arg, &reg,
				     sizeof (reg));

	case PPRCONTROL:
		reg = parport_read_control (port);
		return copy_to_user ((unsigned char *) arg, &reg,
				     sizeof (reg));

	case PPYIELD:
		parport_yield_blocking (pp->pdev);
		return 0;

	case PPRELEASE:
		/* Save the state machine's state. */
		info = &pp->pdev->port->ieee1284;
		pp->state.mode = info->mode;
		pp->state.phase = info->phase;
		info->mode = pp->saved_state.mode;
		info->phase = pp->saved_state.phase;
		parport_release (pp->pdev);
		pp->flags &= ~PP_CLAIMED;
		return 0;

	case PPWCONTROL:
		if (copy_from_user (&reg, (unsigned char *) arg, sizeof (reg)))
			return -EFAULT;
		parport_write_control (port, reg);
		return 0;

	case PPWDATA:
		if (copy_from_user (&reg, (unsigned char *) arg, sizeof (reg)))
			return -EFAULT;
		parport_write_data (port, reg);
		return 0;

	case PPFCONTROL:
		if (copy_from_user (&mask, (unsigned char *) arg,
				    sizeof (mask)))
			return -EFAULT;
		if (copy_from_user (&reg, 1 + (unsigned char *) arg,
				    sizeof (reg)))
			return -EFAULT;
		parport_frob_control (port, mask, reg);
		return 0;

	case PPDATADIR:
		if (copy_from_user (&mode, (int *) arg, sizeof (mode)))
			return -EFAULT;
		if (mode)
			port->ops->data_reverse (port);
		else
			port->ops->data_forward (port);
		return 0;

	case PPNEGOT:
		if (copy_from_user (&mode, (int *) arg, sizeof (mode)))
			return -EFAULT;
		switch ((ret = parport_negotiate (port, mode))) {
		case 0: break;
		case -1: /* handshake failed, peripheral not IEEE 1284 */
			ret = -EIO;
			break;
		case 1:  /* handshake succeeded, peripheral rejected mode */
			ret = -ENXIO;
			break;
		}
		enable_irq (pp);
		return ret;

	case PPWCTLONIRQ:
		if (copy_from_user (&reg, (unsigned char *) arg,
				    sizeof (reg)))
			return -EFAULT;

		/* Remember what to set the control lines to, for next
		 * time we get an interrupt. */
		pp->irqctl = reg;
		pp->irqresponse = 1;
		return 0;

	case PPCLRIRQ:
		ret = atomic_read (&pp->irqc);
		if (copy_to_user ((int *) arg, &ret, sizeof (ret)))
			return -EFAULT;
		atomic_sub (ret, &pp->irqc);
		return 0;

	case PPSETTIME:
		if (copy_from_user (&par_timeout, (struct timeval *)arg,
				    sizeof(struct timeval))) {
			return -EFAULT;
		}
		/* Convert to jiffies, place in pp->pdev->timeout */
		if ((par_timeout.tv_sec < 0) || (par_timeout.tv_usec < 0)) {
			return -EINVAL;
		}
		to_jiffies = ROUND_UP(par_timeout.tv_usec, 1000000/HZ);
		to_jiffies += par_timeout.tv_sec * (long)HZ;
		if (to_jiffies <= 0) {
			return -EINVAL;
		}
		pp->pdev->timeout = to_jiffies;
		return 0;

	case PPGETTIME:
		to_jiffies = pp->pdev->timeout;
		par_timeout.tv_sec = to_jiffies / HZ;
		par_timeout.tv_usec = (to_jiffies % (long)HZ) * (1000000/HZ);
		if (copy_to_user ((struct timeval *)arg, &par_timeout,
				  sizeof(struct timeval))) {
			return -EFAULT;
		}
		return 0;

	default:
		printk (KERN_DEBUG CHRDEV "%x: What? (cmd=0x%x)\n", minor,
			cmd);
		return -EINVAL;
	}

	/* Keep the compiler happy */
	return 0;
}

static int pp_open (struct inode * inode, struct file * file)
{
	unsigned int minor = MINOR (inode->i_rdev);
	struct pp_struct *pp;

	if (minor >= PARPORT_MAX)
		return -ENXIO;

	pp = kmalloc (sizeof (struct pp_struct), GFP_KERNEL);
	if (!pp)
		return -ENOMEM;

	pp->state.mode = IEEE1284_MODE_COMPAT;
	pp->state.phase = init_phase (pp->state.mode);
	pp->flags = 0;
	pp->irqresponse = 0;
	atomic_set (&pp->irqc, 0);
	init_waitqueue_head (&pp->irq_wait);

	/* Defer the actual device registration until the first claim.
	 * That way, we know whether or not the driver wants to have
	 * exclusive access to the port (PPEXCL).
	 */
	pp->pdev = NULL;
	file->private_data = pp;

	MOD_INC_USE_COUNT;
	return 0;
}

static int pp_release (struct inode * inode, struct file * file)
{
	unsigned int minor = MINOR (inode->i_rdev);
	struct pp_struct *pp = file->private_data;

	if (pp->flags & PP_CLAIMED) {
		parport_release (pp->pdev);
		printk (KERN_DEBUG CHRDEV "%x: released pardevice because "
			"user-space forgot\n", minor);
	}

	if (pp->pdev) {
		const char *name = pp->pdev->name;
		parport_unregister_device (pp->pdev);
		kfree (name);
		pp->pdev = NULL;
		printk (KERN_DEBUG CHRDEV "%x: unregistered pardevice\n",
			minor);
	}

	kfree (pp);

	MOD_DEC_USE_COUNT;
	return 0;
}

static unsigned int pp_poll (struct file * file, poll_table * wait)
{
	struct pp_struct *pp = file->private_data;
	unsigned int mask = 0;

	if (atomic_read (&pp->irqc))
		mask |= POLLIN | POLLRDNORM;

	poll_wait (file, &pp->irq_wait, wait);
	return mask;
}

static struct file_operations pp_fops = {
	pp_lseek,
	pp_read,
	pp_write,
	NULL,	/* pp_readdir */
	pp_poll,
	pp_ioctl,
	NULL,	/* pp_mmap */
	pp_open,
	NULL,   /* pp_flush */
	pp_release
};

#ifdef MODULE
#define pp_init init_module
#endif

int pp_init (void)
{
	if (register_chrdev (PP_MAJOR, CHRDEV, &pp_fops)) {
		printk (KERN_WARNING CHRDEV ": unable to get major %d\n",
			PP_MAJOR);
		return -EIO;
	}

	printk (KERN_INFO PP_VERSION "\n");
	return 0;
}

#ifdef MODULE
void cleanup_module (void)
{
	/* Clean up all parport stuff */
	unregister_chrdev (PP_MAJOR, CHRDEV);
}
#endif /* MODULE */
