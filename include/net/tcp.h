/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the TCP module.
 *
 * Version:	@(#)tcp.h	1.0.5	05/23/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _TCP_H
#define _TCP_H

#define TCP_DEBUG 1

#include <linux/config.h>
#include <linux/tcp.h>
#include <linux/slab.h>
#include <net/checksum.h>
#include <net/sock.h>

/* This is for all connections with a full identity, no wildcards.
 * New scheme, half the table is for TIME_WAIT, the other half is
 * for the rest.  I'll experiment with dynamic table growth later.
 */
struct tcp_ehash_bucket {
	rwlock_t	lock;
	struct sock	*chain;
} __attribute__((__aligned__(8)));

extern int tcp_ehash_size;
extern struct tcp_ehash_bucket *tcp_ehash;

/* This is for listening sockets, thus all sockets which possess wildcards. */
#define TCP_LHTABLE_SIZE	32	/* Yes, really, this is all you need. */

/* tcp_ipv4.c: These need to be shared by v4 and v6 because the lookup
 *             and hashing code needs to work with different AF's yet
 *             the port space is shared.
 */
extern struct sock *tcp_listening_hash[TCP_LHTABLE_SIZE];
extern rwlock_t tcp_lhash_lock;
extern atomic_t tcp_lhash_users;
extern wait_queue_head_t tcp_lhash_wait;

/* There are a few simple rules, which allow for local port reuse by
 * an application.  In essence:
 *
 *	1) Sockets bound to different interfaces may share a local port.
 *	   Failing that, goto test 2.
 *	2) If all sockets have sk->reuse set, and none of them are in
 *	   TCP_LISTEN state, the port may be shared.
 *	   Failing that, goto test 3.
 *	3) If all sockets are bound to a specific sk->rcv_saddr local
 *	   address, and none of them are the same, the port may be
 *	   shared.
 *	   Failing this, the port cannot be shared.
 *
 * The interesting point, is test #2.  This is what an FTP server does
 * all day.  To optimize this case we use a specific flag bit defined
 * below.  As we add sockets to a bind bucket list, we perform a
 * check of: (newsk->reuse && (newsk->state != TCP_LISTEN))
 * As long as all sockets added to a bind bucket pass this test,
 * the flag bit will be set.
 * The resulting situation is that tcp_v[46]_verify_bind() can just check
 * for this flag bit, if it is set and the socket trying to bind has
 * sk->reuse set, we don't even have to walk the owners list at all,
 * we return that it is ok to bind this socket to the requested local port.
 *
 * Sounds like a lot of work, but it is worth it.  In a more naive
 * implementation (ie. current FreeBSD etc.) the entire list of ports
 * must be walked for each data port opened by an ftp server.  Needless
 * to say, this does not scale at all.  With a couple thousand FTP
 * users logged onto your box, isn't it nice to know that new data
 * ports are created in O(1) time?  I thought so. ;-)	-DaveM
 */
struct tcp_bind_bucket {
	unsigned short		port;
	unsigned short		fastreuse;
	struct tcp_bind_bucket	*next;
	struct sock		*owners;
	struct tcp_bind_bucket	**pprev;
};

struct tcp_bind_hashbucket {
	spinlock_t		lock;
	struct tcp_bind_bucket	*chain;
};

extern struct tcp_bind_hashbucket *tcp_bhash;
extern int tcp_bhash_size;
extern spinlock_t tcp_portalloc_lock;

extern kmem_cache_t *tcp_bucket_cachep;
extern struct tcp_bind_bucket *tcp_bucket_create(struct tcp_bind_hashbucket *head,
						 unsigned short snum);
extern void tcp_bucket_unlock(struct sock *sk);
extern int tcp_port_rover;
extern struct sock *tcp_v4_lookup_listener(u32 addr, unsigned short hnum, int dif);

/* These are AF independent. */
static __inline__ int tcp_bhashfn(__u16 lport)
{
	return (lport & (tcp_bhash_size - 1));
}

/* This is a TIME_WAIT bucket.  It works around the memory consumption
 * problems of sockets in such a state on heavily loaded servers, but
 * without violating the protocol specification.
 */
struct tcp_tw_bucket {
	/* These _must_ match the beginning of struct sock precisely.
	 * XXX Yes I know this is gross, but I'd have to edit every single
	 * XXX networking file if I created a "struct sock_header". -DaveM
	 */
	__u32			daddr;
	__u32			rcv_saddr;
	__u16			dport;
	unsigned short		num;
	int			bound_dev_if;
	struct sock		*next;
	struct sock		**pprev;
	struct sock		*bind_next;
	struct sock		**bind_pprev;
	unsigned char		state,
				zapped;
	__u16			sport;
	unsigned short		family;
	unsigned char		reuse,
				nonagle;
	atomic_t		refcnt;

	/* And these are ours. */
	int			hashent;
	__u32			rcv_nxt;
	__u32			snd_nxt;
        __u32			ts_recent;
        long			ts_recent_stamp;
	struct tcp_bind_bucket	*tb;
	struct tcp_tw_bucket	*next_death;
	struct tcp_tw_bucket	**pprev_death;
	int			death_slot;
#ifdef CONFIG_TCP_TW_RECYCLE
	unsigned long		ttd;
	int			rto;
#endif
#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
	struct in6_addr		v6_daddr;
	struct in6_addr		v6_rcv_saddr;
#endif
};

extern kmem_cache_t *tcp_timewait_cachep;

extern __inline__ void tcp_tw_put(struct tcp_tw_bucket *tw)
{
	if (atomic_dec_and_test(&tw->refcnt)) {
#ifdef INET_REFCNT_DEBUG
		printk(KERN_DEBUG "tw_bucket %p released\n", tw);
#endif
		kmem_cache_free(tcp_timewait_cachep, tw);
	}
}

extern int tcp_tw_death_row_slot;
extern void tcp_timewait_kill(struct tcp_tw_bucket *tw);
extern void tcp_tw_schedule(struct tcp_tw_bucket *tw);
extern void tcp_tw_reschedule(struct tcp_tw_bucket *tw);
extern void tcp_tw_deschedule(struct tcp_tw_bucket *tw);


/* Socket demux engine toys. */
#ifdef __BIG_ENDIAN
#define TCP_COMBINED_PORTS(__sport, __dport) \
	(((__u32)(__sport)<<16) | (__u32)(__dport))
#else /* __LITTLE_ENDIAN */
#define TCP_COMBINED_PORTS(__sport, __dport) \
	(((__u32)(__dport)<<16) | (__u32)(__sport))
#endif

#if (BITS_PER_LONG == 64)
#ifdef __BIG_ENDIAN
#define TCP_V4_ADDR_COOKIE(__name, __saddr, __daddr) \
	__u64 __name = (((__u64)(__saddr))<<32)|((__u64)(__daddr));
#else /* __LITTLE_ENDIAN */
#define TCP_V4_ADDR_COOKIE(__name, __saddr, __daddr) \
	__u64 __name = (((__u64)(__daddr))<<32)|((__u64)(__saddr));
#endif /* __BIG_ENDIAN */
#define TCP_IPV4_MATCH(__sk, __cookie, __saddr, __daddr, __ports, __dif)\
	(((*((__u64 *)&((__sk)->daddr)))== (__cookie))	&&		\
	 ((*((__u32 *)&((__sk)->dport)))== (__ports))   &&		\
	 (!((__sk)->bound_dev_if) || ((__sk)->bound_dev_if == (__dif))))
#else /* 32-bit arch */
#define TCP_V4_ADDR_COOKIE(__name, __saddr, __daddr)
#define TCP_IPV4_MATCH(__sk, __cookie, __saddr, __daddr, __ports, __dif)\
	(((__sk)->daddr			== (__saddr))	&&		\
	 ((__sk)->rcv_saddr		== (__daddr))	&&		\
	 ((*((__u32 *)&((__sk)->dport)))== (__ports))   &&		\
	 (!((__sk)->bound_dev_if) || ((__sk)->bound_dev_if == (__dif))))
#endif /* 64-bit arch */

#define TCP_IPV6_MATCH(__sk, __saddr, __daddr, __ports, __dif)			   \
	(((*((__u32 *)&((__sk)->dport)))== (__ports))   			&& \
	 ((__sk)->family		== AF_INET6)				&& \
	 !ipv6_addr_cmp(&(__sk)->net_pinfo.af_inet6.daddr, (__saddr))		&& \
	 !ipv6_addr_cmp(&(__sk)->net_pinfo.af_inet6.rcv_saddr, (__daddr))	&& \
	 (!((__sk)->bound_dev_if) || ((__sk)->bound_dev_if == (__dif))))

/* These can have wildcards, don't try too hard. */
static __inline__ int tcp_lhashfn(unsigned short num)
{
	return num & (TCP_LHTABLE_SIZE - 1);
}

static __inline__ int tcp_sk_listen_hashfn(struct sock *sk)
{
	return tcp_lhashfn(sk->num);
}

/* Note, that it is > than ipv6 header */
#define NETHDR_SIZE	(sizeof(struct iphdr) + 40)

/*
 * 40 is maximal IP options size
 * 20 is the maximum TCP options size we can currently construct on a SYN.
 * 40 is the maximum possible TCP options size.
 */

#define MAX_SYN_SIZE	(NETHDR_SIZE + sizeof(struct tcphdr) + 20 + MAX_HEADER + 15)
#define MAX_FIN_SIZE	(NETHDR_SIZE + sizeof(struct tcphdr) + MAX_HEADER + 15)
#define BASE_ACK_SIZE	(NETHDR_SIZE + MAX_HEADER + 15)
#define MAX_ACK_SIZE	(NETHDR_SIZE + sizeof(struct tcphdr) + MAX_HEADER + 15)
#define MAX_RESET_SIZE	(NETHDR_SIZE + sizeof(struct tcphdr) + MAX_HEADER + 15)
#define MAX_TCPHEADER_SIZE (NETHDR_SIZE + sizeof(struct tcphdr) + 20 + MAX_HEADER + 15)

/* 
 * Never offer a window over 32767 without using window scaling. Some
 * poor stacks do signed 16bit maths! 
 */
#define MAX_WINDOW	32767	
#define MAX_DELAY_ACK	2

/* 
 * How much of the receive buffer do we advertize 
 * (the rest is reserved for headers and driver packet overhead)
 * Use a power of 2.
 */
#define WINDOW_ADVERTISE_DIVISOR 2

/* urg_data states */
#define URG_VALID	0x0100
#define URG_NOTYET	0x0200
#define URG_READ	0x0400

#define TCP_RETR1	7	/*
				 * This is how many retries it does before it
				 * tries to figure out if the gateway is
				 * down.
				 */

#define TCP_RETR2	15	/*
				 * This should take at least
				 * 90 minutes to time out.
				 */

#define TCP_TIMEOUT_LEN	(15*60*HZ) /* should be about 15 mins		*/
#define TCP_TIMEWAIT_LEN (60*HZ) /* how long to wait to successfully 
				  * close the socket, about 60 seconds	*/
#define TCP_FIN_TIMEOUT (3*60*HZ) /* BSD style FIN_WAIT2 deadlock breaker */

#define TCP_ACK_TIME	(3*HZ)	/* time to delay before sending an ACK	*/
#define TCP_WRITE_TIME	(30*HZ)	/* initial time to wait for an ACK,
			         * after last transmit			*/
#define TCP_TIMEOUT_INIT (3*HZ)	/* RFC 1122 initial timeout value	*/
#define TCP_SYN_RETRIES	 10	/* number of times to retry opening a
				 * connection 	(TCP_RETR2-....)	*/
#define TCP_PROBEWAIT_LEN (1*HZ)/* time to wait between probes when
				 * I've got something to write and
				 * there is no window			*/
#define TCP_KEEPALIVE_TIME (120*60*HZ)		/* two hours */
#define TCP_KEEPALIVE_PROBES	9		/* Max of 9 keepalive probes	*/
#define TCP_KEEPALIVE_INTVL	(75*HZ)

#define MAX_TCP_KEEPIDLE	32767
#define MAX_TCP_KEEPINTVL	32767
#define MAX_TCP_KEEPCNT		127
#define MAX_TCP_SYNCNT		127

#define TCP_SYNACK_PERIOD	(HZ/2) /* How often to run the synack slow timer */
#define TCP_QUICK_TRIES		8  /* How often we try to retransmit, until
				    * we tell the link layer that it is something
				    * wrong (e.g. that it can expire redirects) */

/* TIME_WAIT reaping mechanism. */
#define TCP_TWKILL_SLOTS	8	/* Please keep this a power of 2. */
#define TCP_TWKILL_PERIOD	((HZ*60)/TCP_TWKILL_SLOTS)

/*
 *	TCP option
 */
 
#define TCPOPT_NOP		1	/* Padding */
#define TCPOPT_EOL		0	/* End of options */
#define TCPOPT_MSS		2	/* Segment size negotiating */
#define TCPOPT_WINDOW		3	/* Window scaling */
#define TCPOPT_SACK_PERM        4       /* SACK Permitted */
#define TCPOPT_SACK             5       /* SACK Block */
#define TCPOPT_TIMESTAMP	8	/* Better RTT estimations/PAWS */

/*
 *     TCP option lengths
 */

#define TCPOLEN_MSS            4
#define TCPOLEN_WINDOW         3
#define TCPOLEN_SACK_PERM      2
#define TCPOLEN_TIMESTAMP      10

/* But this is what stacks really send out. */
#define TCPOLEN_TSTAMP_ALIGNED		12
#define TCPOLEN_WSCALE_ALIGNED		4
#define TCPOLEN_SACKPERM_ALIGNED	4
#define TCPOLEN_SACK_BASE		2
#define TCPOLEN_SACK_BASE_ALIGNED	4
#define TCPOLEN_SACK_PERBLOCK		8

#define TIME_WRITE	1	/* Not yet used */
#define TIME_RETRANS	2	/* Retransmit timer */
#define TIME_DACK	3	/* Delayed ack timer */
#define TIME_PROBE0	4
#define TIME_KEEPOPEN	5

/* sysctl variables for tcp */
extern int sysctl_tcp_keepalive_time;
extern int sysctl_tcp_keepalive_probes;
extern int sysctl_tcp_keepalive_intvl;
extern int sysctl_tcp_syn_retries;

struct open_request;

struct or_calltable {
	int  family;
	void (*rtx_syn_ack)	(struct sock *sk, struct open_request *req);
	void (*send_ack)	(struct sk_buff *skb, struct open_request *req);
	void (*destructor)	(struct open_request *req);
	void (*send_reset)	(struct sk_buff *skb);
};

struct tcp_v4_open_req {
	__u32			loc_addr;
	__u32			rmt_addr;
	struct ip_options	*opt;
};

#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
struct tcp_v6_open_req {
	struct in6_addr		loc_addr;
	struct in6_addr		rmt_addr;
	struct sk_buff		*pktopts;
	int			iif;
};
#endif

/* this structure is too big */
struct open_request {
	struct open_request	*dl_next; /* Must be first member! */
	__u32			rcv_isn;
	__u32			snt_isn;
	__u16			rmt_port;
	__u16			mss;
	__u8			retrans;
	__u8			__pad;
	unsigned snd_wscale : 4, 
		rcv_wscale : 4, 
		tstamp_ok : 1,
		sack_ok : 1,
		wscale_ok : 1;
	/* The following two fields can be easily recomputed I think -AK */
	__u32			window_clamp;	/* window clamp at creation time */
	__u32			rcv_wnd;	/* rcv_wnd offered first time */
	__u32			ts_recent;
	unsigned long		expires;
	struct or_calltable	*class;
	struct sock		*sk;
	union {
		struct tcp_v4_open_req v4_req;
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
		struct tcp_v6_open_req v6_req;
#endif
	} af;
};

/* SLAB cache for open requests. */
extern kmem_cache_t *tcp_openreq_cachep;

#define tcp_openreq_alloc()	kmem_cache_alloc(tcp_openreq_cachep, SLAB_ATOMIC)
#define tcp_openreq_free(req)	kmem_cache_free(tcp_openreq_cachep, req)

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#define TCP_INET_FAMILY(fam) ((fam) == AF_INET)
#else
#define TCP_INET_FAMILY(fam) 1
#endif

/*
 *	Pointers to address related TCP functions
 *	(i.e. things that depend on the address family)
 *
 * 	BUGGG_FUTURE: all the idea behind this struct is wrong.
 *	It mixes socket frontend with transport function.
 *	With port sharing between IPv6/v4 it gives the only advantage,
 *	only poor IPv6 needs to permanently recheck, that it
 *	is still IPv6 8)8) It must be cleaned up as soon as possible.
 *						--ANK (980802)
 */

struct tcp_func {
	int			(*queue_xmit)		(struct sk_buff *skb);

	void			(*send_check)		(struct sock *sk,
							 struct tcphdr *th,
							 int len,
							 struct sk_buff *skb);

	int			(*rebuild_header)	(struct sock *sk);

	int			(*conn_request)		(struct sock *sk,
							 struct sk_buff *skb);

	struct sock *		(*syn_recv_sock)	(struct sock *sk,
							 struct sk_buff *skb,
							 struct open_request *req,
							 struct dst_entry *dst);
	
	int			(*hash_connecting)	(struct sock *sk);

	__u16			net_header_len;



	int			(*setsockopt)		(struct sock *sk, 
							 int level, 
							 int optname, 
							 char *optval, 
							 int optlen);

	int			(*getsockopt)		(struct sock *sk, 
							 int level, 
							 int optname, 
							 char *optval, 
							 int *optlen);


	void			(*addr2sockaddr)	(struct sock *sk,
							 struct sockaddr *);

	int sockaddr_len;
};

/*
 * The next routines deal with comparing 32 bit unsigned ints
 * and worry about wraparound (automatic with unsigned arithmetic).
 */

extern __inline int before(__u32 seq1, __u32 seq2)
{
        return (__s32)(seq1-seq2) < 0;
}

extern __inline int after(__u32 seq1, __u32 seq2)
{
	return (__s32)(seq2-seq1) < 0;
}


/* is s2<=s1<=s3 ? */
extern __inline int between(__u32 seq1, __u32 seq2, __u32 seq3)
{
	return seq3 - seq2 >= seq1 - seq2;
}


extern struct proto tcp_prot;
extern struct tcp_mib tcp_statistics;

extern void			tcp_put_port(struct sock *sk);
extern void			__tcp_put_port(struct sock *sk);
extern void			tcp_inherit_port(struct sock *sk, struct sock *child);

extern void			tcp_v4_err(struct sk_buff *skb,
					   unsigned char *, int);

extern void			tcp_shutdown (struct sock *sk, int how);

extern int			tcp_v4_rcv(struct sk_buff *skb,
					   unsigned short len);

extern int			tcp_do_sendmsg(struct sock *sk, struct msghdr *msg);

extern int			tcp_ioctl(struct sock *sk, 
					  int cmd, 
					  unsigned long arg);

extern int			tcp_rcv_state_process(struct sock *sk, 
						      struct sk_buff *skb,
						      struct tcphdr *th,
						      unsigned len);

extern int			tcp_rcv_established(struct sock *sk, 
						    struct sk_buff *skb,
						    struct tcphdr *th, 
						    unsigned len);

enum tcp_tw_status
{
	TCP_TW_SUCCESS = 0,
	TCP_TW_RST = 1,
	TCP_TW_ACK = 2,
	TCP_TW_SYN = 3
};

extern enum tcp_tw_status	tcp_timewait_state_process(struct tcp_tw_bucket *tw,
							   struct sk_buff *skb,
							   struct tcphdr *th,
							   unsigned len);

extern struct sock *		tcp_check_req(struct sock *sk,struct sk_buff *skb,
					      struct open_request *req,
					      struct open_request *prev);

extern void			tcp_close(struct sock *sk, 
					  long timeout);
extern struct sock *		tcp_accept(struct sock *sk, int flags, int *err);
extern unsigned int		tcp_poll(struct file * file, struct socket *sock, struct poll_table_struct *wait);
extern void			tcp_write_space(struct sock *sk); 

extern int			tcp_getsockopt(struct sock *sk, int level, 
					       int optname, char *optval, 
					       int *optlen);
extern int			tcp_setsockopt(struct sock *sk, int level, 
					       int optname, char *optval, 
					       int optlen);
extern void			tcp_set_keepalive(struct sock *sk, int val);
extern int			tcp_recvmsg(struct sock *sk, 
					    struct msghdr *msg,
					    int len, int nonblock, 
					    int flags, int *addr_len);

extern void			tcp_parse_options(struct sock *sk, struct tcphdr *th,
						  struct tcp_opt *tp, int no_fancy);

/*
 *	TCP v4 functions exported for the inet6 API
 */

extern int		       	tcp_v4_rebuild_header(struct sock *sk);

extern int		       	tcp_v4_build_header(struct sock *sk, 
						    struct sk_buff *skb);

extern void		       	tcp_v4_send_check(struct sock *sk, 
						  struct tcphdr *th, int len, 
						  struct sk_buff *skb);

extern int			tcp_v4_conn_request(struct sock *sk,
						    struct sk_buff *skb);

extern struct sock *		tcp_create_openreq_child(struct sock *sk,
							 struct open_request *req,
							 struct sk_buff *skb);

extern struct sock *		tcp_v4_syn_recv_sock(struct sock *sk,
						     struct sk_buff *skb,
						     struct open_request *req,
							struct dst_entry *dst);

extern int			tcp_v4_do_rcv(struct sock *sk,
					      struct sk_buff *skb);

extern int			tcp_v4_connect(struct sock *sk,
					       struct sockaddr *uaddr,
					       int addr_len);

extern int			tcp_connect(struct sock *sk,
					    struct sk_buff *skb);

extern struct sk_buff *		tcp_make_synack(struct sock *sk,
						struct dst_entry *dst,
						struct open_request *req);

extern int			tcp_disconnect(struct sock *sk, int flags);

extern void			tcp_unhash(struct sock *sk);

extern int			tcp_v4_hash_connecting(struct sock *sk);


/* From syncookies.c */
extern struct sock *cookie_v4_check(struct sock *sk, struct sk_buff *skb, 
				    struct ip_options *opt);
extern __u32 cookie_v4_init_sequence(struct sock *sk, struct sk_buff *skb, 
				     __u16 *mss);

/* tcp_output.c */

extern void tcp_read_wakeup(struct sock *);
extern void tcp_write_xmit(struct sock *);
extern void tcp_time_wait(struct sock *);
extern int tcp_retransmit_skb(struct sock *, struct sk_buff *);
extern void tcp_fack_retransmit(struct sock *);
extern void tcp_xmit_retransmit_queue(struct sock *);
extern void tcp_simple_retransmit(struct sock *);

extern void tcp_send_probe0(struct sock *);
extern void tcp_send_partial(struct sock *);
extern void tcp_write_wakeup(struct sock *);
extern void tcp_send_fin(struct sock *sk);
extern void tcp_send_active_reset(struct sock *sk, int priority);
extern int  tcp_send_synack(struct sock *);
extern void tcp_transmit_skb(struct sock *, struct sk_buff *);
extern void tcp_send_skb(struct sock *, struct sk_buff *, int force_queue);
extern void tcp_send_ack(struct sock *sk);
extern void tcp_send_delayed_ack(struct sock *sk, int max_timeout);

/* tcp_timer.c */
extern void tcp_reset_xmit_timer(struct sock *, int, unsigned long);
extern void tcp_init_xmit_timers(struct sock *);
extern void tcp_clear_xmit_timers(struct sock *);

extern void tcp_retransmit_timer(unsigned long);
extern void tcp_delack_timer(unsigned long);
extern void tcp_probe_timer(unsigned long);

extern void tcp_delete_keepalive_timer (struct sock *);
extern void tcp_reset_keepalive_timer (struct sock *, unsigned long);
extern void tcp_keepalive_timer (unsigned long);

/*
 *	TCP slow timer
 */
extern struct timer_list	tcp_slow_timer;

struct tcp_sl_timer {
	atomic_t	count;
	unsigned long	period;
	unsigned long	last;
	void (*handler)	(unsigned long);
};

#define TCP_SLT_SYNACK		0
#define TCP_SLT_TWKILL		1
#define TCP_SLT_MAX		2

extern struct tcp_sl_timer tcp_slt_array[TCP_SLT_MAX];
 
extern int tcp_sync_mss(struct sock *sk, u32 pmtu);

/* Compute the current effective MSS, taking SACKs and IP options,
 * and even PMTU discovery events into account.
 */

static __inline__ unsigned int tcp_current_mss(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	struct dst_entry *dst = sk->dst_cache;
	int mss_now = tp->mss_cache; 

	if (dst && dst->pmtu != tp->pmtu_cookie)
		mss_now = tcp_sync_mss(sk, dst->pmtu);

	if(tp->sack_ok && tp->num_sacks)
		mss_now -= (TCPOLEN_SACK_BASE_ALIGNED +
			    (tp->num_sacks * TCPOLEN_SACK_PERBLOCK));
	return mss_now > 8 ? mss_now : 8;
}

/* Initialize RCV_MSS value.
 * RCV_MSS is an our guess about MSS used by the peer.
 * We haven't any direct information about the MSS.
 * It's better to underestimate the RCV_MSS rather than overestimate.
 * Overestimations make us ACKing less frequently than needed.
 * Underestimations are more easy to detect and fix by tcp_measure_rcv_mss().
 */

extern __inline__ void tcp_initialize_rcv_mss(struct sock *sk)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	struct dst_entry *dst = __sk_dst_get(sk);
	int mss;

	if (dst)
		mss = dst->advmss;
	else
		mss = tp->mss_cache;

	tp->rcv_mss = max(min(mss, 536), 8);
}

/* Compute the actual receive window we are currently advertising.
 * Rcv_nxt can be after the window if our peer push more data
 * than the offered window.
 */
static __inline__ u32 tcp_receive_window(struct tcp_opt *tp)
{
	s32 win = tp->rcv_wup + tp->rcv_wnd - tp->rcv_nxt;

	if (win < 0)
		win = 0;
	return (u32) win;
}

/* Choose a new window, without checks for shrinking, and without
 * scaling applied to the result.  The caller does these things
 * if necessary.  This is a "raw" window selection.
 */
extern u32	__tcp_select_window(struct sock *sk);

/* Chose a new window to advertise, update state in tcp_opt for the
 * socket, and return result with RFC1323 scaling applied.  The return
 * value can be stuffed directly into th->window for an outgoing
 * frame.
 */
extern __inline__ u16 tcp_select_window(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	u32 cur_win = tcp_receive_window(tp);
	u32 new_win = __tcp_select_window(sk);

	/* Never shrink the offered window */
	if(new_win < cur_win) {
		/* Danger Will Robinson!
		 * Don't update rcv_wup/rcv_wnd here or else
		 * we will not be able to advertise a zero
		 * window in time.  --DaveM
		 */
		new_win = cur_win;
	} else {
		tp->rcv_wnd = new_win;
		tp->rcv_wup = tp->rcv_nxt;
	}

	/* RFC1323 scaling applied */
	return new_win >> tp->rcv_wscale;
}

/* See if we can advertise non-zero, and if so how much we
 * can increase our advertisement.  If it becomes more than
 * twice what we are talking about right now, return true.
 */
extern __inline__ int tcp_raise_window(struct sock *sk)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	u32 cur_win = tcp_receive_window(tp);
	u32 new_win = __tcp_select_window(sk);

	return (new_win && (new_win > (cur_win << 1)));
}


/* TCP timestamps are only 32-bits, this causes a slight
 * complication on 64-bit systems since we store a snapshot
 * of jiffies in the buffer control blocks below.  We decidely
 * only use of the low 32-bits of jiffies and hide the ugly
 * casts with the following macro.
 */
#define tcp_time_stamp		((__u32)(jiffies))

/* This is what the send packet queueing engine uses to pass
 * TCP per-packet control information to the transmission
 * code.  We also store the host-order sequence numbers in
 * here too.  This is 36 bytes on 32-bit architectures,
 * 40 bytes on 64-bit machines, if this grows please adjust
 * skbuff.h:skbuff->cb[xxx] size appropriately.
 */
struct tcp_skb_cb {
	union {
		struct inet_skb_parm	h4;
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
		struct inet6_skb_parm	h6;
#endif
	} header;	/* For incoming frames		*/
	__u32		seq;		/* Starting sequence number	*/
	__u32		end_seq;	/* SEQ + FIN + SYN + datalen	*/
	__u32		when;		/* used to compute rtt's	*/
	__u8		flags;		/* TCP header flags.		*/

	/* NOTE: These must match up to the flags byte in a
	 *       real TCP header.
	 */
#define TCPCB_FLAG_FIN		0x01
#define TCPCB_FLAG_SYN		0x02
#define TCPCB_FLAG_RST		0x04
#define TCPCB_FLAG_PSH		0x08
#define TCPCB_FLAG_ACK		0x10
#define TCPCB_FLAG_URG		0x20

	__u8		sacked;		/* State flags for SACK/FACK.	*/
#define TCPCB_SACKED_ACKED	0x01	/* SKB ACK'd by a SACK block	*/
#define TCPCB_SACKED_RETRANS	0x02	/* SKB retransmitted		*/

	__u16		urg_ptr;	/* Valid w/URG flags is set.	*/
	__u32		ack_seq;	/* Sequence number ACK'd	*/
};

#define TCP_SKB_CB(__skb)	((struct tcp_skb_cb *)&((__skb)->cb[0]))

/* This determines how many packets are "in the network" to the best
 * of our knowledge.  In many cases it is conservative, but where
 * detailed information is available from the receiver (via SACK
 * blocks etc.) we can make more aggressive calculations.
 *
 * Use this for decisions involving congestion control, use just
 * tp->packets_out to determine if the send queue is empty or not.
 *
 * Read this equation as:
 *
 *	"Packets sent once on transmission queue" MINUS
 *	"Packets acknowledged by FACK information" PLUS
 *	"Packets fast retransmitted"
 */
static __inline__ int tcp_packets_in_flight(struct tcp_opt *tp)
{
	return tp->packets_out - tp->fackets_out + tp->retrans_out;
}

/* Recalculate snd_ssthresh, we want to set it to:
 *
 * 	one half the current congestion window, but no
 *	less than two segments
 *
 * We must take into account the current send window
 * as well, however we keep track of that using different
 * units so a conversion is necessary.  -DaveM
 *
 * RED-PEN.
 *  RFC 2581: "an easy mistake to make is to simply use cwnd,
 *             rather than FlightSize"
 * I see no references to FlightSize here. snd_wnd is not FlightSize,
 * it is also apriory characteristics.
 *
 *   FlightSize = min((snd_nxt-snd_una)/mss, packets_out) ?
 */
extern __inline__ __u32 tcp_recalc_ssthresh(struct tcp_opt *tp)
{
	u32 FlightSize = (tp->snd_nxt - tp->snd_una)/tp->mss_cache;

	FlightSize = min(FlightSize, tcp_packets_in_flight(tp));

	return max(min(FlightSize, tp->snd_cwnd) >> 1, 2);
}

/* This checks if the data bearing packet SKB (usually tp->send_head)
 * should be put on the wire right now.
 */
static __inline__ int tcp_snd_test(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
	int nagle_check = 1;

	/*	RFC 1122 - section 4.2.3.4
	 *
	 *	We must queue if
	 *
	 *	a) The right edge of this frame exceeds the window
	 *	b) There are packets in flight and we have a small segment
	 *	   [SWS avoidance and Nagle algorithm]
	 *	   (part of SWS is done on packetization)
	 *	c) We are retransmiting [Nagle]
	 *	d) We have too many packets 'in flight'
	 *
	 * 	Don't use the nagle rule for urgent data (or
	 *	for the final FIN -DaveM).
	 */
	if ((sk->nonagle == 2 && (skb->len < tp->mss_cache)) ||
	    (!sk->nonagle &&
	     skb->len < (tp->mss_cache >> 1) &&
	     tp->packets_out &&
	     !(TCP_SKB_CB(skb)->flags & (TCPCB_FLAG_URG|TCPCB_FLAG_FIN))))
		nagle_check = 0;

	/*
	 * Reset CWND after idle period longer rto. Actually, it would
	 * be better to save last send time, but VJ in SIGCOMM'88 proposes
	 * to use keepalive timestamp. Well, it is not good, certainly,
	 * because SMTP is still broken, but it is better than nothing yet.
	 */
	if (tp->packets_out==0 && (s32)(tcp_time_stamp - tp->rcv_tstamp) > tp->rto)
		tp->snd_cwnd = min(tp->snd_cwnd, 2);

	/* Don't be strict about the congestion window for the
	 * final FIN frame.  -DaveM
	 */
	return (nagle_check &&
		((tcp_packets_in_flight(tp) < tp->snd_cwnd) ||
		 (TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN)) &&
		!after(TCP_SKB_CB(skb)->end_seq, tp->snd_una + tp->snd_wnd) &&
		tp->retransmits == 0);
}

/* Push out any pending frames which were held back due to
 * TCP_CORK or attempt at coalescing tiny packets.
 * The socket must be locked by the caller.
 */
static __inline__ void tcp_push_pending_frames(struct sock *sk, struct tcp_opt *tp)
{
	if(tp->send_head) {
		if(tcp_snd_test(sk, tp->send_head))
			tcp_write_xmit(sk);
		else if(tp->packets_out == 0 && !tp->pending) {
			/* We held off on this in tcp_send_skb() */
			tp->pending = TIME_PROBE0;
			tcp_reset_xmit_timer(sk, TIME_PROBE0, tp->rto);
		}
	}
}

/* This tells the input processing path that an ACK should go out
 * right now.
 */
#define tcp_enter_quickack_mode(__tp)	((__tp)->ato |= (1<<31))
#define tcp_exit_quickack_mode(__tp)	((__tp)->ato &= ~(1<<31))
#define tcp_in_quickack_mode(__tp)	(((__tp)->ato & (1 << 31)) != 0)

/*
 * List all states of a TCP socket that can be viewed as a "connected"
 * state.  This now includes TCP_SYN_RECV, although I am not yet fully
 * convinced that this is the solution for the 'getpeername(2)'
 * problem. Thanks to Stephen A. Wood <saw@cebaf.gov>  -FvK
 */

extern __inline const int tcp_connected(const int state)
{
	return ((1 << state) &
	       	(TCPF_ESTABLISHED|TCPF_CLOSE_WAIT|TCPF_FIN_WAIT1|
		 TCPF_FIN_WAIT2|TCPF_SYN_RECV));
}

extern __inline const int tcp_established(const int state)
{
	return ((1 << state) &
	       	(TCPF_ESTABLISHED|TCPF_CLOSE_WAIT|TCPF_FIN_WAIT1|
		 TCPF_FIN_WAIT2));
}


extern void			tcp_destroy_sock(struct sock *sk);


/*
 * Calculate(/check) TCP checksum
 */
static __inline__ u16 tcp_v4_check(struct tcphdr *th, int len,
				   unsigned long saddr, unsigned long daddr, 
				   unsigned long base)
{
	return csum_tcpudp_magic(saddr,daddr,len,IPPROTO_TCP,base);
}

#undef STATE_TRACE

#ifdef STATE_TRACE
static char *statename[]={
	"Unused","Established","Syn Sent","Syn Recv",
	"Fin Wait 1","Fin Wait 2","Time Wait", "Close",
	"Close Wait","Last ACK","Listen","Closing"
};
#endif

static __inline__ void tcp_set_state(struct sock *sk, int state)
{
	int oldstate = sk->state;

	switch (state) {
	case TCP_ESTABLISHED:
		if (oldstate != TCP_ESTABLISHED)
			tcp_statistics.TcpCurrEstab++;
		break;

	case TCP_CLOSE:
		sk->prot->unhash(sk);
		/* fall through */
	default:
		if (oldstate==TCP_ESTABLISHED)
			tcp_statistics.TcpCurrEstab--;
	}

	/* Change state AFTER socket is unhashed to avoid closed
	 * socket sitting in hash tables.
	 */
	sk->state = state;

#ifdef STATE_TRACE
	SOCK_DEBUG(sk, "TCP sk=%p, State %s -> %s\n",sk, statename[oldstate],statename[state]);
#endif	
}

static __inline__ void tcp_done(struct sock *sk)
{
	sk->shutdown = SHUTDOWN_MASK;

	if (!sk->dead) 
		sk->state_change(sk);
	else
		tcp_destroy_sock(sk);
}

static __inline__ void tcp_build_and_update_options(__u32 *ptr, struct tcp_opt *tp, __u32 tstamp)
{
	if (tp->tstamp_ok) {
		*ptr++ = __constant_htonl((TCPOPT_NOP << 24) |
					  (TCPOPT_NOP << 16) |
					  (TCPOPT_TIMESTAMP << 8) |
					  TCPOLEN_TIMESTAMP);
		*ptr++ = htonl(tstamp);
		*ptr++ = htonl(tp->ts_recent);
	}
	if(tp->sack_ok && tp->num_sacks) {
		int this_sack;

		*ptr++ = __constant_htonl((TCPOPT_NOP << 24) |
					  (TCPOPT_NOP << 16) |
					  (TCPOPT_SACK << 8) |
					  (TCPOLEN_SACK_BASE +
					   (tp->num_sacks * TCPOLEN_SACK_PERBLOCK)));
		for(this_sack = 0; this_sack < tp->num_sacks; this_sack++) {
			*ptr++ = htonl(tp->selective_acks[this_sack].start_seq);
			*ptr++ = htonl(tp->selective_acks[this_sack].end_seq);
		}
	}
}

/* Construct a tcp options header for a SYN or SYN_ACK packet.
 * If this is every changed make sure to change the definition of
 * MAX_SYN_SIZE to match the new maximum number of options that you
 * can generate.
 */
extern __inline__ void tcp_syn_build_options(__u32 *ptr, int mss, int ts, int sack,
					     int offer_wscale, int wscale, __u32 tstamp, __u32 ts_recent)
{
	/* We always get an MSS option.
	 * The option bytes which will be seen in normal data
	 * packets should timestamps be used, must be in the MSS
	 * advertised.  But we subtract them from tp->mss_cache so
	 * that calculations in tcp_sendmsg are simpler etc.
	 * So account for this fact here if necessary.  If we
	 * don't do this correctly, as a receiver we won't
	 * recognize data packets as being full sized when we
	 * should, and thus we won't abide by the delayed ACK
	 * rules correctly.
	 * SACKs don't matter, we never delay an ACK when we
	 * have any of those going out.
	 */
	*ptr++ = htonl((TCPOPT_MSS << 24) | (TCPOLEN_MSS << 16) | mss);
	if (ts) {
		if(sack)
			*ptr++ = __constant_htonl((TCPOPT_SACK_PERM << 24) | (TCPOLEN_SACK_PERM << 16) |
						  (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP);
		else
			*ptr++ = __constant_htonl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16) |
						  (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP);
		*ptr++ = htonl(tstamp);		/* TSVAL */
		*ptr++ = htonl(ts_recent);	/* TSECR */
	} else if(sack)
		*ptr++ = __constant_htonl((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16) |
					  (TCPOPT_SACK_PERM << 8) | TCPOLEN_SACK_PERM);
	if (offer_wscale)
		*ptr++ = htonl((TCPOPT_NOP << 24) | (TCPOPT_WINDOW << 16) | (TCPOLEN_WINDOW << 8) | (wscale));
}

/* Determine a window scaling and initial window to offer.
 * Based on the assumption that the given amount of space
 * will be offered. Store the results in the tp structure.
 * NOTE: for smooth operation initial space offering should
 * be a multiple of mss if possible. We assume here that mss >= 1.
 * This MUST be enforced by all callers.
 */
extern __inline__ void tcp_select_initial_window(int space, __u32 mss,
	__u32 *rcv_wnd,
	__u32 *window_clamp,
	int wscale_ok,
	__u8 *rcv_wscale)
{
	/* If no clamp set the clamp to the max possible scaled window */
	if (*window_clamp == 0)
		(*window_clamp) = (65535<<14);
	space = min(*window_clamp,space);

	/* Quantize space offering to a multiple of mss if possible. */
	if (space > mss)
		space = (space/mss)*mss;

	/* NOTE: offering an initial window larger than 32767
	 * will break some buggy TCP stacks. We try to be nice.
	 * If we are not window scaling, then this truncates
	 * our initial window offering to 32k. There should also
	 * be a sysctl option to stop being nice.
	 */
	(*rcv_wnd) = min(space, MAX_WINDOW);
	(*rcv_wscale) = 0;
	if (wscale_ok) {
		/* See RFC1323 for an explanation of the limit to 14 */
		while (space > 65535 && (*rcv_wscale) < 14) {
			space >>= 1;
			(*rcv_wscale)++;
		}
	}
	/* Set the clamp no higher than max representable value */
	(*window_clamp) = min(65535<<(*rcv_wscale),*window_clamp);
}

/* Note: caller must be prepared to deal with negative returns */ 
extern __inline__ int tcp_space(struct sock *sk)
{
	return (sk->rcvbuf - atomic_read(&sk->rmem_alloc)) / 
		WINDOW_ADVERTISE_DIVISOR; 
} 

extern __inline__ int tcp_full_space( struct sock *sk)
{
	return sk->rcvbuf / WINDOW_ADVERTISE_DIVISOR; 
}

extern __inline__ void tcp_synq_unlink(struct tcp_opt *tp, struct open_request *req, struct open_request *prev)
{
	if(!req->dl_next)
		tp->syn_wait_last = (struct open_request **)prev;
	prev->dl_next = req->dl_next;
}

extern __inline__ void tcp_synq_queue(struct tcp_opt *tp, struct open_request *req)
{ 
	req->dl_next = NULL;
	*tp->syn_wait_last = req; 
	tp->syn_wait_last = &req->dl_next;
}

extern __inline__ void tcp_synq_init(struct tcp_opt *tp)
{
	tp->syn_wait_queue = NULL;
	tp->syn_wait_last = &tp->syn_wait_queue;
}

extern void __tcp_inc_slow_timer(struct tcp_sl_timer *slt);
extern __inline__ void tcp_inc_slow_timer(int timer)
{
	struct tcp_sl_timer *slt = &tcp_slt_array[timer];
	
	if (atomic_read(&slt->count) == 0)
	{
		__tcp_inc_slow_timer(slt);
	}

	atomic_inc(&slt->count);
}

extern __inline__ void tcp_dec_slow_timer(int timer)
{
	struct tcp_sl_timer *slt = &tcp_slt_array[timer];

	atomic_dec(&slt->count);
}

extern const char timer_bug_msg[];

static inline void tcp_clear_xmit_timer(struct sock *sk, int what)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	struct timer_list *timer;
	
	switch (what) {
	case TIME_RETRANS:
		timer = &tp->retransmit_timer;
		break;
	case TIME_DACK:
		timer = &tp->delack_timer;
		break;
	case TIME_PROBE0:
		timer = &tp->probe_timer;
		break;	
	default:
		printk(timer_bug_msg);
		return;
	};

	spin_lock_bh(&sk->timer_lock);
	if (timer->prev != NULL && del_timer(timer))
		__sock_put(sk);
	spin_unlock_bh(&sk->timer_lock);
}

/* This function does not return reliable answer. You is only as advice.
 */

static inline int tcp_timer_is_set(struct sock *sk, int what)
{
	struct tcp_opt *tp = &sk->tp_pinfo.af_tcp;
	int ret;

	switch (what) {
	case TIME_RETRANS:
		ret = tp->retransmit_timer.prev != NULL;
		break;
	case TIME_DACK:
		ret = tp->delack_timer.prev != NULL;
		break;
	case TIME_PROBE0:
		ret = tp->probe_timer.prev != NULL;
		break;	
	default:
		ret = 0;
		printk(timer_bug_msg);
	};
	return ret;
}


extern void tcp_listen_wlock(void);

/* - We may sleep inside this lock.
 * - If sleeping is not required (or called from BH),
 *   use plain read_(un)lock(&tcp_lhash_lock).
 */

extern __inline__ void tcp_listen_lock(void)
{
	/* read_lock synchronizes to candidates to writers */
	read_lock(&tcp_lhash_lock);
	atomic_inc(&tcp_lhash_users);
	read_unlock(&tcp_lhash_lock);
}

extern __inline__ void tcp_listen_unlock(void)
{
	if (atomic_dec_and_test(&tcp_lhash_users))
		wake_up(&tcp_lhash_wait);
}

static inline int keepalive_intvl_when(struct tcp_opt *tp)
{
	if (tp->keepalive_intvl)
		return tp->keepalive_intvl;
	else
		return sysctl_tcp_keepalive_intvl;
}

static inline int keepalive_time_when(struct tcp_opt *tp)
{
	if (tp->keepalive_time)
		return tp->keepalive_time;
	else
		return sysctl_tcp_keepalive_time;
}

#endif	/* _TCP_H */
