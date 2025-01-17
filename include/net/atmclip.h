/* net/atm/atmarp.h - RFC1577 ATM ARP */
 
/* Written 1995-1998 by Werner Almesberger, EPFL LRC/ICA */
 
 
#ifndef _ATMCLIP_H
#define _ATMCLIP_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/atm.h>
#include <linux/atmdev.h>
#include <linux/atmarp.h>
#include <net/neighbour.h>


#define CLIP_VCC(vcc) ((struct clip_vcc *) ((vcc)->user_back))
#define NEIGH2ENTRY(neigh) ((struct atmarp_entry *) (neigh)->primary_key)


struct clip_vcc {
	struct atm_vcc	*vcc;		/* VCC descriptor */
	struct atmarp_entry *entry;	/* ATMARP table entry, NULL if IP addr.
					   isn't known yet */
	unsigned char	encap;		/* 0: NULL, 1: LLC/SNAP */
	unsigned long	last_use;	/* last send or receive operation */
	unsigned long	idle_timeout;	/* keep open idle for so many jiffies*/
	void (*old_push)(struct atm_vcc *vcc,struct sk_buff *skb);
					/* keep old push fn for detaching */
	struct clip_vcc	*next;		/* next VCC */
};


struct atmarp_entry {
	u32		ip;		/* IP address */
	struct clip_vcc	*vccs;		/* active VCCs; NULL if resolution is
					   pending */
	unsigned long	expires;	/* entry expiration time */
	struct neighbour *neigh;	/* neighbour back-pointer */
};


#define PRIV(dev) ((struct clip_priv *) ((struct net_device *) (dev)+1))


struct clip_priv {
	char name[8];			/* interface name */
	int number;			/* for convenience ... */
	struct net_device_stats stats;
	struct net_device *next;		/* next CLIP interface */
};


extern struct atm_vcc *atmarpd; /* ugly */
extern struct neigh_table clip_tbl;

int clip_create(int number);
int clip_mkip(struct atm_vcc *vcc,int timeout);
int clip_setentry(struct atm_vcc *vcc,u32 ip);
int clip_encap(struct atm_vcc *vcc,int mode);
void atm_clip_init(void);

#endif
