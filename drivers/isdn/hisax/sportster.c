/* $Id: sportster.c,v 1.10 1999/09/04 06:20:06 keil Exp $

 * sportster.c     low level stuff for USR Sportster internal TA
 *
 * Author       Karsten Keil (keil@temic-ech.spacenet.de)
 *
 * Thanks to Christian "naddy" Weisgerber (3Com, US Robotics) for documentation
 *
 * $Log: sportster.c,v $
 * Revision 1.10  1999/09/04 06:20:06  keil
 * Changes from kernel set_current_state()
 *
 * Revision 1.9  1999/07/12 21:05:29  keil
 * fix race in IRQ handling
 * added watchdog for lost IRQs
 *
 * Revision 1.8  1999/07/01 08:12:10  keil
 * Common HiSax version for 2.0, 2.1, 2.2 and 2.3 kernel
 *
 * Revision 1.7  1998/11/15 23:55:22  keil
 * changes from 2.0
 *
 * Revision 1.6  1998/04/15 16:44:35  keil
 * new init code
 *
 * Revision 1.5  1998/02/02 13:29:46  keil
 * fast io
 *
 * Revision 1.4  1997/11/08 21:35:52  keil
 * new l1 init
 *
 * Revision 1.3  1997/11/06 17:09:29  keil
 * New 2.1 init code
 *
 * Revision 1.2  1997/10/29 18:51:18  keil
 * New files
 *
 * Revision 1.1.2.1  1997/10/17 22:10:58  keil
 * new files on 2.0
 *
 */
#define __NO_VERSION__
#include "hisax.h"
#include "isac.h"
#include "hscx.h"
#include "isdnl1.h"

extern const char *CardType[];
const char *sportster_revision = "$Revision: 1.10 $";

#define byteout(addr,val) outb(val,addr)
#define bytein(addr) inb(addr)

#define	 SPORTSTER_ISAC		0xC000
#define	 SPORTSTER_HSCXA	0x0000
#define	 SPORTSTER_HSCXB	0x4000
#define	 SPORTSTER_RES_IRQ	0x8000
#define	 SPORTSTER_RESET	0x80
#define	 SPORTSTER_INTE		0x40

static inline int
calc_off(unsigned int base, unsigned int off)
{
	return(base + ((off & 0xfc)<<8) + ((off & 3)<<1));
}

static inline void
read_fifo(unsigned int adr, u_char * data, int size)
{
	insb(adr, data, size);
}

static void
write_fifo(unsigned int adr, u_char * data, int size)
{
	outsb(adr, data, size);
}

/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	return (bytein(calc_off(cs->hw.spt.isac, offset)));
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	byteout(calc_off(cs->hw.spt.isac, offset), value);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	read_fifo(cs->hw.spt.isac, data, size);
}

static void
WriteISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	write_fifo(cs->hw.spt.isac, data, size);
}

static u_char
ReadHSCX(struct IsdnCardState *cs, int hscx, u_char offset)
{
	return (bytein(calc_off(cs->hw.spt.hscx[hscx], offset)));
}

static void
WriteHSCX(struct IsdnCardState *cs, int hscx, u_char offset, u_char value)
{
	byteout(calc_off(cs->hw.spt.hscx[hscx], offset), value);
}

/*
 * fast interrupt HSCX stuff goes here
 */

#define READHSCX(cs, nr, reg) bytein(calc_off(cs->hw.spt.hscx[nr], reg))
#define WRITEHSCX(cs, nr, reg, data) byteout(calc_off(cs->hw.spt.hscx[nr], reg), data)
#define READHSCXFIFO(cs, nr, ptr, cnt) read_fifo(cs->hw.spt.hscx[nr], ptr, cnt)
#define WRITEHSCXFIFO(cs, nr, ptr, cnt) write_fifo(cs->hw.spt.hscx[nr], ptr, cnt)

#include "hscx_irq.c"

static void
sportster_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val;

	if (!cs) {
		printk(KERN_WARNING "Sportster: Spurious interrupt!\n");
		return;
	}
	val = READHSCX(cs, 1, HSCX_ISTA);
      Start_HSCX:
	if (val)
		hscx_int_main(cs, val);
	val = ReadISAC(cs, ISAC_ISTA);
      Start_ISAC:
	if (val)
		isac_interrupt(cs, val);
	val = READHSCX(cs, 1, HSCX_ISTA);
	if (val) {
		if (cs->debug & L1_DEB_HSCX)
			debugl1(cs, "HSCX IntStat after IntRoutine");
		goto Start_HSCX;
	}
	val = ReadISAC(cs, ISAC_ISTA);
	if (val) {
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "ISAC IntStat after IntRoutine");
		goto Start_ISAC;
	}
	/* get a new irq impulse if there any pending */
	bytein(cs->hw.spt.cfg_reg + SPORTSTER_RES_IRQ +1);
}

void
release_io_sportster(struct IsdnCardState *cs)
{
	int i, adr;

	byteout(cs->hw.spt.cfg_reg + SPORTSTER_RES_IRQ, 0);
	for (i=0; i<64; i++) {
		adr = cs->hw.spt.cfg_reg + i *1024;
		release_region(adr, 8);
	}
}

void
reset_sportster(struct IsdnCardState *cs)
{
	long flags;

	cs->hw.spt.res_irq |= SPORTSTER_RESET; /* Reset On */
	byteout(cs->hw.spt.cfg_reg + SPORTSTER_RES_IRQ, cs->hw.spt.res_irq);
	save_flags(flags);
	sti();
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);
	cs->hw.spt.res_irq &= ~SPORTSTER_RESET; /* Reset Off */
	byteout(cs->hw.spt.cfg_reg + SPORTSTER_RES_IRQ, cs->hw.spt.res_irq);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);
	restore_flags(flags);
}

static int
Sportster_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_sportster(cs);
			return(0);
		case CARD_RELEASE:
			release_io_sportster(cs);
			return(0);
		case CARD_INIT:
			inithscxisac(cs, 1);
			cs->hw.spt.res_irq |= SPORTSTER_INTE; /* IRQ On */
			byteout(cs->hw.spt.cfg_reg + SPORTSTER_RES_IRQ, cs->hw.spt.res_irq);
			inithscxisac(cs, 2);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

__initfunc(int
get_io_range(struct IsdnCardState *cs))
{
	int i, j, adr;
	
	for (i=0;i<64;i++) {
		adr = cs->hw.spt.cfg_reg + i *1024;
		if (check_region(adr, 8)) {
			printk(KERN_WARNING
				"HiSax: %s config port %x-%x already in use\n",
				CardType[cs->typ], adr, adr + 8);
			break;
		} else
			request_region(adr, 8, "sportster");
	}
	if (i==64)
		return(1);
	else {
		for (j=0; j<i; j++) {
			adr = cs->hw.spt.cfg_reg + j *1024;
			release_region(adr, 8);
		}
		return(0);
	}
}

__initfunc(int
setup_sportster(struct IsdnCard *card))
{
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, sportster_revision);
	printk(KERN_INFO "HiSax: USR Sportster driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ != ISDN_CTYPE_SPORTSTER)
		return (0);

	cs->hw.spt.cfg_reg = card->para[1];
	cs->irq = card->para[0];
	if (!get_io_range(cs))
		return (0);
	cs->hw.spt.isac = cs->hw.spt.cfg_reg + SPORTSTER_ISAC;
	cs->hw.spt.hscx[0] = cs->hw.spt.cfg_reg + SPORTSTER_HSCXA;
	cs->hw.spt.hscx[1] = cs->hw.spt.cfg_reg + SPORTSTER_HSCXB;
	
	switch(cs->irq) {
		case 5:	cs->hw.spt.res_irq = 1;
			break;
		case 7:	cs->hw.spt.res_irq = 2;
			break;
		case 10:cs->hw.spt.res_irq = 3;
			break;
		case 11:cs->hw.spt.res_irq = 4;
			break;
		case 12:cs->hw.spt.res_irq = 5;
			break;
		case 14:cs->hw.spt.res_irq = 6;
			break;
		case 15:cs->hw.spt.res_irq = 7;
			break;
		default:release_io_sportster(cs);
			printk(KERN_WARNING "Sportster: wrong IRQ\n");
			return(0);
	}
	reset_sportster(cs);
	printk(KERN_INFO
	       "HiSax: %s config irq:%d cfg:0x%X\n",
	       CardType[cs->typ], cs->irq,
	       cs->hw.spt.cfg_reg);

	cs->readisac = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Read_Reg = &ReadHSCX;
	cs->BC_Write_Reg = &WriteHSCX;
	cs->BC_Send_Data = &hscx_fill_fifo;
	cs->cardmsg = &Sportster_card_msg;
	cs->irq_func = &sportster_interrupt;
	ISACVersion(cs, "Sportster:");
	if (HscxVersion(cs, "Sportster:")) {
		printk(KERN_WARNING
		       "Sportster: wrong HSCX versions check IO address\n");
		release_io_sportster(cs);
		return (0);
	}
	return (1);
}
