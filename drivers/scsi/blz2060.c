/* blz2060.c: Driver for Blizzard 2060 SCSI Controller.
 *
 * Copyright (C) 1996 Jesper Skov (jskov@cygnus.co.uk)
 *
 * This driver is based on the CyberStorm driver, hence the occasional
 * reference to CyberStorm.
 */

/* TODO:
 *
 * 1) Figure out how to make a cleaner merge with the sparc driver with regard
 *    to the caches and the Sparc MMU mapping.
 * 2) Make as few routines required outside the generic driver. A lot of the
 *    routines in this file used to be inline!
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/blk.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>

#include "scsi.h"
#include "hosts.h"
#include "NCR53C9x.h"
#include "blz2060.h"

#include <linux/zorro.h>
#include <asm/irq.h>
#include <asm/amigaints.h>
#include <asm/amigahw.h>

#include <asm/pgtable.h>

static int  dma_bytes_sent(struct NCR_ESP *esp, int fifo_count);
static int  dma_can_transfer(struct NCR_ESP *esp, Scsi_Cmnd *sp);
static void dma_dump_state(struct NCR_ESP *esp);
static void dma_init_read(struct NCR_ESP *esp, __u32 addr, int length);
static void dma_init_write(struct NCR_ESP *esp, __u32 addr, int length);
static void dma_ints_off(struct NCR_ESP *esp);
static void dma_ints_on(struct NCR_ESP *esp);
static int  dma_irq_p(struct NCR_ESP *esp);
static void dma_led_off(struct NCR_ESP *esp);
static void dma_led_on(struct NCR_ESP *esp);
static int  dma_ports_p(struct NCR_ESP *esp);
static void dma_setup(struct NCR_ESP *esp, __u32 addr, int count, int write);

volatile unsigned char cmd_buffer[16];
				/* This is where all commands are put
				 * before they are transfered to the ESP chip
				 * via PIO.
				 */

/***************************************************************** Detection */
int __init blz2060_esp_detect(Scsi_Host_Template *tpnt)
{
	struct NCR_ESP *esp;
	const struct ConfigDev *esp_dev;
	unsigned int key;
	unsigned long address;

	if ((key = zorro_find(ZORRO_PROD_PHASE5_BLIZZARD_2060, 0, 0))){
		esp_dev = zorro_get_board(key);
		esp = esp_allocate(tpnt, (void *) esp_dev);

		/* Do command transfer with programmed I/O */
		esp->do_pio_cmds = 1;

		/* Required functions */
		esp->dma_bytes_sent = &dma_bytes_sent;
		esp->dma_can_transfer = &dma_can_transfer;
		esp->dma_dump_state = &dma_dump_state;
		esp->dma_init_read = &dma_init_read;
		esp->dma_init_write = &dma_init_write;
		esp->dma_ints_off = &dma_ints_off;
		esp->dma_ints_on = &dma_ints_on;
		esp->dma_irq_p = &dma_irq_p;
		esp->dma_ports_p = &dma_ports_p;
		esp->dma_setup = &dma_setup;

		/* Optional functions */
		esp->dma_barrier = 0;
		esp->dma_drain = 0;
		esp->dma_invalidate = 0;
		esp->dma_irq_entry = 0;
		esp->dma_irq_exit = 0;
		esp->dma_led_on = &dma_led_on;
		esp->dma_led_off = &dma_led_off;
		esp->dma_poll = 0;
		esp->dma_reset = 0;

		/* SCSI chip speed */
		esp->cfreq = 40000000;

		/* The DMA registers on the Blizzard are mapped
		 * relative to the device (i.e. in the same Zorro
		 * I/O block).
		 */
		address = (unsigned long)ZTWO_VADDR(esp_dev->cd_BoardAddr);
		esp->dregs = (void *)(address + BLZ2060_DMA_ADDR);

		/* ESP register base */
		esp->eregs = (struct ESP_regs *)(address + BLZ2060_ESP_ADDR);
		
		/* Set the command buffer */
		esp->esp_command = (volatile unsigned char*) cmd_buffer;
		esp->esp_command_dvma = virt_to_bus(cmd_buffer);

		esp->irq = IRQ_AMIGA_PORTS;
		esp->slot = key;
		request_irq(IRQ_AMIGA_PORTS, esp_intr, SA_SHIRQ,
			    "Blizzard 2060 SCSI", esp_intr);

		/* Figure out our scsi ID on the bus */
		esp->scsi_id = 7;
		
		/* We don't have a differential SCSI-bus. */
		esp->diff = 0;

		esp_initialize(esp);

		zorro_config_board(key, 0);

		printk("ESP: Total of %d ESP hosts found, %d actually in use.\n", nesps, esps_in_use);
		esps_running = esps_in_use;
		return esps_in_use;
	}
	return 0;
}

/************************************************************* DMA Functions */
static int dma_bytes_sent(struct NCR_ESP *esp, int fifo_count)
{
	/* Since the Blizzard DMA is fully dedicated to the ESP chip,
	 * the number of bytes sent (to the ESP chip) equals the number
	 * of bytes in the FIFO - there is no buffering in the DMA controller.
	 * XXXX Do I read this right? It is from host to ESP, right?
	 */
	return fifo_count;
}

static int dma_can_transfer(struct NCR_ESP *esp, Scsi_Cmnd *sp)
{
	/* I don't think there's any limit on the Blizzard DMA. So we use what
	 * the ESP chip can handle (24 bit).
	 */
	unsigned long sz = sp->SCp.this_residual;
	if(sz > 0x1000000)
		sz = 0x1000000;
	return sz;
}

static void dma_dump_state(struct NCR_ESP *esp)
{
	ESPLOG(("intreq:<%04x>, intena:<%04x>\n",
		custom.intreqr, custom.intenar));
}

static void dma_init_read(struct NCR_ESP *esp, __u32 addr, int length)
{
	struct blz2060_dma_registers *dregs = 
		(struct blz2060_dma_registers *) (esp->dregs);

	cache_clear(addr, length);

	addr >>= 1;
	addr &= ~(BLZ2060_DMA_WRITE);
	dregs->dma_addr3 = (addr      ) & 0xff;
	dregs->dma_addr2 = (addr >>  8) & 0xff;
	dregs->dma_addr1 = (addr >> 16) & 0xff;
	dregs->dma_addr0 = (addr >> 24) & 0xff;
}

static void dma_init_write(struct NCR_ESP *esp, __u32 addr, int length)
{
	struct blz2060_dma_registers *dregs = 
		(struct blz2060_dma_registers *) (esp->dregs);

	cache_push(addr, length);

	addr >>= 1;
	addr |= BLZ2060_DMA_WRITE;
	dregs->dma_addr3 = (addr      ) & 0xff;
	dregs->dma_addr2 = (addr >>  8) & 0xff;
	dregs->dma_addr1 = (addr >> 16) & 0xff;
	dregs->dma_addr0 = (addr >> 24) & 0xff;
}

static void dma_ints_off(struct NCR_ESP *esp)
{
	disable_irq(esp->irq);
}

static void dma_ints_on(struct NCR_ESP *esp)
{
	enable_irq(esp->irq);
}

static int dma_irq_p(struct NCR_ESP *esp)
{
	return (esp_read(esp->eregs->esp_status) & ESP_STAT_INTR);
}

static void dma_led_off(struct NCR_ESP *esp)
{
	((struct blz2060_dma_registers *) (esp->dregs))->dma_led_ctrl =
		BLZ2060_DMA_LED;
}

static void dma_led_on(struct NCR_ESP *esp)
{
	((struct blz2060_dma_registers *) (esp->dregs))->dma_led_ctrl = 0;
}

static int dma_ports_p(struct NCR_ESP *esp)
{
	return ((custom.intenar) & IF_PORTS);
}

static void dma_setup(struct NCR_ESP *esp, __u32 addr, int count, int write)
{
	/* On the Sparc, DMA_ST_WRITE means "move data from device to memory"
	 * so when (write) is true, it actually means READ!
	 */
	if(write){
		dma_init_read(esp, addr, count);
	} else {
		dma_init_write(esp, addr, count);
	}
}

#ifdef MODULE

#define HOSTS_C

#include "blz2060.h"

Scsi_Host_Template driver_template = SCSI_BLZ2060;

#include "scsi_module.c"

#endif

int blz2060_esp_release(struct Scsi_Host *instance)
{
#ifdef MODULE
	unsigned int key;

	key = ((struct NCR_ESP *)instance->hostdata)->slot;
	esp_deallocate((struct NCR_ESP *)instance->hostdata);
	esp_release();
	zorro_unconfig_board(key, 0);
	free_irq(IRQ_AMIGA_PORTS, esp_intr);
#endif
	return 1;
}
