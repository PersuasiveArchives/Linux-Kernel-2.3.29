/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the AF_INET socket handler.
 *
 * Version:	@(#)sock.h	1.0.4	05/13/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche <flla@stud.uni-sb.de>
 *
 * Fixes:
 *		Alan Cox	:	Volatiles in skbuff pointers. See
 *					skbuff comments. May be overdone,
 *					better to prove they can be removed
 *					than the reverse.
 *		Alan Cox	:	Added a zapped field for tcp to note
 *					a socket is reset and must stay shut up
 *		Alan Cox	:	New fields for options
 *	Pauline Middelink	:	identd support
 *		Alan Cox	:	Eliminate low level recv/recvfrom
 *		David S. Miller	:	New socket lookup architecture.
 *              Steve Whitehouse:       Default routines for sock_ops
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _SOCK_H
#define _SOCK_H

#include <linux/config.h>
#include <linux/timer.h>
#include <linux/in.h>		/* struct sockaddr_in */

#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
#include <linux/in6.h>		/* struct sockaddr_in6 */
#include <linux/ipv6.h>		/* dest_cache, inet6_options */
#include <linux/icmpv6.h>
#include <net/if_inet6.h>	/* struct ipv6_mc_socklist */
#endif

#if defined(CONFIG_INET) || defined (CONFIG_INET_MODULE)
#include <linux/icmp.h>
#endif
#include <linux/tcp.h>		/* struct tcphdr */

#include <linux/netdevice.h>
#include <linux/skbuff.h>	/* struct sk_buff */
#include <net/protocol.h>		/* struct inet_protocol */
#if defined(CONFIG_X25) || defined(CONFIG_X25_MODULE)
#include <net/x25.h>
#endif
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
#include <net/ax25.h>
#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
#include <net/netrom.h>
#endif
#if defined(CONFIG_ROSE) || defined(CONFIG_ROSE_MODULE)
#include <net/rose.h>
#endif
#endif

#if defined(CONFIG_IPX) || defined(CONFIG_IPX_MODULE)
#if defined(CONFIG_SPX) || defined(CONFIG_SPX_MODULE)
#include <net/spx.h>
#else
#include <net/ipx.h>
#endif /* CONFIG_SPX */
#endif /* CONFIG_IPX */

#if defined(CONFIG_ATALK) || defined(CONFIG_ATALK_MODULE)
#include <linux/atalk.h>
#endif

#if defined(CONFIG_DECNET) || defined(CONFIG_DECNET_MODULE)
#include <net/dn.h>
#endif

#if defined(CONFIG_IRDA) || defined(CONFIG_IRDA_MODULE)
#include <net/irda/irda.h>
#endif

#if defined(CONFIG_ATM) || defined(CONFIG_ATM_MODULE)
struct atm_vcc;
#endif

#ifdef CONFIG_FILTER
#include <linux/filter.h>
#endif

#include <asm/atomic.h>
#include <net/dst.h>

#define MIN_WRITE_SPACE	2048

/* The AF_UNIX specific socket options */
struct unix_opt {
	struct unix_address	*addr;
	struct dentry *		dentry;
	struct semaphore	readsem;
	struct sock *		other;
	struct sock **		list;
	struct sock *		gc_tree;
	atomic_t		inflight;
	rwlock_t		lock;
	wait_queue_head_t	peer_wait;
};


/* Once the IPX ncpd patches are in these are going into protinfo. */
#if defined(CONFIG_IPX) || defined(CONFIG_IPX_MODULE)
struct ipx_opt {
	ipx_address		dest_addr;
	ipx_interface		*intrfc;
	unsigned short		port;
#ifdef CONFIG_IPX_INTERN
	unsigned char           node[IPX_NODE_LEN];
#endif
	unsigned short		type;
/* 
 * To handle special ncp connection-handling sockets for mars_nwe,
 * the connection number must be stored in the socket.
 */
	unsigned short		ipx_ncp_conn;
};
#endif

#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
struct ipv6_pinfo {
	struct in6_addr 	saddr;
	struct in6_addr 	rcv_saddr;
	struct in6_addr		daddr;
	struct in6_addr		*daddr_cache;

	__u32			flow_label;
	__u32			frag_size;
	int			hop_limit;
	int			mcast_hops;
	int			mcast_oif;

	/* pktoption flags */
	union {
		struct {
			__u8	srcrt:2,
			        rxinfo:1,
				rxhlim:1,
				hopopts:1,
				dstopts:1,
                                authhdr:1,
                                rxflow:1;
		} bits;
		__u8		all;
	} rxopt;

	/* sockopt flags */
	__u8			mc_loop:1,
	                        recverr:1,
	                        sndflow:1,
	                        pmtudisc:2;

	struct ipv6_mc_socklist	*ipv6_mc_list;
	struct ipv6_fl_socklist *ipv6_fl_list;
	__u32			dst_cookie;

	struct ipv6_txoptions	*opt;
	struct sk_buff		*pktoptions;
};

struct raw6_opt {
	__u32			checksum;	/* perform checksum */
	__u32			offset;		/* checksum offset  */

	struct icmp6_filter	filter;
};

#endif /* IPV6 */

#if defined(CONFIG_INET) || defined(CONFIG_INET_MODULE)
struct raw_opt {
	struct icmp_filter	filter;
};
#endif

#if defined(CONFIG_INET) || defined (CONFIG_INET_MODULE)
struct inet_opt
{
	int			ttl;			/* TTL setting */
	int			tos;			/* TOS */
	unsigned	   	cmsg_flags;
	struct ip_options	*opt;
	unsigned char		hdrincl;		/* Include headers ? */
	__u8			mc_ttl;			/* Multicasting TTL */
	__u8			mc_loop;		/* Loopback */
	__u8			recverr;
	__u8			pmtudisc;
	int			mc_index;		/* Multicast device index */
	__u32			mc_addr;
	struct ip_mc_socklist	*mc_list;		/* Group array */
};
#endif


/* This defines a selective acknowledgement block. */
struct tcp_sack_block {
	__u32	start_seq;
	__u32	end_seq;
};

struct tcp_opt {
	int	tcp_header_len;	/* Bytes of tcp header to send		*/

/*
 *	Header prediction flags
 *	0x5?10 << 16 + snd_wnd in net byte order
 */
	__u32	pred_flags;

/*
 *	RFC793 variables by their proper names. This means you can
 *	read the code and the spec side by side (and laugh ...)
 *	See RFC793 and RFC1122. The RFC writes these in capitals.
 */
 	__u32	rcv_nxt;	/* What we want to receive next 	*/
 	__u32	snd_nxt;	/* Next sequence we send		*/

 	__u32	snd_una;	/* First byte we want an ack for	*/
	__u32	rcv_tstamp;	/* timestamp of last received packet	*/
	__u32	lrcvtime;	/* timestamp of last received data packet*/
	__u32	srtt;		/* smothed round trip time << 3		*/

	__u32	ato;		/* delayed ack timeout			*/
	__u32	snd_wl1;	/* Sequence for window update		*/

	__u32	snd_wl2;	/* Ack sequence for update		*/
	__u32	snd_wnd;	/* The window we expect to receive	*/
	__u32	max_window;
	__u32	pmtu_cookie;	/* Last pmtu seen by socket		*/
	__u16	mss_cache;	/* Cached effective mss, not including SACKS */
	__u16	mss_clamp;	/* Maximal mss, negotiated at connection setup */
	__u16	ext_header_len;	/* Dave, do you allow mw to use this hole? 8) --ANK */
	__u8	pending;	/* pending events			*/
	__u8	retransmits;
	__u32	last_ack_sent;	/* last ack we sent			*/

	__u32	backoff;	/* backoff				*/
	__u32	mdev;		/* medium deviation			*/
 	__u32	snd_cwnd;	/* Sending congestion window		*/
	__u32	rto;		/* retransmit timeout			*/

	__u32	packets_out;	/* Packets which are "in flight"	*/
	__u32	fackets_out;	/* Non-retrans SACK'd packets		*/
	__u32	retrans_out;	/* Fast-retransmitted packets out	*/
	__u32	high_seq;	/* snd_nxt at onset of congestion	*/
/*
 *	Slow start and congestion control (see also Nagle, and Karn & Partridge)
 */
 	__u32	snd_ssthresh;	/* Slow start size threshold		*/
 	__u16	snd_cwnd_cnt;	/* Linear increase counter		*/
	__u16	snd_cwnd_clamp; /* Do not allow snd_cwnd to grow above this */
	__u8	dup_acks;	/* Consequetive duplicate acks seen from other end */
	__u8	delayed_acks;
	__u16	user_mss;  	/* mss requested by user in ioctl */

	/* Two commonly used timers in both sender and receiver paths. */
 	struct timer_list	retransmit_timer;	/* Resend (no ack)	*/
 	struct timer_list	delack_timer;		/* Ack delay 		*/

	struct sk_buff_head	out_of_order_queue; /* Out of order segments go here */

	struct tcp_func		*af_specific;	/* Operations which are AF_INET{4,6} specific	*/
	struct sk_buff		*send_head;	/* Front of stuff to transmit			*/
	struct sk_buff		*retrans_head;	/* retrans head can be 
						 * different to the head of
						 * write queue if we are doing
						 * fast retransmit
						 */

 	__u32	rcv_wnd;	/* Current receiver window		*/
	__u32	rcv_wup;	/* rcv_nxt on last window update sent	*/
	__u32	write_seq;
	__u32	copied_seq;
/*
 *      Options received (usually on last packet, some only on SYN packets).
 */
	char	tstamp_ok,	/* TIMESTAMP seen on SYN packet		*/
		wscale_ok,	/* Wscale seen on SYN packet		*/
		sack_ok;	/* SACK seen on SYN packet		*/
	char	saw_tstamp;	/* Saw TIMESTAMP on last packet		*/
        __u8	snd_wscale;	/* Window scaling received from sender	*/
        __u8	rcv_wscale;	/* Window scaling to send to receiver	*/
	__u8	rexmt_done;	/* Retransmitted up to send head?	*/
        __u32	rcv_tsval;	/* Time stamp value             	*/
        __u32	rcv_tsecr;	/* Time stamp echo reply        	*/
        __u32	ts_recent;	/* Time stamp to echo next		*/
        long	ts_recent_stamp;/* Time we stored ts_recent (for aging) */
	int	num_sacks;	/* Number of SACK blocks		*/
	struct tcp_sack_block selective_acks[4]; /* The SACKS themselves*/

 	struct timer_list	probe_timer;		/* Probes	*/
	__u32	window_clamp;	/* XXX Document this... -DaveM		*/
	__u32	probes_out;	/* unanswered 0 window probes		*/
	__u32	syn_seq;
	__u32	fin_seq;
	__u32	urg_seq;
	__u32	urg_data;

	__u32	last_seg_size;	/* Size of last incoming segment */
	__u32	rcv_mss;	/* MSS used for delayed ACK decisions */ 

	struct open_request	*syn_wait_queue;
	struct open_request	**syn_wait_last;

	int syn_backlog;	/* Backlog of received SYNs */
	int write_pending;
	
	unsigned int		keepalive_time;	  /* time before keep alive takes place */
	unsigned int		keepalive_intvl;  /* time interval between keep alive probes */
	unsigned char  		keepalive_probes; /* num of allowed keep alive probes */
	unsigned char		syn_retries;	  /* num of allowed syn retries */
};

 	
/*
 * This structure really needs to be cleaned up.
 * Most of it is for TCP, and not used by any of
 * the other protocols.
 */

/*
 * The idea is to start moving to a newer struct gradualy
 * 
 * IMHO the newer struct should have the following format:
 * 
 *	struct sock {
 *		sockmem [mem, proto, callbacks]
 *
 *		union or struct {
 *			ax25;
 *		} ll_pinfo;
 *	
 *		union {
 *			ipv4;
 *			ipv6;
 *			ipx;
 *			netrom;
 *			rose;
 * 			x25;
 *		} net_pinfo;
 *
 *		union {
 *			tcp;
 *			udp;
 *			spx;
 *			netrom;
 *		} tp_pinfo;
 *
 *	}
 *
 * The idea failed because IPv6 transition asssumes dual IP/IPv6 sockets.
 * So, net_pinfo is IPv6 are really, and protinfo unifies all another
 * private areas.
 */

/* Define this to get the sk->debug debugging facility. */
#define SOCK_DEBUGGING
#ifdef SOCK_DEBUGGING
#define SOCK_DEBUG(sk, msg...) do { if((sk) && ((sk)->debug)) printk(KERN_DEBUG ## msg); } while (0)
#else
#define SOCK_DEBUG(sk, msg...) do { } while (0)
#endif

/* This is the per-socket lock.  The spinlock provides a synchronization
 * between user contexts and software interrupt processing, whereas the
 * mini-semaphore synchronizes multiple users amongst themselves.
 */
typedef struct {
	spinlock_t		slock;
	unsigned int		users;
	wait_queue_head_t	wq;
} socket_lock_t;

#define sock_lock_init(__sk) \
do {	spin_lock_init(&((__sk)->lock.slock)); \
	(__sk)->lock.users = 0; \
	init_waitqueue_head(&((__sk)->lock.wq)); \
} while(0);

struct sock {
	/* Socket demultiplex comparisons on incoming packets. */
	__u32			daddr;		/* Foreign IPv4 addr			*/
	__u32			rcv_saddr;	/* Bound local IPv4 addr		*/
	__u16			dport;		/* Destination port			*/
	unsigned short		num;		/* Local port				*/
	int			bound_dev_if;	/* Bound device index if != 0		*/

	/* Main hash linkage for various protocol lookup tables. */
	struct sock		*next;
	struct sock		**pprev;
	struct sock		*bind_next;
	struct sock		**bind_pprev;

	volatile unsigned char	state,		/* Connection state			*/
				zapped;		/* In ax25 & ipx means not linked	*/
	__u16			sport;		/* Source port				*/

	unsigned short		family;		/* Address family			*/
	unsigned char		reuse,		/* SO_REUSEADDR setting			*/
				nonagle;	/* Disable Nagle algorithm?		*/
	atomic_t		refcnt;		/* Reference count			*/

	socket_lock_t		lock;		/* Synchronizer...			*/
	int			rcvbuf;		/* Size of receive buffer in bytes	*/

	wait_queue_head_t	*sleep;		/* Sock wait queue			*/
	struct dst_entry	*dst_cache;	/* Destination cache			*/
	rwlock_t		dst_lock;
	atomic_t		rmem_alloc;	/* Receive queue bytes committed	*/
	struct sk_buff_head	receive_queue;	/* Incoming packets			*/
	atomic_t		wmem_alloc;	/* Transmit queue bytes committed	*/
	struct sk_buff_head	write_queue;	/* Packet sending queue			*/
	atomic_t		omem_alloc;	/* "o" is "option" or "other" */
	__u32			saddr;		/* Sending source			*/
	unsigned int		allocation;	/* Allocation mode			*/
	int			sndbuf;		/* Size of send buffer in bytes		*/
	struct sock		*prev;

	/* Not all are volatile, but some are, so we might as well say they all are.
	 * XXX Make this a flag word -DaveM
	 */
	volatile char		dead,
				done,
				urginline,
				keepopen,
				linger,
				destroy,
				no_check,
				broadcast,
				bsdism;
	unsigned char		debug;
	int			proc;
	unsigned long	        lingertime;

	int			hashent;
	struct sock		*pair;

	/* The backlog queue is special, it is always used with
	 * the per-socket spinlock held and requires low latency
	 * access.  Therefore we special case it's implementation.
	 */
	struct {
		struct sk_buff *head;
		struct sk_buff *tail;
	} backlog;

	rwlock_t		callback_lock;

	/* Error queue, rarely used. */
	struct sk_buff_head	error_queue;

	struct proto		*prot;

	unsigned short		shutdown;

#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	union {
		struct ipv6_pinfo	af_inet6;
	} net_pinfo;
#endif

	union {
		struct tcp_opt		af_tcp;
#if defined(CONFIG_INET) || defined (CONFIG_INET_MODULE)
		struct raw_opt		tp_raw4;
#endif
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
		struct raw6_opt		tp_raw;
#endif /* CONFIG_IPV6 */
#if defined(CONFIG_SPX) || defined (CONFIG_SPX_MODULE)
		struct spx_opt		af_spx;
#endif /* CONFIG_SPX */

	} tp_pinfo;

	int			err, err_soft;	/* Soft holds errors that don't
						   cause failure but are the cause
						   of a persistent failure not just
						   'timed out' */
	unsigned short		ack_backlog;
	unsigned short		max_ack_backlog;
	__u32			priority;
	unsigned short		type;
	unsigned char		localroute;	/* Route locally only */
	unsigned char		protocol;
	struct ucred		peercred;

#ifdef CONFIG_FILTER
	/* Socket Filtering Instructions */
	struct sk_filter      	*filter;
#endif /* CONFIG_FILTER */

	/* This is where all the private (optional) areas that don't
	 * overlap will eventually live. 
	 */
	union {
		void *destruct_hook;
	  	struct unix_opt	af_unix;
#if defined(CONFIG_INET) || defined (CONFIG_INET_MODULE)
		struct inet_opt af_inet;
#endif
#if defined(CONFIG_ATALK) || defined(CONFIG_ATALK_MODULE)
		struct atalk_sock	af_at;
#endif
#if defined(CONFIG_IPX) || defined(CONFIG_IPX_MODULE)
		struct ipx_opt		af_ipx;
#endif
#if defined (CONFIG_DECNET) || defined(CONFIG_DECNET_MODULE)
		struct dn_scp           dn;
#endif
#if defined (CONFIG_PACKET) || defined(CONFIG_PACKET_MODULE)
		struct packet_opt	*af_packet;
#endif
#if defined(CONFIG_X25) || defined(CONFIG_X25_MODULE)
		x25_cb			*x25;
#endif
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
		ax25_cb			*ax25;
#endif
#if defined(CONFIG_NETROM) || defined(CONFIG_NETROM_MODULE)
		nr_cb			*nr;
#endif
#if defined(CONFIG_ROSE) || defined(CONFIG_ROSE_MODULE)
		rose_cb			*rose;
#endif
#ifdef CONFIG_NETLINK
		struct netlink_opt	*af_netlink;
#endif
#if defined(CONFIG_ECONET) || defined(CONFIG_ECONET_MODULE)
		struct econet_opt	*af_econet;
#endif
#if defined(CONFIG_ATM) || defined(CONFIG_ATM_MODULE)
		struct atm_vcc		*af_atm;
#endif
#if defined(CONFIG_IRDA) || defined(CONFIG_IRDA_MODULE)
		struct irda_sock        *irda;
#endif
	} protinfo;  		


	/* This part is used for the timeout functions. */
	spinlock_t		timer_lock;	/* Required until timer in core is repaired */
	struct timer_list	timer;		/* This is the sock cleanup timer. */
	struct timeval		stamp;

	/* Identd */
	struct socket		*socket;

	/* RPC layer private data */
	void			*user_data;
  
	/* Callbacks */
	void			(*state_change)(struct sock *sk);
	void			(*data_ready)(struct sock *sk,int bytes);
	void			(*write_space)(struct sock *sk);
	void			(*error_report)(struct sock *sk);

  	int			(*backlog_rcv) (struct sock *sk,
						struct sk_buff *skb);  
	void                    (*destruct)(struct sock *sk);
};

/* The per-socket spinlock must be held here. */
#define sk_add_backlog(__sk, __skb)			\
do {	if((__sk)->backlog.tail == NULL) {		\
		(__sk)->backlog.head =			\
		     (__sk)->backlog.tail = (__skb);	\
	} else {					\
		((__sk)->backlog.tail)->next = (__skb);	\
		(__sk)->backlog.tail = (__skb);		\
	}						\
	(__skb)->next = NULL;				\
} while(0)

/* IP protocol blocks we attach to sockets.
 * socket layer -> transport layer interface
 * transport -> network interface is defined by struct inet_proto
 */
struct proto {
	void			(*close)(struct sock *sk, 
					long timeout);
	int			(*connect)(struct sock *sk,
				        struct sockaddr *uaddr, 
					int addr_len);
	int			(*disconnect)(struct sock *sk, int flags);

	struct sock *		(*accept) (struct sock *sk, int flags, int *err);
	void			(*retransmit)(struct sock *sk, int all);
	void			(*write_wakeup)(struct sock *sk);
	void			(*read_wakeup)(struct sock *sk);

	unsigned int		(*poll)(struct file * file, struct socket *sock,
					struct poll_table_struct *wait);

	int			(*ioctl)(struct sock *sk, int cmd,
					 unsigned long arg);
	int			(*init)(struct sock *sk);
	int			(*destroy)(struct sock *sk);
	void			(*shutdown)(struct sock *sk, int how);
	int			(*setsockopt)(struct sock *sk, int level, 
					int optname, char *optval, int optlen);
	int			(*getsockopt)(struct sock *sk, int level, 
					int optname, char *optval, 
					int *option);  	 
	int			(*sendmsg)(struct sock *sk, struct msghdr *msg,
					   int len);
	int			(*recvmsg)(struct sock *sk, struct msghdr *msg,
					int len, int noblock, int flags, 
					int *addr_len);
	int			(*bind)(struct sock *sk, 
					struct sockaddr *uaddr, int addr_len);

	int			(*backlog_rcv) (struct sock *sk, 
						struct sk_buff *skb);

	/* Keeping track of sk's, looking them up, and port selection methods. */
	void			(*hash)(struct sock *sk);
	void			(*unhash)(struct sock *sk);
	int			(*get_port)(struct sock *sk, unsigned short snum);

	unsigned short		max_header;
	unsigned long		retransmits;
	char			name[32];
	int			inuse, highestinuse;
};


/* About 10 seconds */
#define SOCK_DESTROY_TIME (10*HZ)

/* Sockets 0-1023 can't be bound to unless you are superuser */
#define PROT_SOCK	1024

#define SHUTDOWN_MASK	3
#define RCV_SHUTDOWN	1
#define SEND_SHUTDOWN	2

/* Used by processes to "lock" a socket state, so that
 * interrupts and bottom half handlers won't change it
 * from under us. It essentially blocks any incoming
 * packets, so that we won't get any new data or any
 * packets that change the state of the socket.
 *
 * While locked, BH processing will add new packets to
 * the backlog queue.  This queue is processed by the
 * owner of the socket lock right before it is released.
 */
extern void __lock_sock(struct sock *sk);
extern void __release_sock(struct sock *sk);
#define lock_sock(__sk) \
do {	spin_lock_bh(&((__sk)->lock.slock)); \
	if ((__sk)->lock.users != 0) \
		__lock_sock(__sk); \
	(__sk)->lock.users = 1; \
	spin_unlock_bh(&((__sk)->lock.slock)); \
} while(0)
#define release_sock(__sk) \
do {	spin_lock_bh(&((__sk)->lock.slock)); \
	(__sk)->lock.users = 0; \
	if ((__sk)->backlog.tail != NULL) \
		__release_sock(__sk); \
	wake_up(&((__sk)->lock.wq)); \
	spin_unlock_bh(&((__sk)->lock.slock)); \
} while(0)

/* BH context may only use the following locking interface. */
#define bh_lock_sock(__sk)	spin_lock(&((__sk)->lock.slock))
#define bh_unlock_sock(__sk)	spin_unlock(&((__sk)->lock.slock))

/*
 *	This might not be the most appropriate place for this two	 
 *	but since they are used by a lot of the net related code
 *	at least they get declared on a include that is common to all
 */

static __inline__ int min(unsigned int a, unsigned int b)
{
	if (a > b)
		a = b; 
	return a;
}

static __inline__ int max(unsigned int a, unsigned int b)
{
	if (a < b)
		a = b;
	return a;
}

extern struct sock *		sk_alloc(int family, int priority, int zero_it);
extern void			sk_free(struct sock *sk);

extern struct sk_buff		*sock_wmalloc(struct sock *sk,
					      unsigned long size, int force,
					      int priority);
extern struct sk_buff		*sock_rmalloc(struct sock *sk,
					      unsigned long size, int force,
					      int priority);
extern void			sock_wfree(struct sk_buff *skb);
extern void			sock_rfree(struct sk_buff *skb);
extern void			sock_cfree(struct sk_buff *skb);
extern unsigned long		sock_rspace(struct sock *sk);
extern unsigned long		sock_wspace(struct sock *sk);

extern int			sock_setsockopt(struct socket *sock, int level,
						int op, char *optval,
						int optlen);

extern int			sock_getsockopt(struct socket *sock, int level,
						int op, char *optval, 
						int *optlen);
extern struct sk_buff 		*sock_alloc_send_skb(struct sock *sk,
						     unsigned long size,
						     unsigned long fallback,
						     int noblock,
						     int *errcode);
extern void *sock_kmalloc(struct sock *sk, int size, int priority);
extern void sock_kfree_s(struct sock *sk, void *mem, int size);


/*
 * Functions to fill in entries in struct proto_ops when a protocol
 * does not implement a particular function.
 */
extern int                      sock_no_release(struct socket *);
extern int                      sock_no_bind(struct socket *, 
					     struct sockaddr *, int);
extern int                      sock_no_connect(struct socket *,
						struct sockaddr *, int, int);
extern int                      sock_no_socketpair(struct socket *,
						   struct socket *);
extern int                      sock_no_accept(struct socket *,
					       struct socket *, int);
extern int                      sock_no_getname(struct socket *,
						struct sockaddr *, int *, int);
extern unsigned int             sock_no_poll(struct file *, struct socket *,
					     struct poll_table_struct *);
extern int                      sock_no_ioctl(struct socket *, unsigned int,
					      unsigned long);
extern int			sock_no_listen(struct socket *, int);
extern int                      sock_no_shutdown(struct socket *, int);
extern int			sock_no_getsockopt(struct socket *, int , int,
						   char *, int *);
extern int			sock_no_setsockopt(struct socket *, int, int,
						   char *, int);
extern int 			sock_no_fcntl(struct socket *, 
					      unsigned int, unsigned long);
extern int                      sock_no_sendmsg(struct socket *,
						struct msghdr *, int,
						struct scm_cookie *);
extern int                      sock_no_recvmsg(struct socket *,
						struct msghdr *, int,
						struct scm_cookie *);
extern int			sock_no_mmap(struct file *file,
					     struct socket *sock,
					     struct vm_area_struct *vma);

/*
 *	Default socket callbacks and setup code
 */
 
extern void sock_def_callback1(struct sock *);
extern void sock_def_callback2(struct sock *, int);
extern void sock_def_callback3(struct sock *);
extern void sock_def_destruct(struct sock *);

/* Initialise core socket variables */
extern void sock_init_data(struct socket *sock, struct sock *sk);

extern void sklist_remove_socket(struct sock **list, struct sock *sk);
extern void sklist_insert_socket(struct sock **list, struct sock *sk);
extern void sklist_destroy_socket(struct sock **list, struct sock *sk);

#ifdef CONFIG_FILTER
/*
 * Run the filter code and then cut skb->data to correct size returned by
 * sk_run_filter. If pkt_len is 0 we toss packet. If skb->len is smaller
 * than pkt_len we keep whole skb->data.
 */
extern __inline__ int sk_filter(struct sk_buff *skb, struct sk_filter *filter)
{
	int pkt_len;

        pkt_len = sk_run_filter(skb, filter->insns, filter->len);
        if(!pkt_len)
                return 1;	/* Toss Packet */
        else
                skb_trim(skb, pkt_len);

	return 0;
}

extern __inline__ void sk_filter_release(struct sock *sk, struct sk_filter *fp)
{
	unsigned int size = sk_filter_len(fp);

	atomic_sub(size, &sk->omem_alloc);

	if (atomic_dec_and_test(&fp->refcnt))
		kfree_s(fp, size);
}

extern __inline__ void sk_filter_charge(struct sock *sk, struct sk_filter *fp)
{
	atomic_inc(&fp->refcnt);
	atomic_add(sk_filter_len(fp), &sk->omem_alloc);
}

#endif /* CONFIG_FILTER */

/*
 * Socket reference counting postulates.
 *
 * * Each user of socket SHOULD hold a reference count.
 * * Each access point to socket (an hash table bucket, reference from a list,
 *   running timer, skb in flight MUST hold a reference count.
 * * When reference count hits 0, it means it will never increase back.
 * * When reference count hits 0, it means that no references from
 *   outside exist to this socket and current process on current CPU
 *   is last user and may/should destroy this socket.
 * * sk_free is called from any context: process, BH, IRQ. When
 *   it is called, socket has no references from outside -> sk_free
 *   may release descendant resources allocated by the socket, but
 *   to the time when it is called, socket is NOT referenced by any
 *   hash tables, lists etc.
 * * Packets, delivered from outside (from network or from another process)
 *   and enqueued on receive/error queues SHOULD NOT grab reference count,
 *   when they sit in queue. Otherwise, packets will leak to hole, when
 *   socket is looked up by one cpu and unhasing is made by another CPU.
 *   It is true for udp/raw, netlink (leak to receive and error queues), tcp
 *   (leak to backlog). Packet socket does all the processing inside
 *   ptype_lock, so that it has not this race condition. UNIX sockets
 *   use separate SMP lock, so that they are prone too.
 */

/* Grab socket reference count. This operation is valid only
   when sk is ALREADY grabbed f.e. it is found in hash table
   or a list and the lookup is made under lock preventing hash table
   modifications.
 */

extern __inline__ void sock_hold(struct sock *sk)
{
	atomic_inc(&sk->refcnt);
}

/* Ungrab socket in the context, which assumes that socket refcnt
   cannot hit zero, f.e. it is true in context of any socketcall.
 */
extern __inline__ void __sock_put(struct sock *sk)
{
	atomic_dec(&sk->refcnt);
}

/* Ungrab socket and destroy it, if it was the last reference. */
extern __inline__ void sock_put(struct sock *sk)
{
	if (atomic_dec_and_test(&sk->refcnt))
		sk_free(sk);
}

extern __inline__ struct dst_entry *
__sk_dst_get(struct sock *sk)
{
	return sk->dst_cache;
}

extern __inline__ struct dst_entry *
sk_dst_get(struct sock *sk)
{
	struct dst_entry *dst;

	read_lock(&sk->dst_lock);
	dst = sk->dst_cache;
	if (dst)
		dst_hold(dst);
	read_unlock(&sk->dst_lock);
	return dst;
}

extern __inline__ void
__sk_dst_set(struct sock *sk, struct dst_entry *dst)
{
	struct dst_entry *old_dst;

	old_dst = sk->dst_cache;
	sk->dst_cache = dst;
	dst_release(old_dst);
}

extern __inline__ void
sk_dst_set(struct sock *sk, struct dst_entry *dst)
{
	write_lock(&sk->dst_lock);
	__sk_dst_set(sk, dst);
	write_unlock(&sk->dst_lock);
}

extern __inline__ void
__sk_dst_reset(struct sock *sk)
{
	struct dst_entry *old_dst;

	old_dst = sk->dst_cache;
	sk->dst_cache = NULL;
	dst_release(old_dst);
}

extern __inline__ void
sk_dst_reset(struct sock *sk)
{
	write_lock(&sk->dst_lock);
	__sk_dst_reset(sk);
	write_unlock(&sk->dst_lock);
}

extern __inline__ struct dst_entry *
__sk_dst_check(struct sock *sk, u32 cookie)
{
	struct dst_entry *dst = sk->dst_cache;

	if (dst && dst->obsolete && dst->ops->check(dst, cookie) == NULL) {
		sk->dst_cache = NULL;
		return NULL;
	}

	return dst;
}

extern __inline__ struct dst_entry *
sk_dst_check(struct sock *sk, u32 cookie)
{
	struct dst_entry *dst = sk_dst_get(sk);

	if (dst && dst->obsolete && dst->ops->check(dst, cookie) == NULL) {
		sk_dst_reset(sk);
		return NULL;
	}

	return dst;
}


/*
 * 	Queue a received datagram if it will fit. Stream and sequenced
 *	protocols can't normally use this as they need to fit buffers in
 *	and play with them.
 *
 * 	Inlined as it's very short and called for pretty much every
 *	packet ever received.
 */

extern __inline__ void skb_set_owner_w(struct sk_buff *skb, struct sock *sk)
{
	sock_hold(sk);
	skb->sk = sk;
	skb->destructor = sock_wfree;
	atomic_add(skb->truesize, &sk->wmem_alloc);
}

extern __inline__ void skb_set_owner_r(struct sk_buff *skb, struct sock *sk)
{
	skb->sk = sk;
	skb->destructor = sock_rfree;
	atomic_add(skb->truesize, &sk->rmem_alloc);
}

extern __inline__ void skb_set_owner_c(struct sk_buff *skb, struct sock *sk)
{
	sock_hold(sk);
	skb->sk = sk;
	skb->destructor = sock_cfree;
}


extern __inline__ int sock_queue_rcv_skb(struct sock *sk, struct sk_buff *skb)
{
	/* Cast skb->rcvbuf to unsigned... It's pointless, but reduces
	   number of warnings when compiling with -W --ANK
	 */
	if (atomic_read(&sk->rmem_alloc) + skb->truesize >= (unsigned)sk->rcvbuf)
                return -ENOMEM;

#ifdef CONFIG_FILTER
	if (sk->filter) {
		int err = 0;
		struct sk_filter *filter;

		/* It would be deadlock, if sock_queue_rcv_skb is used
		   with socket lock! We assume that users of this
		   function are lock free.
		 */
		bh_lock_sock(sk);
		if ((filter = sk->filter) != NULL && sk_filter(skb, filter))
			err = -EPERM;
		bh_unlock_sock(sk);
		if (err)
			return err;	/* Toss packet */
	}
#endif /* CONFIG_FILTER */

	skb_set_owner_r(skb, sk);
	skb_queue_tail(&sk->receive_queue, skb);
	if (!sk->dead)
		sk->data_ready(sk,skb->len);
	return 0;
}

extern __inline__ int sock_queue_err_skb(struct sock *sk, struct sk_buff *skb)
{
	/* Cast skb->rcvbuf to unsigned... It's pointless, but reduces
	   number of warnings when compiling with -W --ANK
	 */
	if (atomic_read(&sk->rmem_alloc) + skb->truesize >= (unsigned)sk->rcvbuf)
		return -ENOMEM;
	skb_set_owner_r(skb, sk);
	skb_queue_tail(&sk->error_queue,skb);
	if (!sk->dead)
		sk->data_ready(sk,skb->len);
	return 0;
}

/*
 *	Recover an error report and clear atomically
 */
 
extern __inline__ int sock_error(struct sock *sk)
{
	int err=xchg(&sk->err,0);
	return -err;
}

extern __inline__ unsigned long sock_wspace(struct sock *sk)
{
	int amt = 0;

	if (!(sk->shutdown & SEND_SHUTDOWN)) {
		amt = sk->sndbuf - atomic_read(&sk->wmem_alloc);
		if (amt < 0) 
			amt = 0;
	}
	return amt;
}

/*
 *	Default write policy as shown to user space via poll/select/SIGIO
 *	Kernel internally doesn't use the MIN_WRITE_SPACE threshold.
 */
extern __inline__ int sock_writeable(struct sock *sk) 
{
	return sock_wspace(sk) >= MIN_WRITE_SPACE;
}

extern __inline__ int gfp_any(void)
{
	return in_interrupt() ? GFP_ATOMIC : GFP_KERNEL;
}


/* 
 *	Enable debug/info messages 
 */

#if 0
#define NETDEBUG(x)	do { } while (0)
#else
#define NETDEBUG(x)	do { x; } while (0)
#endif

/*
 * Macros for sleeping on a socket. Use them like this:
 *
 * SOCK_SLEEP_PRE(sk)
 * if (condition)
 * 	schedule();
 * SOCK_SLEEP_POST(sk)
 *
 */

#define SOCK_SLEEP_PRE(sk) 	{ struct task_struct *tsk = current; \
				DECLARE_WAITQUEUE(wait, tsk); \
				tsk->state = TASK_INTERRUPTIBLE; \
				add_wait_queue((sk)->sleep, &wait); \
				release_sock(sk);

#define SOCK_SLEEP_POST(sk)	tsk->state = TASK_RUNNING; \
				remove_wait_queue((sk)->sleep, &wait); \
				lock_sock(sk); \
				}

#endif	/* _SOCK_H */
