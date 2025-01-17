/*
* cycx_main.c	Cyclades Cyclom 2X WAN Link Driver. Main module.
*
* Author:	Arnaldo Carvalho de Melo <acme@conectiva.com.br>
*
* Copyright:	(c) 1998, 1999 Arnaldo Carvalho de Melo
*
* Based on sdlamain.c by Gene Kozin <genek@compuserve.com> &
*			 Jaspreet Singh	<jaspreet@sangoma.com>
*
*		This program is free software; you can redistribute it and/or
*		modify it under the terms of the GNU General Public License
*		as published by the Free Software Foundation; either version
*		2 of the License, or (at your option) any later version.
* ============================================================================
* 1999/11/06	acme		cycx_down back to life (it needs to be
*				called to iounmap the dpmbase)
* 1999/08/09	acme		removed references to enable_tx_int
*				use spinlocks instead of cli/sti in
*				cyclomx_set_state
* 1999/05/19	acme		works directly linked into the kernel
*				init_waitqueue_head for 2.3.* kernel
* 1999/05/18	acme		major cleanup (polling not needed), etc
* 1998/08/28	acme		minor cleanup (ioctls for firmware deleted)
*				queue_task activated
* 1998/08/08	acme		Initial version.
*/

#include <linux/config.h>	/* OS configuration options */
#include <linux/stddef.h>	/* offsetof(), etc. */
#include <linux/errno.h>	/* return codes */
#include <linux/string.h>	/* inline memset(), etc. */
#include <linux/malloc.h>	/* kmalloc(), kfree() */
#include <linux/kernel.h>	/* printk(), and other useful stuff */
#include <linux/module.h>	/* support for loadable modules */
#include <linux/ioport.h>	/* request_region(), release_region() */
#include <linux/tqueue.h>	/* for kernel task queues */
#include <linux/wanrouter.h>	/* WAN router definitions */
#include <linux/cyclomx.h>	/* cyclomx common user API definitions */
#include <asm/uaccess.h>	/* kernel <-> user copy */
#include <linux/init.h>         /* __init (when not using as a module) */

#ifdef MODULE
MODULE_AUTHOR("Arnaldo Carvalho de Melo");
MODULE_DESCRIPTION("Cyclom 2X Sync Card Driver.");
#endif

/* Defines & Macros */

#define	DRV_VERSION	0		/* version number */
#define	DRV_RELEASE	4		/* release (minor version) number */
#define	MAX_CARDS	1		/* max number of adapters */

#ifndef	CONFIG_CYCLOMX_CARDS		/* configurable option */
#define	CONFIG_CYCLOMX_CARDS 1
#endif

/* Function Prototypes */

/* Module entry points */
int init_module (void);
void cleanup_module (void);

/* WAN link driver entry points */
static int setup (wan_device_t *wandev, wandev_conf_t *conf);
static int shutdown (wan_device_t *wandev);
static int ioctl (wan_device_t *wandev, unsigned cmd, unsigned long arg);

/* Miscellaneous functions */
static void cycx_isr (int irq, void *dev_id, struct pt_regs *regs);

/* Global Data
 * Note: All data must be explicitly initialized!!!
 */

/* private data */
static char drvname[]	= "cyclomx";
static char fullname[]	= "CYCLOM 2X(tm) Sync Card Driver";
static char copyright[]	= "(c) 1998, 1999 Arnaldo Carvalho de Melo";
static int ncards = CONFIG_CYCLOMX_CARDS;
static cycx_t *card_array = NULL;	/* adapter data space */

/* Kernel Loadable Module Entry Points */

/*
 * Module 'insert' entry point.
 * o print announcement
 * o allocate adapter data space
 * o initialize static data
 * o register all cards with WAN router
 * o calibrate CYCX shared memory access delay.
 *
 * Return:	0	Ok
 *		< 0	error.
 * Context:	process
 */
#ifdef MODULE
int init_module (void)
#else
int __init cyclomx_init (void)
#endif
{
	int cnt, err = 0;

	printk(KERN_INFO "%s v%u.%u %s\n",
		fullname, DRV_VERSION, DRV_RELEASE, copyright);

	/* Verify number of cards and allocate adapter data space */
	ncards = min(ncards, MAX_CARDS);
	ncards = max(ncards, 1);
	card_array = kmalloc(sizeof(cycx_t) * ncards, GFP_KERNEL);

	if (card_array == NULL) return -ENOMEM;

	memset(card_array, 0, sizeof(cycx_t) * ncards);

	/* Register adapters with WAN router */
	for (cnt = 0; cnt < ncards; ++cnt) {
		cycx_t *card = &card_array[cnt];
		wan_device_t *wandev = &card->wandev;

		sprintf(card->devname, "%s%d", drvname, cnt + 1);
		wandev->magic    = ROUTER_MAGIC;
		wandev->name     = card->devname;
		wandev->private  = card;
		wandev->setup    = &setup;
		wandev->shutdown = &shutdown;
		wandev->ioctl    = &ioctl;
		err = register_wan_device(wandev);

		if (err) {
			printk(KERN_ERR
				"%s: %s registration failed with error %d!\n",
				drvname, card->devname, err);
			break;
		}
	}

	if (cnt) ncards = cnt;	/* adjust actual number of cards */
	else {
		kfree(card_array);
		err = -ENODEV;
	}

	return err;
}

/*
 * Module 'remove' entry point.
 * o unregister all adapters from the WAN router
 * o release all remaining system resources
 */
#ifdef MODULE
void cleanup_module (void)
{
	int i = 0;

	for (; i < ncards; ++i) {
		cycx_t *card = &card_array[i];
		unregister_wan_device(card->devname);
	}

	kfree(card_array);
}
#endif
/* WAN Device Driver Entry Points */
/*
 * Setup/confugure WAN link driver.
 * o check adapter state
 * o make sure firmware is present in configuration
 * o allocate interrupt vector
 * o setup CYCLOM X hardware
 * o call appropriate routine to perform protocol-specific initialization
 * o mark I/O region as used
 *
 * This function is called when router handles ROUTER_SETUP IOCTL. The
 * configuration structure is in kernel memory (including extended data, if
 * any).
 */
static int setup (wan_device_t *wandev, wandev_conf_t *conf)
{
	cycx_t *card;
	int err = 0;
	int irq;

	/* Sanity checks */
	if (!wandev || !wandev->private || !conf) return -EFAULT;

	card = wandev->private;

	if (wandev->state != WAN_UNCONFIGURED) return -EBUSY;

	if (!conf->data_size || (conf->data == NULL)) {
		printk(KERN_ERR "%s: firmware not found in configuration "
				"data!\n", wandev->name);
		return -EINVAL;
	}

	if (conf->irq <= 0) {
		printk(KERN_ERR "%s: can't configure without IRQ!\n",
				wandev->name);
		return -EINVAL;
	}

	/* Allocate IRQ */
	irq = conf->irq == 2 ? 9 : conf->irq;	/* IRQ2 -> IRQ9 */

	if (request_irq(irq, cycx_isr, 0, wandev->name, card)) {
		printk(KERN_ERR "%s: can't reserve IRQ %d!\n",
				wandev->name, irq);
		return -EINVAL;
	}

	/* Configure hardware, load firmware, etc. */
	memset(&card->hw, 0, sizeof(cycxhw_t));
	card->hw.irq = (conf->irq == 9) ? 2 : conf->irq;
	card->hw.dpmbase = conf->maddr;
	card->hw.dpmsize = CYCX_WINDOWSIZE;
	card->hw.fwid = CFID_X25_2X;
	card->lock = SPIN_LOCK_UNLOCKED;
#if LINUX_VERSION_CODE >= 0x020300
	init_waitqueue_head(&card->wait_stats);
#else
	card->wait_stats = NULL;
#endif
	err = cycx_setup(&card->hw, conf->data, conf->data_size);

	if (err) {
		free_irq(irq, card);
		return err;
	}

	/* Intialize WAN device data space */
	wandev->irq       = irq;
	wandev->dma       = wandev->ioport = 0;
	wandev->maddr     = (unsigned long*)card->hw.dpmbase;
	wandev->msize     = card->hw.dpmsize;
	wandev->hw_opt[2] = 0;
	wandev->hw_opt[3] = card->hw.fwid;

	/* Protocol-specific initialization */
	switch (card->hw.fwid) {
#ifdef CONFIG_CYCLOMX_X25
		case CFID_X25_2X: err = cyx_init(card, conf); break;
#endif
		default:
			printk(KERN_ERR "%s: this firmware is not supported!\n",
					wandev->name);
			err = -EINVAL;
	}

	if (err) {
		cycx_down(&card->hw);
		free_irq(irq, card);
		return err;
	}

	return 0;
}

/*
 * Shut down WAN link driver. 
 * o shut down adapter hardware
 * o release system resources.
 *
 * This function is called by the router when device is being unregistered or
 * when it handles ROUTER_DOWN IOCTL.
 */
static int shutdown (wan_device_t *wandev)
{
	cycx_t *card;

	/* sanity checks */
	if (!wandev || !wandev->private) return -EFAULT;

	if (wandev->state == WAN_UNCONFIGURED) return 0;

	card = wandev->private;
	wandev->state = WAN_UNCONFIGURED;
	cycx_down(&card->hw);
	printk(KERN_INFO "%s: irq %d being freed!\n", wandev->name, wandev->irq);
	free_irq(wandev->irq, card);
	return 0;
}

/*
 * Driver I/O control. 
 * o verify arguments
 * o perform requested action
 *
 * This function is called when router handles one of the reserved user
 * IOCTLs.  Note that 'arg' stil points to user address space.
 */
static int ioctl (wan_device_t *wandev, unsigned cmd, unsigned long arg)
{
	return -EINVAL;
}

/* Miscellaneous */
/*
 * CYCX Interrupt Service Routine.
 * o acknowledge CYCX hardware interrupt.
 * o call protocol-specific interrupt service routine, if any.
 */
static void cycx_isr (int irq, void *dev_id, struct pt_regs *regs)
{
#define	card	((cycx_t*)dev_id)
	if (!card || card->wandev.state == WAN_UNCONFIGURED)
		return;

	if (card->in_isr) {
		printk(KERN_WARNING "%s: interrupt re-entrancy on IRQ %d!\n",
				    card->devname, card->wandev.irq);
		return;
	}

	if (card->isr)
		card->isr(card);
#undef	card
}

/*
 * This routine is called by the protocol-specific modules when network
 * interface is being open.  The only reason we need this, is because we
 * have to call MOD_INC_USE_COUNT, but cannot include 'module.h' where it's
 * defined more than once into the same kernel module.
 */
void cyclomx_open (cycx_t *card)
{
	++card->open_cnt;
	MOD_INC_USE_COUNT;
}

/*
 * This routine is called by the protocol-specific modules when network
 * interface is being closed.  The only reason we need this, is because we
 * have to call MOD_DEC_USE_COUNT, but cannot include 'module.h' where it's
 * defined more than once into the same kernel module.
 */
void cyclomx_close (cycx_t *card)
{
	--card->open_cnt;
	MOD_DEC_USE_COUNT;
}

/* Set WAN device state.  */
void cyclomx_set_state (cycx_t *card, int state)
{
	unsigned long host_cpu_flags;

	spin_lock_irqsave(&card->lock, host_cpu_flags);

	if (card->wandev.state != state) {
		switch (state) {
			case WAN_CONNECTED:
				printk (KERN_INFO "%s: link connected!\n",
						  card->devname);
				break;

			case WAN_CONNECTING:
				printk (KERN_INFO "%s: link connecting...\n",
						  card->devname);
				break;

			case WAN_DISCONNECTED:
				printk (KERN_INFO "%s: link disconnected!\n",
						  card->devname);
				break;
		}

		card->wandev.state = state;
	}

	card->state_tick = jiffies;
	spin_unlock_irqrestore(&card->lock, host_cpu_flags);
}

/* End */
