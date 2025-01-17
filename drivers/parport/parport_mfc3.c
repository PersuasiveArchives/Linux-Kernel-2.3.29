/* Low-level parallel port routines for the Multiface 3 card
 *
 * Author: Joerg Dorchain <dorchain@wirbel.com>
 *
 * (C) The elitist m68k Users(TM)
 *
 * based on the existing parport_amiga and lp_mfc
 *
 *
 * From the MFC3 documentation:
 * 
 * Miscellaneous PIA Details
 * -------------------------
 * 
 * 	The two open-drain interrupt outputs /IRQA and /IRQB are routed to
 * /INT2 of the Z2 bus.
 * 
 * 	The CPU data bus of the PIA (D0-D7) is connected to D8-D15 on the Z2
 * bus. This means that any PIA registers are accessed at even addresses.
 * 
 * Centronics Pin Connections for the PIA
 * --------------------------------------
 * 
 * 	The following table shows the connections between the PIA and the
 * Centronics interface connector. These connections implement a single, but
 * very complete, Centronics type interface. The Pin column gives the pin
 * numbers of the PIA. The Centronics pin numbers can be found in the section
 * "Parallel Connectors".
 * 
 * 
 *    Pin | PIA | Dir | Centronics Names
 * -------+-----+-----+---------------------------------------------------------
 *     19 | CB2 | --> | /STROBE (aka /DRDY)
 *  10-17 | PBx | <-> | DATA0 - DATA7
 *     18 | CB1 | <-- | /ACK
 *     40 | CA1 | <-- | BUSY
 *      3 | PA1 | <-- | PAPER-OUT (aka POUT)
 *      4 | PA2 | <-- | SELECTED (aka SEL)
 *      9 | PA7 | --> | /INIT (aka /RESET or /INPUT-PRIME)
 *      6 | PA4 | <-- | /ERROR (aka /FAULT)
 *      7 | PA5 | --> | DIR (aka /SELECT-IN)
 *      8 | PA6 | --> | /AUTO-FEED-XT
 *     39 | CA2 | --> | open
 *      5 | PA3 | <-- | /ACK (same as CB1!)
 *      2 | PA0 | <-- | BUSY (same as CA1!)
 * -------+-----+-----+---------------------------------------------------------
 * 
 * Should be enough to understand some of the driver.
 */

#include "multiface.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/parport.h>
#include <linux/delay.h>
#include <linux/mc6821.h>
#include <linux/zorro.h>
#include <asm/setup.h>
#include <asm/amigahw.h>
#include <asm/irq.h>
#include <asm/amigaints.h>

/* Maximum Number of Cards supported */
#define MAX_MFC 5

#undef DEBUG
#ifdef DEBUG
#define DPRINTK printk
#else
static inline int DPRINTK() {return 0;}
#endif

static struct parport *this_port[MAX_MFC] = {NULL, };
static volatile int dummy; /* for trigger readds */

#define pia(dev) ((struct pia *)(dev->base))
static struct parport_operations pp_mfc3_ops;

static void mfc3_write_data(struct parport *p, unsigned char data)
{
DPRINTK("write_data %c\n",data);

	dummy = pia(p)->pprb; /* clears irq bit */
	/* Triggers also /STROBE.*/
	pia(p)->pprb = data;
}

static unsigned char mfc3_read_data(struct parport *p)
{
	/* clears interupt bit. Triggers also /STROBE. */
	return pia(p)->pprb;
}

static unsigned char control_pc_to_mfc3(unsigned char control)
{
	unsigned char ret = 32|64;

	if (control & PARPORT_CONTROL_DIRECTION) /* XXX: What is this? */
		;
	if (control & PARPORT_CONTROL_INTEN) /* XXX: What is INTEN? */
		;
	if (control & PARPORT_CONTROL_SELECT) /* XXX: What is SELECP? */
		ret &= ~32; /* /SELECT_IN */
	if (control & PARPORT_CONTROL_INIT) /* INITP */
		ret |= 128;
	if (control & PARPORT_CONTROL_AUTOFD) /* AUTOLF */
		ret &= ~64;
	if (control & PARPORT_CONTROL_STROBE) /* Strobe */
		/* Handled directly by hardware */;
	return ret;
}

static unsigned char control_mfc3_to_pc(unsigned char control)
{
	unsigned char ret = PARPORT_CONTROL_INTEN | PARPORT_CONTROL_STROBE 
			  | PARPORT_CONTROL_AUTOFD | PARPORT_CONTROL_SELECT;

	if (control & 128) /* /INITP */
		ret |= PARPORT_CONTROL_INIT;
	if (control & 64) /* /AUTOLF */
		ret &= ~PARPORT_CONTROL_AUTOFD;
	if (control & 32) /* /SELECT_IN */
		ret &= ~PARPORT_CONTROL_SELECT;
	return ret;
}

static void mfc3_write_control(struct parport *p, unsigned char control)
{
DPRINTK("write_control %02x\n",control);
	pia(p)->ppra = (pia(p)->ppra & 0x1f) | control_pc_to_mfc3(control);
}
	
static unsigned char mfc3_read_control( struct parport *p)
{
DPRINTK("read_control \n");
	return control_mfc3_to_pc(pia(p)->ppra & 0xe0);
}

static unsigned char mfc3_frob_control( struct parport *p, unsigned char mask, unsigned char val)
{
	unsigned char old;

DPRINTK("frob_control mask %02x, value %02x\n",mask,val);
	old = mfc3_read_control(p);
	mfc3_write_control(p, (old & ~mask) ^ val);
	return old;
}


static unsigned char status_pc_to_mfc3(unsigned char status)
{
	unsigned char ret = 1;

	if (status & PARPORT_STATUS_BUSY) /* Busy */
		ret &= ~1;
	if (status & PARPORT_STATUS_ACK) /* Ack */
		ret |= 8;
	if (status & PARPORT_STATUS_PAPEROUT) /* PaperOut */
		ret |= 2;
	if (status & PARPORT_STATUS_SELECT) /* select */
		ret |= 4;
	if (status & PARPORT_STATUS_ERROR) /* error */
		ret |= 16;
	return ret;
}

static unsigned char status_mfc3_to_pc(unsigned char status)
{
	unsigned char ret = PARPORT_STATUS_BUSY;

	if (status & 1) /* Busy */
		ret &= ~PARPORT_STATUS_BUSY;
	if (status & 2) /* PaperOut */
		ret |= PARPORT_STATUS_PAPEROUT;
	if (status & 4) /* Selected */
		ret |= PARPORT_STATUS_SELECT;
	if (status & 8) /* Ack */
		ret |= PARPORT_STATUS_ACK;
	if (status & 16) /* /ERROR */
		ret |= PARPORT_STATUS_ERROR;

	return ret;
}

static void mfc3_write_status( struct parport *p, unsigned char status)
{
DPRINTK("write_status %02x\n",status);
	pia(p)->ppra = (pia(p)->ppra & 0xe0) | status_pc_to_mfc3(status);
}

static unsigned char mfc3_read_status(struct parport *p)
{
	unsigned char status;

	status = status_mfc3_to_pc(pia(p)->ppra & 0x1f);
DPRINTK("read_status %02x\n", status);
	return status;
}

static void mfc3_change_mode( struct parport *p, int m)
{
	/* XXX: This port only has one mode, and I am
	not sure about the corresponding PC-style mode*/
}

static int use_cnt = 0;

static void mfc3_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	int i;

	for( i = 0; i < MAX_MFC; i++)
		if (this_port[i] != NULL)
			if (pia(this_port[i])->crb & 128) { /* Board caused interrupt */
				dummy = pia(this_port[i])->pprb; /* clear irq bit */
				parport_generic_irq(irq, this_port[i], regs);
			}
}

static int mfc3_claim_resources(struct parport *p)
{
DPRINTK("claim_resources\n");
}

static void mfc3_init_state(struct parport_state *s)
{
	s->u.amiga.data = 0;
	s->u.amiga.datadir = 255;
	s->u.amiga.status = 0;
	s->u.amiga.statusdir = 0xe0;
}

static void mfc3_save_state(struct parport *p, struct parport_state *s)
{
	s->u.amiga.data = pia(p)->pprb;
	pia(p)->crb &= ~PIA_DDR;
	s->u.amiga.datadir = pia(p)->pddrb;
	pia(p)->crb |= PIA_DDR;
	s->u.amiga.status = pia(p)->ppra;
	pia(p)->cra &= ~PIA_DDR;
	s->u.amiga.statusdir = pia(p)->pddrb;
	pia(p)->cra |= PIA_DDR;
}

static void mfc3_restore_state(struct parport *p, struct parport_state *s)
{
	pia(p)->pprb = s->u.amiga.data;
	pia(p)->crb &= ~PIA_DDR;
	pia(p)->pddrb = s->u.amiga.datadir;
	pia(p)->crb |= PIA_DDR;
	pia(p)->ppra = s->u.amiga.status;
	pia(p)->cra &= ~PIA_DDR;
	pia(p)->pddrb = s->u.amiga.statusdir;
	pia(p)->cra |= PIA_DDR;
}

static void mfc3_enable_irq(struct parport *p)
{
	pia(p)->crb |= PIA_C1_ENABLE_IRQ;
}

static void mfc3_disable_irq(struct parport *p)
{
	pia(p)->crb &= ~PIA_C1_ENABLE_IRQ;
}

static void mfc3_inc_use_count(void)
{
	MOD_INC_USE_COUNT;
}

static void mfc3_dec_use_count(void)
{
	MOD_DEC_USE_COUNT;
}

static struct parport_operations pp_mfc3_ops = {
	mfc3_write_data,
	mfc3_read_data,

	mfc3_write_control,
	mfc3_read_control,
	mfc3_frob_control,

	mfc3_read_status,

	mfc3_enable_irq,
	mfc3_disable_irq,

	NULL, /* data_forward - FIXME */
	NULL, /* data_reverse - FIXME */

	mfc3_init_state,
	mfc3_save_state,
	mfc3_restore_state,

	mfc3_inc_use_count,
	mfc3_dec_use_count,

	parport_ieee1284_epp_write_data,
	parport_ieee1284_epp_read_data,
	parport_ieee1284_epp_write_addr,
	parport_ieee1284_epp_read_addr,

	parport_ieee1284_ecp_write_data,
	parport_ieee1284_ecp_read_data,
	parport_ieee1284_ecp_write_addr,

	parport_ieee1284_write_compat,
	parport_ieee1284_read_nibble,
	parport_ieee1284_read_byte,
};

/* ----------- Initialisation code --------------------------------- */

int __init parport_mfc3_init(void)
{
	struct parport *p;
	int pias = 0;
	struct pia *pp;
	unsigned int key = 0;
	const struct ConfigDev *cd;

	if (MACH_IS_AMIGA) {
		while ((key = zorro_find(ZORRO_PROD_BSC_MULTIFACE_III, 0, key))) {
			cd = zorro_get_board(key);
			pp = (struct pia *)ZTWO_VADDR((((u_char *)cd->cd_BoardAddr)+PIABASE));
			if (pias < MAX_MFC) {
				pp->crb = 0;
				pp->pddrb = 255; /* all data pins output */
				pp->crb = PIA_DDR|32|8;
				dummy = pp->pddrb; /* reading clears interrupt */
				pp->cra = 0;
				pp->pddra = 0xe0; /* /RESET,  /DIR ,/AUTO-FEED output */
				pp->cra = PIA_DDR;
				pp->ppra = 0; /* reset printer */
				udelay(10);
				pp->ppra = 128;
				if ((p = parport_register_port((unsigned long)pp,
					IRQ_AMIGA_PORTS, PARPORT_DMA_NONE,
					&pp_mfc3_ops))) {
					this_port[pias++] = p;
					printk(KERN_INFO "%s: Multiface III port using irq\n", p->name);
					/* XXX: set operating mode */
					parport_proc_register(p);

					if (p->irq != PARPORT_IRQ_NONE)
						if (use_cnt++ == 0)
							if (request_irq(IRQ_AMIGA_PORTS, mfc3_interrupt, SA_SHIRQ, p->name, &pp_mfc3_ops))
								use_cnt--;

					if (parport_probe_hook)
						(*parport_probe_hook)(p);
					zorro_config_board(key, 0);
					p->private_data = (void *)key;
					parport_announce_port (p);
				}
			}
		}
	}
	return pias;
}

#ifdef MODULE

MODULE_AUTHOR("Joerg Dorchain");
MODULE_DESCRIPTION("Parport Driver for Multiface 3 expansion cards Paralllel Port");
MODULE_SUPPORTED_DEVICE("Multiface 3 Parallel Port");

int init_module(void)
{
	return ! parport_mfc3_init();
}

void cleanup_module(void)
{
	int i;

	for (i = 0; i < MAX_MFC; i++)
		if (this_port[i] != NULL) {
			if (p->irq != PARPORT_IRQ_NONE) 
				if (--use_cnt == 0) 
			free_irq(IRQ_AMIGA_PORTS, &pp_mfc3_ops);
			parport_proc_unregister(this_port[i]);
			parport_unregister_port(this_port[i]);
			zorro_unconfig_board((unsigned int)this_port[i]->private_data, 0);
		}
}
#endif


