/* atm_tcp.h - Driver-specific declarations of the ATMTCP driver (for use by
	       driver-specific utilities) */

/* Written 1997-1999 by Werner Almesberger, EPFL LRC/ICA */


#ifndef LINUX_ATM_TCP_H
#define LINUX_ATM_TCP_H

#ifdef __KERNEL__
#include <linux/types.h>
#endif
#include <linux/atmioc.h>


/*
 * All values in struct atmtcp_hdr are in network byte order
 */

struct atmtcp_hdr {
	uint16_t	vpi;
	uint16_t	vci;
	uint32_t	length;		/* ... of data part */
};

/*
 * All values in struct atmtcp_command are in host byte order
 */

#define ATMTCP_HDR_MAGIC	(~0)	/* this length indicates a command */
#define ATMTCP_CTRL_OPEN	1	/* request/reply */
#define ATMTCP_CTRL_CLOSE	2	/* request/reply */

struct atmtcp_control {
	struct atmtcp_hdr hdr;	/* must be first */
	int		type;	/* message type; both directions */
	unsigned long	vcc;	/* both directions */
	struct sockaddr_atmpvc addr; /* suggested value from kernel */
	struct atm_qos	qos;	/* both directions */
	int result;		/* to kernel only */
};

/*
 * Field usage:
 * Messge type	dir.	hdr.v?i	type	addr	qos	vcc	result
 * -----------  ----	------- ----	----	---	---	------
 * OPEN		K->D	Y	Y	Y	Y	Y	0
 * OPEN		D->K	-	Y	Y	Y	Y	Y
 * CLOSE	K->D	-	-	Y	-	Y	0
 * CLOSE	D->K	-	-	-	-	Y	Y
 */

#define SIOCSIFATMTCP	_IO('a',ATMIOC_ITF)	/* set ATMTCP mode */
#define ATMTCP_CREATE	_IO('a',ATMIOC_ITF+14)	/* create persistent ATMTCP
						   interface */
#define ATMTCP_REMOVE	_IO('a',ATMIOC_ITF+15)	/* destroy persistent ATMTCP
						   interface */


#ifdef __KERNEL__

struct atm_tcp_ops {
	int (*attach)(struct atm_vcc *vcc,int itf);
	int (*create_persistent)(int itf);
	int (*remove_persistent)(int itf);
};

extern struct atm_tcp_ops atm_tcp_ops;

#endif

#endif
