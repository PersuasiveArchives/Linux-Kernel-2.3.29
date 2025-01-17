/* $Id: teleint.c,v 1.11 1999/09/04 06:20:06 keil Exp $

 * teleint.c     low level stuff for TeleInt isdn cards
 *
 * Author     Karsten Keil (keil@isdn4linux.de)
 *
 *
 * $Log: teleint.c,v $
 * Revision 1.11  1999/09/04 06:20:06  keil
 * Changes from kernel set_current_state()
 *
 * Revision 1.10  1999/08/31 11:20:27  paul
 * various spelling corrections (new checksums may be needed, Karsten!)
 *
 * Revision 1.9  1999/07/12 21:05:30  keil
 * fix race in IRQ handling
 * added watchdog for lost IRQs
 *
 * Revision 1.8  1999/07/01 08:12:12  keil
 * Common HiSax version for 2.0, 2.1, 2.2 and 2.3 kernel
 *
 * Revision 1.7  1998/11/15 23:55:26  keil
 * changes from 2.0
 *
 * Revision 1.6  1998/04/15 16:45:31  keil
 * new init code
 *
 * Revision 1.5  1998/02/02 13:40:47  keil
 * fast io
 *
 * Revision 1.4  1997/11/08 21:35:53  keil
 * new l1 init
 *
 * Revision 1.3  1997/11/06 17:09:30  keil
 * New 2.1 init code
 *
 * Revision 1.2  1997/10/29 18:55:53  keil
 * changes for 2.1.60 (irq2dev_map)
 *
 * Revision 1.1  1997/09/11 17:32:32  keil
 * new
 *
 *
 */

#define __NO_VERSION__
#include "hisax.h"
#include "isac.h"
#include "hfc_2bs0.h"
#include "isdnl1.h"

extern const char *CardType[];

const char *TeleInt_revision = "$Revision: 1.11 $";

#define byteout(addr,val) outb(val,addr)
#define bytein(addr) inb(addr)

static inline u_char
readreg(unsigned int ale, unsigned int adr, u_char off)
{
	register u_char ret;
	int max_delay = 2000;
	long flags;

	save_flags(flags);
	cli();
	byteout(ale, off);
	ret = HFC_BUSY & bytein(ale);
	while (ret && --max_delay)
		ret = HFC_BUSY & bytein(ale);
	if (!max_delay) {
		printk(KERN_WARNING "TeleInt Busy not inactive\n");
		restore_flags(flags);
		return (0);
	}
	ret = bytein(adr);
	restore_flags(flags);
	return (ret);
}

static inline void
readfifo(unsigned int ale, unsigned int adr, u_char off, u_char * data, int size)
{
	register u_char ret;
	register int max_delay = 20000;
	register int i;
	
	byteout(ale, off);
	for (i = 0; i<size; i++) {
		ret = HFC_BUSY & bytein(ale);
		while (ret && --max_delay)
			ret = HFC_BUSY & bytein(ale);
		if (!max_delay) {
			printk(KERN_WARNING "TeleInt Busy not inactive\n");
			return;
		}
		data[i] = bytein(adr);
	}
}


static inline void
writereg(unsigned int ale, unsigned int adr, u_char off, u_char data)
{
	register u_char ret;
	int max_delay = 2000;
	long flags;

	save_flags(flags);
	cli();
	byteout(ale, off);
	ret = HFC_BUSY & bytein(ale);
	while (ret && --max_delay)
		ret = HFC_BUSY & bytein(ale);
	if (!max_delay) {
		printk(KERN_WARNING "TeleInt Busy not inactive\n");
		restore_flags(flags);
		return;
	}
	byteout(adr, data);
	restore_flags(flags);
}

static inline void
writefifo(unsigned int ale, unsigned int adr, u_char off, u_char * data, int size)
{
	register u_char ret;
	register int max_delay = 20000;
	register int i;
	
	/* fifo write without cli because it's allready done  */
	byteout(ale, off);
	for (i = 0; i<size; i++) {
		ret = HFC_BUSY & bytein(ale);
		while (ret && --max_delay)
			ret = HFC_BUSY & bytein(ale);
		if (!max_delay) {
			printk(KERN_WARNING "TeleInt Busy not inactive\n");
			return;
		}
		byteout(adr, data[i]);
	}
}

/* Interface functions */

static u_char
ReadISAC(struct IsdnCardState *cs, u_char offset)
{
	cs->hw.hfc.cip = offset;
	return (readreg(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, offset));
}

static void
WriteISAC(struct IsdnCardState *cs, u_char offset, u_char value)
{
	cs->hw.hfc.cip = offset;
	writereg(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, offset, value);
}

static void
ReadISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	cs->hw.hfc.cip = 0;
	readfifo(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, 0, data, size);
}

static void
WriteISACfifo(struct IsdnCardState *cs, u_char * data, int size)
{
	cs->hw.hfc.cip = 0;
	writefifo(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, 0, data, size);
}

static u_char
ReadHFC(struct IsdnCardState *cs, int data, u_char reg)
{
	register u_char ret;

	if (data) {
		cs->hw.hfc.cip = reg;
		byteout(cs->hw.hfc.addr | 1, reg);
		ret = bytein(cs->hw.hfc.addr);
		if (cs->debug & L1_DEB_HSCX_FIFO && (data != 2))
			debugl1(cs, "hfc RD %02x %02x", reg, ret);
	} else
		ret = bytein(cs->hw.hfc.addr | 1);
	return (ret);
}

static void
WriteHFC(struct IsdnCardState *cs, int data, u_char reg, u_char value)
{
	byteout(cs->hw.hfc.addr | 1, reg);
	cs->hw.hfc.cip = reg;
	if (data)
		byteout(cs->hw.hfc.addr, value);
	if (cs->debug & L1_DEB_HSCX_FIFO && (data != 2))
		debugl1(cs, "hfc W%c %02x %02x", data ? 'D' : 'C', reg, value);
}

static void
TeleInt_interrupt(int intno, void *dev_id, struct pt_regs *regs)
{
	struct IsdnCardState *cs = dev_id;
	u_char val;

	if (!cs) {
		printk(KERN_WARNING "TeleInt: Spurious interrupt!\n");
		return;
	}
	val = readreg(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, ISAC_ISTA);
      Start_ISAC:
	if (val)
		isac_interrupt(cs, val);
	val = readreg(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, ISAC_ISTA);
	if (val) {
		if (cs->debug & L1_DEB_ISAC)
			debugl1(cs, "ISAC IntStat after IntRoutine");
		goto Start_ISAC;
	}
	writereg(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, ISAC_MASK, 0xFF);
	writereg(cs->hw.hfc.addr | 1, cs->hw.hfc.addr, ISAC_MASK, 0x0);
}

static void
TeleInt_Timer(struct IsdnCardState *cs)
{
	int stat = 0;

	if (cs->bcs[0].mode) {
		stat |= 1;
		main_irq_hfc(&cs->bcs[0]);
	}
	if (cs->bcs[1].mode) {
		stat |= 2;
		main_irq_hfc(&cs->bcs[1]);
	}
	cs->hw.hfc.timer.expires = jiffies + 1;
	add_timer(&cs->hw.hfc.timer);
}

void
release_io_TeleInt(struct IsdnCardState *cs)
{
	del_timer(&cs->hw.hfc.timer);
	releasehfc(cs);
	if (cs->hw.hfc.addr)
		release_region(cs->hw.hfc.addr, 2);
}

static void
reset_TeleInt(struct IsdnCardState *cs)
{
	long flags;

	printk(KERN_INFO "TeleInt: resetting card\n");
	cs->hw.hfc.cirm |= HFC_RESET;
	byteout(cs->hw.hfc.addr | 1, cs->hw.hfc.cirm);	/* Reset On */
	save_flags(flags);
	sti();
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((30*HZ)/1000);
	cs->hw.hfc.cirm &= ~HFC_RESET;
	byteout(cs->hw.hfc.addr | 1, cs->hw.hfc.cirm);	/* Reset Off */
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout((10*HZ)/1000);
	restore_flags(flags);
}

static int
TeleInt_card_msg(struct IsdnCardState *cs, int mt, void *arg)
{
	switch (mt) {
		case CARD_RESET:
			reset_TeleInt(cs);
			return(0);
		case CARD_RELEASE:
			release_io_TeleInt(cs);
			return(0);
		case CARD_INIT:
			inithfc(cs);
			clear_pending_isac_ints(cs);
			initisac(cs);
			/* Reenable all IRQ */
			cs->writeisac(cs, ISAC_MASK, 0);
			cs->writeisac(cs, ISAC_CMDR, 0x41);
			cs->hw.hfc.timer.expires = jiffies + 1;
			add_timer(&cs->hw.hfc.timer);
			return(0);
		case CARD_TEST:
			return(0);
	}
	return(0);
}

__initfunc(int
setup_TeleInt(struct IsdnCard *card))
{
	struct IsdnCardState *cs = card->cs;
	char tmp[64];

	strcpy(tmp, TeleInt_revision);
	printk(KERN_INFO "HiSax: TeleInt driver Rev. %s\n", HiSax_getrev(tmp));
	if (cs->typ != ISDN_CTYPE_TELEINT)
		return (0);

	cs->hw.hfc.addr = card->para[1] & 0x3fe;
	cs->irq = card->para[0];
	cs->hw.hfc.cirm = HFC_CIRM;
	cs->hw.hfc.isac_spcr = 0x00;
	cs->hw.hfc.cip = 0;
	cs->hw.hfc.ctmt = HFC_CTMT | HFC_CLTIMER;
	cs->bcs[0].hw.hfc.send = NULL;
	cs->bcs[1].hw.hfc.send = NULL;
	cs->hw.hfc.fifosize = 7 * 1024 + 512;
	cs->hw.hfc.timer.function = (void *) TeleInt_Timer;
	cs->hw.hfc.timer.data = (long) cs;
	init_timer(&cs->hw.hfc.timer);
	if (check_region((cs->hw.hfc.addr), 2)) {
		printk(KERN_WARNING
		       "HiSax: %s config port %x-%x already in use\n",
		       CardType[card->typ],
		       cs->hw.hfc.addr,
		       cs->hw.hfc.addr + 2);
		return (0);
	} else {
		request_region(cs->hw.hfc.addr, 2, "TeleInt isdn");
	}
	/* HW IO = IO */
	byteout(cs->hw.hfc.addr, cs->hw.hfc.addr & 0xff);
	byteout(cs->hw.hfc.addr | 1, ((cs->hw.hfc.addr & 0x300) >> 8) | 0x54);
	switch (cs->irq) {
		case 3:
			cs->hw.hfc.cirm |= HFC_INTA;
			break;
		case 4:
			cs->hw.hfc.cirm |= HFC_INTB;
			break;
		case 5:
			cs->hw.hfc.cirm |= HFC_INTC;
			break;
		case 7:
			cs->hw.hfc.cirm |= HFC_INTD;
			break;
		case 10:
			cs->hw.hfc.cirm |= HFC_INTE;
			break;
		case 11:
			cs->hw.hfc.cirm |= HFC_INTF;
			break;
		default:
			printk(KERN_WARNING "TeleInt: wrong IRQ\n");
			release_io_TeleInt(cs);
			return (0);
	}
	byteout(cs->hw.hfc.addr | 1, cs->hw.hfc.cirm);
	byteout(cs->hw.hfc.addr | 1, cs->hw.hfc.ctmt);

	printk(KERN_INFO
	       "TeleInt: defined at 0x%x IRQ %d\n",
	       cs->hw.hfc.addr,
	       cs->irq);

	reset_TeleInt(cs);
	cs->readisac = &ReadISAC;
	cs->writeisac = &WriteISAC;
	cs->readisacfifo = &ReadISACfifo;
	cs->writeisacfifo = &WriteISACfifo;
	cs->BC_Read_Reg = &ReadHFC;
	cs->BC_Write_Reg = &WriteHFC;
	cs->cardmsg = &TeleInt_card_msg;
	cs->irq_func = &TeleInt_interrupt;
	ISACVersion(cs, "TeleInt:");
	return (1);
}
