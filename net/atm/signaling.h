/* net/atm/signaling.h - ATM signaling */
 
/* Written 1995-1999 by Werner Almesberger, EPFL LRC/ICA */
 

#ifndef NET_ATM_SIGNALING_H
#define NET_ATM_SIGNALING_H

#include <linux/atm.h>
#include <linux/atmdev.h>
#include <linux/atmsvc.h>


#define WAITING 1 /* for reply: 0: no error, < 0: error, ... */


extern struct atm_vcc *sigd; /* needed in svc_release */


void sigd_enq(struct atm_vcc *vcc,enum atmsvc_msg_type type,
    const struct atm_vcc *listen_vcc,const struct sockaddr_atmpvc *pvc,
    const struct sockaddr_atmsvc *svc);
int sigd_attach(struct atm_vcc *vcc);
void signaling_init(void);

#endif
