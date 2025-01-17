/*
 * sound/sscape.c
 *
 * Low level driver for Ensoniq SoundScape
 */

/*
 * Copyright (C) by Hannu Savolainen 1993-1997
 *
 * OSS/Free for Linux is distributed under the GNU GENERAL PUBLIC LICENSE (GPL)
 * Version 2 (June 1991). See the "COPYING" file distributed with this software
 * for more info.
 */

/*
 * Thomas Sailer   : ioctl code reworked (vmalloc/vfree removed)
 */

#include <linux/config.h>
#include <linux/module.h>

#include "sound_config.h"
#include "soundmodule.h"

#ifdef CONFIG_SSCAPE

#include "coproc.h"

/*
 *    I/O ports
 */
#define MIDI_DATA       0
#define MIDI_CTRL       1
#define HOST_CTRL       2
#define TX_READY	0x02
#define RX_READY	0x01
#define HOST_DATA       3
#define ODIE_ADDR       4
#define ODIE_DATA       5

/*
 *    Indirect registers
 */

#define GA_INTSTAT_REG	0
#define GA_INTENA_REG	1
#define GA_DMAA_REG	2
#define GA_DMAB_REG	3
#define GA_INTCFG_REG	4
#define GA_DMACFG_REG	5
#define GA_CDCFG_REG	6
#define GA_SMCFGA_REG	7
#define GA_SMCFGB_REG	8
#define GA_HMCTL_REG	9

/*
 * DMA channel identifiers (A and B)
 */

#define SSCAPE_DMA_A	0
#define SSCAPE_DMA_B	1

#define PORT(name)	(devc->base+name)

/*
 * Host commands recognized by the OBP microcode
 */
 
#define CMD_GEN_HOST_ACK	0x80
#define CMD_GEN_MPU_ACK		0x81
#define CMD_GET_BOARD_TYPE	0x82
#define CMD_SET_CONTROL		0x88	/* Old firmware only */
#define CMD_GET_CONTROL		0x89	/* Old firmware only */
#define 	CTL_MASTER_VOL		0
#define 	CTL_MIC_MODE		2
#define 	CTL_SYNTH_VOL		4
#define 	CTL_WAVE_VOL		7
#define CMD_SET_EXTMIDI		0x8a
#define CMD_GET_EXTMIDI		0x8b
#define CMD_SET_MT32		0x8c
#define CMD_GET_MT32		0x8d

#define CMD_ACK			0x80

typedef struct sscape_info
{
	int	base, irq, dma;
	int	ok;	/* Properly detected */
	int	failed;
	int	dma_allocated;
	int	codec_audiodev;
	int	opened;
	int	*osp;
	int	my_audiodev;
} sscape_info;

static struct sscape_info adev_info = {
	0
};

static struct sscape_info *devc = &adev_info;
static int sscape_mididev = -1;

/* Some older cards have assigned interrupt bits differently than new ones */
static char valid_interrupts_old[] = {
	9, 7, 5, 15
};

static char valid_interrupts_new[] = {
	9, 5, 7, 10
};

static char *valid_interrupts = valid_interrupts_new;

/*
 *	See the bottom of the driver. This can be set by spea =0/1.
 */
 
#ifdef REVEAL_SPEA
static char old_hardware = 1;
#else
static char old_hardware = 0;
#endif

static void sleep(unsigned howlong)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(howlong);
}

static unsigned char sscape_read(struct sscape_info *devc, int reg)
{
	unsigned long flags;
	unsigned char val;

	save_flags(flags);
	cli();
	outb(reg, PORT(ODIE_ADDR));
	val = inb(PORT(ODIE_DATA));
	restore_flags(flags);
	return val;
}

static void sscape_write(struct sscape_info *devc, int reg, int data)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	outb(reg, PORT(ODIE_ADDR));
	outb(data, PORT(ODIE_DATA));
	restore_flags(flags);
}

static void host_open(struct sscape_info *devc)
{
	outb((0x00), PORT(HOST_CTRL));	/* Put the board to the host mode */
}

static void host_close(struct sscape_info *devc)
{
	outb((0x03), PORT(HOST_CTRL));	/* Put the board to the MIDI mode */
}

static int host_write(struct sscape_info *devc, unsigned char *data, int count)
{
	unsigned long flags;
	int i, timeout_val;

	save_flags(flags);
	cli();

	/*
	 * Send the command and data bytes
	 */

	for (i = 0; i < count; i++)
	{
		for (timeout_val = 10000; timeout_val > 0; timeout_val--)
			if (inb(PORT(HOST_CTRL)) & TX_READY)
				break;

		if (timeout_val <= 0)
		{
			    restore_flags(flags);
			    return 0;
		}
		outb(data[i], PORT(HOST_DATA));
	}
	restore_flags(flags);
	return 1;
}

static int host_read(struct sscape_info *devc)
{
	unsigned long flags;
	int timeout_val;
	unsigned char data;

	save_flags(flags);
	cli();

	/*
	 * Read a byte
	 */

	for (timeout_val = 10000; timeout_val > 0; timeout_val--)
		if (inb(PORT(HOST_CTRL)) & RX_READY)
			break;

	if (timeout_val <= 0)
	{
		restore_flags(flags);
		return -1;
	}
	data = inb(PORT(HOST_DATA));
	restore_flags(flags);
	return data;
}


static int host_command2(struct sscape_info *devc, int cmd, int parm1)
{
	unsigned char buf[10];

	buf[0] = (unsigned char) (cmd & 0xff);
	buf[1] = (unsigned char) (parm1 & 0xff);

	return host_write(devc, buf, 2);
}

static int host_command3(struct sscape_info *devc, int cmd, int parm1, int parm2)
{
	unsigned char buf[10];

	buf[0] = (unsigned char) (cmd & 0xff);
	buf[1] = (unsigned char) (parm1 & 0xff);
	buf[2] = (unsigned char) (parm2 & 0xff);
	return host_write(devc, buf, 3);
}

static void set_mt32(struct sscape_info *devc, int value)
{
	host_open(devc);
	host_command2(devc, CMD_SET_MT32, value ? 1 : 0);
	if (host_read(devc) != CMD_ACK)
	{
		/* printk( "SNDSCAPE: Setting MT32 mode failed\n"); */
	}
	host_close(devc);
}

static void set_control(struct sscape_info *devc, int ctrl, int value)
{
	host_open(devc);
	host_command3(devc, CMD_SET_CONTROL, ctrl, value);
	if (host_read(devc) != CMD_ACK)
	{
		/* printk( "SNDSCAPE: Setting control (%d) failed\n",  ctrl); */
	}
	host_close(devc);
}

static void do_dma(struct sscape_info *devc, int dma_chan, unsigned long buf, int blk_size, int mode)
{
	unsigned char temp;

	if (dma_chan != SSCAPE_DMA_A)
	{
		printk(KERN_WARNING "soundscape: Tried to use DMA channel  != A. Why?\n");
		return;
	}
	audio_devs[devc->codec_audiodev]->flags &= ~DMA_AUTOMODE;
	DMAbuf_start_dma(devc->codec_audiodev, buf, blk_size, mode);
	audio_devs[devc->codec_audiodev]->flags |= DMA_AUTOMODE;

	temp = devc->dma << 4;	/* Setup DMA channel select bits */
	if (devc->dma <= 3)
		temp |= 0x80;	/* 8 bit DMA channel */

	temp |= 1;		/* Trigger DMA */
	sscape_write(devc, GA_DMAA_REG, temp);
	temp &= 0xfe;		/* Clear DMA trigger */
	sscape_write(devc, GA_DMAA_REG, temp);
}

static int verify_mpu(struct sscape_info *devc)
{
	/*
	 * The SoundScape board could be in three modes (MPU, 8250 and host).
	 * If the card is not in the MPU mode, enabling the MPU driver will
	 * cause infinite loop (the driver believes that there is always some
	 * received data in the buffer.
	 *
	 * Detect this by looking if there are more than 10 received MIDI bytes
	 * (0x00) in the buffer.
	 */

	int i;

	for (i = 0; i < 10; i++)
	{
		if (inb(devc->base + HOST_CTRL) & 0x80)
			return 1;

		if (inb(devc->base) != 0x00)
			return 1;
	}
	printk(KERN_WARNING "SoundScape: The device is not in the MPU-401 mode\n");
	return 0;
}

static int sscape_coproc_open(void *dev_info, int sub_device)
{
	if (sub_device == COPR_MIDI)
	{
		set_mt32(devc, 0);
		if (!verify_mpu(devc))
			return -EIO;
	}
	return 0;
}

static void sscape_coproc_close(void *dev_info, int sub_device)
{
	struct sscape_info *devc = dev_info;
	unsigned long   flags;

	save_flags(flags);
	cli();
	if (devc->dma_allocated)
	{
		sscape_write(devc, GA_DMAA_REG, 0x20);	/* DMA channel disabled */
		devc->dma_allocated = 0;
	}
	restore_flags(flags);
	return;
}

static void sscape_coproc_reset(void *dev_info)
{
}

static int sscape_download_boot(struct sscape_info *devc, unsigned char *block, int size, int flag)
{
	unsigned long flags;
	unsigned char temp;
	volatile int done, timeout_val;
	static unsigned char codec_dma_bits = 0;

	if (flag & CPF_FIRST)
	{
		/*
		 * First block. Have to allocate DMA and to reset the board
		 * before continuing.
		 */

		save_flags(flags);
		cli();
		codec_dma_bits = sscape_read(devc, GA_CDCFG_REG);

		if (devc->dma_allocated == 0)
			devc->dma_allocated = 1;

		restore_flags(flags);

		sscape_write(devc, GA_HMCTL_REG, 
			(temp = sscape_read(devc, GA_HMCTL_REG)) & 0x3f);	/*Reset */

		for (timeout_val = 10000; timeout_val > 0; timeout_val--)
			sscape_read(devc, GA_HMCTL_REG);	/* Delay */

		/* Take board out of reset */
		sscape_write(devc, GA_HMCTL_REG,
			(temp = sscape_read(devc, GA_HMCTL_REG)) | 0x80);
	}
	/*
	 * Transfer one code block using DMA
	 */
	if (audio_devs[devc->codec_audiodev]->dmap_out->raw_buf == NULL)
	{
		printk(KERN_WARNING "soundscape: DMA buffer not available\n");
		return 0;
	}
	memcpy(audio_devs[devc->codec_audiodev]->dmap_out->raw_buf, block, size);

	save_flags(flags);
	cli();
	
	/******** INTERRUPTS DISABLED NOW ********/
	
	do_dma(devc, SSCAPE_DMA_A,
	       audio_devs[devc->codec_audiodev]->dmap_out->raw_buf_phys,
	       size, DMA_MODE_WRITE);

	/*
	 * Wait until transfer completes.
	 */
	
	done = 0;
	timeout_val = 30;
	while (!done && timeout_val-- > 0)
	{
		int resid;

		if (HZ / 50)
			sleep(HZ / 50);
		clear_dma_ff(devc->dma);
		if ((resid = get_dma_residue(devc->dma)) == 0)
			done = 1;
	}

	restore_flags(flags);
	if (!done)
		return 0;

	if (flag & CPF_LAST)
	{
		/*
		 * Take the board out of reset
		 */
		outb((0x00), PORT(HOST_CTRL));
		outb((0x00), PORT(MIDI_CTRL));

		temp = sscape_read(devc, GA_HMCTL_REG);
		temp |= 0x40;
		sscape_write(devc, GA_HMCTL_REG, temp);	/* Kickstart the board */

		/*
		 * Wait until the ODB wakes up
		 */

		save_flags(flags);
		cli();
		done = 0;
		timeout_val = 5 * HZ;
		while (!done && timeout_val-- > 0)
		{
			unsigned char x;
			
			sleep(1);
			x = inb(PORT(HOST_DATA));
			if (x == 0xff || x == 0xfe)		/* OBP startup acknowledge */
			{
				DDB(printk("Soundscape: Acknowledge = %x\n", x));
				done = 1;
			}
		}
		sscape_write(devc, GA_CDCFG_REG, codec_dma_bits);

		restore_flags(flags);
		if (!done)
		{
			printk(KERN_ERR "soundscape: The OBP didn't respond after code download\n");
			return 0;
		}
		save_flags(flags);
		cli();
		done = 0;
		timeout_val = 5 * HZ;
		while (!done && timeout_val-- > 0)
		{
			sleep(1);
			if (inb(PORT(HOST_DATA)) == 0xfe)	/* Host startup acknowledge */
				done = 1;
		}
		restore_flags(flags);
		if (!done)
		{
			printk(KERN_ERR "soundscape: OBP Initialization failed.\n");
			return 0;
		}
		printk(KERN_INFO "SoundScape board initialized OK\n");
		set_control(devc, CTL_MASTER_VOL, 100);
		set_control(devc, CTL_SYNTH_VOL, 100);

#ifdef SSCAPE_DEBUG3
		/*
		 * Temporary debugging aid. Print contents of the registers after
		 * downloading the code.
		 */
		{
			int i;

			for (i = 0; i < 13; i++)
				printk("I%d = %02x (new value)\n", i, sscape_read(devc, i));
		}
#endif

	}
	return 1;
}

static int download_boot_block(void *dev_info, copr_buffer * buf)
{
	if (buf->len <= 0 || buf->len > sizeof(buf->data))
		return -EINVAL;

	if (!sscape_download_boot(devc, buf->data, buf->len, buf->flags))
	{
		printk(KERN_ERR "soundscape: Unable to load microcode block to the OBP.\n");
		return -EIO;
	}
	return 0;
}

static int sscape_coproc_ioctl(void *dev_info, unsigned int cmd, caddr_t arg, int local)
{
	copr_buffer *buf;
	int err;

	switch (cmd) 
	{
		case SNDCTL_COPR_RESET:
			sscape_coproc_reset(dev_info);
			return 0;

		case SNDCTL_COPR_LOAD:
			buf = (copr_buffer *) vmalloc(sizeof(copr_buffer));
			if (buf == NULL)
				return -ENOSPC;
			if (copy_from_user(buf, arg, sizeof(copr_buffer))) 
			{
				vfree(buf);
				return -EFAULT;
			}
			err = download_boot_block(dev_info, buf);
			vfree(buf);
			return err;
		
		default:
			return -EINVAL;
	}
}

static coproc_operations sscape_coproc_operations =
{
	"SoundScape M68K",
	sscape_coproc_open,
	sscape_coproc_close,
	sscape_coproc_ioctl,
	sscape_coproc_reset,
	&adev_info
};

static int sscape_detected = 0;

void attach_sscape(struct address_info *hw_config)
{
#ifndef SSCAPE_REGS
	/*
	 * Config register values for Spea/V7 Media FX and Ensoniq S-2000.
	 * These values are card
	 * dependent. If you have another SoundScape based card, you have to
	 * find the correct values. Do the following:
	 *  - Compile this driver with SSCAPE_DEBUG1 defined.
	 *  - Shut down and power off your machine.
	 *  - Boot with DOS so that the SSINIT.EXE program is run.
	 *  - Warm boot to {Linux|SYSV|BSD} and write down the lines displayed
	 *    when detecting the SoundScape.
	 *  - Modify the following list to use the values printed during boot.
	 *    Undefine the SSCAPE_DEBUG1
	 */
#define SSCAPE_REGS { \
/* I0 */	0x00, \
		0xf0, /* Note! Ignored. Set always to 0xf0 */ \
		0x20, /* Note! Ignored. Set always to 0x20 */ \
		0x20, /* Note! Ignored. Set always to 0x20 */ \
		0xf5, /* Ignored */ \
		0x10, \
		0x00, \
		0x2e, /* I7 MEM config A. Likely to vary between models */ \
		0x00, /* I8 MEM config B. Likely to vary between models */ \
/* I9 */	0x40 /* Ignored */ \
	}
#endif

	unsigned long   flags;
	static unsigned char regs[10] = SSCAPE_REGS;

	int i, irq_bits = 0xff;

	if (sscape_detected != hw_config->io_base)
		return;

	request_region(devc->base + 2, 6, "SoundScape");
	if (old_hardware)
	{
		valid_interrupts = valid_interrupts_old;
		conf_printf("Ensoniq SoundScape (old)", hw_config);
	}
	else
		conf_printf("Ensoniq SoundScape", hw_config);

	for (i = 0; i < sizeof(valid_interrupts); i++)
	{
		if (hw_config->irq == valid_interrupts[i])
		{
			irq_bits = i;
			break;
		}
	}
	if (hw_config->irq > 15 || (regs[4] = irq_bits == 0xff))
	{
		printk(KERN_ERR "Invalid IRQ%d\n", hw_config->irq);
		return;
	}
	save_flags(flags);
	cli();

	for (i = 1; i < 10; i++)
	{
		switch (i)
		{
			case 1:	/* Host interrupt enable */
				sscape_write(devc, i, 0xf0);	/* All interrupts enabled */
				break;

			case 2:	/* DMA A status/trigger register */
			case 3:	/* DMA B status/trigger register */
				sscape_write(devc, i, 0x20);	/* DMA channel disabled */
				break;

			case 4:	/* Host interrupt config reg */
				sscape_write(devc, i, 0xf0 | (irq_bits << 2) | irq_bits);
				break;

			case 5:	/* Don't destroy CD-ROM DMA config bits (0xc0) */
				sscape_write(devc, i, (regs[i] & 0x3f) | (sscape_read(devc, i) & 0xc0));
				break;

			case 6:	/* CD-ROM config (WSS codec actually) */
				sscape_write(devc, i, regs[i]);
				break;

			case 9:	/* Master control reg. Don't modify CR-ROM bits. Disable SB emul */
				sscape_write(devc, i, (sscape_read(devc, i) & 0xf0) | 0x08);
				break;

			default:
				sscape_write(devc, i, regs[i]);
		}
	}
	restore_flags(flags);

#ifdef SSCAPE_DEBUG2
	/*
	 * Temporary debugging aid. Print contents of the registers after
	 * changing them.
	 */
	{
		int i;

		for (i = 0; i < 13; i++)
			printk("I%d = %02x (new value)\n", i, sscape_read(devc, i));
	}
#endif

#if defined(CONFIG_MIDI) && defined(CONFIG_MPU_EMU)
	if (probe_mpu401(hw_config))
		hw_config->always_detect = 1;
	hw_config->name = "SoundScape";

	hw_config->irq *= -1;	/* Negative value signals IRQ sharing */
	attach_mpu401(hw_config);
	hw_config->irq *= -1;	/* Restore it */

	if (hw_config->slots[1] != -1)	/* The MPU driver installed itself */
	{
		sscape_mididev = hw_config->slots[1];
		midi_devs[hw_config->slots[1]]->coproc = &sscape_coproc_operations;
	}
#endif
	sscape_write(devc, GA_INTENA_REG, 0x80);	/* Master IRQ enable */
	devc->ok = 1;
	devc->failed = 0;
}

static int detect_ga(sscape_info * devc)
{
	unsigned char save;

	DDB(printk("Entered Soundscape detect_ga(%x)\n", devc->base));

	if (check_region(devc->base, 8))
		return 0;

	/*
	 * First check that the address register of "ODIE" is
	 * there and that it has exactly 4 writable bits.
	 * First 4 bits
	 */
	
	if ((save = inb(PORT(ODIE_ADDR))) & 0xf0)
	{
		DDB(printk("soundscape: Detect error A\n"));
		return 0;
	}
	outb((0x00), PORT(ODIE_ADDR));
	if (inb(PORT(ODIE_ADDR)) != 0x00)
	{
		DDB(printk("soundscape: Detect error B\n"));
		return 0;
	}
	outb((0xff), PORT(ODIE_ADDR));
	if (inb(PORT(ODIE_ADDR)) != 0x0f)
	{
		DDB(printk("soundscape: Detect error C\n"));
		return 0;
	}
	outb((save), PORT(ODIE_ADDR));

	/*
	 * Now verify that some indirect registers return zero on some bits.
	 * This may break the driver with some future revisions of "ODIE" but...
	 */

	if (sscape_read(devc, 0) & 0x0c)
	{
		DDB(printk("soundscape: Detect error D (%x)\n", sscape_read(devc, 0)));
		return 0;
	}
	if (sscape_read(devc, 1) & 0x0f)
	{
		DDB(printk("soundscape: Detect error E\n"));
		return 0;
	}
	if (sscape_read(devc, 5) & 0x0f)
	{
		DDB(printk("soundscape: Detect error F\n"));
		return 0;
	}
	return 1;
}

int probe_sscape(struct address_info *hw_config)
{

	if (sscape_detected != 0 && sscape_detected != hw_config->io_base)
		return 0;

	devc->base = hw_config->io_base;
	devc->irq = hw_config->irq;
	devc->dma = hw_config->dma;
	devc->osp = hw_config->osp;

#ifdef SSCAPE_DEBUG1
	/*
	 * Temporary debugging aid. Print contents of the registers before
	 * changing them.
	 */
	{
		int i;

		for (i = 0; i < 13; i++)
			printk("I%d = %02x (old value)\n", i, sscape_read(devc, i));
	}
#endif
	devc->failed = 1;

	if (!detect_ga(devc))
		return 0;

	if (old_hardware)	/* Check that it's really an old Spea/Reveal card. */
	{
		unsigned char   tmp;
		int             cc;

		if (!((tmp = sscape_read(devc, GA_HMCTL_REG)) & 0xc0))
		{
			sscape_write(devc, GA_HMCTL_REG, tmp | 0x80);
			for (cc = 0; cc < 200000; ++cc)
				inb(devc->base + ODIE_ADDR);
		}
	}
	sscape_detected = hw_config->io_base;
	return 1;
}

int probe_ss_ms_sound(struct address_info *hw_config)
{
	int i, irq_bits = 0xff;
	int ad_flags = 0;

	if (devc->failed)
	{
		  printk(KERN_ERR "soundscape: Card not detected\n");
		  return 0;
	}
	if (devc->ok == 0)
	{
		printk(KERN_ERR "soundscape: Invalid initialization order.\n");
		return 0;
	}
	for (i = 0; i < sizeof(valid_interrupts); i++)
	{
		if (hw_config->irq == valid_interrupts[i])
		{
			irq_bits = i;
			break;
		}
	}
	if (hw_config->irq > 15 || irq_bits == 0xff)
	{
		printk(KERN_ERR "soundscape: Invalid MSS IRQ%d\n", hw_config->irq);
		return 0;
	}
	if (old_hardware)
		ad_flags = 0x12345677;	/* Tell that we may have a CS4248 chip (Spea-V7 Media FX) */
	return ad1848_detect(hw_config->io_base, &ad_flags, hw_config->osp);
}

void attach_ss_ms_sound(struct address_info *hw_config)
{
	/*
	 * This routine configures the SoundScape card for use with the
	 * Win Sound System driver. The AD1848 codec interface uses the CD-ROM
	 * config registers of the "ODIE".
	 */

	int i, irq_bits = 0xff;

	hw_config->dma = devc->dma;	/* Share the DMA with the ODIE/OPUS chip */

	/*
	 * Setup the DMA polarity.
	 */

	sscape_write(devc, GA_DMACFG_REG, 0x50);

	/*
	 * Take the gate-array off of the DMA channel.
	 */
	
	sscape_write(devc, GA_DMAB_REG, 0x20);

	/*
	 * Init the AD1848 (CD-ROM) config reg.
	 */

	for (i = 0; i < sizeof(valid_interrupts); i++)
	{
		if (hw_config->irq == valid_interrupts[i])
		{
			irq_bits = i;
			break;
		}
	}
	sscape_write(devc, GA_CDCFG_REG, 0x89 | (hw_config->dma << 4) | (irq_bits << 1));

	if (hw_config->irq == devc->irq)
		printk(KERN_WARNING "soundscape: Warning! The WSS mode can't share IRQ with MIDI\n");

	hw_config->slots[0] = ad1848_init("SoundScape", hw_config->io_base,
					  hw_config->irq,
					  hw_config->dma,
					  hw_config->dma,
					  0,
					  devc->osp);

	if (hw_config->slots[0] != -1)	/* The AD1848 driver installed itself */
	{
		audio_devs[hw_config->slots[0]]->coproc = &sscape_coproc_operations;
		devc->codec_audiodev = hw_config->slots[0];
		devc->my_audiodev = hw_config->slots[0];

		/* Set proper routings here (what are they) */
		AD1848_REROUTE(SOUND_MIXER_LINE1, SOUND_MIXER_LINE);
	}
#ifdef SSCAPE_DEBUG5
	/*
	 * Temporary debugging aid. Print contents of the registers
	 * after the AD1848 device has been initialized.
	 */
	{
		int i;

		for (i = 0; i < 13; i++)
			printk("I%d = %02x\n", i, sscape_read(devc, i));
	}
#endif

}

void unload_sscape(struct address_info *hw_config)
{
	release_region(devc->base + 2, 6);
#if defined(CONFIG_MPU_EMU) && defined(CONFIG_MIDI)
	unload_mpu401(hw_config);
#endif
}

void unload_ss_ms_sound(struct address_info *hw_config)
{
	ad1848_unload(hw_config->io_base,
		      hw_config->irq,
		      devc->dma,
		      devc->dma,
		      0);
	sound_unload_audiodev(hw_config->slots[0]);
}

#ifdef MODULE

int             dma = -1;
int             irq = -1;
int             io = -1;

int             mpu_irq = -1;
int             mpu_io = -1;

int		spea = -1;

static int      mss = 0;

MODULE_PARM(dma, "i");
MODULE_PARM(irq, "i");
MODULE_PARM(io, "i");
MODULE_PARM(spea, "i");		/* spea=0/1 set the old_hardware */
MODULE_PARM(mpu_irq, "i");
MODULE_PARM(mpu_io, "i");
MODULE_PARM(mss, "i");

struct address_info config;
struct address_info mpu_config;

int init_module(void)
{
	printk(KERN_INFO "Soundscape driver Copyright (C) by Hannu Savolainen 1993-1996\n");
	if (dma == -1 || irq == -1 || io == -1)
	{
		printk(KERN_ERR "DMA, IRQ, and IO port must be specified.\n");
		return -EINVAL;
	}
	if (mpu_irq == -1 && mpu_io != -1)
	{
		  printk(KERN_ERR "CONFIG_MPU_IRQ must be specified if CONFIG_MPU_IO is set.\n");
		  return -EINVAL;
	}
	config.irq = irq;
	config.dma = dma;
	config.io_base = io;

	mpu_config.irq = mpu_irq;
	mpu_config.io_base = mpu_io;
	/* WEH - Try to get right dma channel */
        mpu_config.dma = dma;
      
	if(spea != -1)
	{
		old_hardware = spea;
		printk(KERN_INFO "Forcing %s hardware support.\n",
			spea?"new":"old");
	}	
	if (probe_sscape(&mpu_config) == 0)
		return -ENODEV;

	attach_sscape(&mpu_config);
	
	mss = probe_ss_ms_sound(&config);

	if (mss)
		attach_ss_ms_sound(&config);
	SOUND_LOCK;
	return 0;
}

void cleanup_module(void)
{
	if (mss)
		unload_ss_ms_sound(&config);
	SOUND_LOCK_END;
	unload_sscape(&mpu_config);
}

#endif
#endif
