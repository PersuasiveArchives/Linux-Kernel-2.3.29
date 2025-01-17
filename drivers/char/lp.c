/*
 * Generic parallel printer driver
 *
 * Copyright (C) 1992 by Jim Weigand and Linus Torvalds
 * Copyright (C) 1992,1993 by Michael K. Johnson
 * - Thanks much to Gunter Windau for pointing out to me where the error
 *   checking ought to be.
 * Copyright (C) 1993 by Nigel Gamble (added interrupt code)
 * Copyright (C) 1994 by Alan Cox (Modularised it)
 * LPCAREFUL, LPABORT, LPGETSTATUS added by Chris Metcalf, metcalf@lcs.mit.edu
 * Statistics and support for slow printers by Rob Janssen, rob@knoware.nl
 * "lp=" command line parameters added by Grant Guenther, grant@torque.net
 * lp_read (Status readback) support added by Carsten Gross,
 *                                             carsten@sol.wohnheim.uni-ulm.de
 * Support for parport by Philip Blundell <Philip.Blundell@pobox.com>
 * Parport sharing hacking by Andrea Arcangeli
 * Fixed kernel_(to/from)_user memory copy to check for errors
 * 				by Riccardo Facchetti <fizban@tin.it>
 * Redesigned interrupt handling for handle printers with buggy handshake
 *				by Andrea Arcangeli, 11 May 1998
 * Full efficient handling of printer with buggy irq handshake (now I have
 * understood the meaning of the strange handshake). This is done sending new
 * characters if the interrupt is just happened, even if the printer say to
 * be still BUSY. This is needed at least with Epson Stylus Color. To enable
 * the new TRUST_IRQ mode read the `LP OPTIMIZATION' section below...
 * Fixed the irq on the rising edge of the strobe case.
 * Obsoleted the CAREFUL flag since a printer that doesn' t work with
 * CAREFUL will block a bit after in lp_check_status().
 *				Andrea Arcangeli, 15 Oct 1998
 * Obsoleted and removed all the lowlevel stuff implemented in the last
 * month to use the IEEE1284 functions (that handle the _new_ compatibilty
 * mode fine).
 */

/* This driver should, in theory, work with any parallel port that has an
 * appropriate low-level driver; all I/O is done through the parport
 * abstraction layer.
 *
 * If this driver is built into the kernel, you can configure it using the
 * kernel command-line.  For example:
 *
 *	lp=parport1,none,parport2	(bind lp0 to parport1, disable lp1 and
 *					 bind lp2 to parport2)
 *
 *	lp=auto				(assign lp devices to all ports that
 *				         have printers attached, as determined
 *					 by the IEEE-1284 autoprobe)
 * 
 *	lp=reset			(reset the printer during 
 *					 initialisation)
 *
 *	lp=off				(disable the printer driver entirely)
 *
 * If the driver is loaded as a module, similar functionality is available
 * using module parameters.  The equivalent of the above commands would be:
 *
 *	# insmod lp.o parport=1,none,2
 *
 *	# insmod lp.o parport=auto
 *
 *	# insmod lp.o reset=1
 */

/* COMPATIBILITY WITH OLD KERNELS
 *
 * Under Linux 2.0 and previous versions, lp devices were bound to ports at
 * particular I/O addresses, as follows:
 *
 *	lp0		0x3bc
 *	lp1		0x378
 *	lp2		0x278
 *
 * The new driver, by default, binds lp devices to parport devices as it
 * finds them.  This means that if you only have one port, it will be bound
 * to lp0 regardless of its I/O address.  If you need the old behaviour, you
 * can force it using the parameters described above.
 */

/*
 * The new interrupt handling code take care of the buggy handshake
 * of some HP and Epson printer:
 * ___
 * ACK    _______________    ___________
 *                       |__|
 * ____
 * BUSY   _________              _______
 *                 |____________|
 *
 * I discovered this using the printer scanner that you can find at:
 *
 *	ftp://e-mind.com/pub/linux/pscan/
 *
 *					11 May 98, Andrea Arcangeli
 *
 * My printer scanner run on an Epson Stylus Color show that such printer
 * generates the irq on the _rising_ edge of the STROBE. Now lp handle
 * this case fine too.
 *
 *					15 Oct 1998, Andrea Arcangeli
 *
 * The so called `buggy' handshake is really the well documented
 * compatibility mode IEEE1284 handshake. They changed the well known
 * Centronics handshake acking in the middle of busy expecting to not
 * break drivers or legacy application, while they broken linux lp
 * until I fixed it reverse engineering the protocol by hand some
 * month ago...
 *
 *                                     14 Dec 1998, Andrea Arcangeli
 */

#include <linux/module.h>
#include <linux/init.h>

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/malloc.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/console.h>

#include <linux/parport.h>
#undef LP_STATS
#include <linux/lp.h>

#include <asm/irq.h>
#include <asm/uaccess.h>
#include <asm/system.h>

/* if you have more than 3 printers, remember to increase LP_NO */
#define LP_NO 3

struct lp_struct lp_table[LP_NO];

static unsigned int lp_count = 0;

/* Test if printer is ready */
#define	LP_READY(status)	((status) & LP_PBUSY)
/* Test if the printer is not acking the strobe */
#define	LP_NO_ACKING(status)	((status) & LP_PACK)
/* Test if the printer has error conditions */
#define LP_NO_ERROR(status)	((status) & LP_PERRORP)

#undef LP_DEBUG

/* If you want to see if you can get lp_poll working, define this. */
#undef SUPPORT_POLL

/* --- parport support ----------------------------------------- */

static int lp_preempt(void *handle)
{
       struct lp_struct *lps = (struct lp_struct *)handle;

       if (!(lps->flags & LP_PORT_BUSY)) {
	       /* Let the port go. */
	       clear_bit (LP_HAVE_PORT_BIT, &lps->flags);
	       return 0;
       }

       if (!(lps->flags & LP_PORT_BUSY)) {
	       /* Let the port go. */
	       clear_bit (LP_HAVE_PORT_BIT, &lps->flags);
	       return 0;
       }

       /* Don't actually release the port now */
       return 1;
}

static void lp_check_data (struct lp_struct *lp)
{
#if !defined(CONFIG_PARPORT_1284) || !defined (SUPPORT_POLL)
	return;
#else
	struct pardevice *dev = lp->dev;
	if (!(lp->flags & LP_NO_REVERSE)) {
		int err = parport_negotiate (dev->port, IEEE1284_MODE_NIBBLE);
		if (err)
			lp->flags |= LP_NO_REVERSE;
		else {
			unsigned char s = parport_read_status (dev->port);
			if (s & PARPORT_STATUS_ERROR)
				lp->flags &= ~LP_DATA_AVAIL;
			else {
				lp->flags |= LP_DATA_AVAIL;
				if (waitqueue_active (&lp->dataq))
					wake_up_interruptible (&lp->dataq);
			}
		}
	}
#endif /* IEEE 1284 support */
}

static void lp_parport_release (int minor)
{
	lp_check_data (&lp_table[minor]);
	if (test_and_clear_bit (LP_HAVE_PORT_BIT, &lp_table[minor].flags))
		parport_release (lp_table[minor].dev);

	lp_table[minor].flags &= ~LP_PORT_BUSY;
}

static void lp_parport_claim (int minor)
{
	if (!test_and_set_bit (LP_HAVE_PORT_BIT, &lp_table[minor].flags))
		parport_claim_or_block (lp_table[minor].dev);

	lp_table[minor].flags |= LP_PORT_BUSY;
}

/* --- low-level port access ----------------------------------- */

#define r_dtr(x)	(parport_read_data(lp_table[(x)].dev->port))
#define r_str(x)	(parport_read_status(lp_table[(x)].dev->port))
#define w_ctr(x,y)	do { parport_write_control(lp_table[(x)].dev->port, (y)); } while (0)
#define w_dtr(x,y)	do { parport_write_data(lp_table[(x)].dev->port, (y)); } while (0)

static int lp_reset(int minor)
{
	int retval;
	lp_parport_claim (minor);
	w_ctr(minor, LP_PSELECP);
	udelay (LP_DELAY);
	w_ctr(minor, LP_PSELECP | LP_PINITP);
	retval = r_str(minor);
	lp_parport_release (minor);
	return retval;
}

static void lp_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct lp_struct *lp_dev = (struct lp_struct *) dev_id;
	if (!(lp_dev->flags & LP_PORT_BUSY))
		/* We must have the port since we got an interrupt. */
		lp_check_data (lp_dev);
	if (waitqueue_active (&lp_dev->waitq))
		wake_up_interruptible (&lp_dev->waitq);
}

static void lp_wakeup (void *handle)
{
	struct lp_struct *lp_dev = handle;

	if (lp_dev->flags & LP_PORT_BUSY)
		return;

	/* Grab the port if it can help (i.e. reverse mode is possible). */
	if (!(lp_dev->flags & LP_NO_REVERSE)) {
		parport_claim (lp_dev->dev);
		set_bit (LP_HAVE_PORT_BIT, &lp_dev->flags);
		lp_check_data (lp_dev);
		if (waitqueue_active (&lp_dev->waitq))
			wake_up_interruptible (&lp_dev->waitq);
	}
}

static void lp_error (int minor)
{
	int polling;

	if (LP_F(minor) & LP_ABORT)
		return;

	polling = lp_table[minor].dev->port->irq == PARPORT_IRQ_NONE;
	if (polling) lp_parport_release (minor);
	interruptible_sleep_on_timeout (&lp_table[minor].waitq,
					LP_TIMEOUT_POLLED);
	if (polling) lp_parport_claim (minor);
	else parport_yield_blocking (lp_table[minor].dev);
}

static int lp_check_status(int minor)
{
	int error = 0;
	unsigned int last = lp_table[minor].last_error;
	unsigned char status = r_str(minor);
	if (status & LP_PERRORP)
		/* No error. */
		last = 0;
	else if ((status & LP_POUTPA)) {
		if (last != LP_POUTPA) {
			last = LP_POUTPA;
			printk(KERN_INFO "lp%d out of paper\n", minor);
		}
		error = -ENOSPC;
	} else if (!(status & LP_PSELECD)) {
		if (last != LP_PSELECD) {
			last = LP_PSELECD;
			printk(KERN_INFO "lp%d off-line\n", minor);
		}
		error = -EIO;
	} else {
		if (last != LP_PERRORP) {
			last = LP_PERRORP;
			printk(KERN_INFO "lp%d on fire\n", minor);
		}
		error = -EIO;
	}

	lp_table[minor].last_error = last;

	if (last != 0)
		lp_error(minor);

	return error;
}

static ssize_t lp_write(struct file * file, const char * buf,
		        size_t count, loff_t *ppos)
{
	unsigned int minor = MINOR(file->f_dentry->d_inode->i_rdev);
	struct parport *port = lp_table[minor].dev->port;
	char *kbuf = lp_table[minor].lp_buffer;
	ssize_t retv = 0;
	ssize_t written;
	size_t copy_size = count;

#ifdef LP_STATS
	if (jiffies-lp_table[minor].lastcall > LP_TIME(minor))
		lp_table[minor].runchars = 0;

	lp_table[minor].lastcall = jiffies;
#endif

	/* Need to copy the data from user-space. */
	if (copy_size > LP_BUFFER_SIZE)
		copy_size = LP_BUFFER_SIZE;

	if (copy_from_user (kbuf, buf, copy_size))
		return -EFAULT;

	if (down_interruptible (&lp_table[minor].port_mutex))
		return -EINTR;

 	/* Claim Parport or sleep until it becomes available
 	 */
 	lp_parport_claim (minor);

	/* Go to compatibility mode. */
	parport_negotiate (port, IEEE1284_MODE_COMPAT);

	do {
		/* Write the data. */
		written = parport_write (port, kbuf, copy_size);
		if (written >= 0) {
			copy_size -= written;
			count -= written;
			buf  += written;
			retv += written;
		}

		if (signal_pending (current)) {
			if (retv == 0)
				retv = -EINTR;

			break;
		}

		if (copy_size > 0) {
			/* incomplete write -> check error ! */
			int error = lp_check_status (minor);

			if (LP_F(minor) & LP_ABORT) {
				if (retv == 0)
					retv = error;
				break;
			}

			parport_yield_blocking (lp_table[minor].dev);
		} else if (current->need_resched)
			schedule ();

		if (count) {
			copy_size = count;
			if (copy_size > LP_BUFFER_SIZE)
				copy_size = LP_BUFFER_SIZE;

			if (copy_from_user(kbuf, buf, copy_size)) {
				if (retv == 0)
					retv = -EFAULT;
				break;
			}
		}	
	} while (count > 0);

 	lp_parport_release (minor);

	up (&lp_table[minor].port_mutex);

 	return retv;
}

static long long lp_lseek(struct file * file, long long offset, int origin)
{
	return -ESPIPE;
}

#ifdef CONFIG_PARPORT_1284

/* Status readback conforming to ieee1284 */
static ssize_t lp_read(struct file * file, char * buf,
		       size_t count, loff_t *ppos)
{
	unsigned int minor=MINOR(file->f_dentry->d_inode->i_rdev);
	struct parport *port = lp_table[minor].dev->port;
	ssize_t retval = 0;
	char *kbuf = lp_table[minor].lp_buffer;

	if (count > LP_BUFFER_SIZE)
		count = LP_BUFFER_SIZE;

	if (down_interruptible (&lp_table[minor].port_mutex))
		return -EINTR;

	lp_parport_claim (minor);

	for (;;) {
		retval = parport_read (port, kbuf, count);

		if (retval)
			break;

		if (file->f_flags & O_NONBLOCK)
			break;

		/* Wait for an interrupt. */
		interruptible_sleep_on_timeout (&lp_table[minor].waitq,
						LP_TIMEOUT_POLLED);

		if (signal_pending (current)) {
			retval = -EINTR;
			break;
		}
	}

	lp_parport_release (minor);

	if (retval > 0 && copy_to_user (buf, kbuf, retval))
		retval = -EFAULT;

	up (&lp_table[minor].port_mutex);

	return retval;
}

#endif /* IEEE 1284 support */

static int lp_open(struct inode * inode, struct file * file)
{
	unsigned int minor = MINOR(inode->i_rdev);

	if (minor >= LP_NO)
		return -ENXIO;
	if ((LP_F(minor) & LP_EXIST) == 0)
		return -ENXIO;
	if (test_and_set_bit(LP_BUSY_BIT_POS, &LP_F(minor)))
		return -EBUSY;

	MOD_INC_USE_COUNT;

	/* If ABORTOPEN is set and the printer is offline or out of paper,
	   we may still want to open it to perform ioctl()s.  Therefore we
	   have commandeered O_NONBLOCK, even though it is being used in
	   a non-standard manner.  This is strictly a Linux hack, and
	   should most likely only ever be used by the tunelp application. */
	if ((LP_F(minor) & LP_ABORTOPEN) && !(file->f_flags & O_NONBLOCK)) {
		int status;
		lp_parport_claim (minor);
		status = r_str(minor);
		lp_parport_release (minor);
		if (status & LP_POUTPA) {
			printk(KERN_INFO "lp%d out of paper\n", minor);
			MOD_DEC_USE_COUNT;
			LP_F(minor) &= ~LP_BUSY;
			return -ENOSPC;
		} else if (!(status & LP_PSELECD)) {
			printk(KERN_INFO "lp%d off-line\n", minor);
			MOD_DEC_USE_COUNT;
			LP_F(minor) &= ~LP_BUSY;
			return -EIO;
		} else if (!(status & LP_PERRORP)) {
			printk(KERN_ERR "lp%d printer error\n", minor);
			MOD_DEC_USE_COUNT;
			LP_F(minor) &= ~LP_BUSY;
			return -EIO;
		}
	}
	lp_table[minor].lp_buffer = (char *) kmalloc(LP_BUFFER_SIZE, GFP_KERNEL);
	if (!lp_table[minor].lp_buffer) {
		MOD_DEC_USE_COUNT;
		LP_F(minor) &= ~LP_BUSY;
		return -ENOMEM;
	}
	return 0;
}

static int lp_release(struct inode * inode, struct file * file)
{
	unsigned int minor = MINOR(inode->i_rdev);

	kfree_s(lp_table[minor].lp_buffer, LP_BUFFER_SIZE);
	lp_table[minor].lp_buffer = NULL;
	MOD_DEC_USE_COUNT;
	LP_F(minor) &= ~LP_BUSY;
	return 0;
}

static int lp_ioctl(struct inode *inode, struct file *file,
		    unsigned int cmd, unsigned long arg)
{
	unsigned int minor = MINOR(inode->i_rdev);
	int status;
	int retval = 0;

#ifdef LP_DEBUG
	printk(KERN_DEBUG "lp%d ioctl, cmd: 0x%x, arg: 0x%lx\n", minor, cmd, arg);
#endif
	if (minor >= LP_NO)
		return -ENODEV;
	if ((LP_F(minor) & LP_EXIST) == 0)
		return -ENODEV;
	switch ( cmd ) {
		case LPTIME:
			LP_TIME(minor) = arg * HZ/100;
			break;
		case LPCHAR:
			LP_CHAR(minor) = arg;
			break;
		case LPABORT:
			if (arg)
				LP_F(minor) |= LP_ABORT;
			else
				LP_F(minor) &= ~LP_ABORT;
			break;
		case LPABORTOPEN:
			if (arg)
				LP_F(minor) |= LP_ABORTOPEN;
			else
				LP_F(minor) &= ~LP_ABORTOPEN;
			break;
#ifdef OBSOLETED
		case LPCAREFUL:
			if (arg)
				LP_F(minor) |= LP_CAREFUL;
			else
				LP_F(minor) &= ~LP_CAREFUL;
			break;
#endif
		case LPWAIT:
			LP_WAIT(minor) = arg;
			break;
		case LPSETIRQ: 
			return -EINVAL;
			break;
		case LPGETIRQ:
			if (copy_to_user((int *) arg, &LP_IRQ(minor),
					sizeof(int)))
				return -EFAULT;
			break;
		case LPGETSTATUS:
			lp_parport_claim(minor);
			status = r_str(minor);
			lp_parport_release(minor);

			if (copy_to_user((int *) arg, &status, sizeof(int)))
				return -EFAULT;
			break;
		case LPRESET:
			lp_reset(minor);
			break;
#ifdef LP_STATS
		case LPGETSTATS:
			if (copy_to_user((int *) arg, &LP_STAT(minor),
					sizeof(struct lp_stats)))
				return -EFAULT;
			if (suser())
				memset(&LP_STAT(minor), 0,
						sizeof(struct lp_stats));
			break;
#endif
 		case LPGETFLAGS:
 			status = LP_F(minor);
			if (copy_to_user((int *) arg, &status, sizeof(int)))
				return -EFAULT;
			break;
		default:
			retval = -EINVAL;
	}
	return retval;
}

#ifdef CONFIG_PARPORT_1284
static unsigned int lp_poll (struct file *filp, struct poll_table_struct *wait)
{
	unsigned int minor = MINOR (filp->f_dentry->d_inode->i_rdev);
	unsigned int mask = POLLOUT | POLLWRNORM; /* always writable */

	poll_wait (filp, &lp_table[minor].dataq, wait);

	if (lp_table[minor].flags & LP_DATA_AVAIL)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}
#endif /* IEEE 1284 support */

static struct file_operations lp_fops = {
	lp_lseek,
#ifdef CONFIG_PARPORT_1284
	lp_read,
#else
	NULL,
#endif
	lp_write,
	NULL,		/* lp_readdir */
#ifdef CONFIG_PARPORT_1284
	lp_poll,
#else
	NULL,
#endif
	lp_ioctl,
	NULL,		/* lp_mmap */
	lp_open,
	NULL,		/* flush */
	lp_release
};

/* --- support for console on the line printer ----------------- */

#ifdef CONFIG_LP_CONSOLE

#define CONSOLE_LP 0

/* If the printer is out of paper, we can either lose the messages or
 * stall until the printer is happy again.  Define CONSOLE_LP_STRICT
 * non-zero to get the latter behaviour. */
#define CONSOLE_LP_STRICT 1

static void lp_console_write (struct console *co, const char *s,
			      unsigned count)
{
	struct pardevice *dev = lp_table[CONSOLE_LP].dev;
	struct parport *port = dev->port;
	ssize_t written;
	signed long old_to;

	if (!(lp_table[CONSOLE_LP].flags & (1<<LP_HAVE_PORT_BIT))) {
		if (parport_claim (dev))
			/* Nothing we can do. */
			return;
		set_bit (LP_HAVE_PORT_BIT, &lp_table[CONSOLE_LP].flags);
	}

	old_to = parport_set_timeout (dev, 0);

	/* Go to compatibility mode. */
	parport_negotiate (port, IEEE1284_MODE_COMPAT);

	do {
		/* Write the data. */
		written = parport_write (port, s, count);
		if (written > 0) {
			s += written;
			count -= written;
		}
	} while (count > 0 && (CONSOLE_LP_STRICT || written > 0));

	parport_set_timeout (dev, old_to);
}

static kdev_t lp_console_device (struct console *c)
{
	return MKDEV(LP_MAJOR, CONSOLE_LP);
}

static struct console lpcons = {
	"lp",
	lp_console_write,
	NULL,
	lp_console_device,
	NULL,
	NULL,
	NULL,
	CON_PRINTBUFFER,
	-1,
	0,
	NULL
};

#endif /* console on line printer */

/* --- initialisation code ------------------------------------- */

#ifdef MODULE

static int parport_nr[LP_NO] = { [0 ... LP_NO-1] = LP_PARPORT_UNSPEC };
static char *parport[LP_NO] = { NULL,  };
static int reset = 0;

MODULE_PARM(parport, "1-" __MODULE_STRING(LP_NO) "s");
MODULE_PARM(reset, "i");

#else

static int parport_nr[LP_NO] = { [0 ... LP_NO-1] = LP_PARPORT_UNSPEC };
static int reset = 0;

static int parport_ptr = 0;

void __init lp_setup(char *str, int *ints)
{
	if (!str) {
		if (ints[0] == 0 || ints[1] == 0) {
			/* disable driver on "lp=" or "lp=0" */
			parport_nr[0] = LP_PARPORT_OFF;
		} else {
			printk(KERN_WARNING "warning: 'lp=0x%x' is deprecated, ignored\n", ints[1]);
		}
	} else if (!strncmp(str, "parport", 7)) {
		int n = simple_strtoul(str+7, NULL, 10);
		if (parport_ptr < LP_NO)
			parport_nr[parport_ptr++] = n;
		else
			printk(KERN_INFO "lp: too many ports, %s ignored.\n",
			       str);
	} else if (!strcmp(str, "auto")) {
		parport_nr[0] = LP_PARPORT_AUTO;
	} else if (!strcmp(str, "none")) {
		parport_nr[parport_ptr++] = LP_PARPORT_NONE;
	} else if (!strcmp(str, "reset")) {
		reset = 1;
	}
}

#endif

static int lp_register(int nr, struct parport *port)
{
	lp_table[nr].dev = parport_register_device(port, "lp", 
						   lp_preempt, lp_wakeup,
						   lp_interrupt, 
						   0,
						   (void *) &lp_table[nr]);
	if (lp_table[nr].dev == NULL)
		return 1;
	lp_table[nr].flags |= LP_EXIST;

	if (reset)
		lp_reset(nr);

	printk(KERN_INFO "lp%d: using %s (%s).\n", nr, port->name, 
	       (port->irq == PARPORT_IRQ_NONE)?"polling":"interrupt-driven");

#ifdef CONFIG_LP_CONSOLE
	if (!nr) {
		if (port->modes & PARPORT_MODE_SAFEININT) {
			register_console (&lpcons);
			printk (KERN_INFO "lp%d: console ready\n", CONSOLE_LP);
		} else
			printk (KERN_ERR "lp%d: cannot run console on %s\n",
				CONSOLE_LP, port->name);
	}
#endif

	return 0;
}

static void lp_attach (struct parport *port)
{
	unsigned int i;

	switch (parport_nr[0])
	{
	case LP_PARPORT_UNSPEC:
	case LP_PARPORT_AUTO:
		if (parport_nr[0] == LP_PARPORT_AUTO &&
		    port->probe_info[0].class != PARPORT_CLASS_PRINTER)
			return;

		if (!lp_register(lp_count, port))
			if (++lp_count == LP_NO)
				break;

		break;

	default:
		for (i = 0; i < LP_NO; i++) {
			if (port->number == parport_nr[i]) {
				if (!lp_register(i, port))
					lp_count++;
				break;
			}
		}
		break;
	}
}

static void lp_detach (struct parport *port)
{
	/* Write this some day. */
}

static struct parport_driver lp_driver = {
	"lp",
	lp_attach,
	lp_detach,
	NULL
};

int __init lp_init (void)
{
	int i;

	if (parport_nr[0] == LP_PARPORT_OFF)
		return 0;

	for (i = 0; i < LP_NO; i++) {
		lp_table[i].dev = NULL;
		lp_table[i].flags = 0;
		lp_table[i].chars = LP_INIT_CHAR;
		lp_table[i].time = LP_INIT_TIME;
		lp_table[i].wait = LP_INIT_WAIT;
		lp_table[i].lp_buffer = NULL;
#ifdef LP_STATS
		lp_table[i].lastcall = 0;
		lp_table[i].runchars = 0;
		memset (&lp_table[i].stats, 0, sizeof (struct lp_stats));
#endif
		lp_table[i].last_error = 0;
		init_waitqueue_head (&lp_table[i].waitq);
		init_waitqueue_head (&lp_table[i].dataq);
		init_MUTEX (&lp_table[i].port_mutex);
	}

	if (register_chrdev (LP_MAJOR, "lp", &lp_fops)) {
		printk ("lp: unable to get major %d\n", LP_MAJOR);
		return -EIO;
	}

	if (parport_register_driver (&lp_driver)) {
		printk ("lp: unable to register with parport\n");
		return -EIO;
	}

	if (!lp_count) {
		printk (KERN_INFO "lp: driver loaded but no devices found\n");
#ifndef CONFIG_PARPORT_12843
		if (parport_nr[0] == LP_PARPORT_AUTO)
			printk (KERN_INFO "lp: (is IEEE 1284.3 support enabled?)\n");
#endif
	}

	return 0;
}

#ifdef MODULE
int init_module(void)
{
	if (parport[0]) {
		/* The user gave some parameters.  Let's see what they were.  */
		if (!strncmp(parport[0], "auto", 4))
			parport_nr[0] = LP_PARPORT_AUTO;
		else {
			int n;
			for (n = 0; n < LP_NO && parport[n]; n++) {
				if (!strncmp(parport[n], "none", 4))
					parport_nr[n] = LP_PARPORT_NONE;
				else {
					char *ep;
					unsigned long r = simple_strtoul(parport[n], &ep, 0);
					if (ep != parport[n]) 
						parport_nr[n] = r;
					else {
						printk(KERN_ERR "lp: bad port specifier `%s'\n", parport[n]);
						return -ENODEV;
					}
				}
			}
		}
	}

	return lp_init();
}

void cleanup_module(void)
{
	unsigned int offset;

	parport_unregister_driver (&lp_driver);

#ifdef CONFIG_LP_CONSOLE
	unregister_console (&lpcons);
#endif

	unregister_chrdev(LP_MAJOR, "lp");
	for (offset = 0; offset < LP_NO; offset++) {
		if (lp_table[offset].dev == NULL)
			continue;
		if (lp_table[offset].flags & (1<<LP_HAVE_PORT_BIT))
			parport_release (lp_table[offset].dev);
		parport_unregister_device(lp_table[offset].dev);
	}
}
#endif
