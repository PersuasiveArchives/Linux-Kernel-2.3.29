/*
 *  arch/m68k/mvme16x/config.c
 *
 *  Copyright (C) 1995 Richard Hirst [richard@sleepie.demon.co.uk]
 *
 * Based on:
 *
 *  linux/amiga/config.c
 *
 *  Copyright (C) 1993 Hamish Macdonald
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file README.legal in the main directory of this archive
 * for more details.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/kd.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/major.h>

#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/traps.h>
#include <asm/machdep.h>
#include <asm/mvme16xhw.h>

int atari_SCC_reset_done = 1;		/* So SCC doesn't get reset */
u_long atari_mch_cookie = 0;

static MK48T08ptr_t volatile rtc = (MK48T08ptr_t)MVME_RTC_BASE;

extern void mvme16x_process_int (int level, struct pt_regs *regs);
extern void mvme16x_init_IRQ (void);
extern void mvme16x_free_irq (unsigned int, void *);
extern int  mvme16x_get_irq_list (char *);
extern void mvme16x_enable_irq (unsigned int);
extern void mvme16x_disable_irq (unsigned int);
static void mvme16x_get_model(char *model);
static int  mvme16x_get_hardware_list(char *buffer);
extern int  mvme16x_request_irq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *), unsigned long flags, const char *devname, void *dev_id);
extern void mvme16x_sched_init(void (*handler)(int, void *, struct pt_regs *));
extern int  mvme16x_keyb_init(void);
extern int  mvme16x_kbdrate (struct kbd_repeat *);
extern unsigned long mvme16x_gettimeoffset (void);
extern void mvme16x_gettod (int *year, int *mon, int *day, int *hour,
                           int *min, int *sec);
extern int mvme16x_hwclk (int, struct hwclk_time *);
extern int mvme16x_set_clock_mmss (unsigned long);
extern void mvme16x_check_partition (struct gendisk *hd, unsigned int dev);
extern void mvme16x_mksound( unsigned int count, unsigned int ticks );
extern void mvme16x_reset (void);
extern void mvme16x_waitbut(void);

int bcd2int (unsigned char b);

/* Save tick handler routine pointer, will point to do_timer() in
 * kernel/sched.c, called via mvme16x_process_int() */

static void (*tick_handler)(int, void *, struct pt_regs *);


unsigned short mvme16x_config;


int mvme16x_kbdrate (struct kbd_repeat *k)
{
	return 0;
}

void mvme16x_mksound( unsigned int count, unsigned int ticks )
{
}

void mvme16x_reset()
{
	printk ("\r\n\nCalled mvme16x_reset\r\n"
			"\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r");
	/* The string of returns is to delay the reset until the whole
	 * message is output.  Assert reset bit in GCSR */
	*(volatile char *)0xfff40107 = 0x80;
}

static void mvme16x_get_model(char *model)
{
    p_bdid p = (p_bdid)mvme_bdid_ptr;
    char suf[4];

    suf[1] = p->brdsuffix[0];
    suf[2] = p->brdsuffix[1];
    suf[3] = '\0';
    suf[0] = suf[1] ? '-' : '\0';

    sprintf(model, "Motorola MVME%x%s", p->brdno, suf);
}


static int mvme16x_get_hardware_list(char *buffer)
{
    p_bdid p = (p_bdid)mvme_bdid_ptr;
    int len = 0;

    if (p->brdno == 0x0162 || p->brdno == 0x0172)
    {
	unsigned char rev = *(unsigned char *)MVME162_VERSION_REG;

	len += sprintf (buffer+len, "VMEchip2        %spresent\n",
			rev & MVME16x_CONFIG_NO_VMECHIP2 ? "NOT " : "");
	len += sprintf (buffer+len, "SCSI interface  %spresent\n",
			rev & MVME16x_CONFIG_NO_SCSICHIP ? "NOT " : "");
	len += sprintf (buffer+len, "Ethernet i/f    %spresent\n",
			rev & MVME16x_CONFIG_NO_ETHERNET ? "NOT " : "");
    }
    else
	*buffer = '\0';

    return (len);
}


#define pcc2chip	((volatile u_char *)0xfff42000)
#define PccSCCMICR	0x1d
#define PccSCCTICR	0x1e
#define PccSCCRICR	0x1f

void __init config_mvme16x(void)
{
    p_bdid p = (p_bdid)mvme_bdid_ptr;
    char id[40];

    mach_sched_init      = mvme16x_sched_init;
    mach_keyb_init       = mvme16x_keyb_init;
    mach_kbdrate         = mvme16x_kbdrate;
    mach_init_IRQ        = mvme16x_init_IRQ;
    mach_gettimeoffset   = mvme16x_gettimeoffset;
    mach_gettod  	 = mvme16x_gettod;
    mach_hwclk           = mvme16x_hwclk;
    mach_set_clock_mmss	 = mvme16x_set_clock_mmss;
/*  kd_mksound           = mvme16x_mksound; */
    mach_reset		 = mvme16x_reset;
    mach_free_irq	 = mvme16x_free_irq;
    mach_process_int	 = mvme16x_process_int;
    mach_get_irq_list	 = mvme16x_get_irq_list;
    mach_request_irq	 = mvme16x_request_irq;
    enable_irq           = mvme16x_enable_irq;
    disable_irq          = mvme16x_disable_irq;
    mach_get_model       = mvme16x_get_model;
    mach_get_hardware_list = mvme16x_get_hardware_list;

    /* Report board revision */

    if (strncmp("BDID", p->bdid, 4))
    {
	printk ("\n\nBug call .BRD_ID returned garbage - giving up\n\n");
	while (1)
		;
    }
    mvme16x_get_model(id);
    printk ("\nBRD_ID: %s   BUG %x.%x %02x/%02x/%02x\n", id, p->rev>>4,
					p->rev&0xf, p->yr, p->mth, p->day);
    if (p->brdno == 0x0162 || p->brdno == 0x172)
    {
	unsigned char rev = *(unsigned char *)MVME162_VERSION_REG;

	mvme16x_config = rev | MVME16x_CONFIG_GOT_SCCA;

	printk ("MVME%x Hardware status:\n", p->brdno);
	printk ("    CPU Type           68%s040\n",
			rev & MVME16x_CONFIG_GOT_FPU ? "" : "LC");
	printk ("    CPU clock          %dMHz\n",
			rev & MVME16x_CONFIG_SPEED_32 ? 32 : 25);
	printk ("    VMEchip2           %spresent\n",
			rev & MVME16x_CONFIG_NO_VMECHIP2 ? "NOT " : "");
	printk ("    SCSI interface     %spresent\n",
			rev & MVME16x_CONFIG_NO_SCSICHIP ? "NOT " : "");
	printk ("    Ethernet interface %spresent\n",
			rev & MVME16x_CONFIG_NO_ETHERNET ? "NOT " : "");
    }
    else
    {
	mvme16x_config = MVME16x_CONFIG_GOT_LP | MVME16x_CONFIG_GOT_CD2401;

	/* Dont allow any interrupts from the CD2401 until the interrupt */
	/* handlers are installed					 */

	pcc2chip[PccSCCMICR] = 0x10;
	pcc2chip[PccSCCTICR] = 0x10;
	pcc2chip[PccSCCRICR] = 0x10;
    }
}

static void mvme16x_abort_int (int irq, void *dev_id, struct pt_regs *fp)
{
	p_bdid p = (p_bdid)mvme_bdid_ptr;
	unsigned long *new = (unsigned long *)vectors;
	unsigned long *old = (unsigned long *)0xffe00000;
	volatile unsigned char uc, *ucp;

	if (p->brdno == 0x0162 || p->brdno == 0x172)
	{
		ucp = (volatile unsigned char *)0xfff42043;
		uc = *ucp | 8;
		*ucp = uc;
	}
	else
	{
		*(volatile unsigned long *)0xfff40074 = 0x40000000;
	}
	*(new+4) = *(old+4);		/* Illegal instruction */
	*(new+9) = *(old+9);		/* Trace */
	*(new+47) = *(old+47);		/* Trap #15 */

	if (p->brdno == 0x0162 || p->brdno == 0x172)
		*(new+0x5e) = *(old+0x5e);	/* ABORT switch */
	else
		*(new+0x6e) = *(old+0x6e);	/* ABORT switch */
}

static void mvme16x_timer_int (int irq, void *dev_id, struct pt_regs *fp)
{
    *(volatile unsigned char *)0xfff4201b |= 8;
    tick_handler(irq, dev_id, fp);
}

void mvme16x_sched_init (void (*timer_routine)(int, void *, struct pt_regs *))
{
    p_bdid p = (p_bdid)mvme_bdid_ptr;
    int irq;

    tick_handler = timer_routine;
    /* Using PCCchip2 or MC2 chip tick timer 1 */
    *(volatile unsigned long *)0xfff42008 = 0;
    *(volatile unsigned long *)0xfff42004 = 10000;	/* 10ms */
    *(volatile unsigned char *)0xfff42017 |= 3;
    *(volatile unsigned char *)0xfff4201b = 0x16;
    if (request_irq(MVME16x_IRQ_TIMER, mvme16x_timer_int, 0,
				"timer", mvme16x_timer_int))
	panic ("Couldn't register timer int");

    if (p->brdno == 0x0162 || p->brdno == 0x172)
	irq = MVME162_IRQ_ABORT;
    else
        irq = MVME167_IRQ_ABORT;
    if (request_irq(irq, mvme16x_abort_int, 0,
				"abort", mvme16x_abort_int))
	panic ("Couldn't register abort int");
}


/* This is always executed with interrupts disabled.  */
unsigned long mvme16x_gettimeoffset (void)
{
    return (*(volatile unsigned long *)0xfff42008);
}

extern void mvme16x_gettod (int *year, int *mon, int *day, int *hour,
                           int *min, int *sec)
{
	rtc->ctrl = RTC_READ;
	*year = bcd2int (rtc->bcd_year);
	*mon = bcd2int (rtc->bcd_mth);
	*day = bcd2int (rtc->bcd_dom);
	*hour = bcd2int (rtc->bcd_hr);
	*min = bcd2int (rtc->bcd_min);
	*sec = bcd2int (rtc->bcd_sec);
	rtc->ctrl = 0;
}

int bcd2int (unsigned char b)
{
	return ((b>>4)*10 + (b&15));
}

int mvme16x_hwclk(int op, struct hwclk_time *t)
{
	return 0;
}

int mvme16x_set_clock_mmss (unsigned long nowtime)
{
	return 0;
}

int mvme16x_keyb_init (void)
{
	return 0;
}

/*-------------------  Serial console stuff ------------------------*/

extern void mvme167_serial_console_setup(int cflag);
extern void serial167_write(struct console *co, const char *str, unsigned cnt);
extern void vme_scc_write(struct console *co, const char *str, unsigned cnt);


void mvme16x_init_console_port (struct console *co, int cflag)
{
	p_bdid p = (p_bdid)mvme_bdid_ptr;

	switch (p->brdno)
	{
#ifdef CONFIG_MVME162_SCC
	case 0x0162:
	case 0x0172:
		co->write = vme_scc_write;
		return;
#endif
#ifdef CONFIG_SERIAL167
	case 0x0166:
	case 0x0167:
	case 0x0176:
	case 0x0177:
		co->write = serial167_write;
		mvme167_serial_console_setup (cflag);
		return;
#endif
	default:
		panic ("No console support for MVME%x\n", p->brdno);
	}
	return;
}


#ifdef CONFIG_MVME162_SCC

static void scc_delay (void)
{
	int n;
	volatile int trash;

	for (n = 0; n < 20; n++)
		trash = n;
}

static void scc_write (char ch)
{
	volatile char *p = (volatile char *)MVME_SCC_A_ADDR;

	do {
		scc_delay();
	}
	while (!(*p & 4));
	scc_delay();
	*p = 8;
	scc_delay();
	*p = ch;
}


void vme_scc_write (struct console *co, const char *str, unsigned count)
{
	unsigned long	flags;

	save_flags(flags);
	cli();

	while (count--)
	{
		if (*str == '\n')
			scc_write ('\r');
		scc_write (*str++);
	}
	restore_flags(flags);
}
#endif
