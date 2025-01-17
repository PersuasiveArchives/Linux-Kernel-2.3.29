/*
 * net/sched/sch_red.c	Random Early Detection queue.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 * Changes:
 * J Hadi Salim <hadi@nortel.com> 980914:	computation fixes
 * Alexey Makarenko <makar@phoenix.kharkov.ua> 990814: qave on idle link was calculated incorrectly.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/notifier.h>
#include <net/ip.h>
#include <net/route.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/pkt_sched.h>


/*	Random Early Detection (RED) algorithm.
	=======================================

	Source: Sally Floyd and Van Jacobson, "Random Early Detection Gateways
	for Congestion Avoidance", 1993, IEEE/ACM Transactions on Networking.

	This file codes a "divisionless" version of RED algorithm
	as written down in Fig.17 of the paper.

Short description.
------------------

	When a new packet arrives we calculate the average queue length:

	avg = (1-W)*avg + W*current_queue_len,

	W is the filter time constant (choosen as 2^(-Wlog)), it controls
	the inertia of the algorithm. To allow larger bursts, W should be
	decreased.

	if (avg > th_max) -> packet marked (dropped).
	if (avg < th_min) -> packet passes.
	if (th_min < avg < th_max) we calculate probability:

	Pb = max_P * (avg - th_min)/(th_max-th_min)

	and mark (drop) packet with this probability.
	Pb changes from 0 (at avg==th_min) to max_P (avg==th_max).
	max_P should be small (not 1), usually 0.01..0.02 is good value.

	max_P is chosen as a number, so that max_P/(th_max-th_min)
	is a negative power of two in order arithmetics to contain
	only shifts.


	Parameters, settable by user:
	-----------------------------

	limit		- bytes (must be > qth_max + burst)

	Hard limit on queue length, should be chosen >qth_max
	to allow packet bursts. This parameter does not
	affect the algorithms behaviour and can be chosen
	arbitrarily high (well, less than ram size)
	Really, this limit will never be reached
	if RED works correctly.

	qth_min		- bytes (should be < qth_max/2)
	qth_max		- bytes (should be at least 2*qth_min and less limit)
	Wlog	       	- bits (<32) log(1/W).
	Plog	       	- bits (<32)

	Plog is related to max_P by formula:

	max_P = (qth_max-qth_min)/2^Plog;

	F.e. if qth_max=128K and qth_min=32K, then Plog=22
	corresponds to max_P=0.02

	Scell_log
	Stab

	Lookup table for log((1-W)^(t/t_ave).


NOTES:

Upper bound on W.
-----------------

	If you want to allow bursts of L packets of size S,
	you should choose W:

	L + 1 - th_min/S < (1-(1-W)^L)/W

	th_min/S = 32         th_min/S = 4
			                       
	log(W)	L
	-1	33
	-2	35
	-3	39
	-4	46
	-5	57
	-6	75
	-7	101
	-8	135
	-9	190
	etc.
 */

struct red_sched_data
{
/* Parameters */
	u32		limit;		/* HARD maximal queue length	*/
	u32		qth_min;	/* Min average length threshold: A scaled */
	u32		qth_max;	/* Max average length threshold: A scaled */
	u32		Rmask;
	u32		Scell_max;
	char		Wlog;		/* log(W)		*/
	char		Plog;		/* random number bits	*/
	char		Scell_log;
	u8		Stab[256];

/* Variables */
	unsigned long	qave;		/* Average queue length: A scaled */
	int		qcount;		/* Packets since last random number generation */
	u32		qR;		/* Cached random number */

	psched_time_t	qidlestart;	/* Start of idle period		*/
};

static int
red_enqueue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct red_sched_data *q = (struct red_sched_data *)sch->data;

	psched_time_t now;

	if (!PSCHED_IS_PASTPERFECT(q->qidlestart)) {
		long us_idle;
		int  shift;

		PSCHED_GET_TIME(now);
		us_idle = PSCHED_TDIFF_SAFE(now, q->qidlestart, q->Scell_max, 0);
		PSCHED_SET_PASTPERFECT(q->qidlestart);

/*
   The problem: ideally, average length queue recalcultion should
   be done over constant clock intervals. This is too expensive, so that
   the calculation is driven by outgoing packets.
   When the queue is idle we have to model this clock by hand.

   SF+VJ proposed to "generate" m = idletime/(average_pkt_size/bandwidth)
   dummy packets as a burst after idle time, i.e.

          q->qave *= (1-W)^m

   This is an apparently overcomplicated solution (f.e. we have to precompute
   a table to make this calculation in reasonable time)
   I believe that a simpler model may be used here,
   but it is field for experiments.
*/
		shift = q->Stab[us_idle>>q->Scell_log];

		if (shift) {
			q->qave >>= shift;
		} else {
			/* Approximate initial part of exponent
			   with linear function:
			   (1-W)^m ~= 1-mW + ...

			   Seems, it is the best solution to
			   problem of too coarce exponent tabulation.
			 */

			us_idle = (q->qave * us_idle)>>q->Scell_log;
			if (us_idle < q->qave/2)
				q->qave -= us_idle;
			else
				q->qave >>= 1;
		}
	} else {
		q->qave += sch->stats.backlog - (q->qave >> q->Wlog);
		/* NOTE:
		   q->qave is fixed point number with point at Wlog.
		   The formulae above is equvalent to floating point
		   version:

		   qave = qave*(1-W) + sch->stats.backlog*W;
		                                           --ANK (980924)
		 */
	}

	if (q->qave < q->qth_min) {
		q->qcount = -1;
enqueue:
		if (sch->stats.backlog <= q->limit) {
			__skb_queue_tail(&sch->q, skb);
			sch->stats.backlog += skb->len;
			sch->stats.bytes += skb->len;
			sch->stats.packets++;
			return 0;
		}
		kfree_skb(skb);
		sch->stats.drops++;
		return NET_XMIT_DROP;
	}
	if (q->qave >= q->qth_max) {
		q->qcount = -1;
		sch->stats.overlimits++;
mark:
		kfree_skb(skb);
		sch->stats.drops++;
		return NET_XMIT_CN;
	}
	if (++q->qcount) {
		/* The formula used below causes questions.

		   OK. qR is random number in the interval 0..Rmask
		   i.e. 0..(2^Plog). If we used floating point
		   arithmetics, it would be: (2^Plog)*rnd_num,
		   where rnd_num is less 1.

		   Taking into account, that qave have fixed
		   point at Wlog, and Plog is related to max_P by
		   max_P = (qth_max-qth_min)/2^Plog; two lines
		   below have the following floating point equivalent:
		   
		   max_P*(qave - qth_min)/(qth_max-qth_min) < rnd/qcount

		   Any questions? --ANK (980924)
		 */
		if (((q->qave - q->qth_min)>>q->Wlog)*q->qcount < q->qR)
			goto enqueue;
		q->qcount = 0;
		q->qR = net_random()&q->Rmask;
		sch->stats.overlimits++;
		goto mark;
	}
	q->qR = net_random()&q->Rmask;
	goto enqueue;
}

static int
red_requeue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct red_sched_data *q = (struct red_sched_data *)sch->data;

	PSCHED_SET_PASTPERFECT(q->qidlestart);

	__skb_queue_head(&sch->q, skb);
	sch->stats.backlog += skb->len;
	return 0;
}

static struct sk_buff *
red_dequeue(struct Qdisc* sch)
{
	struct sk_buff *skb;
	struct red_sched_data *q = (struct red_sched_data *)sch->data;

	skb = __skb_dequeue(&sch->q);
	if (skb) {
		sch->stats.backlog -= skb->len;
		return skb;
	}
	PSCHED_GET_TIME(q->qidlestart);
	return NULL;
}

static int
red_drop(struct Qdisc* sch)
{
	struct sk_buff *skb;
	struct red_sched_data *q = (struct red_sched_data *)sch->data;

	skb = __skb_dequeue_tail(&sch->q);
	if (skb) {
		sch->stats.backlog -= skb->len;
		sch->stats.drops++;
		kfree_skb(skb);
		return 1;
	}
	PSCHED_GET_TIME(q->qidlestart);
	return 0;
}

static void red_reset(struct Qdisc* sch)
{
	struct red_sched_data *q = (struct red_sched_data *)sch->data;
	struct sk_buff *skb;

	while((skb=__skb_dequeue(&sch->q))!=NULL)
		kfree_skb(skb);
	sch->stats.backlog = 0;
	PSCHED_SET_PASTPERFECT(q->qidlestart);
	q->qave = 0;
	q->qcount = -1;
}

static int red_change(struct Qdisc *sch, struct rtattr *opt)
{
	struct red_sched_data *q = (struct red_sched_data *)sch->data;
	struct rtattr *tb[TCA_RED_STAB];
	struct tc_red_qopt *ctl;

	if (opt == NULL ||
	    rtattr_parse(tb, TCA_RED_STAB, RTA_DATA(opt), RTA_PAYLOAD(opt)) ||
	    tb[TCA_RED_PARMS-1] == 0 || tb[TCA_RED_STAB-1] == 0 ||
	    RTA_PAYLOAD(tb[TCA_RED_PARMS-1]) < sizeof(*ctl) ||
	    RTA_PAYLOAD(tb[TCA_RED_STAB-1]) < 256)
		return -EINVAL;

	ctl = RTA_DATA(tb[TCA_RED_PARMS-1]);

	sch_tree_lock(sch);
	q->Wlog = ctl->Wlog;
	q->Plog = ctl->Plog;
	q->Rmask = ctl->Plog < 32 ? ((1<<ctl->Plog) - 1) : ~0UL;
	q->Scell_log = ctl->Scell_log;
	q->Scell_max = (255<<q->Scell_log);
	q->qth_min = ctl->qth_min<<ctl->Wlog;
	q->qth_max = ctl->qth_max<<ctl->Wlog;
	q->limit = ctl->limit;
	memcpy(q->Stab, RTA_DATA(tb[TCA_RED_STAB-1]), 256);

	q->qcount = -1;
	if (skb_queue_len(&sch->q) == 0)
		PSCHED_SET_PASTPERFECT(q->qidlestart);
	sch_tree_unlock(sch);
	return 0;
}

static int red_init(struct Qdisc* sch, struct rtattr *opt)
{
	int err;

	MOD_INC_USE_COUNT;

	if ((err = red_change(sch, opt)) != 0) {
		MOD_DEC_USE_COUNT;
	}
	return err;
}


#ifdef CONFIG_RTNETLINK
static int red_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct red_sched_data *q = (struct red_sched_data *)sch->data;
	unsigned char	 *b = skb->tail;
	struct rtattr *rta;
	struct tc_red_qopt opt;

	rta = (struct rtattr*)b;
	RTA_PUT(skb, TCA_OPTIONS, 0, NULL);
	opt.limit = q->limit;
	opt.qth_min = q->qth_min>>q->Wlog;
	opt.qth_max = q->qth_max>>q->Wlog;
	opt.Wlog = q->Wlog;
	opt.Plog = q->Plog;
	opt.Scell_log = q->Scell_log;
	RTA_PUT(skb, TCA_RED_PARMS, sizeof(opt), &opt);
	rta->rta_len = skb->tail - b;

	return skb->len;

rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}
#endif

static void red_destroy(struct Qdisc *sch)
{
	MOD_DEC_USE_COUNT;
}

struct Qdisc_ops red_qdisc_ops =
{
	NULL,
	NULL,
	"red",
	sizeof(struct red_sched_data),

	red_enqueue,
	red_dequeue,
	red_requeue,
	red_drop,

	red_init,
	red_reset,
	red_destroy,
	red_change,

#ifdef CONFIG_RTNETLINK
	red_dump,
#endif
};


#ifdef MODULE
int init_module(void)
{
	return register_qdisc(&red_qdisc_ops);
}

void cleanup_module(void) 
{
	unregister_qdisc(&red_qdisc_ops);
}
#endif
