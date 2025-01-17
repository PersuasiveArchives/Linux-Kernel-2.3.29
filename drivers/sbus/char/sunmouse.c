/* sunmouse.c: Sun mouse driver for the Sparc
 *
 * Copyright (C) 1995, 1996, 1997 David S. Miller (davem@caip.rutgers.edu)
 * Copyright (C) 1995 Miguel de Icaza (miguel@nuclecu.unam.mx)
 *
 * Parts based on the psaux.c driver written by:
 * Johan Myreen.
 *
 * Dec/19/95 Added SunOS mouse ioctls - miguel.
 * Jan/5/96  Added VUID support, sigio support - miguel.
 * Mar/5/96  Added proper mouse stream support - miguel.
 * Sep/96    Allow more than one reader -miguel.
 * Aug/97    Added PCI 8042 controller support -DaveM
 */

/* The mouse is run off of one of the Zilog serial ports.  On
 * that port is the mouse and the keyboard, each gets a zs channel.
 * The mouse itself is mouse-systems in nature.  So the protocol is:
 *
 * Byte 1) Button state which is bit-encoded as
 *            0x4 == left-button down, else up
 *            0x2 == middle-button down, else up
 *            0x1 == right-button down, else up
 *
 * Byte 2) Delta-x
 * Byte 3) Delta-y
 * Byte 4) Delta-x again
 * Byte 5) Delta-y again
 *
 * One day this driver will have to support more than one mouse in the system.
 *
 * This driver has two modes of operation: the default VUID_NATIVE is
 * set when the device is opened and allows the application to see the
 * mouse character stream as we get it from the serial (for gpm for
 * example).  The second method, VUID_FIRM_EVENT will provide cooked
 * events in Firm_event records as expected by SunOS/Solaris applications.
 *
 * FIXME: We need to support more than one mouse.
 * */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fcntl.h>
#include <linux/signal.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/vuid_event.h>
#include <linux/random.h>
/* The following keeps track of software state for the Sun
 * mouse.
 */
#define STREAM_SIZE   2048
#define EV_SIZE       (STREAM_SIZE/sizeof (Firm_event))
#define BUTTON_LEFT   4
#define BUTTON_MIDDLE 2
#define BUTTON_RIGHT  1

struct sun_mouse {
	unsigned char transaction[5];  /* Each protocol transaction */
	unsigned char byte;            /* Counter, starts at 0 */
	unsigned char button_state;    /* Current button state */
	unsigned char prev_state;      /* Previous button state */
	int delta_x;                   /* Current delta-x */
	int delta_y;                   /* Current delta-y */
	int present;                   
	int ready;		       /* set if there if data is available */
	int active;		       /* set if device is open */
        int vuid_mode;	               /* VUID_NATIVE or VUID_FIRM_EVENT */
	wait_queue_head_t proc_list;
	struct fasync_struct *fasync;
	
	/* The event/stream queue */
	unsigned int head;
	unsigned int tail;
	union {
		char stream [STREAM_SIZE];
		Firm_event ev [0];
	} queue;
};

static struct sun_mouse sunmouse;
#define gen_events (sunmouse.vuid_mode != VUID_NATIVE)
#define bstate sunmouse.button_state
#define pstate sunmouse.prev_state

extern void mouse_put_char(char ch);

#undef SMOUSE_DEBUG

static int
push_event (Firm_event *ev)
{
	int next = (sunmouse.head + 1) % EV_SIZE;
	
	if (next != sunmouse.tail){
		sunmouse.queue.ev [sunmouse.head] = *ev;
		sunmouse.head = next;
		return 1;
	}
	return 0;
}

static int
queue_empty (void)
{
	return sunmouse.head == sunmouse.tail;
}

static Firm_event *
get_from_queue (void)
{
	Firm_event *result;
	
	result = &sunmouse.queue.ev [sunmouse.tail];
	sunmouse.tail = (sunmouse.tail + 1) % EV_SIZE;
	return result;
}

static void
push_char (char c)
{
	int next = (sunmouse.head + 1) % STREAM_SIZE;

	if (next != sunmouse.tail){
#ifdef SMOUSE_DEBUG
		printk("P<%02x>\n", (unsigned char)c);
#endif
		sunmouse.queue.stream [sunmouse.head] = c;
		sunmouse.head = next;
	}
	sunmouse.ready = 1;
	if (sunmouse.fasync)
		kill_fasync (sunmouse.fasync, SIGIO, POLL_IN);
	wake_up_interruptible (&sunmouse.proc_list);
}

/* Auto baud rate "detection".  ;-) */
static int mouse_bogon_bytes = 0;
static int mouse_baud_changing = 0;	/* For reporting things to the user. */
static int mouse_baud = 4800;		/* Initial rate set by zilog driver. */

/* Change the baud rate after receiving too many "bogon bytes". */
void sun_mouse_change_baud(void)
{
	extern void rs_change_mouse_baud(int newbaud);

	if(mouse_baud == 1200)
		mouse_baud = 2400;
	else if(mouse_baud == 2400)
		mouse_baud = 4800;
	else if(mouse_baud == 4800)
		mouse_baud = 9600;
	else
		mouse_baud = 1200;

	rs_change_mouse_baud(mouse_baud);
	mouse_baud_changing = 1;
}

void mouse_baud_detection(unsigned char c)
{
	static int wait_for_synchron = 1;
	static int ctr = 0;

	if(wait_for_synchron) {
		if((c & ~0x0f) != 0x80)
			mouse_bogon_bytes++;
		else {
		        if (c & 8) {
				ctr = 2;
				wait_for_synchron = 0;
			} else {
				ctr = 0;
				wait_for_synchron = 0;
			}
		}
	} else {
		ctr++;
		if(ctr >= 4) {
			ctr = 0;
			wait_for_synchron = 1;
			if(mouse_baud_changing == 1) {
				printk(KERN_DEBUG "sunmouse: Successfully adjusted to %d baud.\n",
				       mouse_baud);
				mouse_baud_changing = 0;
			}
		}
	}
	if(mouse_bogon_bytes > 12) {
		sun_mouse_change_baud();
		mouse_bogon_bytes = 0;
		wait_for_synchron = 1;
	}
}

/* The following is called from the zs driver when bytes are received on
 * the Mouse zs8530 channel.
 */
void
sun_mouse_inbyte(unsigned char byte)
{
	signed char mvalue;
	int d, pushed = 0;
	Firm_event ev;

	add_mouse_randomness (byte);
	if(!sunmouse.active)
		return;

	mouse_baud_detection(byte);

	if (!gen_events){
		if (((byte & ~0x0f) == 0x80) && (byte & 0x8)) {
			/* Push dummy 4th and 5th byte for last txn */
			push_char(0x0);
			push_char(0x0);
		}
		push_char (byte);
		return;
	}

	/* If the mouse sends us a byte from 0x80 to 0x87
	 * we are starting at byte zero in the transaction
	 * protocol.
	 */
	if((byte & ~0x0f) == 0x80) 
		sunmouse.byte = 0;

	mvalue = (signed char) byte;
	switch(sunmouse.byte) {
	case 0:
		/* Button state */
		sunmouse.button_state = (~byte) & 0x7;
#ifdef SMOUSE_DEBUG
		printk("B<Left %s, Middle %s, Right %s>",
		       ((sunmouse.button_state & 0x4) ? "DOWN" : "UP"),
		       ((sunmouse.button_state & 0x2) ? "DOWN" : "UP"),
		       ((sunmouse.button_state & 0x1) ? "DOWN" : "UP"));
#endif
		/* To deal with the Sparcbook 3 */
		if (byte & 0x8) {
			sunmouse.byte += 2;
			sunmouse.delta_y = 0;
			sunmouse.delta_x = 0;
		}
		sunmouse.byte++;
		return;
	case 1:
		/* Delta-x 1 */
#ifdef SMOUSE_DEBUG
		printk("DX1<%d>", mvalue);
#endif
		sunmouse.delta_x = mvalue;
		sunmouse.byte++;
		return;
	case 2:
		/* Delta-y 1 */
#ifdef SMOUSE_DEBUG
		printk("DY1<%d>", mvalue);
#endif
		sunmouse.delta_y = mvalue;
		sunmouse.byte++;
		return;
	case 3:
		/* Delta-x 2 */
#ifdef SMOUSE_DEBUG
		printk("DX2<%d>", mvalue);
#endif
		sunmouse.delta_x += mvalue;
		sunmouse.byte++;
		return;
	case 4:
		/* Last byte, Delta-y 2 */
#ifdef SMOUSE_DEBUG
		printk("DY2<%d>", mvalue);
#endif
		sunmouse.delta_y += mvalue;
		sunmouse.byte = 69;  /* Some ridiculous value */
		break;
	case 69:
		/* Until we get the (0x80 -> 0x87) value we aren't
		 * in the middle of a real transaction, so just
		 * return.
		 */
		return;
	default:
		printk("sunmouse: bogon transaction state\n");
		sunmouse.byte = 69;  /* What could cause this? */
		return;
	};
	d = bstate ^ pstate;
	pstate = bstate;
	if (d){
		if (d & BUTTON_LEFT){
			ev.id = MS_LEFT;
			ev.value = bstate & BUTTON_LEFT;
		}
		if (d & BUTTON_RIGHT){
			ev.id = MS_RIGHT;
			ev.value = bstate & BUTTON_RIGHT;
		}
		if (d & BUTTON_MIDDLE){
			ev.id = MS_MIDDLE;
			ev.value = bstate & BUTTON_MIDDLE;
		}
		ev.time = xtime;
		ev.value = ev.value ? VKEY_DOWN : VKEY_UP;
		pushed += push_event (&ev);
	}
	if (sunmouse.delta_x){
		ev.id = LOC_X_DELTA;
		ev.time = xtime;
		ev.value = sunmouse.delta_x;
		pushed += push_event (&ev);
		sunmouse.delta_x = 0;
	}
	if (sunmouse.delta_y){
		ev.id = LOC_Y_DELTA;
		ev.time = xtime;
		ev.value = sunmouse.delta_y;
		pushed += push_event (&ev);
	}
	
	if(pushed != 0) {
		/* We just completed a transaction, wake up whoever is awaiting
		 * this event.
		 */
		sunmouse.ready = 1;
		if (sunmouse.fasync)
			kill_fasync (sunmouse.fasync, SIGIO, POLL_IN);
		wake_up_interruptible(&sunmouse.proc_list);
	}
	return;
}

static int
sun_mouse_open(struct inode * inode, struct file * file)
{
	if(sunmouse.active++)
		return 0;
	if(!sunmouse.present)
		return -EINVAL;
	sunmouse.ready = sunmouse.delta_x = sunmouse.delta_y = 0;
	sunmouse.button_state = 0x80;
	sunmouse.vuid_mode = VUID_NATIVE;
	return 0;
}

static int sun_mouse_fasync (int fd, struct file *filp, int on)
{
	int retval;

	retval = fasync_helper (fd, filp, on, &sunmouse.fasync);
	if (retval < 0)
		return retval;
	return 0;
}

static int
sun_mouse_close(struct inode *inode, struct file *file)
{
	sun_mouse_fasync (-1, file, 0);
	if (--sunmouse.active)
		return 0;
	sunmouse.ready = 0;
	return 0;
}

static ssize_t
sun_mouse_write(struct file *file, const char *buffer,
		size_t count, loff_t *ppos)
{
	return -EINVAL;  /* foo on you */
}

static ssize_t
sun_mouse_read(struct file *file, char *buffer,
	       size_t count, loff_t *ppos)
{
	DECLARE_WAITQUEUE(wait, current);

	if (queue_empty ()){
		if (file->f_flags & O_NONBLOCK)
			return -EWOULDBLOCK;
		add_wait_queue (&sunmouse.proc_list, &wait);
		while (queue_empty () && !signal_pending(current)) {
			current->state = TASK_INTERRUPTIBLE;
			schedule ();
		}
		current->state = TASK_RUNNING;
		remove_wait_queue (&sunmouse.proc_list, &wait);
	}
	if (gen_events){
		char *p = buffer, *end = buffer+count;
		
		while (p < end && !queue_empty ()){
#ifdef CONFIG_SPARC32_COMPAT
			if (current->thread.flags & SPARC_FLAG_32BIT) {
				Firm_event *q = get_from_queue();
				
				copy_to_user_ret((Firm_event *)p, q, 
						 sizeof(Firm_event)-sizeof(struct timeval),
						 -EFAULT);
				p += sizeof(Firm_event)-sizeof(struct timeval);
				__put_user_ret(q->time.tv_sec, (u32 *)p, -EFAULT);
				p += sizeof(u32);
				__put_user_ret(q->time.tv_usec, (u32 *)p, -EFAULT);
				p += sizeof(u32);
			} else
#endif	
			{	
				copy_to_user_ret((Firm_event *)p, get_from_queue(),
				     		 sizeof(Firm_event), -EFAULT);
				p += sizeof (Firm_event);
			}
		}
		sunmouse.ready = !queue_empty ();
		file->f_dentry->d_inode->i_atime = CURRENT_TIME;
		return p-buffer;
	} else {
		int c;
		
		for (c = count; !queue_empty() && c; c--){
			put_user_ret(sunmouse.queue.stream[sunmouse.tail], buffer, -EFAULT);
			buffer++;
			sunmouse.tail = (sunmouse.tail + 1) % STREAM_SIZE;
		}
		sunmouse.ready = !queue_empty();
		file->f_dentry->d_inode->i_atime = CURRENT_TIME;
		return count-c;
	}
	/* Only called if nothing was sent */
	if (signal_pending(current))
		return -ERESTARTSYS;
	return 0;
}

static unsigned int sun_mouse_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &sunmouse.proc_list, wait);
	if(sunmouse.ready)
		return POLLIN | POLLRDNORM;
	return 0;
}
int
sun_mouse_ioctl (struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	int i;
	
	switch (cmd){
		/* VUIDGFORMAT - Get input device byte stream format */
	case _IOR('v', 2, int):
		put_user_ret(sunmouse.vuid_mode, (int *) arg, -EFAULT);
		break;

		/* VUIDSFORMAT - Set input device byte stream format*/
	case _IOW('v', 1, int):
		get_user_ret(i, (int *) arg, -EFAULT);
		if (i == VUID_NATIVE || i == VUID_FIRM_EVENT){
			int value;

			get_user_ret(value, (int *)arg, -EFAULT);
			sunmouse.vuid_mode = value;
			sunmouse.head = sunmouse.tail = 0;
		} else
			return -EINVAL;
		break;

	case 0x8024540b:
	case 0x40245408:
		/* This is a buggy application doing termios on the mouse driver */
		/* we ignore it.  I keep this check here so that we will notice   */
		/* future mouse vuid ioctls */
		break;
		
	default:
#ifdef DEBUG
		printk ("[MOUSE-ioctl: %8.8x]\n", cmd);
#endif
		return -1;
	}
	return 0;
}

struct file_operations sun_mouse_fops = {
	NULL,
	sun_mouse_read,
	sun_mouse_write,
	NULL,
	sun_mouse_poll,
	sun_mouse_ioctl,
	NULL,
	sun_mouse_open,
	NULL,		/* flush */
	sun_mouse_close,
	NULL,
	sun_mouse_fasync,
};

static struct miscdevice sun_mouse_mouse = {
	SUN_MOUSE_MINOR, "sunmouse", &sun_mouse_fops
};

int __init sun_mouse_init(void)
{
	if (!sunmouse.present)
		return -ENODEV;

	printk("Sun Mouse-Systems mouse driver version 1.00\n");

	sunmouse.ready = sunmouse.active = 0;
	misc_register (&sun_mouse_mouse);
	sunmouse.delta_x = sunmouse.delta_y = 0;
	sunmouse.button_state = 0x80;
	init_waitqueue_head(&sunmouse.proc_list);
	sunmouse.byte = 69;
	return 0;
}

void
sun_mouse_zsinit(void)
{
	sunmouse.present = 1;
}
