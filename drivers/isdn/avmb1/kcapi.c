/*
 * $Id: kcapi.c,v 1.10 1999/10/26 15:30:32 calle Exp $
 * 
 * Kernel CAPI 2.0 Module
 * 
 * (c) Copyright 1999 by Carsten Paeth (calle@calle.in-berlin.de)
 * 
 * $Log: kcapi.c,v $
 * Revision 1.10  1999/10/26 15:30:32  calle
 * Generate error message if user want to add card, but driver module is
 * not loaded.
 *
 * Revision 1.9  1999/10/11 22:04:12  keil
 * COMPAT_NEED_UACCESS (no include in isdn_compat.h)
 *
 * Revision 1.8  1999/09/10 17:24:18  calle
 * Changes for proposed standard for CAPI2.0:
 * - AK148 "Linux Exention"
 *
 * Revision 1.7  1999/09/04 06:20:05  keil
 * Changes from kernel set_current_state()
 *
 * Revision 1.6  1999/07/20 06:41:49  calle
 * Bugfix: After the redesign of the AVM B1 driver, the driver didn't even
 *         compile, if not selected as modules.
 *
 * Revision 1.5  1999/07/09 15:05:48  keil
 * compat.h is now isdn_compat.h
 *
 * Revision 1.4  1999/07/08 14:15:17  calle
 * Forgot to count down ncards in drivercb_detach_ctr.
 *
 * Revision 1.3  1999/07/06 07:42:02  calle
 * - changes in /proc interface
 * - check and changed calls to [dev_]kfree_skb and [dev_]alloc_skb.
 *
 * Revision 1.2  1999/07/05 15:09:52  calle
 * - renamed "appl_release" to "appl_released".
 * - version und profile data now cleared on controller reset
 * - extended /proc interface, to allow driver and controller specific
 *   informations to include by driver hackers.
 *
 * Revision 1.1  1999/07/01 15:26:42  calle
 * complete new version (I love it):
 * + new hardware independed "capi_driver" interface that will make it easy to:
 *   - support other controllers with CAPI-2.0 (i.e. USB Controller)
 *   - write a CAPI-2.0 for the passive cards
 *   - support serial link CAPI-2.0 boxes.
 * + wrote "capi_driver" for all supported cards.
 * + "capi_driver" (supported cards) now have to be configured with
 *   make menuconfig, in the past all supported cards where included
 *   at once.
 * + new and better informations in /proc/capi/
 * + new ioctl to switch trace of capi messages per controller
 *   using "avmcapictrl trace [contr] on|off|...."
 * + complete testcircle with all supported cards and also the
 *   PCMCIA cards (now patch for pcmcia-cs-3.0.13 needed) done.
 *
 */
#define CONFIG_AVMB1_COMPAT

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <asm/segment.h>
#include <linux/proc_fs.h>
#include <linux/skbuff.h>
#include <linux/tqueue.h>
#include <linux/capi.h>
#include <linux/kernelcapi.h>
#include <asm/uaccess.h>
#include "capicmd.h"
#include "capiutil.h"
#include "capilli.h"
#ifdef CONFIG_AVMB1_COMPAT
#include <linux/b1lli.h>
#endif

static char *revision = "$Revision: 1.10 $";

/* ------------------------------------------------------------- */

#define CARD_FREE	0
#define CARD_DETECTED	1
#define CARD_LOADING	2
#define CARD_RUNNING	3

/* ------------------------------------------------------------- */

int showcapimsgs = 0;

MODULE_AUTHOR("Carsten Paeth <calle@calle.in-berlin.de>");
MODULE_PARM(showcapimsgs, "0-4i");

/* ------------------------------------------------------------- */

struct msgidqueue {
	struct msgidqueue *next;
	__u16 msgid;
};

struct capi_ncci {
	struct capi_ncci *next;
	__u16 applid;
	__u32 ncci;
	__u32 winsize;
	int   nmsg;
	struct msgidqueue *msgidqueue;
	struct msgidqueue *msgidlast;
	struct msgidqueue *msgidfree;
	struct msgidqueue msgidpool[CAPI_MAXDATAWINDOW];
};

struct capi_appl {
	__u16 applid;
	capi_register_params rparam;
	int releasing;
	__u32 param;
	void (*signal) (__u16 applid, __u32 param);
	struct sk_buff_head recv_queue;
	int nncci;
	struct capi_ncci *nccilist;

	unsigned long nrecvctlpkt;
	unsigned long nrecvdatapkt;
	unsigned long nsentctlpkt;
	unsigned long nsentdatapkt;
};

/* ------------------------------------------------------------- */

static struct capi_version driver_version = {2, 0, 1, 1<<4};
static char driver_serial[CAPI_SERIAL_LEN] = "0004711";
static char capi_manufakturer[64] = "AVM Berlin";

#define APPL(a)		   (&applications[(a)-1])
#define	VALID_APPLID(a)	   ((a) && (a) <= CAPI_MAXAPPL && APPL(a)->applid == a)
#define APPL_IS_FREE(a)    (APPL(a)->applid == 0)
#define APPL_MARK_FREE(a)  do{ APPL(a)->applid=0; MOD_DEC_USE_COUNT; }while(0);
#define APPL_MARK_USED(a)  do{ APPL(a)->applid=(a); MOD_INC_USE_COUNT; }while(0);

#define NCCI2CTRL(ncci)    (((ncci) >> 24) & 0x7f)

#define VALID_CARD(c)	   ((c) > 0 && (c) <= CAPI_MAXCONTR)
#define CARD(c)		   (&cards[(c)-1])
#define CARDNR(cp)	   (((cp)-cards)+1)

static struct capi_appl applications[CAPI_MAXAPPL];
static struct capi_ctr cards[CAPI_MAXCONTR];
static int ncards = 0;
static struct sk_buff_head recv_queue;
static struct capi_interface_user *capi_users = 0;
static struct capi_driver *drivers;
#ifdef CONFIG_AVMB1_COMPAT
static struct capi_driver *b1isa_driver;
static struct capi_driver *t1isa_driver;
#endif
static long notify_up_set = 0;
static long notify_down_set = 0;

static struct tq_struct tq_state_notify;
static struct tq_struct tq_recv_notify;

/* -------- util functions ------------------------------------ */

static char *cardstate2str(unsigned short cardstate)
{
	switch (cardstate) {
        	default:
		case CARD_FREE:		return "free";
		case CARD_DETECTED:	return "detected";
		case CARD_LOADING:	return "loading";
		case CARD_RUNNING:	return "running";
	}
}

static inline int capi_cmd_valid(__u8 cmd)
{
	switch (cmd) {
	case CAPI_ALERT:
	case CAPI_CONNECT:
	case CAPI_CONNECT_ACTIVE:
	case CAPI_CONNECT_B3_ACTIVE:
	case CAPI_CONNECT_B3:
	case CAPI_CONNECT_B3_T90_ACTIVE:
	case CAPI_DATA_B3:
	case CAPI_DISCONNECT_B3:
	case CAPI_DISCONNECT:
	case CAPI_FACILITY:
	case CAPI_INFO:
	case CAPI_LISTEN:
	case CAPI_MANUFACTURER:
	case CAPI_RESET_B3:
	case CAPI_SELECT_B_PROTOCOL:
		return 1;
	}
	return 0;
}

static inline int capi_subcmd_valid(__u8 subcmd)
{
	switch (subcmd) {
	case CAPI_REQ:
	case CAPI_CONF:
	case CAPI_IND:
	case CAPI_RESP:
		return 1;
	}
	return 0;
}

/* -------- /proc functions ----------------------------------- */
/*
 * /proc/capi/applications:
 *      applid l3cnt dblkcnt dblklen #ncci recvqueuelen
 */
static int proc_applications_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	struct capi_appl *ap;
	int i;
	int len = 0;
	off_t begin = 0;

	for (i=0; i < CAPI_MAXAPPL; i++) {
		ap = &applications[i];
		if (ap->applid == 0) continue;
		len += sprintf(page+len, "%u %d %d %d %d %d\n",
			ap->applid,
			ap->rparam.level3cnt,
			ap->rparam.datablkcnt,
			ap->rparam.datablklen,
			ap->nncci,
                        skb_queue_len(&ap->recv_queue));
		if (len+begin > off+count)
			goto endloop;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
endloop:
	if (i >= CAPI_MAXAPPL)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * /proc/capi/ncci:
 *	applid ncci winsize nblk
 */
static int proc_ncci_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	struct capi_appl *ap;
	struct capi_ncci *np;
	int i;
	int len = 0;
	off_t begin = 0;

	for (i=0; i < CAPI_MAXAPPL; i++) {
		ap = &applications[i];
		if (ap->applid == 0) continue;
		for (np = ap->nccilist; np; np = np->next) {
			len += sprintf(page+len, "%d 0x%x %d %d\n",
				np->applid,
				np->ncci,
				np->winsize,
				np->nmsg);
			if (len+begin > off+count)
				goto endloop;
			if (len+begin < off) {
				begin += len;
				len = 0;
			}
		}
	}
endloop:
	if (i >= CAPI_MAXAPPL)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * /proc/capi/driver:
 *	driver ncontroller
 */
static int proc_driver_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	struct capi_driver *driver;
	int len = 0;
	off_t begin = 0;

	for (driver = drivers; driver; driver = driver->next) {
		len += sprintf(page+len, "%-32s %d %s\n",
					driver->name,
					driver->ncontroller,
					driver->revision);
		if (len+begin > off+count)
			goto endloop;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
endloop:
	if (!driver)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * /proc/capi/users:
 *	name
 */
static int proc_users_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
        struct capi_interface_user *cp;
	int len = 0;
	off_t begin = 0;

        for (cp = capi_users; cp ; cp = cp->next) {
		len += sprintf(page+len, "%s\n", cp->name);
		if (len+begin > off+count)
			goto endloop;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
endloop:
	if (cp == 0)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * /proc/capi/controller:
 *	cnr driver cardstate name driverinfo
 */
static int proc_controller_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	struct capi_ctr *cp;
	int i;
	int len = 0;
	off_t begin = 0;

	for (i=0; i < CAPI_MAXCONTR; i++) {
		cp = &cards[i];
		if (cp->cardstate == CARD_FREE) continue;
		len += sprintf(page+len, "%d %-10s %-8s %-16s %s\n",
			cp->cnr, cp->driver->name, 
			cardstate2str(cp->cardstate),
			cp->name,
			cp->driver->procinfo ?  cp->driver->procinfo(cp) : ""
			);
		if (len+begin > off+count)
			goto endloop;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
endloop:
	if (i >= CAPI_MAXCONTR)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * /proc/capi/applstats:
 *	applid nrecvctlpkt nrecvdatapkt nsentctlpkt nsentdatapkt
 */
static int proc_applstats_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	struct capi_appl *ap;
	int i;
	int len = 0;
	off_t begin = 0;

	for (i=0; i < CAPI_MAXAPPL; i++) {
		ap = &applications[i];
		if (ap->applid == 0) continue;
		len += sprintf(page+len, "%u %lu %lu %lu %lu\n",
			ap->applid,
			ap->nrecvctlpkt,
			ap->nrecvdatapkt,
			ap->nsentctlpkt,
			ap->nsentdatapkt);
		if (len+begin > off+count)
			goto endloop;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
endloop:
	if (i >= CAPI_MAXAPPL)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * /proc/capi/contrstats:
 *	cnr nrecvctlpkt nrecvdatapkt nsentctlpkt nsentdatapkt
 */
static int proc_contrstats_read_proc(char *page, char **start, off_t off,
                                       int count, int *eof, void *data)
{
	struct capi_ctr *cp;
	int i;
	int len = 0;
	off_t begin = 0;

	for (i=0; i < CAPI_MAXCONTR; i++) {
		cp = &cards[i];
		if (cp->cardstate == CARD_FREE) continue;
		len += sprintf(page+len, "%d %lu %lu %lu %lu\n",
			cp->cnr, 
			cp->nrecvctlpkt,
			cp->nrecvdatapkt,
			cp->nsentctlpkt,
			cp->nsentdatapkt);
		if (len+begin > off+count)
			goto endloop;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
endloop:
	if (i >= CAPI_MAXCONTR)
		*eof = 1;
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

static struct procfsentries {
  char *name;
  mode_t mode;
  int (*read_proc)(char *page, char **start, off_t off,
                                       int count, int *eof, void *data);
  struct proc_dir_entry *procent;
} procfsentries[] = {
   { "capi",		  S_IFDIR, 0 },
   { "capi/applications", 0	 , proc_applications_read_proc },
   { "capi/ncci", 	  0	 , proc_ncci_read_proc },
   { "capi/driver",       0	 , proc_driver_read_proc },
   { "capi/users", 	  0	 , proc_users_read_proc },
   { "capi/controller",   0	 , proc_controller_read_proc },
   { "capi/applstats",    0	 , proc_applstats_read_proc },
   { "capi/contrstats",   0	 , proc_contrstats_read_proc },
   { "capi/drivers",	  S_IFDIR, 0 },
   { "capi/controllers",  S_IFDIR, 0 },
};

static void proc_capi_init(void)
{
    int nelem = sizeof(procfsentries)/sizeof(procfsentries[0]);
    int i;

    for (i=0; i < nelem; i++) {
        struct procfsentries *p = procfsentries + i;
	p->procent = create_proc_entry(p->name, p->mode, 0);
	if (p->procent) p->procent->read_proc = p->read_proc;
    }
}

static void proc_capi_exit(void)
{
    int nelem = sizeof(procfsentries)/sizeof(procfsentries[0]);
    int i;

    for (i=nelem-1; i >= 0; i--) {
        struct procfsentries *p = procfsentries + i;
	if (p->procent) {
	   remove_proc_entry(p->name, 0);
	   p->procent = 0;
	}
    }
}

/* -------- NCCI Handling ------------------------------------- */

static inline void mq_init(struct capi_ncci * np)
{
	int i;
	np->msgidqueue = 0;
	np->msgidlast = 0;
	np->nmsg = 0;
	memset(np->msgidpool, 0, sizeof(np->msgidpool));
	np->msgidfree = &np->msgidpool[0];
	for (i = 1; i < np->winsize; i++) {
		np->msgidpool[i].next = np->msgidfree;
		np->msgidfree = &np->msgidpool[i];
	}
}

static inline int mq_enqueue(struct capi_ncci * np, __u16 msgid)
{
	struct msgidqueue *mq;
	if ((mq = np->msgidfree) == 0)
		return 0;
	np->msgidfree = mq->next;
	mq->msgid = msgid;
	mq->next = 0;
	if (np->msgidlast)
		np->msgidlast->next = mq;
	np->msgidlast = mq;
	if (!np->msgidqueue)
		np->msgidqueue = mq;
	np->nmsg++;
	return 1;
}

static inline int mq_dequeue(struct capi_ncci * np, __u16 msgid)
{
	struct msgidqueue **pp;
	for (pp = &np->msgidqueue; *pp; pp = &(*pp)->next) {
		if ((*pp)->msgid == msgid) {
			struct msgidqueue *mq = *pp;
			*pp = mq->next;
			if (mq == np->msgidlast)
				np->msgidlast = 0;
			mq->next = np->msgidfree;
			np->msgidfree = mq;
			np->nmsg--;
			return 1;
		}
	}
	return 0;
}

static void controllercb_appl_registered(struct capi_ctr * card, __u16 appl)
{
}

static void controllercb_appl_released(struct capi_ctr * card, __u16 appl)
{
	struct capi_ncci **pp, **nextpp;
	for (pp = &APPL(appl)->nccilist; *pp; pp = nextpp) {
		if (NCCI2CTRL((*pp)->ncci) == card->cnr) {
			struct capi_ncci *np = *pp;
			*pp = np->next;
			printk(KERN_INFO "kcapi: appl %d ncci 0x%x down!\n", appl, np->ncci);
			kfree(np);
			APPL(appl)->nncci--;
			nextpp = pp;
		} else {
			nextpp = &(*pp)->next;
		}
	}
	APPL(appl)->releasing--;
	if (APPL(appl)->releasing <= 0) {
		APPL(appl)->signal = 0;
		APPL_MARK_FREE(appl);
		printk(KERN_INFO "kcapi: appl %d down\n", appl);
	}
}
/*
 * ncci managment
 */

static void controllercb_new_ncci(struct capi_ctr * card,
					__u16 appl, __u32 ncci, __u32 winsize)
{
	struct capi_ncci *np;
	if (!VALID_APPLID(appl)) {
		printk(KERN_ERR "avmb1_handle_new_ncci: illegal appl %d\n", appl);
		return;
	}
	if ((np = (struct capi_ncci *) kmalloc(sizeof(struct capi_ncci), GFP_ATOMIC)) == 0) {
		printk(KERN_ERR "capi_new_ncci: alloc failed ncci 0x%x\n", ncci);
		return;
	}
	if (winsize > CAPI_MAXDATAWINDOW) {
		printk(KERN_ERR "capi_new_ncci: winsize %d too big, set to %d\n",
		       winsize, CAPI_MAXDATAWINDOW);
		winsize = CAPI_MAXDATAWINDOW;
	}
	np->applid = appl;
	np->ncci = ncci;
	np->winsize = winsize;
	mq_init(np);
	np->next = APPL(appl)->nccilist;
	APPL(appl)->nccilist = np;
	APPL(appl)->nncci++;
	printk(KERN_INFO "kcapi: appl %d ncci 0x%x up\n", appl, ncci);

}

static void controllercb_free_ncci(struct capi_ctr * card,
				__u16 appl, __u32 ncci)
{
	struct capi_ncci **pp;
	if (!VALID_APPLID(appl)) {
		printk(KERN_ERR "free_ncci: illegal appl %d\n", appl);
		return;
	}
	for (pp = &APPL(appl)->nccilist; *pp; pp = &(*pp)->next) {
		if ((*pp)->ncci == ncci) {
			struct capi_ncci *np = *pp;
			*pp = np->next;
			kfree(np);
			APPL(appl)->nncci--;
			printk(KERN_INFO "kcapi: appl %d ncci 0x%x down\n", appl, ncci);
			return;
		}
	}
	printk(KERN_ERR "free_ncci: ncci 0x%x not found\n", ncci);
}


static struct capi_ncci *find_ncci(struct capi_appl * app, __u32 ncci)
{
	struct capi_ncci *np;
	for (np = app->nccilist; np; np = np->next) {
		if (np->ncci == ncci)
			return np;
	}
	return 0;
}

/* -------- Receiver ------------------------------------------ */

static void recv_handler(void *dummy)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&recv_queue)) != 0) {
		__u16 appl = CAPIMSG_APPID(skb->data);
		struct capi_ncci *np;
		if (!VALID_APPLID(appl)) {
			printk(KERN_ERR "kcapi: recv_handler: applid %d ? (%s)\n",
			       appl, capi_message2str(skb->data));
			kfree_skb(skb);
			continue;
		}
		if (APPL(appl)->signal == 0) {
			printk(KERN_ERR "kcapi: recv_handler: applid %d has no signal function\n",
			       appl);
			kfree_skb(skb);
			continue;
		}
		if (   CAPIMSG_COMMAND(skb->data) == CAPI_DATA_B3
		    && CAPIMSG_SUBCOMMAND(skb->data) == CAPI_CONF
	            && (np = find_ncci(APPL(appl), CAPIMSG_NCCI(skb->data))) != 0
		    && mq_dequeue(np, CAPIMSG_MSGID(skb->data)) == 0) {
			printk(KERN_ERR "kcapi: msgid %hu ncci 0x%x not on queue\n",
				CAPIMSG_MSGID(skb->data), np->ncci);
		}
		if (   CAPIMSG_COMMAND(skb->data) == CAPI_DATA_B3
		    && CAPIMSG_SUBCOMMAND(skb->data) == CAPI_IND) {
			APPL(appl)->nrecvdatapkt++;
		} else {
			APPL(appl)->nrecvctlpkt++;
		}
		skb_queue_tail(&APPL(appl)->recv_queue, skb);
		(APPL(appl)->signal) (APPL(appl)->applid, APPL(appl)->param);
	}
}

static void controllercb_handle_capimsg(struct capi_ctr * card,
				__u16 appl, struct sk_buff *skb)
{
	int showctl = 0;
	__u8 cmd, subcmd;

	if (card->cardstate != CARD_RUNNING) {
		printk(KERN_INFO "kcapi: controller %d not active, got: %s",
		       card->cnr, capi_message2str(skb->data));
		goto error;
	}
	cmd = CAPIMSG_COMMAND(skb->data);
        subcmd = CAPIMSG_SUBCOMMAND(skb->data);
	if (cmd == CAPI_DATA_B3 && subcmd == CAPI_IND) {
		card->nrecvdatapkt++;
	        if (card->traceflag > 2) showctl |= 2;
	        if (card->traceflag) showctl |= 2;
	} else {
		card->nrecvctlpkt++;
	}
	showctl |= (card->traceflag & 1);
	if (showctl & 2) {
		if (showctl & 1) {
			printk(KERN_DEBUG "kcapi: got [0x%lx] id#%d %s len=%u\n",
			       (unsigned long) card->cnr,
			       CAPIMSG_APPID(skb->data),
			       capi_cmd2str(cmd, subcmd),
			       CAPIMSG_LEN(skb->data));
		} else {
			printk(KERN_DEBUG "kcapi: got [0x%lx] %s\n",
					(unsigned long) card->cnr,
					capi_message2str(skb->data));
		}

	}
	skb_queue_tail(&recv_queue, skb);
	queue_task(&tq_recv_notify, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	return;

error:
	kfree_skb(skb);
}

/* -------- Notifier ------------------------------------------ */

static void notify_up(__u32 contr)
{
	struct capi_interface_user *p;

        printk(KERN_NOTICE "kcapi: notify up contr %d\n", contr);
	for (p = capi_users; p; p = p->next) {
		if (!p->callback) continue;
		(*p->callback) (KCI_CONTRUP, contr, &CARD(contr)->profile);
	}
}

static void notify_down(__u32 contr)
{
	struct capi_interface_user *p;
        printk(KERN_NOTICE "kcapi: notify down contr %d\n", contr);
	for (p = capi_users; p; p = p->next) {
		if (!p->callback) continue;
		(*p->callback) (KCI_CONTRDOWN, contr, 0);
	}
}

static void notify_handler(void *dummy)
{
	__u32 contr;

	for (contr=1; VALID_CARD(contr); contr++)
		 if (test_and_clear_bit(contr, &notify_up_set))
			 notify_up(contr);
	for (contr=1; VALID_CARD(contr); contr++)
		 if (test_and_clear_bit(contr, &notify_down_set))
			 notify_down(contr);
}


static void controllercb_ready(struct capi_ctr * card)
{
	__u16 appl;

	card->cardstate = CARD_RUNNING;

	for (appl = 1; appl <= CAPI_MAXAPPL; appl++) {
		if (!VALID_APPLID(appl)) continue;
		if (APPL(appl)->releasing) continue;
		card->driver->register_appl(card, appl, &APPL(appl)->rparam);
	}

        set_bit(CARDNR(card), &notify_up_set);
        queue_task(&tq_state_notify, &tq_scheduler);

        printk(KERN_NOTICE "kcapi: card %d \"%s\" ready.\n",
		CARDNR(card), card->name);
}

static void controllercb_reseted(struct capi_ctr * card)
{
	__u16 appl;

        if (card->cardstate == CARD_FREE)
		return;
        if (card->cardstate == CARD_DETECTED)
		return;

        card->cardstate = CARD_DETECTED;

	memset(card->manu, 0, sizeof(card->manu));
	memset(&card->version, 0, sizeof(card->version));
	memset(&card->profile, 0, sizeof(card->profile));
	memset(card->serial, 0, sizeof(card->serial));

	for (appl = 1; appl <= CAPI_MAXAPPL; appl++) {
		struct capi_ncci **pp, **nextpp;
		for (pp = &APPL(appl)->nccilist; *pp; pp = nextpp) {
			if (NCCI2CTRL((*pp)->ncci) == card->cnr) {
				struct capi_ncci *np = *pp;
				*pp = np->next;
				printk(KERN_INFO "kcapi: appl %d ncci 0x%x forced down!\n", appl, np->ncci);
				kfree(np);
				nextpp = pp;
			} else {
				nextpp = &(*pp)->next;
			}
		}
	}
	set_bit(CARDNR(card), &notify_down_set);
	queue_task(&tq_state_notify, &tq_scheduler);
	printk(KERN_NOTICE "kcapi: card %d down.\n", CARDNR(card));
}

static void controllercb_suspend_output(struct capi_ctr *card)
{
	if (!card->blocked) {
		printk(KERN_DEBUG "kcapi: card %d suspend\n", CARDNR(card));
		card->blocked = 1;
	}
}

static void controllercb_resume_output(struct capi_ctr *card)
{
	if (card->blocked) {
		printk(KERN_DEBUG "kcapi: card %d resume\n", CARDNR(card));
		card->blocked = 0;
	}
}

/* ------------------------------------------------------------- */


struct capi_ctr *
drivercb_attach_ctr(struct capi_driver *driver, char *name, void *driverdata)
{
	struct capi_ctr *card, **pp;
	int i;

	for (i=0; i < CAPI_MAXCONTR && cards[i].cardstate != CARD_FREE; i++) ;
   
	if (i == CAPI_MAXCONTR) {
		printk(KERN_ERR "kcapi: out of controller slots\n");
	   	return 0;
	}
	card = &cards[i];
	memset(card, 0, sizeof(struct capi_ctr));
	card->driver = driver;
	card->cnr = CARDNR(card);
	strncpy(card->name, name, sizeof(card->name));
	card->cardstate = CARD_DETECTED;
	card->blocked = 0;
	card->driverdata = driverdata;
	card->traceflag = showcapimsgs;

        card->ready = controllercb_ready; 
        card->reseted = controllercb_reseted; 
        card->suspend_output = controllercb_suspend_output;
        card->resume_output = controllercb_resume_output;
        card->handle_capimsg = controllercb_handle_capimsg;
	card->appl_registered = controllercb_appl_registered;
	card->appl_released = controllercb_appl_released;
        card->new_ncci = controllercb_new_ncci;
        card->free_ncci = controllercb_free_ncci;

	for (pp = &driver->controller; *pp; pp = &(*pp)->next) ;
	card->next = 0;
	*pp = card;
	driver->ncontroller++;
	sprintf(card->procfn, "capi/controllers/%d", card->cnr);
	card->procent = create_proc_entry(card->procfn, 0, 0);
	if (card->procent) {
	   card->procent->read_proc = 
		(int (*)(char *,char **,off_t,int,int *,void *))
			driver->ctr_read_proc;
	   card->procent->data = card;
	}

	ncards++;
	printk(KERN_NOTICE "kcapi: Controller %d: %s attached\n",
			card->cnr, card->name);
	return card;
}

static int drivercb_detach_ctr(struct capi_ctr *card)
{
	struct capi_driver *driver = card->driver;
	struct capi_ctr **pp;

        if (card->cardstate == CARD_FREE)
		return 0;
        if (card->cardstate != CARD_DETECTED)
		controllercb_reseted(card);
	for (pp = &driver->controller; *pp ; pp = &(*pp)->next) {
        	if (*pp == card) {
	        	*pp = card->next;
			driver->ncontroller--;
			ncards--;
	        	break;
		}
	}
	if (card->procent) {
	   remove_proc_entry(card->procfn, 0);
	   card->procent = 0;
	}
	card->cardstate = CARD_FREE;
	printk(KERN_NOTICE "kcapi: Controller %d: %s unregistered\n",
			card->cnr, card->name);
	return 0;
}

/* ------------------------------------------------------------- */

/* fallback if no driver read_proc function defined by driver */

static int driver_read_proc(char *page, char **start, off_t off,
        		int count, int *eof, void *data)
{
	struct capi_driver *driver = (struct capi_driver *)data;
	int len = 0;

	len += sprintf(page+len, "%-16s %s\n", "name", driver->name);
	len += sprintf(page+len, "%-16s %s\n", "revision", driver->revision);

	if (len < off) 
           return 0;
	*eof = 1;
	*start = page - off;
	return ((count < len-off) ? count : len-off);
}

/* ------------------------------------------------------------- */

static struct capi_driver_interface di = {
    drivercb_attach_ctr,
    drivercb_detach_ctr,
};

struct capi_driver_interface *attach_capi_driver(struct capi_driver *driver)
{
	struct capi_driver **pp;

	for (pp = &drivers; *pp; pp = &(*pp)->next) ;
	driver->next = 0;
	*pp = driver;
	printk(KERN_NOTICE "kcapi: driver %s attached\n", driver->name);
#ifdef CONFIG_AVMB1_COMPAT
        if (strcmp(driver->name, "b1isa") == 0 && driver->add_card)
		b1isa_driver = driver;
        if (strcmp(driver->name, "t1isa") == 0 && driver->add_card)
		t1isa_driver = driver;
#endif
	sprintf(driver->procfn, "capi/drivers/%s", driver->name);
	driver->procent = create_proc_entry(driver->procfn, 0, 0);
	if (driver->procent) {
	   if (driver->driver_read_proc) {
		   driver->procent->read_proc = 
	       		(int (*)(char *,char **,off_t,int,int *,void *))
					driver->driver_read_proc;
	   } else {
		   driver->procent->read_proc = driver_read_proc;
	   }
	   driver->procent->data = driver;
	}
	return &di;
}

void detach_capi_driver(struct capi_driver *driver)
{
	struct capi_driver **pp;
	for (pp = &drivers; *pp && *pp != driver; pp = &(*pp)->next) ;
	if (*pp) {
		*pp = (*pp)->next;
#ifdef CONFIG_AVMB1_COMPAT
		if (driver == b1isa_driver) b1isa_driver = 0;
		if (driver == t1isa_driver) t1isa_driver = 0;
#endif
		printk(KERN_NOTICE "kcapi: driver %s detached\n", driver->name);
	} else {
		printk(KERN_ERR "kcapi: driver %s double detach ?\n", driver->name);
	}
	if (driver->procent) {
	   remove_proc_entry(driver->procfn, 0);
	   driver->procent = 0;
	}
}

/* ------------------------------------------------------------- */
/* -------- CAPI2.0 Interface ---------------------------------- */
/* ------------------------------------------------------------- */

static __u16 capi_isinstalled(void)
{
	int i;
	for (i = 0; i < CAPI_MAXCONTR; i++) {
		if (cards[i].cardstate == CARD_RUNNING)
			return CAPI_NOERROR;
	}
	return CAPI_REGNOTINSTALLED;
}

static __u16 capi_register(capi_register_params * rparam, __u16 * applidp)
{
	int appl;
	int i;

	if (rparam->datablklen < 128)
		return CAPI_LOGBLKSIZETOSMALL;

	for (appl = 1; appl <= CAPI_MAXAPPL; appl++) {
		if (APPL_IS_FREE(appl))
			break;
	}
	if (appl > CAPI_MAXAPPL)
		return CAPI_TOOMANYAPPLS;

	APPL_MARK_USED(appl);
	skb_queue_head_init(&APPL(appl)->recv_queue);
	APPL(appl)->nncci = 0;

	memcpy(&APPL(appl)->rparam, rparam, sizeof(capi_register_params));

	for (i = 0; i < CAPI_MAXCONTR; i++) {
		if (cards[i].cardstate != CARD_RUNNING)
			continue;
		cards[i].driver->register_appl(&cards[i], appl,
						&APPL(appl)->rparam);
	}
	*applidp = appl;
	printk(KERN_INFO "kcapi: appl %d up\n", appl);

	return CAPI_NOERROR;
}

static __u16 capi_release(__u16 applid)
{
	struct sk_buff *skb;
	int i;

	if (!VALID_APPLID(applid) || APPL(applid)->releasing)
		return CAPI_ILLAPPNR;
	while ((skb = skb_dequeue(&APPL(applid)->recv_queue)) != 0)
		kfree_skb(skb);
	for (i = 0; i < CAPI_MAXCONTR; i++) {
		if (cards[i].cardstate != CARD_RUNNING)
			continue;
		APPL(applid)->releasing++;
		cards[i].driver->release_appl(&cards[i], applid);
	}
	if (APPL(applid)->releasing <= 0) {
	        APPL(applid)->signal = 0;
		APPL_MARK_FREE(applid);
		printk(KERN_INFO "kcapi: appl %d down\n", applid);
	}
	return CAPI_NOERROR;
}

static __u16 capi_put_message(__u16 applid, struct sk_buff *skb)
{
	struct capi_ncci *np;
	__u32 contr;
	int showctl = 0;
	__u8 cmd, subcmd;

	if (ncards == 0)
		return CAPI_REGNOTINSTALLED;
	if (!VALID_APPLID(applid))
		return CAPI_ILLAPPNR;
	if (skb->len < 12
	    || !capi_cmd_valid(CAPIMSG_COMMAND(skb->data))
	    || !capi_subcmd_valid(CAPIMSG_SUBCOMMAND(skb->data)))
		return CAPI_ILLCMDORSUBCMDORMSGTOSMALL;
	contr = CAPIMSG_CONTROLLER(skb->data);
	if (!VALID_CARD(contr) || CARD(contr)->cardstate != CARD_RUNNING) {
		contr = 1;
	        if (CARD(contr)->cardstate != CARD_RUNNING) 
			return CAPI_REGNOTINSTALLED;
	}
	if (CARD(contr)->blocked)
		return CAPI_SENDQUEUEFULL;

	cmd = CAPIMSG_COMMAND(skb->data);
        subcmd = CAPIMSG_SUBCOMMAND(skb->data);

	if (cmd == CAPI_DATA_B3 && subcmd== CAPI_REQ) {
	    	if ((np = find_ncci(APPL(applid), CAPIMSG_NCCI(skb->data))) != 0
	            && mq_enqueue(np, CAPIMSG_MSGID(skb->data)) == 0)
			return CAPI_SENDQUEUEFULL;
		CARD(contr)->nsentdatapkt++;
		APPL(applid)->nsentdatapkt++;
	        if (CARD(contr)->traceflag > 2) showctl |= 2;
	} else {
		CARD(contr)->nsentctlpkt++;
		APPL(applid)->nsentctlpkt++;
	        if (CARD(contr)->traceflag) showctl |= 2;
	}
	showctl |= (CARD(contr)->traceflag & 1);
	if (showctl & 2) {
		if (showctl & 1) {
			printk(KERN_DEBUG "kcapi: put [0x%lx] id#%d %s len=%u\n",
			       (unsigned long) contr,
			       CAPIMSG_APPID(skb->data),
			       capi_cmd2str(cmd, subcmd),
			       CAPIMSG_LEN(skb->data));
		} else {
			printk(KERN_DEBUG "kcapi: put [0x%lx] %s\n",
					(unsigned long) contr,
					capi_message2str(skb->data));
		}

	}
	CARD(contr)->driver->send_message(CARD(contr), skb);
	return CAPI_NOERROR;
}

static __u16 capi_get_message(__u16 applid, struct sk_buff **msgp)
{
	struct sk_buff *skb;

	if (!VALID_APPLID(applid))
		return CAPI_ILLAPPNR;
	if ((skb = skb_dequeue(&APPL(applid)->recv_queue)) == 0)
		return CAPI_RECEIVEQUEUEEMPTY;
	*msgp = skb;
	return CAPI_NOERROR;
}

static __u16 capi_set_signal(__u16 applid,
			     void (*signal) (__u16 applid, __u32 param),
			     __u32 param)
{
	if (!VALID_APPLID(applid))
		return CAPI_ILLAPPNR;
	APPL(applid)->signal = signal;
	APPL(applid)->param = param;
	return CAPI_NOERROR;
}

static __u16 capi_get_manufacturer(__u32 contr, __u8 buf[CAPI_MANUFACTURER_LEN])
{
	if (contr == 0) {
		strncpy(buf, capi_manufakturer, CAPI_MANUFACTURER_LEN);
		return CAPI_NOERROR;
	}
	if (!VALID_CARD(contr) || CARD(contr)->cardstate != CARD_RUNNING) 
		return CAPI_REGNOTINSTALLED;

	strncpy(buf, CARD(contr)->manu, CAPI_MANUFACTURER_LEN);
	return CAPI_NOERROR;
}

static __u16 capi_get_version(__u32 contr, struct capi_version *verp)
{
	if (contr == 0) {
		*verp = driver_version;
		return CAPI_NOERROR;
	}
	if (!VALID_CARD(contr) || CARD(contr)->cardstate != CARD_RUNNING) 
		return CAPI_REGNOTINSTALLED;

	memcpy((void *) verp, &CARD(contr)->version, sizeof(capi_version));
	return CAPI_NOERROR;
}

static __u16 capi_get_serial(__u32 contr, __u8 serial[CAPI_SERIAL_LEN])
{
	if (contr == 0) {
		strncpy(serial, driver_serial, CAPI_SERIAL_LEN);
		return CAPI_NOERROR;
	}
	if (!VALID_CARD(contr) || CARD(contr)->cardstate != CARD_RUNNING) 
		return CAPI_REGNOTINSTALLED;

	strncpy((void *) serial, CARD(contr)->serial, CAPI_SERIAL_LEN);
	return CAPI_NOERROR;
}

static __u16 capi_get_profile(__u32 contr, struct capi_profile *profp)
{
	if (contr == 0) {
		profp->ncontroller = ncards;
		return CAPI_NOERROR;
	}
	if (!VALID_CARD(contr) || CARD(contr)->cardstate != CARD_RUNNING) 
		return CAPI_REGNOTINSTALLED;

	memcpy((void *) profp, &CARD(contr)->profile,
			sizeof(struct capi_profile));
	return CAPI_NOERROR;
}

#ifdef CONFIG_AVMB1_COMPAT
static int old_capi_manufacturer(unsigned int cmd, void *data)
{
	avmb1_loadandconfigdef ldef;
	avmb1_extcarddef cdef;
	avmb1_resetdef rdef;
	avmb1_getdef gdef;
	struct capi_driver *driver;
	struct capi_ctr *card;
	capicardparams cparams;
	capiloaddata ldata;
	int retval;

	switch (cmd) {
	case AVMB1_ADDCARD:
	case AVMB1_ADDCARD_WITH_TYPE:
		if (cmd == AVMB1_ADDCARD) {
		   if ((retval = copy_from_user((void *) &cdef, data,
					    sizeof(avmb1_carddef))))
			   return retval;
		   cdef.cardtype = AVM_CARDTYPE_B1;
		} else {
		   if ((retval = copy_from_user((void *) &cdef, data,
					    sizeof(avmb1_extcarddef))))
			   return retval;
		}
		cparams.port = cdef.port;
		cparams.irq = cdef.irq;
		cparams.cardnr = cdef.cardnr;

                switch (cdef.cardtype) {
			case AVM_CARDTYPE_B1: driver = b1isa_driver; break;
			case AVM_CARDTYPE_T1: driver = t1isa_driver; break;
			default: driver = 0;
		}
		if (!driver) {
			printk(KERN_ERR "kcapi: driver not loaded.\n");
			return -EIO;
		}
		if (!driver->add_card) {
			printk(KERN_ERR "kcapi: driver has no add card function.\n");
			return -EIO;
		}

		return driver->add_card(driver, &cparams);

	case AVMB1_LOAD:
	case AVMB1_LOAD_AND_CONFIG:

		if (cmd == AVMB1_LOAD) {
			if ((retval = copy_from_user((void *) &ldef, data,
						sizeof(avmb1_loaddef))))
				return retval;
			ldef.t4config.len = 0;
			ldef.t4config.data = 0;
		} else {
			if ((retval = copy_from_user((void *) &ldef, data,
					    	sizeof(avmb1_loadandconfigdef))))
				return retval;
		}
		if (!VALID_CARD(ldef.contr))
			return -ESRCH;

		card = CARD(ldef.contr);
		if (card->cardstate == CARD_FREE)
			return -ESRCH;

		if (ldef.t4file.len <= 0) {
			printk(KERN_DEBUG "kcapi: load: invalid parameter: length of t4file is %d ?\n", ldef.t4file.len);
			return -EINVAL;
		}
		if (ldef.t4file.data == 0) {
			printk(KERN_DEBUG "kcapi: load: invalid parameter: dataptr is 0\n");
			return -EINVAL;
		}

		ldata.firmware.user = 1;
		ldata.firmware.data = ldef.t4file.data;
		ldata.firmware.len = ldef.t4file.len;
		ldata.configuration.user = 1;
		ldata.configuration.data = ldef.t4config.data;
		ldata.configuration.len = ldef.t4config.len;

		if (card->cardstate != CARD_DETECTED) {
			printk(KERN_INFO "kcapi: load: contr=%d not in detect state\n", ldef.contr);
			return -EBUSY;
		}
		card->cardstate = CARD_LOADING;

		retval = card->driver->load_firmware(card, &ldata);

		if (retval) {
			card->cardstate = CARD_DETECTED;
			return retval;
		}

		while (card->cardstate != CARD_RUNNING) {

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ/10);	/* 0.1 sec */

			if (signal_pending(current))
				return -EINTR;
		}
		return 0;

	case AVMB1_RESETCARD:
		if ((retval = copy_from_user((void *) &rdef, data,
					 sizeof(avmb1_resetdef))))
			return retval;
		if (!VALID_CARD(rdef.contr))
			return -ESRCH;
		card = CARD(rdef.contr);

		if (card->cardstate == CARD_FREE)
			return -ESRCH;
		if (card->cardstate == CARD_DETECTED)
			return 0;

		card->driver->reset_ctr(card);

		while (card->cardstate > CARD_DETECTED) {

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ/10);	/* 0.1 sec */

			if (signal_pending(current))
				return -EINTR;
		}
		return 0;

	case AVMB1_GET_CARDINFO:
		if ((retval = copy_from_user((void *) &gdef, data,
					 sizeof(avmb1_getdef))))
			return retval;

		if (!VALID_CARD(gdef.contr))
			return -ESRCH;

		card = CARD(gdef.contr);

		if (card->cardstate == CARD_FREE)
			return -ESRCH;

		gdef.cardstate = card->cardstate;
		if (card->driver == b1isa_driver)
			gdef.cardtype = AVM_CARDTYPE_B1;
		else if (card->driver == t1isa_driver)
			gdef.cardtype = AVM_CARDTYPE_T1;
		else gdef.cardtype = AVM_CARDTYPE_B1;

		if ((retval = copy_to_user(data, (void *) &gdef,
					 sizeof(avmb1_getdef))))
			return retval;

		return 0;

	case AVMB1_REMOVECARD:
		if ((retval = copy_from_user((void *) &rdef, data,
					 sizeof(avmb1_resetdef))))
			return retval;

		if (!VALID_CARD(rdef.contr))
			return -ESRCH;
		card = CARD(rdef.contr);

		if (card->cardstate == CARD_FREE)
			return -ESRCH;

		if (card->cardstate != CARD_DETECTED)
			return -EBUSY;

		card->driver->remove_ctr(card);

		while (card->cardstate != CARD_FREE) {

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ/10);	/* 0.1 sec */

			if (signal_pending(current))
				return -EINTR;
		}
		return 0;
	}
	return -EINVAL;
}
#endif

static int capi_manufacturer(unsigned int cmd, void *data)
{
        struct capi_ctr *card;
	kcapi_flagdef fdef;
	int retval;

	switch (cmd) {
#ifdef CONFIG_AVMB1_COMPAT
	case AVMB1_ADDCARD:
	case AVMB1_ADDCARD_WITH_TYPE:
	case AVMB1_LOAD:
	case AVMB1_LOAD_AND_CONFIG:
	case AVMB1_RESETCARD:
	case AVMB1_GET_CARDINFO:
	case AVMB1_REMOVECARD:
		return old_capi_manufacturer(cmd, data);
#endif
	case KCAPI_CMD_TRACE:
		if ((retval = copy_from_user((void *) &fdef, data,
					 sizeof(kcapi_flagdef))))
			return retval;

		if (!VALID_CARD(fdef.contr))
			return -ESRCH;
		card = CARD(fdef.contr);
		if (card->cardstate == CARD_FREE)
			return -ESRCH;
		card->traceflag = fdef.flag;
		printk(KERN_INFO "kcapi: contr %d set trace=%d\n",
			card->cnr, card->traceflag);
		return 0;
	}
	return -EINVAL;
}

struct capi_interface avmb1_interface =
{
	capi_isinstalled,
	capi_register,
	capi_release,
	capi_put_message,
	capi_get_message,
	capi_set_signal,
	capi_get_manufacturer,
	capi_get_version,
	capi_get_serial,
	capi_get_profile,
	capi_manufacturer
};

/* ------------------------------------------------------------- */
/* -------- Exported Functions --------------------------------- */
/* ------------------------------------------------------------- */

struct capi_interface *attach_capi_interface(struct capi_interface_user *userp)
{
	struct capi_interface_user *p;

	for (p = capi_users; p; p = p->next) {
		if (p == userp) {
			printk(KERN_ERR "kcapi: double attach from %s\n",
			       userp->name);
			return 0;
		}
	}
	userp->next = capi_users;
	capi_users = userp;
	MOD_INC_USE_COUNT;
	printk(KERN_NOTICE "kcapi: %s attached\n", userp->name);

	return &avmb1_interface;
}

int detach_capi_interface(struct capi_interface_user *userp)
{
	struct capi_interface_user **pp;

	for (pp = &capi_users; *pp; pp = &(*pp)->next) {
		if (*pp == userp) {
			*pp = userp->next;
			userp->next = 0;
			MOD_DEC_USE_COUNT;
			printk(KERN_NOTICE "kcapi: %s detached\n", userp->name);
			return 0;
		}
	}
	printk(KERN_ERR "kcapi: double detach from %s\n", userp->name);
	return -1;
}

/* ------------------------------------------------------------- */
/* -------- Init & Cleanup ------------------------------------- */
/* ------------------------------------------------------------- */

EXPORT_SYMBOL(attach_capi_interface);
EXPORT_SYMBOL(detach_capi_interface);
EXPORT_SYMBOL(attach_capi_driver);
EXPORT_SYMBOL(detach_capi_driver);

#ifndef MODULE
#ifdef CONFIG_ISDN_DRV_AVMB1_B1ISA
extern int b1isa_init(void);
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_B1PCI
extern int b1pci_init(void);
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_T1ISA
extern int t1isa_init(void);
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_B1PCMCIA
extern int b1pcmcia_init(void);
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_T1PCI
extern int t1pci_init(void);
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_C4
extern int c4_init(void);
#endif
#endif

/*
 * init / exit functions
 */

#ifdef MODULE
#define kcapi_init init_module
#endif

int kcapi_init(void)
{
	char *p;
	char rev[10];

	skb_queue_head_init(&recv_queue);
	/* init_bh(CAPI_BH, do_capi_bh); */

	tq_state_notify.routine = notify_handler;
	tq_state_notify.data = 0;

	tq_recv_notify.routine = recv_handler;
	tq_recv_notify.data = 0;

        proc_capi_init();

	if ((p = strchr(revision, ':'))) {
		strcpy(rev, p + 1);
		p = strchr(rev, '$');
		*p = 0;
	} else
		strcpy(rev, "1.0");

#ifdef MODULE
        printk(KERN_NOTICE "CAPI-driver Rev%s: loaded\n", rev);
#else
	printk(KERN_NOTICE "CAPI-driver Rev%s: started\n", rev);
#ifdef CONFIG_ISDN_DRV_AVMB1_B1ISA
	(void)b1isa_init();
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_B1PCI
	(void)b1pci_init();
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_T1ISA
	(void)t1isa_init();
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_B1PCMCIA
	(void)b1pcmcia_init();
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_T1PCI
	(void)t1pci_init();
#endif
#ifdef CONFIG_ISDN_DRV_AVMB1_C4
	(void)c4_init();
#endif
#endif
	return 0;
}

#ifdef MODULE
void cleanup_module(void)
{
	char rev[10];
	char *p;

	if ((p = strchr(revision, ':'))) {
		strcpy(rev, p + 1);
		p = strchr(rev, '$');
		*p = 0;
	} else {
		strcpy(rev, "1.0");
	}

	schedule(); /* execute queued tasks .... */
        proc_capi_exit();
	printk(KERN_NOTICE "CAPI-driver Rev%s: unloaded\n", rev);
}
#endif
