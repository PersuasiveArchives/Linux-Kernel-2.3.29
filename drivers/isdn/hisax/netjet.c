/* $Id: netjet.c,v 1.16 1999/10/14 20:25:29 keil Exp $

 * netjet.c     low level stuff for Traverse Technologie NETJet ISDN cards
 *
 * Author     Karsten Keil (keil@isdn4linux.de)
 *
 * Thanks to Traverse Technologie Australia for documents and informations
 *
 * $Log: netjet.c,v $
 * Revision 1.16  1999/10/14 20:25:29  keil
 * add a statistic for error monitoring
 *
 * Revision 1.15  1999/09/04 06:20:06  keil
 * Changes from kernel set_current_state()
 *
 * Revision 1.14  1999/08/31 11:20:25  paul
 * various spelling corrections (new checksums may be needed, Karsten!)
 *
 * Revision 1.13  1999/08/11 21:01:31  keil
 * new PCI codefix
 *
 * Revision 1.12  1999/08/10 16:02:00  calle
 * struct pci_dev changed in 2.3.13. Made the necessary changes.
 *
 * Revision 1.11  1999/08/07 17:32:00  keil
 * Asymetric buffers for improved ping times.  Interframe spacing
 * fix for NJ<->NJ thoughput.  Matt Henderson - www.traverse.com.au
 *
 *
 * Revision 1.10  1999/07/12 21:05:22  keil
 * fix race in IRQ handling
 * added watchdog for lost IRQs
 *
 * Revision 1.9  1999/07/01 08:12:05  keil
 * Common HiSax version for 2.0, 2.1, 2.2 and 2.3 kernel
 *
 * Revision 1.8  1998/11/15 23:55:14  keil
 * changes from 2.0
 *
 * Revision 1.7  1998/09/30 22:24:48  keil
 * Fix missing line in setstack*
 *
 * Revision 1.6  1998/08/13 23:36:54  keil
 * HiSax 3.1 - don't work stable with current LinkLevel
 *
 * Revision 1.5  1998/05/25 12:58:21  keil
 * HiSax golden code from certification, Don't use !!!
 * No leased lines, no X75, but many changes.
 *
 * Revision 1.4  1998/04/15 16:42:35  keil
 * new init code
 * new PCI init (2.1.94)
 *
 * Revision 1.3  1998/02/12 23:08:05  keil
 * change for 2.1.86 (removing FREE_READ/FREE_WRITE from [dev]_kfree_skb()
 *
 * Revision 1.2  1998/02/02 13:32:06  keil
 * New
 *
 *
 *
 */

#define __NO_VERSION__
#include <linux/config.h>
#include "hisax.h"
#include "isac.h"
#include "hscx.h"
#include "isdnl1.h"
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/ppp_defs.h>

#ifndef bus_to_virt
#define bus_to_virt (u_int *)
#endif

#ifndef virt_to_bus
#define virt_to_bus (u_int)
#endif

extern const char *CardType[];

const char *NETjet_revision = "$Revision: 1.16 $";

#define byteout(addr,val) outb(val,addr)
#define bytein(addr) inb(addr)

/* PCI stuff */
#define PCI_VENDOR_TRAVERSE_TECH 0xe159
#define PCI_NETJET_ID	0x0001

#define NETJET_CTRL	0x00
#define NETJET_DMACTRL	0x01
#define NETJET_AUXCTRL	0x02
#define NETJET_AUXDATA	0x03
#define NETJET_IRQMASK0 0x04
#define NETJET_IRQMASK1 0x05
#define NETJET_IRQSTAT0 0x06
#define NETJET_IRQSTAT1 0x07
#define NETJET_DMA_READ_START	0x08
#define NETJET_DMA_READ_IRQ	0x0c
#define NETJET_DMA_READ_END	0x10
#define NETJET_DMA_READ_ADR	0x14
#define NETJET_DMA_WRITE_START	0x18
#define NETJET_DMA_WRITE_IRQ	0x1c
#define NETJET_DMA_WRITE_END	0x20
#define NETJET_DMA_WRITE_ADR	0x24
#define NETJET_PULSE_CNT	0x28

#define NETJET_ISAC_OFF	0xc0
#define NETJET_ISACIRQ	0x10
#define NETJET_IRQM0_READ	0x0c
#define NETJET_IRQM0_READ_1	0x04
#define NETJET_IRQM0_READ_2	0x08
#define NETJET_IRQM0_WRITE	0x03
#define NETJET_IRQM0_WRITE_1	0x01
#define NETJET_IRQM0_WRITE_2	0x02

#define NETJET_DMA_TXSIZE 512
#define NETJET_DMA_RXSIZE 128

#define HDLC_ZERO_SEARCH 0
#define HDLC_FLAG_SEARCH 1
#define HDLC_FLAG_FOUND  2
#define HDLC_FRAME_FOUND 3
#define HDLC_NULL 4
#define HDLC_PART 5
#define HDLC_FULL 6

#define HDLC_FLAG_VALUE	0x7e

/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	long flags;
	u_char ret;
	
	save_flags(flags);
	cli();
	cs->hw.njet.auxd &= 0xfc;
	cs->hw.njet.auxd |= (offset>>4) & 3;
	byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);
	ret = bytein(cs->hw.njet.isac + ((offset & 0xf)<<2));
	restore_flags(flags);
	return(ret);
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	long flags;
	
	save_flags(flags);
	cli();
	cs->hw.njet.auxd &= 0xfc;
	cs->hw.njet.auxd |= (offset>>4) & 3;
	byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);
	byteout(cs->hw.njet.isac + ((offset & 0xf)<<2), value);
	restore_flags(flags);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char *data, int size)
{
	cs->hw.njet.auxd &= 0xfc;
	byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);
	insb(cs->hw.njet.isac, data, size);
}

__u16 fcstab[256] =
{
	0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
	0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
	0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
	0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
	0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
	0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
	0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
	0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
	0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
	0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
	0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
	0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
	0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
	0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
	0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
	0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
	0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
	0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
	0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
	0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
	0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
	0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
	0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
	0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
	0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
	0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
	0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
	0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
	0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
	0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
	0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
	0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

static void 
WriteISACfifo(struct IsdnCardState *cs, u_char *data, int size)
{
	cs->hw.njet.auxd &= 0xfc;
	byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);
	outsb(cs->hw.njet.isac, data, size);
}

void fill_mem(struct BCState *bcs, u_int *pos, u_int cnt, int chan, u_char fill)
{
	u_int mask=0x000000ff, val = 0, *p=pos;
	u_int i;
	
	val |= fill;
	if (chan) {
		val  <<= 8;
		mask <<= 8;
	}
	mask ^= 0xffffffff;
	for (i=0; i<cnt; i++) {
		*p   &= mask;
		*p++ |= val;
		if (p > bcs->hw.tiger.s_end)
			p = bcs->hw.tiger.send;
	}
}

void
mode_tiger(struct BCState *bcs, int mode, int bc)
{
	struct IsdnCardState *cs = bcs->cs;

	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "Tiger mode %d bchan %d/%d",
			mode, bc, bcs->channel);
	bcs->mode = mode;
	bcs->channel = bc;
	switch (mode) {
		case (L1_MODE_NULL):
			fill_mem(bcs, bcs->hw.tiger.send,
				NETJET_DMA_TXSIZE, bc, 0xff);
			if (cs->debug & L1_DEB_HSCX)
				debugl1(cs, "Tiger stat rec %d/%d send %d",
					bcs->hw.tiger.r_tot, bcs->hw.tiger.r_err,
					bcs->hw.tiger.s_tot); 
			if ((cs->bcs[0].mode == L1_MODE_NULL) &&
				(cs->bcs[1].mode == L1_MODE_NULL)) {
				cs->hw.njet.dmactrl = 0;
				byteout(cs->hw.njet.base + NETJET_DMACTRL,
					cs->hw.njet.dmactrl);
				byteout(cs->hw.njet.base + NETJET_IRQMASK0, 0);
			}
			break;
		case (L1_MODE_TRANS):
			break;
		case (L1_MODE_HDLC): 
			fill_mem(bcs, bcs->hw.tiger.send,
				NETJET_DMA_TXSIZE, bc, 0xff);
			bcs->hw.tiger.r_state = HDLC_ZERO_SEARCH;
			bcs->hw.tiger.r_tot = 0;
			bcs->hw.tiger.r_bitcnt = 0;
			bcs->hw.tiger.r_one = 0;
			bcs->hw.tiger.r_err = 0;
			bcs->hw.tiger.s_tot = 0;
			if (! cs->hw.njet.dmactrl) {
				fill_mem(bcs, bcs->hw.tiger.send,
					NETJET_DMA_TXSIZE, !bc, 0xff);
				cs->hw.njet.dmactrl = 1;
				byteout(cs->hw.njet.base + NETJET_DMACTRL,
					cs->hw.njet.dmactrl);
				byteout(cs->hw.njet.base + NETJET_IRQMASK0, 0x3f);
			}
			bcs->hw.tiger.sendp = bcs->hw.tiger.send;
			bcs->hw.tiger.free = NETJET_DMA_TXSIZE;
			test_and_set_bit(BC_FLG_EMPTY, &bcs->Flag);
			break;
	}
	if (cs->debug & L1_DEB_HSCX)
		debugl1(cs, "tiger: set %x %x %x  %x/%x  pulse=%d",
			bytein(cs->hw.njet.base + NETJET_DMACTRL),
			bytein(cs->hw.njet.base + NETJET_IRQMASK0),
			bytein(cs->hw.njet.base + NETJET_IRQSTAT0),
			inl(cs->hw.njet.base + NETJET_DMA_READ_ADR),
			inl(cs->hw.njet.base + NETJET_DMA_WRITE_ADR),
			bytein(cs->hw.njet.base + NETJET_PULSE_CNT));
}

static u_char dummyrr(struct IsdnCardState *cs, int chan, u_char off)
{
	return(5);
}

static void dummywr(struct IsdnCardState *cs, int chan, u_char off, u_char value)
{
}

static void printframe(struct IsdnCardState *cs, u_char *buf, int count, char *s) {
	char tmp[128];
	char *t = tmp;
	int i=count,j;
	u_char *p = buf;

	t += sprintf(t, "tiger %s(%4d)", s, count);
	while (i>0) {
		if (i>16)
			j=16;
		else
			j=i;
		QuickHex(t, p, j);
		debugl1(cs, tmp);
		p += j;
		i -= j;
		t = tmp;
		t += sprintf(t, "tiger %s      ", s);
	}
}

#define MAKE_RAW_BYTE for (j=0; j<8; j++) { \
			bitcnt++;\
			s_val >>= 1;\
			if (val & 1) {\
				s_one++;\
				s_val |= 0x80;\
			} else {\
				s_one = 0;\
				s_val &= 0x7f;\
			}\
			if (bitcnt==8) {\
				bcs->hw.tiger.sendbuf[s_cnt++] = s_val;\
				bitcnt = 0;\
			}\
			if (s_one == 5) {\
				s_val >>= 1;\
				s_val &= 0x7f;\
				bitcnt++;\
				s_one = 0;\
			}\
			if (bitcnt==8) {\
				bcs->hw.tiger.sendbuf[s_cnt++] = s_val;\
				bitcnt = 0;\
			}\
			val >>= 1;\
		}

static int make_raw_data(struct BCState *bcs) {
	register u_int i,s_cnt=0;
	register u_char j;
	register u_char val;
	register u_char s_one = 0;
	register u_char s_val = 0;
	register u_char bitcnt = 0;
	u_int fcs;
	
	if (!bcs->tx_skb) {
		debugl1(bcs->cs, "tiger make_raw: NULL skb");
		return(1);
	}
	bcs->hw.tiger.sendbuf[s_cnt++] = HDLC_FLAG_VALUE;
	fcs = PPP_INITFCS;
	for (i=0; i<bcs->tx_skb->len; i++) {
		val = bcs->tx_skb->data[i];
		fcs = PPP_FCS (fcs, val);
		MAKE_RAW_BYTE;
	}
	fcs ^= 0xffff;
	val = fcs & 0xff;
	MAKE_RAW_BYTE;
	val = (fcs>>8) & 0xff;
	MAKE_RAW_BYTE;
	val = HDLC_FLAG_VALUE;
	for (j=0; j<8; j++) { 
		bitcnt++;
		s_val >>= 1;
		if (val & 1)
			s_val |= 0x80;
		else
			s_val &= 0x7f;
		if (bitcnt==8) {
			bcs->hw.tiger.sendbuf[s_cnt++] = s_val;
			bitcnt = 0;
		}
		val >>= 1;
	}
	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs,"tiger make_raw: in %ld out %d.%d",
			bcs->tx_skb->len, s_cnt, bitcnt);
	if (bitcnt) {
		while (8>bitcnt++) {
			s_val >>= 1;
			s_val |= 0x80;
		}
		bcs->hw.tiger.sendbuf[s_cnt++] = s_val;
		bcs->hw.tiger.sendbuf[s_cnt++] = 0xff;	// NJ<->NJ thoughput bug fix
	}
	bcs->hw.tiger.sendcnt = s_cnt;
	bcs->tx_cnt -= bcs->tx_skb->len;
	bcs->hw.tiger.sp = bcs->hw.tiger.sendbuf;
	return(0);
}

static void got_frame(struct BCState *bcs, int count) {
	struct sk_buff *skb;
		
	if (!(skb = dev_alloc_skb(count)))
		printk(KERN_WARNING "TIGER: receive out of memory\n");
	else {
		memcpy(skb_put(skb, count), bcs->hw.tiger.rcvbuf, count);
		skb_queue_tail(&bcs->rqueue, skb);
	}
	bcs->event |= 1 << B_RCVBUFREADY;
	queue_task(&bcs->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	
	if (bcs->cs->debug & L1_DEB_RECEIVE_FRAME)
		printframe(bcs->cs, bcs->hw.tiger.rcvbuf, count, "rec");
}



static void read_raw(struct BCState *bcs, u_int *buf, int cnt){
	int i;
	register u_char j;
	register u_char val;
	u_int  *pend = bcs->hw.tiger.rec +NETJET_DMA_RXSIZE -1;
	register u_char state = bcs->hw.tiger.r_state;
	register u_char r_one = bcs->hw.tiger.r_one;
	register u_char r_val = bcs->hw.tiger.r_val;
	register u_int bitcnt = bcs->hw.tiger.r_bitcnt;
	u_int *p = buf;
        
	for (i=0;i<cnt;i++) {
		val = bcs->channel ? ((*p>>8) & 0xff) : (*p & 0xff);
		p++;
		if (p > pend)
			p = bcs->hw.tiger.rec;
		if (val == 0xff) {
			state = HDLC_ZERO_SEARCH;
			bcs->hw.tiger.r_tot++;
			bitcnt = 0;
			r_one = 0;
			continue;
		}
		for (j=0;j<8;j++) {
			if (state == HDLC_ZERO_SEARCH) {
				if (val & 1) {
					r_one++;
				} else {
					r_one=0;
					state= HDLC_FLAG_SEARCH;
					if (bcs->cs->debug & L1_DEB_HSCX)
						debugl1(bcs->cs,"tiger read_raw: zBit(%d,%d,%d) %x",
							bcs->hw.tiger.r_tot,i,j,val);
				}
			} else if (state == HDLC_FLAG_SEARCH) { 
				if (val & 1) {
					r_one++;
					if (r_one>6) {
						state=HDLC_ZERO_SEARCH;
					}
				} else {
					if (r_one==6) {
						bitcnt=0;
						r_val=0;
						state=HDLC_FLAG_FOUND;
						if (bcs->cs->debug & L1_DEB_HSCX)
							debugl1(bcs->cs,"tiger read_raw: flag(%d,%d,%d) %x",
								bcs->hw.tiger.r_tot,i,j,val);
					}
					r_one=0;
				}
			} else if (state ==  HDLC_FLAG_FOUND) {
				if (val & 1) {
					r_one++;
					if (r_one>6) {
						state=HDLC_ZERO_SEARCH;
					} else {
						r_val >>= 1;
						r_val |= 0x80;
						bitcnt++;
					}
				} else {
					if (r_one==6) {
						bitcnt=0;
						r_val=0;
						r_one=0;
						val >>= 1;
						continue;
					} else if (r_one!=5) {
						r_val >>= 1;
						r_val &= 0x7f;
						bitcnt++;
					}
					r_one=0;	
				}
				if ((state != HDLC_ZERO_SEARCH) &&
					!(bitcnt & 7)) {
					state=HDLC_FRAME_FOUND;
					bcs->hw.tiger.r_fcs = PPP_INITFCS;
					bcs->hw.tiger.rcvbuf[0] = r_val;
					bcs->hw.tiger.r_fcs = PPP_FCS (bcs->hw.tiger.r_fcs, r_val);
					if (bcs->cs->debug & L1_DEB_HSCX)
						debugl1(bcs->cs,"tiger read_raw: byte1(%d,%d,%d) rval %x val %x i %x",
							bcs->hw.tiger.r_tot,i,j,r_val,val,
							bcs->cs->hw.njet.irqstat0);
				}
			} else if (state ==  HDLC_FRAME_FOUND) {
				if (val & 1) {
					r_one++;
					if (r_one>6) {
						state=HDLC_ZERO_SEARCH;
						bitcnt=0;
					} else {
						r_val >>= 1;
						r_val |= 0x80;
						bitcnt++;
					}
				} else {
					if (r_one==6) {
						r_val=0; 
						r_one=0;
						bitcnt++;
						if (bitcnt & 7) {
							debugl1(bcs->cs, "tiger: frame not byte aligned");
							state=HDLC_FLAG_SEARCH;
							bcs->hw.tiger.r_err++;
#ifdef ERROR_STATISTIC
							bcs->err_inv++;
#endif
						} else {
							if (bcs->cs->debug & L1_DEB_HSCX)
								debugl1(bcs->cs,"tiger frame end(%d,%d): fcs(%x) i %x",
									i,j,bcs->hw.tiger.r_fcs, bcs->cs->hw.njet.irqstat0);
							if (bcs->hw.tiger.r_fcs == PPP_GOODFCS) {
								got_frame(bcs, (bitcnt>>3)-3);
							} else {
								if (bcs->cs->debug) {
									debugl1(bcs->cs, "tiger FCS error");
									printframe(bcs->cs, bcs->hw.tiger.rcvbuf,
										(bitcnt>>3)-1, "rec");
									bcs->hw.tiger.r_err++;
								}
#ifdef ERROR_STATISTIC
							bcs->err_crc++;
#endif
							}
							state=HDLC_FLAG_FOUND;
						}
						bitcnt=0;
					} else if (r_one==5) {
						val >>= 1;
						r_one=0;
						continue;
					} else {
						r_val >>= 1;
						r_val &= 0x7f;
						bitcnt++;
					}
					r_one=0;	
				}
				if ((state == HDLC_FRAME_FOUND) &&
					!(bitcnt & 7)) {
					if ((bitcnt>>3)>=HSCX_BUFMAX) {
						debugl1(bcs->cs, "tiger: frame too big");
						r_val=0; 
						state=HDLC_FLAG_SEARCH;
						bcs->hw.tiger.r_err++;
#ifdef ERROR_STATISTIC
						bcs->err_inv++;
#endif
					} else {
						bcs->hw.tiger.rcvbuf[(bitcnt>>3)-1] = r_val;
						bcs->hw.tiger.r_fcs = 
							PPP_FCS (bcs->hw.tiger.r_fcs, r_val);
					}
				}
			}
			val >>= 1;
		}
		bcs->hw.tiger.r_tot++;
	}
	bcs->hw.tiger.r_state = state;
	bcs->hw.tiger.r_one = r_one;
	bcs->hw.tiger.r_val = r_val;
	bcs->hw.tiger.r_bitcnt = bitcnt;
}

static void read_tiger(struct IsdnCardState *cs) {
	u_int *p;
	int cnt = NETJET_DMA_RXSIZE/2;
	
	if ((cs->hw.njet.irqstat0 & cs->hw.njet.last_is0) & NETJET_IRQM0_READ) {
		debugl1(cs,"tiger warn read double dma %x/%x",
			cs->hw.njet.irqstat0, cs->hw.njet.last_is0);
#ifdef ERROR_STATISTIC
		if (cs->bcs[0].mode)
			cs->bcs[0].err_rdo++;
		if (cs->bcs[1].mode)
			cs->bcs[1].err_rdo++;
#endif
		return;
	} else {
		cs->hw.njet.last_is0 &= ~NETJET_IRQM0_READ;
		cs->hw.njet.last_is0 |= (cs->hw.njet.irqstat0 & NETJET_IRQM0_READ);
	}	
	if (cs->hw.njet.irqstat0 & NETJET_IRQM0_READ_1)
		p = cs->bcs[0].hw.tiger.rec + NETJET_DMA_RXSIZE - 1;
	else
		p = cs->bcs[0].hw.tiger.rec + cnt - 1;
	if (cs->bcs[0].mode == L1_MODE_HDLC)
		read_raw(cs->bcs, p, cnt);
	if (cs->bcs[1].mode == L1_MODE_HDLC)
		read_raw(cs->bcs + 1, p, cnt);
	cs->hw.njet.irqstat0 &= ~NETJET_IRQM0_READ;
}

static void write_raw(struct BCState *bcs, u_int *buf, int cnt);

static void fill_dma(struct BCState *bcs)
{
	register u_int *p, *sp;
	register int cnt;

	if (!bcs->tx_skb)
		return;
	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs,"tiger fill_dma1: c%d %4x", bcs->channel,
			bcs->Flag);
	if (test_and_set_bit(BC_FLG_BUSY, &bcs->Flag))
		return;
	if (make_raw_data(bcs))
		return;		
	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs,"tiger fill_dma2: c%d %4x", bcs->channel,
			bcs->Flag);
	if (test_and_clear_bit(BC_FLG_NOFRAME, &bcs->Flag)) {
		write_raw(bcs, bcs->hw.tiger.sendp, bcs->hw.tiger.free);
	} else if (test_and_clear_bit(BC_FLG_HALF, &bcs->Flag)) {
		p = bus_to_virt(inl(bcs->cs->hw.njet.base + NETJET_DMA_READ_ADR));
		sp = bcs->hw.tiger.sendp;
		if (p == bcs->hw.tiger.s_end)
			p = bcs->hw.tiger.send -1;
		if (sp == bcs->hw.tiger.s_end)
			sp = bcs->hw.tiger.send -1;
		cnt = p - sp;
		if (cnt <0) {
			write_raw(bcs, bcs->hw.tiger.sendp, bcs->hw.tiger.free);
		} else {
			p++;
			cnt++;
			if (p > bcs->hw.tiger.s_end)
				p = bcs->hw.tiger.send;
			p++;
			cnt++;
			if (p > bcs->hw.tiger.s_end)
				p = bcs->hw.tiger.send;
			write_raw(bcs, p, bcs->hw.tiger.free - cnt);
		}
	} else if (test_and_clear_bit(BC_FLG_EMPTY, &bcs->Flag)) {
		p = bus_to_virt(inl(bcs->cs->hw.njet.base + NETJET_DMA_READ_ADR));
		cnt = bcs->hw.tiger.s_end - p;
		if (cnt < 2) {
			p = bcs->hw.tiger.send + 1;
			cnt = NETJET_DMA_TXSIZE/2 - 2;
		} else {
			p++;
			p++;
			if (cnt <= (NETJET_DMA_TXSIZE/2))
				cnt += NETJET_DMA_TXSIZE/2;
			cnt--;
			cnt--;
		}
		write_raw(bcs, p, cnt);
	}
	if (bcs->cs->debug & L1_DEB_HSCX)
		debugl1(bcs->cs,"tiger fill_dma3: c%d %4x", bcs->channel,
			bcs->Flag);
}

static void write_raw(struct BCState *bcs, u_int *buf, int cnt) {
	u_int mask, val, *p=buf;
	u_int i, s_cnt;
        
        if (cnt <= 0)
        	return;
	if (test_bit(BC_FLG_BUSY, &bcs->Flag)) {
		if (bcs->hw.tiger.sendcnt> cnt) {
			s_cnt = cnt;
			bcs->hw.tiger.sendcnt -= cnt;
		} else {
			s_cnt = bcs->hw.tiger.sendcnt;
			bcs->hw.tiger.sendcnt = 0;
		}
		if (bcs->channel)
			mask = 0xffff00ff;
		else
			mask = 0xffffff00;
		for (i=0; i<s_cnt; i++) {
			val = bcs->channel ? ((bcs->hw.tiger.sp[i] <<8) & 0xff00) :
				(bcs->hw.tiger.sp[i]);
			*p   &= mask;
			*p++ |= val;
			if (p>bcs->hw.tiger.s_end)
				p = bcs->hw.tiger.send;
		}
		bcs->hw.tiger.s_tot += s_cnt;
		if (bcs->cs->debug & L1_DEB_HSCX)
			debugl1(bcs->cs,"tiger write_raw: c%d %x-%x %d/%d %d %x", bcs->channel,
				(u_int)buf, (u_int)p, s_cnt, cnt,
				bcs->hw.tiger.sendcnt, bcs->cs->hw.njet.irqstat0);
		if (bcs->cs->debug & L1_DEB_HSCX_FIFO)
			printframe(bcs->cs, bcs->hw.tiger.sp, s_cnt, "snd");
		bcs->hw.tiger.sp += s_cnt;
		bcs->hw.tiger.sendp = p;
		if (!bcs->hw.tiger.sendcnt) {
			if (!bcs->tx_skb) {
				debugl1(bcs->cs,"tiger write_raw: NULL skb s_cnt %d", s_cnt);
			} else {
				if (bcs->st->lli.l1writewakeup &&
					(PACKET_NOACK != bcs->tx_skb->pkt_type))
					bcs->st->lli.l1writewakeup(bcs->st, bcs->tx_skb->len);
				dev_kfree_skb(bcs->tx_skb);
				bcs->tx_skb = NULL;
			}
			test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
			bcs->hw.tiger.free = cnt - s_cnt;
			if (bcs->hw.tiger.free > (NETJET_DMA_TXSIZE/2))
				test_and_set_bit(BC_FLG_HALF, &bcs->Flag);
			else {
				test_and_clear_bit(BC_FLG_HALF, &bcs->Flag);
				test_and_set_bit(BC_FLG_NOFRAME, &bcs->Flag);
			}
			if ((bcs->tx_skb = skb_dequeue(&bcs->squeue))) {
				fill_dma(bcs);
			} else {
				mask ^= 0xffffffff;
				if (s_cnt < cnt) {
					for (i=s_cnt; i<cnt;i++) {
						*p++ |= mask;
						if (p>bcs->hw.tiger.s_end)
							p = bcs->hw.tiger.send;
					}
					if (bcs->cs->debug & L1_DEB_HSCX)
						debugl1(bcs->cs, "tiger write_raw: fill rest %d",
							cnt - s_cnt);
				}
				bcs->event |= 1 << B_XMTBUFREADY;
				queue_task(&bcs->tqueue, &tq_immediate);
				mark_bh(IMMEDIATE_BH);
			}
		}
	} else if (test_and_clear_bit(BC_FLG_NOFRAME, &bcs->Flag)) {
		test_and_set_bit(BC_FLG_HALF, &bcs->Flag);
		fill_mem(bcs, buf, cnt, bcs->channel, 0xff);
		bcs->hw.tiger.free += cnt;
		if (bcs->cs->debug & L1_DEB_HSCX)
			debugl1(bcs->cs,"tiger write_raw: fill half");
	} else if (test_and_clear_bit(BC_FLG_HALF, &bcs->Flag)) {
		test_and_set_bit(BC_FLG_EMPTY, &bcs->Flag);
		fill_mem(bcs, buf, cnt, bcs->channel, 0xff);
		if (bcs->cs->debug & L1_DEB_HSCX)
			debugl1(bcs->cs,"tiger write_raw: fill full");
	}
}

static void write_tiger(struct IsdnCardState *cs) {
	u_int *p, cnt = NETJET_DMA_TXSIZE/2;
	
	if ((cs->hw.njet.irqstat0 & cs->hw.njet.last_is0) & NETJET_IRQM0_WRITE) {
		debugl1(cs,"tiger warn write double dma %x/%x",
			cs->hw.njet.irqstat0, cs->hw.njet.last_is0);
#ifdef ERROR_STATISTIC
		if (cs->bcs[0].mode)
			cs->bcs[0].err_tx++;
		if (cs->bcs[1].mode)
			cs->bcs[1].err_tx++;
#endif
		return;
	} else {
		cs->hw.njet.last_is0 &= ~NETJET_IRQM0_WRITE;
		cs->hw.njet.last_is0 |= (cs->hw.njet.irqstat0 & NETJET_IRQM0_WRITE);
	}	
	if (cs->hw.njet.irqstat0  & NETJET_IRQM0_WRITE_1)
		p = cs->bcs[0].hw.tiger.send + NETJET_DMA_TXSIZE - 1;
	else
		p = cs->bcs[0].hw.tiger.send + cnt - 1;
	if (cs->bcs[0].mode == L1_MODE_HDLC)
		write_raw(cs->bcs, p, cnt);
	if (cs->bcs[1].mode == L1_MODE_HDLC)
		write_raw(cs->bcs + 1, p, cnt);
	cs->hw.njet.irqstat0 &= ~NETJET_IRQM0_WRITE;
}

static void
tiger_l2l1(struct PStack *st, int pr, void *arg)
{
	struct sk_buff *skb = arg;
	long flags;

	switch (pr) {
		case (PH_DATA | REQUEST):
			save_flags(flags);
			cli();
			if (st->l1.bcs->tx_skb) {
				skb_queue_tail(&st->l1.bcs->squeue, skb);
				restore_flags(flags);
			} else {
				st->l1.bcs->tx_skb = skb;
				st->l1.bcs->cs->BC_Send_Data(st->l1.bcs);
				restore_flags(flags);
			}
			break;
		case (PH_PULL | INDICATION):
			if (st->l1.bcs->tx_skb) {
				printk(KERN_WARNING "tiger_l2l1: this shouldn't happen\n");
				break;
			}
			save_flags(flags);
			cli();
			st->l1.bcs->tx_skb = skb;
			st->l1.bcs->cs->BC_Send_Data(st->l1.bcs);
			restore_flags(flags);
			break;
		case (PH_PULL | REQUEST):
			if (!st->l1.bcs->tx_skb) {
				test_and_clear_bit(FLG_L1_PULL_REQ, &st->l1.Flags);
				st->l1.l1l2(st, PH_PULL | CONFIRM, NULL);
			} else
				test_and_set_bit(FLG_L1_PULL_REQ, &st->l1.Flags);
			break;
		case (PH_ACTIVATE | REQUEST):
			test_and_set_bit(BC_FLG_ACTIV, &st->l1.bcs->Flag);
			mode_tiger(st->l1.bcs, st->l1.mode, st->l1.bc);
			l1_msg_b(st, pr, arg);
			break;
		case (PH_DEACTIVATE | REQUEST):
			l1_msg_b(st, pr, arg);
			break;
		case (PH_DEACTIVATE | CONFIRM):
			test_and_clear_bit(BC_FLG_ACTIV, &st->l1.bcs->Flag);
			test_and_clear_bit(BC_FLG_BUSY, &st->l1.bcs->Flag);
			mode_tiger(st->l1.bcs, 0, st->l1.bc);
			st->l1.l1l2(st, PH_DEACTIVATE | CONFIRM, NULL);
			break;
	}
}


void
close_tigerstate(struct BCState *bcs)
{
	mode_tiger(bcs, 0, bcs->channel);
	if (test_and_clear_bit(BC_FLG_INIT, &bcs->Flag)) {
		if (bcs->hw.tiger.rcvbuf) {
			kfree(bcs->hw.tiger.rcvbuf);
			bcs->hw.tiger.rcvbuf = NULL;
		}
		if (bcs->hw.tiger.sendbuf) {
			kfree(bcs->hw.tiger.sendbuf);
			bcs->hw.tiger.sendbuf = NULL;
		}
		discard_queue(&bcs->rqueue);
		discard_queue(&bcs->squeue);
		if (bcs->tx_skb) {
			dev_kfree_skb(bcs->tx_skb);
			bcs->tx_skb = NULL;
			test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
		}
	}
}

static int
open_tigerstate(struct IsdnCardState *cs, struct BCState *bcs)
{
	if (!test_and_set_bit(BC_FLG_INIT, &bcs->Flag)) {
		if (!(bcs->hw.tiger.rcvbuf = kmalloc(HSCX_BUFMAX, GFP_ATOMIC))) {
			printk(KERN_WARNING
			       "HiSax: No memory for tiger.rcvbuf\n");
			return (1);
		}
		if (!(bcs->hw.tiger.sendbuf = kmalloc(RAW_BUFMAX, GFP_ATOMIC))) {
			printk(KERN_WARNING
			       "HiSax: No memory for tiger.sendbuf\n");
			return (1);
		}
		skb_queue_head_init(&bcs->rqueue);
		skb_queue_head_init(&bcs->squeue);
	}
	bcs->tx_skb = NULL;
	bcs->hw.tiger.sendcnt = 0;
	test_and_clear_bit(BC_FLG_BUSY, &bcs->Flag);
	bcs->event = 0;
	bcs->tx_cnt = 0;
	return (0);
}

int
setstack_tiger(struct PStack *st, struct BCState *bcs)
{
	bcs->channel = st->l1.bc;
	if (open_tigerstate(st->l1.hardware, bcs))
		return (-1);
	st->l1.bcs = bcs;
	st->l2.l2l1 = tiger_l2l1;
	setstack_manager(st);
	bcs->st = st;
	setstack_l1_B(st);
	return (0);
}

 
__initfunc(void
inittiger(struct IsdnCardState *cs))
{
	if (!(cs->bcs[0].hw.tiger.send = kmalloc(NETJET_DMA_TXSIZE * sizeof(unsigned int),
		GFP_KERNEL | GFP_DMA))) {
		printk(KERN_WARNING
		       "HiSax: No memory for tiger.send\n");
		return;
	}
	cs->bcs[0].hw.tiger.s_irq = cs->bcs[0].hw.tiger.send + NETJET_DMA_TXSIZE/2 - 1;
	cs->bcs[0].hw.tiger.s_end = cs->bcs[0].hw.tiger.send + NETJET_DMA_TXSIZE - 1;
	cs->bcs[1].hw.tiger.send = cs->bcs[0].hw.tiger.send;
	cs->bcs[1].hw.tiger.s_irq = cs->bcs[0].hw.tiger.s_irq;
	cs->bcs[1].hw.tiger.s_end = cs->bcs[0].hw.tiger.s_end;
	
	memset(cs->bcs[0].hw.tiger.send, 0xff, NETJET_DMA_TXSIZE * sizeof(unsigned int));
	debugl1(cs, "tiger: send buf %x - %x", (u_int)cs->bcs[0].hw.tiger.send,
		(u_int)(cs->bcs[0].hw.tiger.send + NETJET_DMA_TXSIZE - 1));
	outl(virt_to_bus(cs->bcs[0].hw.tiger.send),
		cs->hw.njet.base + NETJET_DMA_READ_START);
	outl(virt_to_bus(cs->bcs[0].hw.tiger.s_irq),
		cs->hw.njet.base + NETJET_DMA_READ_IRQ);
	outl(virt_to_bus(cs->bcs[0].hw.tiger.s_end),
		cs->hw.njet.base + NETJET_DMA_READ_END);
	if (!(cs->bcs[0].hw.tiger.rec = kmalloc(NETJET_DMA_RXSIZE * sizeof(unsigned int),
		GFP_KERNEL | GFP_DMA))) {
		printk(KERN_WARNING
		       "HiSax: No memory for tiger.rec\n");
		return;
	}
	debugl1(cs, "tiger: rec buf %x - %x", (u_int)cs->bcs[0].hw.tiger.rec,
		(u_int)(cs->bcs[0].hw.tiger.rec + NETJET_DMA_RXSIZE - 1));
	cs->bcs[1].hw.tiger.rec = cs->bcs[0].hw.tiger.rec;
	memset(cs->bcs[0].hw.tiger.rec, 0xff, NETJET_DMA_RXSIZE * sizeof(unsigned int));
	outl(virt_to_bus(cs->bcs[0].hw.tiger.rec),
		cs->hw.njet.base + NETJET_DMA_WRITE_START);
	outl(virt_to_bus(cs->bcs[0].hw.tiger.rec + NETJET_DMA_RXSIZE/2 - 1),
		cs->hw.njet.base + NETJET_DMA_WRITE_IRQ);
	outl(virt_to_bus(cs->bcs[0].hw.tiger.rec + NETJET_DMA_RXSIZE - 1),
		cs->hw.njet.base + NETJET_DMA_WRITE_END);
	debugl1(cs, "tiger: dmacfg  %x/%x  pulse=%d",
		inl(cs->hw.njet.base + NETJET_DMA_WRITE_ADR),
		inl(cs->hw.njet.base + NETJET_DMA_READ_ADR),
		bytein(cs->hw.njet.base + NETJET_PULSE_CNT));
	cs->hw.njet.last_is0 = 0;
	cs->bcs[0].BC_SetStack = setstack_tiger;
	cs->bcs[1].BC_SetStack = setstack_tiger;
	cs->bcs[0].BC_Close = close_tigerstate;
	cs->bcs[1].BC_Close = close_tigerstate;
}

void
releasetiger(struct IsdnCardState *cs)
{
	if (cs->bcs[0].hw.tiger.send) {
		kfree(cs->bcs[0].hw.tiger.send);
		cs->bcs[0].hw.tiger.send = NULL;
	}
	if (cs->bcs[1].hw.tiger.send) {
		cs->bcs[1].hw.tiger.send = NULL;
	}
	if (cs->bcs[0].hw.tiger.rec) {
		kfree(cs->bcs[0].hw.tiger.rec);
		cs->bcs[0].hw.tiger.rec = NULL;
	}
	if (cs->bcs[1].hw.tiger.rec) {
		cs->bcs[1].hw.tiger.rec = NULL;
	}
}

static void
netjet_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val, sval;
	long flags;

	if (!cs) {
		printk(KERN_WARNING "NETjet: Spurious interrupt!\n");
		return;
	}
	if (!((sval = bytein(cs->hw.njet.base + NETJET_IRQSTAT1)) &
		NETJET_ISACIRQ)) {
		val = ReadISAC(cs, ISAC_ISTA);
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "tiger: i1 %x %x", sval, val);
		if (val) {
			isac_interrupt(cs, val);
			WriteISAC(cs, ISAC_MASK, 0xFF);
			WriteISAC(cs, ISAC_MASK, 0x0);
		}
	}
	save_flags(flags);
	cli();
	if ((sval = bytein(cs->hw.njet.base + NETJET_IRQSTAT0))) {
		if (test_and_set_bit(FLG_LOCK_ATOMIC, &cs->HW_Flags)) {
			restore_flags(flags);
			return;
		}
		cs->hw.njet.irqstat0 = sval;
		restore_flags(flags);
/*		debugl1(cs, "tiger: ist0 %x  %x %x  %x/%x  pulse=%d",
			sval, 
			bytein(cs->hw.njet.base + NETJET_DMACTRL),
			bytein(cs->hw.njet.base + NETJET_IRQMASK0),
			inl(cs->hw.njet.base + NETJET_DMA_READ_ADR),
			inl(cs->hw.njet.base + NETJET_DMA_WRITE_ADR),
			bytein(cs->hw.njet.base + NETJET_PULSE_CNT));
*/
/*		cs->hw.njet.irqmask0 = ((0x0f & cs->hw.njet.irqstat0) ^ 0x0f) | 0x30;
*/		byteout(cs->hw.njet.base + NETJET_IRQSTAT0, cs->hw.njet.irqstat0);
/*		byteout(cs->hw.njet.base + NETJET_IRQMASK0, cs->hw.njet.irqmask0);
*/		if (cs->hw.njet.irqstat0 & NETJET_IRQM0_READ)
			read_tiger(cs);
		if (cs->hw.njet.irqstat0 & NETJET_IRQM0_WRITE)
			write_tiger(cs);
		test_and_clear_bit(FLG_LOCK_ATOMIC, &cs->HW_Flags);
	} else
		restore_flags(flags);

/*	if (!testcnt--) {
		cs->hw.njet.dmactrl = 0;
		byteout(cs->hw.njet.base + NETJET_DMACTRL,
			cs->hw.njet.dmactrl);
		byteout(cs->hw.njet.base + NETJET_IRQMASK0, 0);
	}
*/
}

static void
reset_netjet(struct IsdnCardState *cs)
{
	long flags;

	save_flags(flags);
	sti();
	cs->hw.njet.ctrl_reg = 0xff;  /* Reset On */
	byteout(cs->hw.njet.base + NETJET_CTRL, cs->hw.njet.ctrl_reg);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);	/* Timeout 10ms */
	cs->hw.njet.ctrl_reg = 0x00;  /* Reset Off and status read clear */
	byteout(cs->hw.njet.base + NETJET_CTRL, cs->hw.njet.ctrl_reg);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);	/* Timeout 10ms */
	restore_flags(flags);
	cs->hw.njet.auxd = 0;
	cs->hw.njet.dmactrl = 0;
	byteout(cs->hw.njet.base + NETJET_AUXCTRL, ~NETJET_ISACIRQ);
	byteout(cs->hw.njet.base + NETJET_IRQMASK1, NETJET_ISACIRQ);
	byteout(cs->hw.njet.auxa, cs->hw.njet.auxd);
}

void
release_io_netjet(struct IsdnCardState *cs)
{
	byteout(cs->hw.njet.base + NETJET_IRQMASK0, 0);
	byteout(cs->hw.njet.base + NETJET_IRQMASK1, 0);
	releasetiger(cs);
	release_region(cs->hw.njet.base, 256);
}


static int
NETjet_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_netjet(cs);
			return(0);
		case CARD_RELEASE:
			release_io_netjet(cs);
			return(0);
		case CARD_INIT:
			inittiger(cs);
			clear_pending_isac_ints(cs);
			initisac(cs);
			/* Reenable all IRQ */
			cs->writeisac(cs, ISAC_MASK, 0);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

static 	struct pci_dev *dev_netjet __initdata = NULL;

__initfunc(int
setup_netjet(struct IsdnCard *card))
{
	int bytecnt;
	struct IsdnCardState *cs = card->cs;
	char tmp[64];
#if CONFIG_PCI
#endif
	strcpy(tmp, NETjet_revision);
	printk(KERN_INFO "HiSax: Traverse Tech. NETjet driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ != ISDN_CTYPE_NETJET)
		return(0);
	test_and_clear_bit(FLG_LOCK_ATOMIC, &cs->HW_Flags);
#if CONFIG_PCI
	if (!pci_present()) {
		printk(KERN_ERR "Netjet: no PCI bus present\n");
		return(0);
	}
	if ((dev_netjet = pci_find_device(PCI_VENDOR_TRAVERSE_TECH,
		PCI_NETJET_ID,  dev_netjet))) {
		cs->irq = dev_netjet->irq;
		if (!cs->irq) {
			printk(KERN_WARNING "NETjet: No IRQ for PCI card found\n");
			return(0);
		}
		cs->hw.njet.base = dev_netjet->resource[ 0].start
			& PCI_BASE_ADDRESS_IO_MASK; 
		if (!cs->hw.njet.base) {
			printk(KERN_WARNING "NETjet: No IO-Adr for PCI card found\n");
			return(0);
		}
	} else {
		printk(KERN_WARNING "NETjet: No PCI card found\n");
		return(0);
	}
	cs->hw.njet.auxa = cs->hw.njet.base + NETJET_AUXDATA;
	cs->hw.njet.isac = cs->hw.njet.base | NETJET_ISAC_OFF;
	bytecnt = 256;
#else
	printk(KERN_WARNING "NETjet: NO_PCI_BIOS\n");
	printk(KERN_WARNING "NETjet: unable to config NETJET PCI\n");
	return (0);
#endif /* CONFIG_PCI */
	printk(KERN_INFO
		"NETjet: PCI card configured at 0x%x IRQ %d\n",
		cs->hw.njet.base, cs->irq);
	if (check_region(cs->hw.njet.base, bytecnt)) {
		printk(KERN_WARNING
		       "HiSax: %s config port %x-%x already in use\n",
		       CardType[card->typ],
		       cs->hw.njet.base,
		       cs->hw.njet.base + bytecnt);
		return (0);
	} else {
		request_region(cs->hw.njet.base, bytecnt, "netjet isdn");
	}
	reset_netjet(cs);
	cs->readisac  = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo  = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Read_Reg  = &dummyrr;
	cs->BC_Write_Reg = &dummywr;
	cs->BC_Send_Data = &fill_dma;
	cs->cardmsg = &NETjet_card_msg;
	cs->irq_func = &netjet_interrupt;
	cs->irq_flags |= SA_SHIRQ;
	ISACVersion(cs, "NETjet:");
	return (1);
}
