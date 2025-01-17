/* $Id: eicon_isa.c,v 1.9 1999/09/08 20:17:31 armin Exp $
 *
 * ISDN low-level module for Eicon.Diehl active ISDN-Cards.
 * Hardware-specific code for old ISA cards.
 *
 * Copyright 1998    by Fritz Elfert (fritz@isdn4linux.de)
 * Copyright 1998,99 by Armin Schindler (mac@melware.de)
 * Copyright 1999    Cytronics & Melware (info@melware.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 * $Log: eicon_isa.c,v $
 * Revision 1.9  1999/09/08 20:17:31  armin
 * Added microchannel patch from Erik Weber.
 *
 * Revision 1.8  1999/09/06 07:29:35  fritz
 * Changed my mail-address.
 *
 * Revision 1.7  1999/08/22 20:26:48  calle
 * backported changes from kernel 2.3.14:
 * - several #include "config.h" gone, others come.
 * - "struct device" changed to "struct net_device" in 2.3.14, added a
 *   define in isdn_compat.h for older kernel versions.
 *
 * Revision 1.6  1999/07/25 15:12:06  armin
 * fix of some debug logs.
 * enabled ISA-cards option.
 *
 * Revision 1.5  1999/04/01 12:48:33  armin
 * Changed some log outputs.
 *
 * Revision 1.4  1999/03/29 11:19:46  armin
 * I/O stuff now in seperate file (eicon_io.c)
 * Old ISA type cards (S,SX,SCOM,Quadro,S2M) implemented.
 *
 * Revision 1.3  1999/03/02 12:37:45  armin
 * Added some important checks.
 * Analog Modem with DSP.
 * Channels will be added to Link-Level after loading firmware.
 *
 * Revision 1.2  1999/01/24 20:14:19  armin
 * Changed and added debug stuff.
 * Better data sending. (still problems with tty's flip buffer)
 *
 * Revision 1.1  1999/01/01 18:09:43  armin
 * First checkin of new eicon driver.
 * DIVA-Server BRI/PCI and PRI/PCI are supported.
 * Old diehl code is obsolete.
 *
 *
 */

#include <linux/config.h>
#include "eicon.h"
#include "eicon_isa.h"

#define check_shmem   check_region
#define release_shmem release_region
#define request_shmem request_region

char *eicon_isa_revision = "$Revision: 1.9 $";

#undef EICON_MCA_DEBUG

#ifdef CONFIG_ISDN_DRV_EICON_ISA

/* Mask for detecting invalid IRQ parameter */
static int eicon_isa_valid_irq[] = {
	0x1c1c, /* 2, 3, 4, 10, 11, 12 (S)*/
	0x1c1c, /* 2, 3, 4, 10, 11, 12 (SX) */
	0x1cbc, /* 2, 3, 4, 5, 7, 10, 11, 12 (SCOM) */
	0x1cbc, /* 2, 3, 4, 5, 6, 10, 11, 12 (Quadro) */
	0x1cbc  /* 2, 3, 4, 5, 7, 10, 11, 12 (S2M) */
};

static void
eicon_isa_release_shmem(eicon_isa_card *card) {
	if (card->mvalid)
		release_shmem((unsigned long)card->shmem, card->ramsize);
	card->mvalid = 0;
}

static void
eicon_isa_release_irq(eicon_isa_card *card) {
	if (!card->master)
		return;
	if (card->ivalid)
		free_irq(card->irq, card);
	card->ivalid = 0;
}

void
eicon_isa_release(eicon_isa_card *card) {
	eicon_isa_release_irq(card);
	eicon_isa_release_shmem(card);
}

void
eicon_isa_printpar(eicon_isa_card *card) {
	switch (card->type) {
		case EICON_CTYPE_S:
		case EICON_CTYPE_SX:
		case EICON_CTYPE_SCOM:
		case EICON_CTYPE_QUADRO:
		case EICON_CTYPE_S2M:
			printk(KERN_INFO "Eicon %s at 0x%lx, irq %d.\n",
			       eicon_ctype_name[card->type],
			       (unsigned long)card->shmem,
			       card->irq);
	}
}

int
eicon_isa_find_card(int Mem, int Irq, char * Id)
{
	int primary = 1;

	if (!strlen(Id))
		return -1;

	/* Check for valid membase address */
	if ((Mem < 0x0c0000) ||
	    (Mem > 0x0fc000) ||
	    (Mem & 0xfff)) { 
		printk(KERN_WARNING "eicon_isa: illegal membase 0x%x for %s\n",
			 Mem, Id);
		return -1;
	}
	if (check_shmem(Mem, RAMSIZE)) {
		printk(KERN_WARNING "eicon_isa_boot: memory at 0x%x already in use.\n", Mem);
		return -1;
	}

        writew(0x55aa, Mem + 0x402);
        if (readw(Mem + 0x402) != 0x55aa) primary = 0;
	writew(0, Mem + 0x402);
	if (readw(Mem + 0x402) != 0) primary = 0;

	printk(KERN_INFO "Eicon: Driver-ID: %s\n", Id);
	if (primary) {
		printk(KERN_INFO "Eicon: assuming pri card at 0x%x\n", Mem);
		writeb(0, Mem + 0x3ffe);
		return EICON_CTYPE_ISAPRI;
	} else {
		printk(KERN_INFO "Eicon: assuming bri card at 0x%x\n", Mem);
		writeb(0, Mem + 0x400);
		return EICON_CTYPE_ISABRI;
	}
	return -1;
}

int
eicon_isa_bootload(eicon_isa_card *card, eicon_isa_codebuf *cb) {
	int	tmp;
	int               timeout;
	eicon_isa_codebuf cbuf;
	unsigned char     *code;
	eicon_isa_boot    *boot;

	if (copy_from_user(&cbuf, cb, sizeof(eicon_isa_codebuf)))
		return -EFAULT;

	/* Allocate code-buffer and copy code from userspace */
	if (cbuf.bootstrap_len > 1024) {
		printk(KERN_WARNING "eicon_isa_boot: Invalid startup-code size %ld\n",
		       cbuf.bootstrap_len);
		return -EINVAL;
	}
	if (!(code = kmalloc(cbuf.bootstrap_len, GFP_KERNEL))) {
		printk(KERN_WARNING "eicon_isa_boot: Couldn't allocate code buffer\n");
		return -ENOMEM;
	}
	if (copy_from_user(code, &cb->code, cbuf.bootstrap_len)) {
		kfree(code);
		return -EFAULT;
	}

	switch(card->type) {
		case EICON_CTYPE_S:
		case EICON_CTYPE_SX:
		case EICON_CTYPE_SCOM:
		case EICON_CTYPE_QUADRO:
		case EICON_CTYPE_ISABRI:
			card->ramsize  = RAMSIZE;
			card->intack   = (__u8 *)card->shmem + INTACK;
			card->startcpu = (__u8 *)card->shmem + STARTCPU;
			card->stopcpu  = (__u8 *)card->shmem + STOPCPU;
			break;
		case EICON_CTYPE_S2M:
		case EICON_CTYPE_ISAPRI:
			card->ramsize  = RAMSIZE_P;
			card->intack   = (__u8 *)card->shmem + INTACK_P;
			card->startcpu = (__u8 *)card->shmem + STARTCPU_P;
			card->stopcpu  = (__u8 *)card->shmem + STOPCPU_P;
			break;
		default:
			printk(KERN_WARNING "eicon_isa_boot: Invalid card type %d\n", card->type);
			return -EINVAL;
	}

	/* Register shmem */
	if (check_shmem((unsigned long)card->shmem, card->ramsize)) {
		printk(KERN_WARNING "eicon_isa_boot: memory at 0x%lx already in use.\n",
			(unsigned long)card->shmem);
		kfree(code);
		return -EBUSY;
	}
	request_shmem((unsigned long)card->shmem, card->ramsize, "Eicon ISA ISDN");
#ifdef EICON_MCA_DEBUG
	printk(KERN_INFO "eicon_isa_boot: card->ramsize = %d.\n", card->ramsize);
#endif
	card->mvalid = 1;

	/* clear any pending irq's */
	readb(card->intack);
#ifdef CONFIG_MCA
	if (card->type == EICON_CTYPE_SCOM) {
		outb_p(0,card->io+1);
	}
	else {
		printk(KERN_WARNING "eicon_isa_boot: Card type yet not supported.\n");
		return -EINVAL;
	};

#ifdef EICON_MCA_DEBUG
	printk(KERN_INFO "eicon_isa_boot: card->io      = %x.\n", card->io);
	printk(KERN_INFO "eicon_isa_boot: card->irq     = %d.\n", (int)card->irq);
#endif
#else
	/* set reset-line active */
	writeb(0, card->stopcpu); 
#endif  /* CONFIG_MCA */
	/* clear irq-requests */
	writeb(0, card->intack);
	readb(card->intack);

	/* Copy code into card */
	memcpy_toio(&card->shmem->c, code, cbuf.bootstrap_len);

	/* Check for properly loaded code */
	if (!check_signature((unsigned long)&card->shmem->c, code, 1020)) {
		printk(KERN_WARNING "eicon_isa_boot: Could not load startup-code\n");
		eicon_isa_release_shmem(card);
		kfree(code);
		return -EIO;
	}
	/* if 16k-ramsize, duplicate the reset-jump-code */
	if (card->ramsize == RAMSIZE_P)
		memcpy_toio((__u8 *)card->shmem + 0x3ff0, &code[0x3f0], 12);

	kfree(code);
	boot = &card->shmem->boot;

	/* Delay 0.2 sec. */
	SLEEP(20);

	/* Start CPU */
	writeb(cbuf.boot_opt, &boot->ctrl);
#ifdef CONFIG_MCA
	outb_p(0, card->io);
#else 
	writeb(0, card->startcpu); 
#endif /* CONFIG_MCA */

	/* Delay 0.2 sec. */
	SLEEP(20);

	timeout = jiffies + (HZ * 22);
	while (timeout > jiffies) {
		if (readb(&boot->ctrl) == 0)
			break;
		SLEEP(10);
	}
	if (readb(&boot->ctrl) != 0) {
		printk(KERN_WARNING "eicon_isa_boot: CPU test failed.\n");
#ifdef EICON_MCA_DEBUG
		printk(KERN_INFO "eicon_isa_boot: &boot->ctrl = %d.\n",
			readb(&boot->ctrl));
#endif
		eicon_isa_release_shmem(card);
		return -EIO;
	}

	/* Check for memory-test errors */
	if (readw(&boot->ebit)) {
		printk(KERN_WARNING "eicon_isa_boot: memory test failed (bit 0x%04x at 0x%08x)\n",
		       readw(&boot->ebit), readl(&boot->eloc));
		eicon_isa_release_shmem(card);
		return -EIO;
	}

        /* Check card type and memory size */
        tmp = readb(&boot->card);
	if ((tmp < 0) || (tmp > 4)) {
		printk(KERN_WARNING "eicon_isa_boot: Type detect failed\n");
		eicon_isa_release_shmem(card);
		return -EIO;
	}
	card->type = tmp;
	((eicon_card *)card->card)->type = tmp;

        tmp = readb(&boot->msize);
        if (tmp != 8 && tmp != 16 && tmp != 24 &&
            tmp != 32 && tmp != 48 && tmp != 60) {
                printk(KERN_WARNING "eicon_isa_boot: invalid memsize\n");
		eicon_isa_release_shmem(card);
                return -EIO;
        }
	printk(KERN_INFO "%s: startup-code loaded\n", eicon_ctype_name[card->type]); 
	if ((card->type == EICON_CTYPE_QUADRO) && (card->master)) {
		tmp = eicon_addcard(card->type, (unsigned long)card->shmem, card->irq, 
					((eicon_card *)card->card)->regname);
		printk(KERN_INFO "Eicon: %d adapters added\n", tmp);
	}
	return 0;
}

int
eicon_isa_load(eicon_isa_card *card, eicon_isa_codebuf *cb) {
	eicon_isa_boot    *boot;
	int               tmp;
	int               timeout;
	int 		  j;
	eicon_isa_codebuf cbuf;
	unsigned char     *code;
	unsigned char     *p;

	if (copy_from_user(&cbuf, cb, sizeof(eicon_isa_codebuf)))
		return -EFAULT;

	if (!(code = kmalloc(cbuf.firmware_len, GFP_KERNEL))) {
		printk(KERN_WARNING "eicon_isa_load: Couldn't allocate code buffer\n");
		return -ENOMEM;
	}

	if (copy_from_user(code, &cb->code, cbuf.firmware_len)) {
		kfree(code);
		return -EFAULT;
	}

	boot = &card->shmem->boot;

	if ((!card->ivalid) && card->master) {
		card->irqprobe = 1;
		/* Check for valid IRQ */
		if ((card->irq < 0) || (card->irq > 15) || 
		    (!((1 << card->irq) & eicon_isa_valid_irq[card->type & 0x0f]))) {
			printk(KERN_WARNING "eicon_isa_load: illegal irq: %d\n", card->irq);
			eicon_isa_release_shmem(card);
			kfree(code);
			return -EINVAL;
		}
		/* Register irq */
		if (!request_irq(card->irq, &eicon_irq, 0, "Eicon ISA ISDN", card))
			card->ivalid = 1;
		else {
			printk(KERN_WARNING "eicon_isa_load: irq %d already in use.\n",
			       card->irq);
			eicon_isa_release_shmem(card);
			kfree(code);
			return -EBUSY;
		}
	}

        tmp = readb(&boot->msize);
        if (tmp != 8 && tmp != 16 && tmp != 24 &&
            tmp != 32 && tmp != 48 && tmp != 60) {
                printk(KERN_WARNING "eicon_isa_load: invalid memsize\n");
		eicon_isa_release_shmem(card);
                return -EIO;
        }

	eicon_isa_printpar(card);

	/* Download firmware */
	printk(KERN_INFO "%s %dkB, loading firmware ...\n", 
	       eicon_ctype_name[card->type],
	       tmp * 16);
	tmp = cbuf.firmware_len >> 8;
	p = code;
	while (tmp--) {
		memcpy_toio(&boot->b, p, 256);
		writeb(1, &boot->ctrl);
		timeout = jiffies + 10;
		while (timeout > jiffies) {
			if (readb(&boot->ctrl) == 0)
				break;
			SLEEP(2);
		}
		if (readb(&boot->ctrl)) {
			printk(KERN_WARNING "eicon_isa_load: download timeout at 0x%x\n", p-code);
			eicon_isa_release(card);
			kfree(code);
			return -EIO;
		}
		p += 256;
	}
	kfree(code);

	/* Initialize firmware parameters */
	memcpy_toio(&card->shmem->c[8], &cbuf.tei, 14);
	memcpy_toio(&card->shmem->c[32], &cbuf.oad, 96);
	memcpy_toio(&card->shmem->c[128], &cbuf.oad, 96);
	
	/* Start firmware, wait for signature */
	writeb(2, &boot->ctrl);
	timeout = jiffies + (5*HZ);
	while (timeout > jiffies) {
		if (readw(&boot->signature) == 0x4447)
			break;
		SLEEP(2);
	}
	if (readw(&boot->signature) != 0x4447) {
		printk(KERN_WARNING "eicon_isa_load: firmware selftest failed %04x\n",
		       readw(&boot->signature));
		eicon_isa_release(card);
		return -EIO;
	}

	card->channels = readb(&card->shmem->c[0x3f6]);

	/* clear irq-requests, reset irq-count */
	readb(card->intack);
	writeb(0, card->intack);

	if (card->master) {
		card->irqprobe = 1;
		/* Trigger an interrupt and check if it is delivered */
		tmp = readb(&card->shmem->com.ReadyInt);
		tmp ++;
		writeb(tmp, &card->shmem->com.ReadyInt);
		timeout = jiffies + 20;
		while (timeout > jiffies) {
			if (card->irqprobe > 1)
				break;
			SLEEP(2);
		}
		if (card->irqprobe == 1) {
			printk(KERN_WARNING "eicon_isa_load: IRQ # %d test failed\n", card->irq);
			eicon_isa_release(card);
			return -EIO;
		}
	}
#ifdef EICON_MCA_DEBUG
	printk(KERN_INFO "eicon_isa_load: IRQ # %d test succeeded.\n", card->irq);
#endif

	writeb(card->irq, &card->shmem->com.Int);

	/* initializing some variables */
	((eicon_card *)card->card)->ReadyInt = 0;
	((eicon_card *)card->card)->ref_in  = 1;
	((eicon_card *)card->card)->ref_out = 1;
	for(j=0; j<256; j++) ((eicon_card *)card->card)->IdTable[j] = NULL;
	for(j=0; j< (card->channels + 1); j++) {
		((eicon_card *)card->card)->bch[j].e.busy = 0;
		((eicon_card *)card->card)->bch[j].e.D3Id = 0;
		((eicon_card *)card->card)->bch[j].e.B2Id = 0;
		((eicon_card *)card->card)->bch[j].e.ref = 0;
		((eicon_card *)card->card)->bch[j].e.Req = 0;
		((eicon_card *)card->card)->bch[j].e.complete = 1;
		((eicon_card *)card->card)->bch[j].fsm_state = EICON_STATE_NULL;
	}

	printk(KERN_INFO "Eicon: Supported channels: %d\n", card->channels); 
	printk(KERN_INFO "%s successfully started\n", eicon_ctype_name[card->type]);

	/* Enable normal IRQ processing */
	card->irqprobe = 0;
	return 0;
}

#endif /* CONFIG_ISDN_DRV_EICON_ISA */
