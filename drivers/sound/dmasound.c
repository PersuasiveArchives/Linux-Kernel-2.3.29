
/* linux/drivers/sound/dmasound.c */

/*

OSS/Free compatible Atari TT/Falcon and Amiga DMA sound driver for Linux/m68k
Extended to support Power Macintosh for Linux/ppc by Paul Mackerras

(c) 1995 by Michael Schlueter & Michael Marte

Michael Schlueter (michael@duck.syd.de) did the basic structure of the VFS
interface and the u-law to signed byte conversion.

Michael Marte (marte@informatik.uni-muenchen.de) did the sound queue,
/dev/mixer, /dev/sndstat and complemented the VFS interface. He would like
to thank:
Michael Schlueter for initial ideas and documentation on the MFP and
the DMA sound hardware.
Therapy? for their CD 'Troublegum' which really made me rock.

/dev/sndstat is based on code by Hannu Savolainen, the author of the
VoxWare family of drivers.

This file is subject to the terms and conditions of the GNU General Public
License.  See the file COPYING in the main directory of this archive
for more details.

History:
1995/8/25	first release

1995/9/02	++roman: fixed atari_stram_alloc() call, the timer programming
			and several race conditions

1995/9/14	++roman: After some discussion with Michael Schlueter, revised
			the interrupt disabling
			Slightly speeded up U8->S8 translation by using long
			operations where possible
			Added 4:3 interpolation for /dev/audio

1995/9/20	++TeSche: Fixed a bug in sq_write and changed /dev/audio
			converting to play at 12517Hz instead of 6258Hz.

1995/9/23	++TeSche: Changed sq_interrupt() and sq_play() to pre-program
			the DMA for another frame while there's still one
			running. This allows the IRQ response to be
			arbitrarily delayed and playing will still continue.

1995/10/14	++Guenther_Kelleter@ac3.maus.de, ++TeSche: better support for
			Falcon audio (the Falcon doesn't raise an IRQ at the
			end of a frame, but at the beginning instead!). uses
			'if (codec_dma)' in lots of places to simply switch
			between Falcon and TT code.

1995/11/06	++TeSche: started introducing a hardware abstraction scheme
			(may perhaps also serve for Amigas?), can now play
			samples at almost all frequencies by means of a more
			generalized expand routine, takes a good deal of care
			to cut data only at sample sizes, buffer size is now
			a kernel runtime option, implemented fsync() & several
			minor improvements
		++Guenther: useful hints and bug fixes, cross-checked it for
			Falcons

1996/3/9	++geert: support added for Amiga, A-law, 16-bit little endian.
			Unification to drivers/sound/dmasound.c.

1996/4/6	++Martin Mitchell: updated to 1.3 kernel.

1996/6/13       ++topi: fixed things that were broken (mainly the amiga
                        14-bit routines), /dev/sndstat shows now the real
                        hardware frequency, the lowpass filter is disabled
			by default now.

1996/9/25	++geert: modularization

1998-06-10	++andreas: converted to use sound_core

*/


#include <linux/module.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/major.h>
#include <linux/config.h>
#include <linux/fcntl.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/sound.h>
#include <linux/init.h>
#include <linux/delay.h>

#if defined(__mc68000__) || defined(CONFIG_APUS)
#include <asm/setup.h>
#endif
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>

#ifdef CONFIG_ATARI
#include <asm/atarihw.h>
#include <asm/atariints.h>
#include <asm/atari_stram.h>
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
#include <asm/amigahw.h>
#include <asm/amigaints.h>
#endif /* CONFIG_AMIGA */
#ifdef CONFIG_PPC
#include <linux/adb.h>
#include <linux/cuda.h>
#include <linux/pmu.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/io.h>
#include <asm/dbdma.h>
#include "awacs_defs.h"
#include <linux/nvram.h>
#include <linux/vt_kern.h>
#endif /* CONFIG_PPC */

#include "dmasound.h"
#include <linux/soundcard.h>

#define HAS_8BIT_TABLES

#ifdef MODULE
static int sq_unit = -1;
static int mixer_unit = -1;
static int state_unit = -1;
static int irq_installed = 0;
#endif /* MODULE */
static char **sound_buffers = NULL;
#ifdef CONFIG_PPC
static char **sound_read_buffers = NULL;
#endif

#ifdef CONFIG_ATARI
extern void atari_microwire_cmd(int cmd);
#endif /* CONFIG_ATARI */

#ifdef CONFIG_AMIGA
   /*
    *	The minimum period for audio depends on htotal (for OCS/ECS/AGA)
    *	(Imported from arch/m68k/amiga/amisound.c)
    */

extern volatile u_short amiga_audio_min_period;


   /*
    *	amiga_mksound() should be able to restore the period after beeping
    *	(Imported from arch/m68k/amiga/amisound.c)
    */

extern u_short amiga_audio_period;


   /*
    *	Audio DMA masks
    */

#define AMI_AUDIO_OFF	(DMAF_AUD0 | DMAF_AUD1 | DMAF_AUD2 | DMAF_AUD3)
#define AMI_AUDIO_8	(DMAF_SETCLR | DMAF_MASTER | DMAF_AUD0 | DMAF_AUD1)
#define AMI_AUDIO_14	(AMI_AUDIO_8 | DMAF_AUD2 | DMAF_AUD3)

#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
/*
 * Interrupt numbers and addresses, obtained from the device tree.
 */
static int awacs_irq, awacs_tx_irq, awacs_rx_irq;
static volatile struct awacs_regs *awacs;
static volatile struct dbdma_regs *awacs_txdma, *awacs_rxdma;
static int awacs_rate_index;
static int awacs_subframe;
static int awacs_spkr_vol;

static int awacs_revision;
#define AWACS_BURGUNDY	100		/* fake revision # for burgundy */

/*
 * Space for the DBDMA command blocks.
 */
static void *awacs_tx_cmd_space;
static volatile struct dbdma_cmd *awacs_tx_cmds;

static void *awacs_rx_cmd_space;
static volatile struct dbdma_cmd *awacs_rx_cmds;

/*
 * Cached values of AWACS registers (we can't read them).
 * Except on the burgundy. XXX
 */
int awacs_reg[5];

#define HAS_16BIT_TABLES
#undef HAS_8BIT_TABLES

/*
 * Stuff for outputting a beep.  The values range from -327 to +327
 * so we can multiply by an amplitude in the range 0..100 to get a
 * signed short value to put in the output buffer.
 */
static short beep_wform[256] = {
	0,	40,	79,	117,	153,	187,	218,	245,
	269,	288,	304,	316,	323,	327,	327,	324,
	318,	310,	299,	288,	275,	262,	249,	236,
	224,	213,	204,	196,	190,	186,	183,	182,
	182,	183,	186,	189,	192,	196,	200,	203,
	206,	208,	209,	209,	209,	207,	204,	201,
	197,	193,	188,	183,	179,	174,	170,	166,
	163,	161,	160,	159,	159,	160,	161,	162,
	164,	166,	168,	169,	171,	171,	171,	170,
	169,	167,	163,	159,	155,	150,	144,	139,
	133,	128,	122,	117,	113,	110,	107,	105,
	103,	103,	103,	103,	104,	104,	105,	105,
	105,	103,	101,	97,	92,	86,	78,	68,
	58,	45,	32,	18,	3,	-11,	-26,	-41,
	-55,	-68,	-79,	-88,	-95,	-100,	-102,	-102,
	-99,	-93,	-85,	-75,	-62,	-48,	-33,	-16,
	0,	16,	33,	48,	62,	75,	85,	93,
	99,	102,	102,	100,	95,	88,	79,	68,
	55,	41,	26,	11,	-3,	-18,	-32,	-45,
	-58,	-68,	-78,	-86,	-92,	-97,	-101,	-103,
	-105,	-105,	-105,	-104,	-104,	-103,	-103,	-103,
	-103,	-105,	-107,	-110,	-113,	-117,	-122,	-128,
	-133,	-139,	-144,	-150,	-155,	-159,	-163,	-167,
	-169,	-170,	-171,	-171,	-171,	-169,	-168,	-166,
	-164,	-162,	-161,	-160,	-159,	-159,	-160,	-161,
	-163,	-166,	-170,	-174,	-179,	-183,	-188,	-193,
	-197,	-201,	-204,	-207,	-209,	-209,	-209,	-208,
	-206,	-203,	-200,	-196,	-192,	-189,	-186,	-183,
	-182,	-182,	-183,	-186,	-190,	-196,	-204,	-213,
	-224,	-236,	-249,	-262,	-275,	-288,	-299,	-310,
	-318,	-324,	-327,	-327,	-323,	-316,	-304,	-288,
	-269,	-245,	-218,	-187,	-153,	-117,	-79,	-40,
};

#define BEEP_SRATE	22050	/* 22050 Hz sample rate */
#define BEEP_BUFLEN	512
#define BEEP_VOLUME	15	/* 0 - 100 */

static int beep_volume = BEEP_VOLUME;
static int beep_playing = 0;
static int awacs_beep_state = 0;
static short *beep_buf;
static volatile struct dbdma_cmd *beep_dbdma_cmd;
static void (*orig_mksound)(unsigned int, unsigned int);
static int is_pbook_3400;
static int is_pbook_G3;
static unsigned char *macio_base;

/* Burgundy functions */
static void awacs_burgundy_wcw(unsigned addr,unsigned newval);
static unsigned awacs_burgundy_rcw(unsigned addr);
static void awacs_burgundy_write_volume(unsigned address, int volume);
static int awacs_burgundy_read_volume(unsigned address);
static void awacs_burgundy_write_mvolume(unsigned address, int volume);
static int awacs_burgundy_read_mvolume(unsigned address);

#ifdef CONFIG_PMAC_PBOOK
/*
 * Stuff for restoring after a sleep.
 */
static int awacs_sleep_notify(struct pmu_sleep_notifier *self, int when);
struct pmu_sleep_notifier awacs_sleep_notifier = {
	awacs_sleep_notify, SLEEP_LEVEL_SOUND,
};
#endif /* CONFIG_PMAC_PBOOK */

#endif /* CONFIG_PPC */

/*** Some declarations *******************************************************/


#define DMASND_TT		1
#define DMASND_FALCON		2
#define DMASND_AMIGA		3
#define DMASND_AWACS		4

#define MAX_CATCH_RADIUS	10
#define MIN_BUFFERS		4
#define MIN_BUFSIZE 		4
#define MAX_BUFSIZE		128	/* Limit for Amiga */

static int catchRadius = 0;
static int numBufs = 4, bufSize = 32;
#ifdef CONFIG_PPC
static int numReadBufs = 4, readbufSize = 32;
#endif

MODULE_PARM(catchRadius, "i");
MODULE_PARM(numBufs, "i");
MODULE_PARM(bufSize, "i");
MODULE_PARM(numReadBufs, "i");
MODULE_PARM(readbufSize, "i");

#define arraysize(x)	(sizeof(x)/sizeof(*(x)))
#define min(x, y)	((x) < (y) ? (x) : (y))
#define le2be16(x)	(((x)<<8 & 0xff00) | ((x)>>8 & 0x00ff))
#define le2be16dbl(x)	(((x)<<8 & 0xff00ff00) | ((x)>>8 & 0x00ff00ff))

#define IOCTL_IN(arg, ret) \
	do { int error = get_user(ret, (int *)(arg)); \
		if (error) return error; \
	} while (0)
#define IOCTL_OUT(arg, ret)	ioctl_return((int *)(arg), ret)


/*** Some low level helpers **************************************************/

#ifdef HAS_8BIT_TABLES
/* 8 bit mu-law */

static char ulaw2dma8[] = {
	-126,	-122,	-118,	-114,	-110,	-106,	-102,	-98,
	-94,	-90,	-86,	-82,	-78,	-74,	-70,	-66,
	-63,	-61,	-59,	-57,	-55,	-53,	-51,	-49,
	-47,	-45,	-43,	-41,	-39,	-37,	-35,	-33,
	-31,	-30,	-29,	-28,	-27,	-26,	-25,	-24,
	-23,	-22,	-21,	-20,	-19,	-18,	-17,	-16,
	-16,	-15,	-15,	-14,	-14,	-13,	-13,	-12,
	-12,	-11,	-11,	-10,	-10,	-9,	-9,	-8,
	-8,	-8,	-7,	-7,	-7,	-7,	-6,	-6,
	-6,	-6,	-5,	-5,	-5,	-5,	-4,	-4,
	-4,	-4,	-4,	-4,	-3,	-3,	-3,	-3,
	-3,	-3,	-3,	-3,	-2,	-2,	-2,	-2,
	-2,	-2,	-2,	-2,	-2,	-2,	-2,	-2,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	0,
	125,	121,	117,	113,	109,	105,	101,	97,
	93,	89,	85,	81,	77,	73,	69,	65,
	62,	60,	58,	56,	54,	52,	50,	48,
	46,	44,	42,	40,	38,	36,	34,	32,
	30,	29,	28,	27,	26,	25,	24,	23,
	22,	21,	20,	19,	18,	17,	16,	15,
	15,	14,	14,	13,	13,	12,	12,	11,
	11,	10,	10,	9,	9,	8,	8,	7,
	7,	7,	6,	6,	6,	6,	5,	5,
	5,	5,	4,	4,	4,	4,	3,	3,
	3,	3,	3,	3,	2,	2,	2,	2,
	2,	2,	2,	2,	1,	1,	1,	1,
	1,	1,	1,	1,	1,	1,	1,	1,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0
};

/* 8 bit A-law */

static char alaw2dma8[] = {
	-22,	-21,	-24,	-23,	-18,	-17,	-20,	-19,
	-30,	-29,	-32,	-31,	-26,	-25,	-28,	-27,
	-11,	-11,	-12,	-12,	-9,	-9,	-10,	-10,
	-15,	-15,	-16,	-16,	-13,	-13,	-14,	-14,
	-86,	-82,	-94,	-90,	-70,	-66,	-78,	-74,
	-118,	-114,	-126,	-122,	-102,	-98,	-110,	-106,
	-43,	-41,	-47,	-45,	-35,	-33,	-39,	-37,
	-59,	-57,	-63,	-61,	-51,	-49,	-55,	-53,
	-2,	-2,	-2,	-2,	-2,	-2,	-2,	-2,
	-2,	-2,	-2,	-2,	-2,	-2,	-2,	-2,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
	-6,	-6,	-6,	-6,	-5,	-5,	-5,	-5,
	-8,	-8,	-8,	-8,	-7,	-7,	-7,	-7,
	-3,	-3,	-3,	-3,	-3,	-3,	-3,	-3,
	-4,	-4,	-4,	-4,	-4,	-4,	-4,	-4,
	21,	20,	23,	22,	17,	16,	19,	18,
	29,	28,	31,	30,	25,	24,	27,	26,
	10,	10,	11,	11,	8,	8,	9,	9,
	14,	14,	15,	15,	12,	12,	13,	13,
	86,	82,	94,	90,	70,	66,	78,	74,
	118,	114,	126,	122,	102,	98,	110,	106,
	43,	41,	47,	45,	35,	33,	39,	37,
	59,	57,	63,	61,	51,	49,	55,	53,
	1,	1,	1,	1,	1,	1,	1,	1,
	1,	1,	1,	1,	1,	1,	1,	1,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	5,	5,	5,	5,	4,	4,	4,	4,
	7,	7,	7,	7,	6,	6,	6,	6,
	2,	2,	2,	2,	2,	2,	2,	2,
	3,	3,	3,	3,	3,	3,	3,	3
};
#endif /* HAS_8BIT_TABLES */

#ifdef HAS_16BIT_TABLES

/* 16 bit mu-law */

static short ulaw2dma16[] = {
	-32124,	-31100,	-30076,	-29052,	-28028,	-27004,	-25980,	-24956,
	-23932,	-22908,	-21884,	-20860,	-19836,	-18812,	-17788,	-16764,
	-15996,	-15484,	-14972,	-14460,	-13948,	-13436,	-12924,	-12412,
	-11900,	-11388,	-10876,	-10364,	-9852,	-9340,	-8828,	-8316,
	-7932,	-7676,	-7420,	-7164,	-6908,	-6652,	-6396,	-6140,
	-5884,	-5628,	-5372,	-5116,	-4860,	-4604,	-4348,	-4092,
	-3900,	-3772,	-3644,	-3516,	-3388,	-3260,	-3132,	-3004,
	-2876,	-2748,	-2620,	-2492,	-2364,	-2236,	-2108,	-1980,
	-1884,	-1820,	-1756,	-1692,	-1628,	-1564,	-1500,	-1436,
	-1372,	-1308,	-1244,	-1180,	-1116,	-1052,	-988,	-924,
	-876,	-844,	-812,	-780,	-748,	-716,	-684,	-652,
	-620,	-588,	-556,	-524,	-492,	-460,	-428,	-396,
	-372,	-356,	-340,	-324,	-308,	-292,	-276,	-260,
	-244,	-228,	-212,	-196,	-180,	-164,	-148,	-132,
	-120,	-112,	-104,	-96,	-88,	-80,	-72,	-64,
	-56,	-48,	-40,	-32,	-24,	-16,	-8,	0,
	32124,	31100,	30076,	29052,	28028,	27004,	25980,	24956,
	23932,	22908,	21884,	20860,	19836,	18812,	17788,	16764,
	15996,	15484,	14972,	14460,	13948,	13436,	12924,	12412,
	11900,	11388,	10876,	10364,	9852,	9340,	8828,	8316,
	7932,	7676,	7420,	7164,	6908,	6652,	6396,	6140,
	5884,	5628,	5372,	5116,	4860,	4604,	4348,	4092,
	3900,	3772,	3644,	3516,	3388,	3260,	3132,	3004,
	2876,	2748,	2620,	2492,	2364,	2236,	2108,	1980,
	1884,	1820,	1756,	1692,	1628,	1564,	1500,	1436,
	1372,	1308,	1244,	1180,	1116,	1052,	988,	924,
	876,	844,	812,	780,	748,	716,	684,	652,
	620,	588,	556,	524,	492,	460,	428,	396,
	372,	356,	340,	324,	308,	292,	276,	260,
	244,	228,	212,	196,	180,	164,	148,	132,
	120,	112,	104,	96,	88,	80,	72,	64,
	56,	48,	40,	32,	24,	16,	8,	0,
};

/* 16 bit A-law */

static short alaw2dma16[] = {
	-5504,	-5248,	-6016,	-5760,	-4480,	-4224,	-4992,	-4736,
	-7552,	-7296,	-8064,	-7808,	-6528,	-6272,	-7040,	-6784,
	-2752,	-2624,	-3008,	-2880,	-2240,	-2112,	-2496,	-2368,
	-3776,	-3648,	-4032,	-3904,	-3264,	-3136,	-3520,	-3392,
	-22016,	-20992,	-24064,	-23040,	-17920,	-16896,	-19968,	-18944,
	-30208,	-29184,	-32256,	-31232,	-26112,	-25088,	-28160,	-27136,
	-11008,	-10496,	-12032,	-11520,	-8960,	-8448,	-9984,	-9472,
	-15104,	-14592,	-16128,	-15616,	-13056,	-12544,	-14080,	-13568,
	-344,	-328,	-376,	-360,	-280,	-264,	-312,	-296,
	-472,	-456,	-504,	-488,	-408,	-392,	-440,	-424,
	-88,	-72,	-120,	-104,	-24,	-8,	-56,	-40,
	-216,	-200,	-248,	-232,	-152,	-136,	-184,	-168,
	-1376,	-1312,	-1504,	-1440,	-1120,	-1056,	-1248,	-1184,
	-1888,	-1824,	-2016,	-1952,	-1632,	-1568,	-1760,	-1696,
	-688,	-656,	-752,	-720,	-560,	-528,	-624,	-592,
	-944,	-912,	-1008,	-976,	-816,	-784,	-880,	-848,
	5504,	5248,	6016,	5760,	4480,	4224,	4992,	4736,
	7552,	7296,	8064,	7808,	6528,	6272,	7040,	6784,
	2752,	2624,	3008,	2880,	2240,	2112,	2496,	2368,
	3776,	3648,	4032,	3904,	3264,	3136,	3520,	3392,
	22016,	20992,	24064,	23040,	17920,	16896,	19968,	18944,
	30208,	29184,	32256,	31232,	26112,	25088,	28160,	27136,
	11008,	10496,	12032,	11520,	8960,	8448,	9984,	9472,
	15104,	14592,	16128,	15616,	13056,	12544,	14080,	13568,
	344,	328,	376,	360,	280,	264,	312,	296,
	472,	456,	504,	488,	408,	392,	440,	424,
	88,	72,	120,	104,	24,	8,	56,	40,
	216,	200,	248,	232,	152,	136,	184,	168,
	1376,	1312,	1504,	1440,	1120,	1056,	1248,	1184,
	1888,	1824,	2016,	1952,	1632,	1568,	1760,	1696,
	688,	656,	752,	720,	560,	528,	624,	592,
	944,	912,	1008,	976,	816,	784,	880,	848,
};
#endif /* HAS_16BIT_TABLES */


#ifdef HAS_14BIT_TABLES

/* 14 bit mu-law (LSB) */

static char alaw2dma14l[] = {
	33,	33,	33,	33,	33,	33,	33,	33,
	33,	33,	33,	33,	33,	33,	33,	33,
	33,	33,	33,	33,	33,	33,	33,	33,
	33,	33,	33,	33,	33,	33,	33,	33,
	1,	1,	1,	1,	1,	1,	1,	1,
	1,	1,	1,	1,	1,	1,	1,	1,
	49,	17,	49,	17,	49,	17,	49,	17,
	49,	17,	49,	17,	49,	17,	49,	17,
	41,	57,	9,	25,	41,	57,	9,	25,
	41,	57,	9,	25,	41,	57,	9,	25,
	37,	45,	53,	61,	5,	13,	21,	29,
	37,	45,	53,	61,	5,	13,	21,	29,
	35,	39,	43,	47,	51,	55,	59,	63,
	3,	7,	11,	15,	19,	23,	27,	31,
	34,	36,	38,	40,	42,	44,	46,	48,
	50,	52,	54,	56,	58,	60,	62,	0,
	31,	31,	31,	31,	31,	31,	31,	31,
	31,	31,	31,	31,	31,	31,	31,	31,
	31,	31,	31,	31,	31,	31,	31,	31,
	31,	31,	31,	31,	31,	31,	31,	31,
	63,	63,	63,	63,	63,	63,	63,	63,
	63,	63,	63,	63,	63,	63,	63,	63,
	15,	47,	15,	47,	15,	47,	15,	47,
	15,	47,	15,	47,	15,	47,	15,	47,
	23,	7,	55,	39,	23,	7,	55,	39,
	23,	7,	55,	39,	23,	7,	55,	39,
	27,	19,	11,	3,	59,	51,	43,	35,
	27,	19,	11,	3,	59,	51,	43,	35,
	29,	25,	21,	17,	13,	9,	5,	1,
	61,	57,	53,	49,	45,	41,	37,	33,
	30,	28,	26,	24,	22,	20,	18,	16,
	14,	12,	10,	8,	6,	4,	2,	0
};

/* 14 bit A-law (LSB) */

static char alaw2dma14l[] = {
	32,	32,	32,	32,	32,	32,	32,	32,
	32,	32,	32,	32,	32,	32,	32,	32,
	16,	48,	16,	48,	16,	48,	16,	48,
	16,	48,	16,	48,	16,	48,	16,	48,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	42,	46,	34,	38,	58,	62,	50,	54,
	10,	14,	2,	6,	26,	30,	18,	22,
	42,	46,	34,	38,	58,	62,	50,	54,
	10,	14,	2,	6,	26,	30,	18,	22,
	40,	56,	8,	24,	40,	56,	8,	24,
	40,	56,	8,	24,	40,	56,	8,	24,
	20,	28,	4,	12,	52,	60,	36,	44,
	20,	28,	4,	12,	52,	60,	36,	44,
	32,	32,	32,	32,	32,	32,	32,	32,
	32,	32,	32,	32,	32,	32,	32,	32,
	48,	16,	48,	16,	48,	16,	48,	16,
	48,	16,	48,	16,	48,	16,	48,	16,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	22,	18,	30,	26,	6,	2,	14,	10,
	54,	50,	62,	58,	38,	34,	46,	42,
	22,	18,	30,	26,	6,	2,	14,	10,
	54,	50,	62,	58,	38,	34,	46,	42,
	24,	8,	56,	40,	24,	8,	56,	40,
	24,	8,	56,	40,	24,	8,	56,	40,
	44,	36,	60,	52,	12,	4,	28,	20,
	44,	36,	60,	52,	12,	4,	28,	20
};
#endif /* HAS_14BIT_TABLES */


/*** Translations ************************************************************/


#ifdef CONFIG_ATARI
static ssize_t ata_ct_law(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t ata_ct_s8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft);
static ssize_t ata_ct_u8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft);
static ssize_t ata_ct_s16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ata_ct_u16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ata_ct_s16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ata_ct_u16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ata_ctx_law(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t ata_ctx_s8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t ata_ctx_u8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t ata_ctx_s16be(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft);
static ssize_t ata_ctx_u16be(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft);
static ssize_t ata_ctx_s16le(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft);
static ssize_t ata_ctx_u16le(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft);
#endif /* CONFIG_ATARI */

#ifdef CONFIG_AMIGA
static ssize_t ami_ct_law(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t ami_ct_s8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft);
static ssize_t ami_ct_u8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft);
static ssize_t ami_ct_s16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ami_ct_u16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ami_ct_s16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t ami_ct_u16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
static ssize_t pmac_ct_law(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t pmac_ct_s8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t pmac_ct_u8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft);
static ssize_t pmac_ct_s16(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t pmac_ct_u16(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t pmac_ctx_law(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t pmac_ctx_s8(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t pmac_ctx_u8(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t pmac_ctx_s16(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t pmac_ctx_u16(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft);
static ssize_t pmac_ct_s16_read(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
static ssize_t pmac_ct_u16_read(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft);
#endif /* CONFIG_PPC */

/*** Machine definitions *****************************************************/


typedef struct {
	int type;
	void *(*dma_alloc)(unsigned int, int);
	void (*dma_free)(void *, unsigned int);
	int (*irqinit)(void);
#ifdef MODULE
	void (*irqcleanup)(void);
#endif /* MODULE */
	void (*init)(void);
	void (*silence)(void);
	int (*setFormat)(int);
	int (*setVolume)(int);
	int (*setBass)(int);
	int (*setTreble)(int);
	int (*setGain)(int);
	void (*play)(void);
} MACHINE;


/*** Low level stuff *********************************************************/


typedef struct {
	int format;		/* AFMT_* */
	int stereo;		/* 0 = mono, 1 = stereo */
	int size;		/* 8/16 bit*/
	int speed;		/* speed */
} SETTINGS;

typedef struct {
	ssize_t (*ct_ulaw)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_alaw)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_s8)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_u8)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_s16be)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_u16be)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_s16le)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
	ssize_t (*ct_u16le)(const u_char *, size_t, u_char *, ssize_t *, ssize_t);
} TRANS;

struct sound_settings {
	MACHINE mach;		/* machine dependent things */
	SETTINGS hard;		/* hardware settings */
	SETTINGS soft;		/* software settings */
	SETTINGS dsp;		/* /dev/dsp default settings */
	TRANS *trans;		/* supported translations */
#if defined(CONFIG_PPC)
	TRANS *read_trans;	/* supported translations */
#endif
	int volume_left;	/* volume (range is machine dependent) */
	int volume_right;
	int bass;		/* tone (range is machine dependent) */
	int treble;
	int gain;
	int minDev;		/* minor device number currently open */
#if defined(CONFIG_ATARI) || defined(CONFIG_PPC)
	int bal;		/* balance factor for expanding (not volume!) */
	u_long data;		/* data for expanding */
#endif /* CONFIG_ATARI */
};

static struct sound_settings sound;


#ifdef CONFIG_ATARI
static void *AtaAlloc(unsigned int size, int flags);
static void AtaFree(void *, unsigned int size);
static int AtaIrqInit(void);
#ifdef MODULE
static void AtaIrqCleanUp(void);
#endif /* MODULE */
static int AtaSetBass(int bass);
static int AtaSetTreble(int treble);
static void TTSilence(void);
static void TTInit(void);
static int TTSetFormat(int format);
static int TTSetVolume(int volume);
static int TTSetGain(int gain);
static void FalconSilence(void);
static void FalconInit(void);
static int FalconSetFormat(int format);
static int FalconSetVolume(int volume);
static void ata_sq_play_next_frame(int index);
static void AtaPlay(void);
static void ata_sq_interrupt(int irq, void *dummy, struct pt_regs *fp);
#endif /* CONFIG_ATARI */

#ifdef CONFIG_AMIGA
static void *AmiAlloc(unsigned int size, int flags);
static void AmiFree(void *, unsigned int);
static int AmiIrqInit(void);
#ifdef MODULE
static void AmiIrqCleanUp(void);
#endif /* MODULE */
static void AmiSilence(void);
static void AmiInit(void);
static int AmiSetFormat(int format);
static int AmiSetVolume(int volume);
static int AmiSetTreble(int treble);
static void ami_sq_play_next_frame(int index);
static void AmiPlay(void);
static void ami_sq_interrupt(int irq, void *dummy, struct pt_regs *fp);
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
static void *PMacAlloc(unsigned int size, int flags);
static void PMacFree(void *ptr, unsigned int size);
static int PMacIrqInit(void);
#ifdef MODULE
static void PMacIrqCleanup(void);
#endif /* MODULE */
static void PMacSilence(void);
static void PMacInit(void);
static void PMacPlay(void);
static void PMacRecord(void);
static int PMacSetFormat(int format);
static int PMacSetVolume(int volume);
static void pmac_awacs_tx_intr(int irq, void *devid, struct pt_regs *regs);
static void pmac_awacs_rx_intr(int irq, void *devid, struct pt_regs *regs);
static void pmac_awacs_intr(int irq, void *devid, struct pt_regs *regs);
static void awacs_write(int val);
static int awacs_get_volume(int reg, int lshift);
static int awacs_volume_setter(int volume, int n, int mute, int lshift);
static void awacs_mksound(unsigned int hz, unsigned int ticks);
static void awacs_nosound(unsigned long xx);
#endif /* CONFIG_PPC */

/*** Mid level stuff *********************************************************/


static void sound_silence(void);
static void sound_init(void);
static int sound_set_format(int format);
static int sound_set_speed(int speed);
static int sound_set_stereo(int stereo);
static int sound_set_volume(int volume);
#ifdef CONFIG_ATARI
static int sound_set_bass(int bass);
#endif /* CONFIG_ATARI */
#if defined(CONFIG_ATARI) || defined(CONFIG_AMIGA)
static int sound_set_treble(int treble);
#endif /* CONFIG_ATARI || CONFIG_AMIGA */
static ssize_t sound_copy_translate(const u_char *userPtr,
				    size_t userCount,
				    u_char frame[], ssize_t *frameUsed,
				    ssize_t frameLeft);
#ifdef CONFIG_PPC
static ssize_t sound_copy_translate_read(const u_char *userPtr,
				    size_t userCount,
				    u_char frame[], ssize_t *frameUsed,
				    ssize_t frameLeft);
#endif


/*
 * /dev/mixer abstraction
 */

struct sound_mixer {
    int busy;
    int modify_counter;
};

static struct sound_mixer mixer;

/*
 * Sound queue stuff, the heart of the driver
 */

struct sound_queue {
	int max_count, block_size;
	char **buffers;
	int max_active;

	/* it shouldn't be necessary to declare any of these volatile */
	int front, rear, count;
	int rear_size;
	/*
	 *	The use of the playing field depends on the hardware
	 *
	 *	Atari, PMac: The number of frames that are loaded/playing
	 *
	 *	Amiga: Bit 0 is set: a frame is loaded
	 *	       Bit 1 is set: a frame is playing
	 */
	int active;
	wait_queue_head_t action_queue, open_queue, sync_queue;
	int open_mode;
	int busy, syncing;
#ifdef CONFIG_ATARI
	int ignore_int;		/* ++TeSche: used for Falcon */
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	int block_size_half, block_size_quarter;
#endif /* CONFIG_AMIGA */
};

static struct sound_queue sq;
#ifdef CONFIG_PPC
static struct sound_queue read_sq;
#endif

#define sq_block_address(i)	(sq.buffers[i])
#define SIGNAL_RECEIVED	(signal_pending(current))
#define NON_BLOCKING(open_mode)	(open_mode & O_NONBLOCK)
#define ONE_SECOND	HZ	/* in jiffies (100ths of a second) */
#define NO_TIME_LIMIT	0xffffffff
#define SLEEP(queue, time_limit) \
	interruptible_sleep_on_timeout(&queue, (time_limit));
#define WAKE_UP(queue)	(wake_up_interruptible(&queue))

/*
 * /dev/sndstat
 */

struct sound_state {
	int busy;
	char buf[512];
	int len, ptr;
};

static struct sound_state state;

/*** Common stuff ********************************************************/

static long long sound_lseek(struct file *file, long long offset, int orig);
static inline int ioctl_return(int *addr, int value)
{
	if (value < 0)
		return(value);

	return put_user(value, addr);
}


/*** Config & Setup **********************************************************/


void dmasound_init(void);
void dmasound_setup(char *str, int *ints);


/*** Translations ************************************************************/


/* ++TeSche: radically changed for new expanding purposes...
 *
 * These two routines now deal with copying/expanding/translating the samples
 * from user space into our buffer at the right frequency. They take care about
 * how much data there's actually to read, how much buffer space there is and
 * to convert samples into the right frequency/encoding. They will only work on
 * complete samples so it may happen they leave some bytes in the input stream
 * if the user didn't write a multiple of the current sample size. They both
 * return the number of bytes they've used from both streams so you may detect
 * such a situation. Luckily all programs should be able to cope with that.
 *
 * I think I've optimized anything as far as one can do in plain C, all
 * variables should fit in registers and the loops are really short. There's
 * one loop for every possible situation. Writing a more generalized and thus
 * parameterized loop would only produce slower code. Feel free to optimize
 * this in assembler if you like. :)
 *
 * I think these routines belong here because they're not yet really hardware
 * independent, especially the fact that the Falcon can play 16bit samples
 * only in stereo is hardcoded in both of them!
 *
 * ++geert: split in even more functions (one per format)
 */

#ifdef CONFIG_ATARI
static ssize_t ata_ct_law(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	char *table = sound.soft.format == AFMT_MU_LAW ? ulaw2dma8 : alaw2dma8;
	ssize_t count, used;
	u_char *p = &frame[*frameUsed];

	count = min(userCount, frameLeft);
	if (sound.soft.stereo)
		count &= ~1;
	used = count;
	while (count > 0) {
		u_char data;
		if (get_user(data, userPtr++))
			return -EFAULT;
		*p++ = table[data];
		count--;
	}
	*frameUsed += used;
	return used;
}


static ssize_t ata_ct_s8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft)
{
	ssize_t count, used;
	void *p = &frame[*frameUsed];

	count = min(userCount, frameLeft);
	if (sound.soft.stereo)
		count &= ~1;
	used = count;
	if (copy_from_user(p, userPtr, count))
		return -EFAULT;
	*frameUsed += used;
	return(used);
}


static ssize_t ata_ct_u8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed,
			 ssize_t frameLeft)
{
	ssize_t count, used;

	if (!sound.soft.stereo) {
		u_char *p = &frame[*frameUsed];
		count = min(userCount, frameLeft);
		used = count;
		while (count > 0) {
			u_char data;
			if (get_user(data, userPtr++))
				return -EFAULT;
			*p++ = data ^ 0x80;
			count--;
		}
	} else {
		u_short *p = (u_short *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>1;
		used = count*2;
		while (count > 0) {
			u_short data;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			*p++ = data ^ 0x8080;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ata_ct_s16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;

	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>1;
		used = count*2;
		while (count > 0) {
			u_short data;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			*p++ = data;
			*p++ = data;
			count--;
		}
		*frameUsed += used*2;
	} else {
		void *p = (u_short *)&frame[*frameUsed];
		count = min(userCount, frameLeft) & ~3;
		used = count;
		if (copy_from_user(p, userPtr, count))
			return -EFAULT;
		*frameUsed += used;
	}
	return(used);
}


static ssize_t ata_ct_u16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;

	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>1;
		used = count*2;
		while (count > 0) {
			u_short data;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data ^= 0x8000;
			*p++ = data;
			*p++ = data;
			count--;
		}
		*frameUsed += used*2;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>2;
		used = count*4;
		while (count > 0) {
			u_long data;
			if (get_user(data, ((u_int *)userPtr)++))
				return -EFAULT;
			*p++ = data ^ 0x80008000;
			count--;
		}
		*frameUsed += used;
	}
	return(used);
}


static ssize_t ata_ct_s16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;

	count = frameLeft;
	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>1;
		used = count*2;
		while (count > 0) {
			u_short data;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data);
			*p++ = data;
			*p++ = data;
			count--;
		}
		*frameUsed += used*2;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>2;
		used = count*4;
		while (count > 0) {
			u_long data;
			if (get_user(data, ((u_int *)userPtr)++))
				return -EFAULT;
			data = le2be16dbl(data);
			*p++ = data;
			count--;
		}
		*frameUsed += used;
	}
	return(used);
}


static ssize_t ata_ct_u16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;

	count = frameLeft;
	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>1;
		used = count*2;
		while (count > 0) {
			u_short data;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data) ^ 0x8000;
			*p++ = data;
			*p++ = data;
		}
		*frameUsed += used*2;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		count = min(userCount, frameLeft)>>2;
		used = count;
		while (count > 0) {
			u_long data;
			if (get_user(data, ((u_int *)userPtr)++))
				return -EFAULT;
			data = le2be16dbl(data) ^ 0x80008000;
			*p++ = data;
			count--;
		}
		*frameUsed += used;
	}
	return(used);
}


static ssize_t ata_ctx_law(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	char *table = sound.soft.format == AFMT_MU_LAW ? ulaw2dma8 : alaw2dma8;
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_char *p = &frame[*frameUsed];
		u_char data = sound.data;
		while (frameLeft) {
			u_char c;
			if (bal < 0) {
				if (!userCount)
					break;
				if (get_user(c, userPtr++))
					return -EFAULT;
				data = table[c];
				userCount--;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft--;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 2) {
			u_char c;
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(c, userPtr++))
					return -EFAULT;
				data = table[c] << 8;
				if (get_user(c, userPtr++))
					return -EFAULT;
				data |= table[c];
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 2;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}


static ssize_t ata_ctx_s8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_char *p = &frame[*frameUsed];
		u_char data = sound.data;
		while (frameLeft) {
			if (bal < 0) {
				if (!userCount)
					break;
				if (get_user(data, userPtr++))
					return -EFAULT;
				userCount--;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft--;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 2) {
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(data, ((u_short *)userPtr)++))
					return -EFAULT;
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 2;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}


static ssize_t ata_ctx_u8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_char *p = &frame[*frameUsed];
		u_char data = sound.data;
		while (frameLeft) {
			if (bal < 0) {
				if (!userCount)
					break;
				if (get_user(data, userPtr++))
					return -EFAULT;
				data ^= 0x80;
				userCount--;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft--;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 2) {
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(data, ((u_short *)userPtr)++))
					return -EFAULT;
				data ^= 0x8080;
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 2;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}


static ssize_t ata_ctx_s16be(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft)
{
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(data, ((u_short *)userPtr)++))
					return -EFAULT;
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		u_long data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 4)
					break;
				if (get_user(data, ((u_int *)userPtr)++))
					return -EFAULT;
				userCount -= 4;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}


static ssize_t ata_ctx_u16be(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft)
{
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(data, ((u_short *)userPtr)++))
					return -EFAULT;
				data ^= 0x8000;
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		u_long data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 4)
					break;
				if (get_user(data, ((u_int *)userPtr)++))
					return -EFAULT;
				data ^= 0x80008000;
				userCount -= 4;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}


static ssize_t ata_ctx_s16le(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft)
{
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(data, ((u_short *)userPtr)++))
					return -EFAULT;
				data = le2be16(data);
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		u_long data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 4)
					break;
				if (get_user(data, ((u_int *)userPtr)++))
					return -EFAULT;
				data = le2be16dbl(data);
				userCount -= 4;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}


static ssize_t ata_ctx_u16le(const u_char *userPtr, size_t userCount,
			     u_char frame[], ssize_t *frameUsed,
			     ssize_t frameLeft)
{
	/* this should help gcc to stuff everything into registers */
	long bal = sound.bal;
	long hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	ssize_t used, usedf;

	used = userCount;
	usedf = frameLeft;
	if (!sound.soft.stereo) {
		u_short *p = (u_short *)&frame[*frameUsed];
		u_short data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 2)
					break;
				if (get_user(data, ((u_short *)userPtr)++))
					return -EFAULT;
				data = le2be16(data) ^ 0x8000;
				userCount -= 2;
				bal += hSpeed;
			}
			*p++ = data;
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	} else {
		u_long *p = (u_long *)&frame[*frameUsed];
		u_long data = sound.data;
		while (frameLeft >= 4) {
			if (bal < 0) {
				if (userCount < 4)
					break;
				if (get_user(data, ((u_int *)userPtr)++))
					return -EFAULT;
				data = le2be16dbl(data) ^ 0x80008000;
				userCount -= 4;
				bal += hSpeed;
			}
			*p++ = data;
			frameLeft -= 4;
			bal -= sSpeed;
		}
		sound.data = data;
	}
	sound.bal = bal;
	used -= userCount;
	*frameUsed += usedf-frameLeft;
	return(used);
}
#endif /* CONFIG_ATARI */


#ifdef CONFIG_AMIGA
static ssize_t ami_ct_law(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	char *table = sound.soft.format == AFMT_MU_LAW ? ulaw2dma8 : alaw2dma8;
	ssize_t count, used;

	if (!sound.soft.stereo) {
		u_char *p = &frame[*frameUsed];
		count = min(userCount, frameLeft) & ~1;
		used = count;
		while (count > 0) {
			u_char data;
			if (get_user(data, userPtr++))
				return -EFAULT;
			*p++ = table[data];
			count--;
		}
	} else {
		u_char *left = &frame[*frameUsed>>1];
		u_char *right = left+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			u_char data;
			if (get_user(data, userPtr++))
				return -EFAULT;
			*left++ = table[data];
			if (get_user(data, userPtr++))
				return -EFAULT;
			*right++ = table[data];
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ami_ct_s8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, used;

	if (!sound.soft.stereo) {
		void *p = &frame[*frameUsed];
		count = min(userCount, frameLeft) & ~1;
		used = count;
		if (copy_from_user(p, userPtr, count))
			return -EFAULT;
	} else {
		u_char *left = &frame[*frameUsed>>1];
		u_char *right = left+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			if (get_user(*left++, userPtr++)
			    || get_user(*right++, userPtr++))
				return -EFAULT;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ami_ct_u8(const u_char *userPtr, size_t userCount,
			 u_char frame[], ssize_t *frameUsed, ssize_t frameLeft)
{
	ssize_t count, used;

	if (!sound.soft.stereo) {
		char *p = &frame[*frameUsed];
		count = min(userCount, frameLeft) & ~1;
		used = count;
		while (count > 0) {
			u_char data;
			if (get_user(data, userPtr++))
				return -EFAULT;
			*p++ = data ^ 0x80;
			count--;
		}
	} else {
		u_char *left = &frame[*frameUsed>>1];
		u_char *right = left+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			u_char data;
			if (get_user(data, userPtr++))
				return -EFAULT;
			*left++ = data ^ 0x80;
			if (get_user(data, userPtr++))
				return -EFAULT;
			*right++ = data ^ 0x80;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ami_ct_s16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;
	u_short data;

	if (!sound.soft.stereo) {
		u_char *high = &frame[*frameUsed>>1];
		u_char *low = high+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			*high++ = data>>8;
			*low++ = (data>>2) & 0x3f;
			count--;
		}
	} else {
		u_char *lefth = &frame[*frameUsed>>2];
		u_char *leftl = lefth+sq.block_size_quarter;
		u_char *righth = lefth+sq.block_size_half;
		u_char *rightl = righth+sq.block_size_quarter;
		count = min(userCount, frameLeft)>>2 & ~1;
		used = count*4;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			*lefth++ = data>>8;
			*leftl++ = (data>>2) & 0x3f;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			*righth++ = data>>8;
			*rightl++ = (data>>2) & 0x3f;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ami_ct_u16be(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;
	u_short data;

	if (!sound.soft.stereo) {
		u_char *high = &frame[*frameUsed>>1];
		u_char *low = high+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data ^= 0x8000;
			*high++ = data>>8;
			*low++ = (data>>2) & 0x3f;
			count--;
		}
	} else {
		u_char *lefth = &frame[*frameUsed>>2];
		u_char *leftl = lefth+sq.block_size_quarter;
		u_char *righth = lefth+sq.block_size_half;
		u_char *rightl = righth+sq.block_size_quarter;
		count = min(userCount, frameLeft)>>2 & ~1;
		used = count*4;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data ^= 0x8000;
			*lefth++ = data>>8;
			*leftl++ = (data>>2) & 0x3f;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data ^= 0x8000;
			*righth++ = data>>8;
			*rightl++ = (data>>2) & 0x3f;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ami_ct_s16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;
	u_short data;

	if (!sound.soft.stereo) {
		u_char *high = &frame[*frameUsed>>1];
		u_char *low = high+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data);
			*high++ = data>>8;
			*low++ = (data>>2) & 0x3f;
			count--;
		}
	} else {
		u_char *lefth = &frame[*frameUsed>>2];
		u_char *leftl = lefth+sq.block_size_quarter;
		u_char *righth = lefth+sq.block_size_half;
		u_char *rightl = righth+sq.block_size_quarter;
		count = min(userCount, frameLeft)>>2 & ~1;
		used = count*4;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data);
			*lefth++ = data>>8;
			*leftl++ = (data>>2) & 0x3f;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data);
			*righth++ = data>>8;
			*rightl++ = (data>>2) & 0x3f;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}


static ssize_t ami_ct_u16le(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	ssize_t count, used;
	u_short data;

	if (!sound.soft.stereo) {
		u_char *high = &frame[*frameUsed>>1];
		u_char *low = high+sq.block_size_half;
		count = min(userCount, frameLeft)>>1 & ~1;
		used = count*2;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data) ^ 0x8000;
			*high++ = data>>8;
			*low++ = (data>>2) & 0x3f;
			count--;
		}
	} else {
		u_char *lefth = &frame[*frameUsed>>2];
		u_char *leftl = lefth+sq.block_size_quarter;
		u_char *righth = lefth+sq.block_size_half;
		u_char *rightl = righth+sq.block_size_quarter;
		count = min(userCount, frameLeft)>>2 & ~1;
		used = count*4;
		while (count > 0) {
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data) ^ 0x8000;
			*lefth++ = data>>8;
			*leftl++ = (data>>2) & 0x3f;
			if (get_user(data, ((u_short *)userPtr)++))
				return -EFAULT;
			data = le2be16(data) ^ 0x8000;
			*righth++ = data>>8;
			*rightl++ = (data>>2) & 0x3f;
			count--;
		}
	}
	*frameUsed += used;
	return(used);
}
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
static ssize_t pmac_ct_law(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	short *table = sound.soft.format == AFMT_MU_LAW ? ulaw2dma16: alaw2dma16;
	ssize_t count, used;
	short *p = (short *) &frame[*frameUsed];
	int val, stereo = sound.soft.stereo;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		u_char data;
		if (get_user(data, userPtr++))
			return -EFAULT;
		val = table[data];
		*p++ = val;
		if (stereo) {
			if (get_user(data, userPtr++))
				return -EFAULT;
			val = table[data];
		}
		*p++ = val;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 2: used;
}


static ssize_t pmac_ct_s8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	ssize_t count, used;
	short *p = (short *) &frame[*frameUsed];
	int val, stereo = sound.soft.stereo;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		u_char data;
		if (get_user(data, userPtr++))
			return -EFAULT;
		val = data << 8;
		*p++ = val;
		if (stereo) {
			if (get_user(data, userPtr++))
				return -EFAULT;
			val = data << 8;
		}
		*p++ = val;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 2: used;
}


static ssize_t pmac_ct_u8(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	ssize_t count, used;
	short *p = (short *) &frame[*frameUsed];
	int val, stereo = sound.soft.stereo;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		u_char data;
		if (get_user(data, userPtr++))
			return -EFAULT;
		val = (data ^ 0x80) << 8;
		*p++ = val;
		if (stereo) {
			if (get_user(data, userPtr++))
				return -EFAULT;
			val = (data ^ 0x80) << 8;
		}
		*p++ = val;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 2: used;
}


static ssize_t pmac_ct_s16(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	ssize_t count, used;
	int stereo = sound.soft.stereo;
	short *fp = (short *) &frame[*frameUsed];

	frameLeft >>= 2;
	userCount >>= (stereo? 2: 1);
	used = count = min(userCount, frameLeft);
	if (!stereo) {
		short *up = (short *) userPtr;
		while (count > 0) {
			short data;
			if (get_user(data, up++))
				return -EFAULT;
			*fp++ = data;
			*fp++ = data;
			count--;
		}
	} else {
		if (copy_from_user(fp, userPtr, count * 4))
			return -EFAULT;
	}
	*frameUsed += used * 4;
	return stereo? used * 4: used * 2;
}

static ssize_t pmac_ct_u16(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	ssize_t count, used;
	int mask = (sound.soft.format == AFMT_U16_LE? 0x0080: 0x8000);
	int stereo = sound.soft.stereo;
	short *fp = (short *) &frame[*frameUsed];
	short *up = (short *) userPtr;

	frameLeft >>= 2;
	userCount >>= (stereo? 2: 1);
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		int data;
		if (get_user(data, up++))
			return -EFAULT;
		data ^= mask;
		*fp++ = data;
		if (stereo) {
			if (get_user(data, up++))
				return -EFAULT;
			data ^= mask;
		}
		*fp++ = data;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 4: used * 2;
}


static ssize_t pmac_ctx_law(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	unsigned short *table = (unsigned short *)
		(sound.soft.format == AFMT_MU_LAW ? ulaw2dma16: alaw2dma16);
	unsigned int data = sound.data;
	unsigned int *p = (unsigned int *) &frame[*frameUsed];
	int bal = sound.bal;
	int hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	int utotal, ftotal;
	int stereo = sound.soft.stereo;
 
	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	ftotal = frameLeft;
	utotal = userCount;
	while (frameLeft) {
		u_char c;
		if (bal < 0) {
			if (userCount == 0)
				break;
			if (get_user(c, userPtr++))
				return -EFAULT;
			data = table[c];
			if (stereo) {
				if (get_user(c, userPtr++))
					return -EFAULT;
				data = (data << 16) + table[c];
			} else
				data = (data << 16) + data;
			userCount--;
			bal += hSpeed;
		}
		*p++ = data;
		frameLeft--;
		bal -= sSpeed;
	}
	sound.bal = bal;
	sound.data = data;
	*frameUsed += (ftotal - frameLeft) * 4;
	utotal -= userCount;
	return stereo? utotal * 2: utotal;
}


static ssize_t pmac_ctx_s8(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	unsigned int *p = (unsigned int *) &frame[*frameUsed];
	unsigned int data = sound.data;
	int bal = sound.bal;
	int hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	int stereo = sound.soft.stereo;
	int utotal, ftotal;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	ftotal = frameLeft;
	utotal = userCount;
	while (frameLeft) {
		u_char c;
		if (bal < 0) {
			if (userCount == 0)
				break;
			if (get_user(c, userPtr++))
				return -EFAULT;
			data = c << 8;
			if (stereo) {
				if (get_user(c, userPtr++))
					return -EFAULT;
				data = (data << 16) + (c << 8);
			} else
				data = (data << 16) + data;
			userCount--;
			bal += hSpeed;
		}
		*p++ = data;
		frameLeft--;
		bal -= sSpeed;
	}
	sound.bal = bal;
	sound.data = data;
	*frameUsed += (ftotal - frameLeft) * 4;
	utotal -= userCount;
	return stereo? utotal * 2: utotal;
}


static ssize_t pmac_ctx_u8(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	unsigned int *p = (unsigned int *) &frame[*frameUsed];
	unsigned int data = sound.data;
	int bal = sound.bal;
	int hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	int stereo = sound.soft.stereo;
	int utotal, ftotal;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	ftotal = frameLeft;
	utotal = userCount;
	while (frameLeft) {
		u_char c;
		if (bal < 0) {
			if (userCount == 0)
				break;
			if (get_user(c, userPtr++))
				return -EFAULT;
			data = (c ^ 0x80) << 8;
			if (stereo) {
				if (get_user(c, userPtr++))
					return -EFAULT;
				data = (data << 16) + ((c ^ 0x80) << 8);
			} else
				data = (data << 16) + data;
			userCount--;
			bal += hSpeed;
		}
		*p++ = data;
		frameLeft--;
		bal -= sSpeed;
	}
	sound.bal = bal;
	sound.data = data;
	*frameUsed += (ftotal - frameLeft) * 4;
	utotal -= userCount;
	return stereo? utotal * 2: utotal;
}


static ssize_t pmac_ctx_s16(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	unsigned int *p = (unsigned int *) &frame[*frameUsed];
	unsigned int data = sound.data;
	unsigned short *up = (unsigned short *) userPtr;
	int bal = sound.bal;
	int hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	int stereo = sound.soft.stereo;
	int utotal, ftotal;

	frameLeft >>= 2;
	userCount >>= (stereo? 2: 1);
	ftotal = frameLeft;
	utotal = userCount;
	while (frameLeft) {
		unsigned short c;
		if (bal < 0) {
			if (userCount == 0)
				break;
			if (get_user(data, up++))
				return -EFAULT;
			if (stereo) {
				if (get_user(c, up++))
					return -EFAULT;
				data = (data << 16) + c;
			} else
				data = (data << 16) + data;
			userCount--;
			bal += hSpeed;
		}
		*p++ = data;
		frameLeft--;
		bal -= sSpeed;
	}
	sound.bal = bal;
	sound.data = data;
	*frameUsed += (ftotal - frameLeft) * 4;
	utotal -= userCount;
	return stereo? utotal * 4: utotal * 2;
}


static ssize_t pmac_ctx_u16(const u_char *userPtr, size_t userCount,
			    u_char frame[], ssize_t *frameUsed,
			    ssize_t frameLeft)
{
	int mask = (sound.soft.format == AFMT_U16_LE? 0x0080: 0x8000);
	unsigned int *p = (unsigned int *) &frame[*frameUsed];
	unsigned int data = sound.data;
	unsigned short *up = (unsigned short *) userPtr;
	int bal = sound.bal;
	int hSpeed = sound.hard.speed, sSpeed = sound.soft.speed;
	int stereo = sound.soft.stereo;
	int utotal, ftotal;

	frameLeft >>= 2;
	userCount >>= (stereo? 2: 1);
	ftotal = frameLeft;
	utotal = userCount;
	while (frameLeft) {
		unsigned short c;
		if (bal < 0) {
			if (userCount == 0)
				break;
			if (get_user(data, up++))
				return -EFAULT;
			data ^= mask;
			if (stereo) {
				if (get_user(c, up++))
					return -EFAULT;
				data = (data << 16) + (c ^ mask);
			} else
				data = (data << 16) + data;
			userCount--;
			bal += hSpeed;
		}
		*p++ = data;
		frameLeft--;
		bal -= sSpeed;
	}
	sound.bal = bal;
	sound.data = data;
	*frameUsed += (ftotal - frameLeft) * 4;
	utotal -= userCount;
	return stereo? utotal * 4: utotal * 2;
}

static ssize_t pmac_ct_s8_read(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	ssize_t count, used;
	short *p = (short *) &frame[*frameUsed];
	int val, stereo = sound.soft.stereo;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		u_char data;

		val = *p++;
		data = val >> 8;
		if (put_user(data, (u_char *)userPtr++))
			return -EFAULT;
		if (stereo) {
			val = *p;
			data = val >> 8;
			if (put_user(data, (u_char *)userPtr++))
				return -EFAULT;
		}
		p++;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 2: used;
}


static ssize_t pmac_ct_u8_read(const u_char *userPtr, size_t userCount,
			  u_char frame[], ssize_t *frameUsed,
			  ssize_t frameLeft)
{
	ssize_t count, used;
	short *p = (short *) &frame[*frameUsed];
	int val, stereo = sound.soft.stereo;

	frameLeft >>= 2;
	if (stereo)
		userCount >>= 1;
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		u_char data;

		val = *p++;
		data = (val >> 8) ^ 0x80;
		if (put_user(data, (u_char *)userPtr++))
			return -EFAULT;
		if (stereo) {
			val = *p;
			data = (val >> 8) ^ 0x80;
			if (put_user(data, (u_char *)userPtr++))
				return -EFAULT;
		}
		p++;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 2: used;
}


static ssize_t pmac_ct_s16_read(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	ssize_t count, used;
	int stereo = sound.soft.stereo;
	short *fp = (short *) &frame[*frameUsed];

	frameLeft >>= 2;
	userCount >>= (stereo? 2: 1);
	used = count = min(userCount, frameLeft);
	if (!stereo) {
		short *up = (short *) userPtr;
		while (count > 0) {
			short data;
			data = *fp;
			if (put_user(data, up++))
				return -EFAULT;
			fp+=2;
			count--;
		}
	} else {
		if (copy_to_user((u_char *)userPtr, fp, count * 4))
			return -EFAULT;
	}
	*frameUsed += used * 4;
	return stereo? used * 4: used * 2;
}

static ssize_t pmac_ct_u16_read(const u_char *userPtr, size_t userCount,
			   u_char frame[], ssize_t *frameUsed,
			   ssize_t frameLeft)
{
	ssize_t count, used;
	int mask = (sound.soft.format == AFMT_U16_LE? 0x0080: 0x8000);
	int stereo = sound.soft.stereo;
	short *fp = (short *) &frame[*frameUsed];
	short *up = (short *) userPtr;

	frameLeft >>= 2;
	userCount >>= (stereo? 2: 1);
	used = count = min(userCount, frameLeft);
	while (count > 0) {
		int data;

		data = *fp++;
		data ^= mask;
		if (put_user(data, up++))
			return -EFAULT;
		if (stereo) {
			data = *fp;
			data ^= mask;
			if (put_user(data, up++))
				return -EFAULT;
		}
		fp++;
		count--;
	}
	*frameUsed += used * 4;
	return stereo? used * 4: used * 2;
}


#endif /* CONFIG_PPC */


#ifdef CONFIG_ATARI
static TRANS transTTNormal = {
	ata_ct_law, ata_ct_law, ata_ct_s8, ata_ct_u8, NULL, NULL, NULL, NULL
};

static TRANS transTTExpanding = {
	ata_ctx_law, ata_ctx_law, ata_ctx_s8, ata_ctx_u8, NULL, NULL, NULL, NULL
};

static TRANS transFalconNormal = {
	ata_ct_law, ata_ct_law, ata_ct_s8, ata_ct_u8,
	ata_ct_s16be, ata_ct_u16be, ata_ct_s16le, ata_ct_u16le
};

static TRANS transFalconExpanding = {
	ata_ctx_law, ata_ctx_law, ata_ctx_s8, ata_ctx_u8,
	ata_ctx_s16be, ata_ctx_u16be, ata_ctx_s16le, ata_ctx_u16le
};
#endif /* CONFIG_ATARI */

#ifdef CONFIG_AMIGA
static TRANS transAmiga = {
	ami_ct_law, ami_ct_law, ami_ct_s8, ami_ct_u8,
	ami_ct_s16be, ami_ct_u16be, ami_ct_s16le, ami_ct_u16le
};
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
static TRANS transAwacsNormal = {
	pmac_ct_law, pmac_ct_law, pmac_ct_s8, pmac_ct_u8,
	pmac_ct_s16, pmac_ct_u16, pmac_ct_s16, pmac_ct_u16
};

static TRANS transAwacsExpand = {
	pmac_ctx_law, pmac_ctx_law, pmac_ctx_s8, pmac_ctx_u8,
	pmac_ctx_s16, pmac_ctx_u16, pmac_ctx_s16, pmac_ctx_u16
};

static TRANS transAwacsNormalRead = {
	NULL, NULL, pmac_ct_s8_read, pmac_ct_u8_read,
	pmac_ct_s16_read, pmac_ct_u16_read, pmac_ct_s16_read, pmac_ct_u16_read
};
#endif /* CONFIG_PPC */

/*** Low level stuff *********************************************************/


#ifdef CONFIG_ATARI

/*
 * Atari (TT/Falcon)
 */

static void *AtaAlloc(unsigned int size, int flags)
{
	return( atari_stram_alloc( size, NULL, "dmasound" ));
}

static void AtaFree(void *obj, unsigned int size)
{
	atari_stram_free( obj );
}

static int __init AtaIrqInit(void)
{
	/* Set up timer A. Timer A
	   will receive a signal upon end of playing from the sound
	   hardware. Furthermore Timer A is able to count events
	   and will cause an interrupt after a programmed number
	   of events. So all we need to keep the music playing is
	   to provide the sound hardware with new data upon
	   an interrupt from timer A. */
	mfp.tim_ct_a = 0;	/* ++roman: Stop timer before programming! */
	mfp.tim_dt_a = 1;	/* Cause interrupt after first event. */
	mfp.tim_ct_a = 8;	/* Turn on event counting. */
	/* Register interrupt handler. */
	request_irq(IRQ_MFP_TIMA, ata_sq_interrupt, IRQ_TYPE_SLOW,
		    "DMA sound", ata_sq_interrupt);
	mfp.int_en_a |= 0x20;	/* Turn interrupt on. */
	mfp.int_mk_a |= 0x20;
	return(1);
}

#ifdef MODULE
static void AtaIrqCleanUp(void)
{
	mfp.tim_ct_a = 0;	/* stop timer */
	mfp.int_en_a &= ~0x20;	/* turn interrupt off */
	free_irq(IRQ_MFP_TIMA, ata_sq_interrupt);
}
#endif /* MODULE */


#define TONE_VOXWARE_TO_DB(v) \
	(((v) < 0) ? -12 : ((v) > 100) ? 12 : ((v) - 50) * 6 / 25)
#define TONE_DB_TO_VOXWARE(v) (((v) * 25 + ((v) > 0 ? 5 : -5)) / 6 + 50)


static int AtaSetBass(int bass)
{
	sound.bass = TONE_VOXWARE_TO_DB(bass);
	atari_microwire_cmd(MW_LM1992_BASS(sound.bass));
	return(TONE_DB_TO_VOXWARE(sound.bass));
}


static int AtaSetTreble(int treble)
{
	sound.treble = TONE_VOXWARE_TO_DB(treble);
	atari_microwire_cmd(MW_LM1992_TREBLE(sound.treble));
	return(TONE_DB_TO_VOXWARE(sound.treble));
}



/*
 * TT
 */


static void TTSilence(void)
{
	tt_dmasnd.ctrl = DMASND_CTRL_OFF;
	atari_microwire_cmd(MW_LM1992_PSG_HIGH); /* mix in PSG signal 1:1 */
}


static void TTInit(void)
{
	int mode, i, idx;
	const int freq[4] = {50066, 25033, 12517, 6258};

	/* search a frequency that fits into the allowed error range */

	idx = -1;
	for (i = 0; i < arraysize(freq); i++)
		/* this isn't as much useful for a TT than for a Falcon, but
		 * then it doesn't hurt very much to implement it for a TT too.
		 */
		if ((100 * abs(sound.soft.speed - freq[i]) / freq[i]) < catchRadius)
			idx = i;
	if (idx > -1) {
		sound.soft.speed = freq[idx];
		sound.trans = &transTTNormal;
	} else
		sound.trans = &transTTExpanding;

	TTSilence();
	sound.hard = sound.soft;

	if (sound.hard.speed > 50066) {
		/* we would need to squeeze the sound, but we won't do that */
		sound.hard.speed = 50066;
		mode = DMASND_MODE_50KHZ;
		sound.trans = &transTTNormal;
	} else if (sound.hard.speed > 25033) {
		sound.hard.speed = 50066;
		mode = DMASND_MODE_50KHZ;
	} else if (sound.hard.speed > 12517) {
		sound.hard.speed = 25033;
		mode = DMASND_MODE_25KHZ;
	} else if (sound.hard.speed > 6258) {
		sound.hard.speed = 12517;
		mode = DMASND_MODE_12KHZ;
	} else {
		sound.hard.speed = 6258;
		mode = DMASND_MODE_6KHZ;
	}

	tt_dmasnd.mode = (sound.hard.stereo ?
			  DMASND_MODE_STEREO : DMASND_MODE_MONO) |
		DMASND_MODE_8BIT | mode;

	sound.bal = -sound.soft.speed;
}


static int TTSetFormat(int format)
{
	/* TT sound DMA supports only 8bit modes */

	switch (format) {
	case AFMT_QUERY:
		return(sound.soft.format);
	case AFMT_MU_LAW:
	case AFMT_A_LAW:
	case AFMT_S8:
	case AFMT_U8:
		break;
	default:
		format = AFMT_S8;
	}

	sound.soft.format = format;
	sound.soft.size = 8;
	if (sound.minDev == SND_DEV_DSP) {
		sound.dsp.format = format;
		sound.dsp.size = 8;
	}
	TTInit();

	return(format);
}


#define VOLUME_VOXWARE_TO_DB(v) \
	(((v) < 0) ? -40 : ((v) > 100) ? 0 : ((v) * 2) / 5 - 40)
#define VOLUME_DB_TO_VOXWARE(v) ((((v) + 40) * 5 + 1) / 2)


static int TTSetVolume(int volume)
{
	sound.volume_left = VOLUME_VOXWARE_TO_DB(volume & 0xff);
	atari_microwire_cmd(MW_LM1992_BALLEFT(sound.volume_left));
	sound.volume_right = VOLUME_VOXWARE_TO_DB((volume & 0xff00) >> 8);
	atari_microwire_cmd(MW_LM1992_BALRIGHT(sound.volume_right));
	return(VOLUME_DB_TO_VOXWARE(sound.volume_left) |
	       (VOLUME_DB_TO_VOXWARE(sound.volume_right) << 8));
}


#define GAIN_VOXWARE_TO_DB(v) \
	(((v) < 0) ? -80 : ((v) > 100) ? 0 : ((v) * 4) / 5 - 80)
#define GAIN_DB_TO_VOXWARE(v) ((((v) + 80) * 5 + 1) / 4)

static int TTSetGain(int gain)
{
	sound.gain = GAIN_VOXWARE_TO_DB(gain);
	atari_microwire_cmd(MW_LM1992_VOLUME(sound.gain));
	return GAIN_DB_TO_VOXWARE(sound.gain);
}



/*
 * Falcon
 */


static void FalconSilence(void)
{
	/* stop playback, set sample rate 50kHz for PSG sound */
	tt_dmasnd.ctrl = DMASND_CTRL_OFF;
	tt_dmasnd.mode = DMASND_MODE_50KHZ | DMASND_MODE_STEREO | DMASND_MODE_8BIT;
	tt_dmasnd.int_div = 0; /* STE compatible divider */
	tt_dmasnd.int_ctrl = 0x0;
	tt_dmasnd.cbar_src = 0x0000; /* no matrix inputs */
	tt_dmasnd.cbar_dst = 0x0000; /* no matrix outputs */
	tt_dmasnd.dac_src = 1; /* connect ADC to DAC, disconnect matrix */
	tt_dmasnd.adc_src = 3; /* ADC Input = PSG */
}


static void FalconInit(void)
{
	int divider, i, idx;
	const int freq[8] = {49170, 32780, 24585, 19668, 16390, 12292, 9834, 8195};

	/* search a frequency that fits into the allowed error range */

	idx = -1;
	for (i = 0; i < arraysize(freq); i++)
		/* if we will tolerate 3% error 8000Hz->8195Hz (2.38%) would
		 * be playable without expanding, but that now a kernel runtime
		 * option
		 */
		if ((100 * abs(sound.soft.speed - freq[i]) / freq[i]) < catchRadius)
			idx = i;
	if (idx > -1) {
		sound.soft.speed = freq[idx];
		sound.trans = &transFalconNormal;
	} else
		sound.trans = &transFalconExpanding;

	FalconSilence();
	sound.hard = sound.soft;

	if (sound.hard.size == 16) {
		/* the Falcon can play 16bit samples only in stereo */
		sound.hard.stereo = 1;
	}

	if (sound.hard.speed > 49170) {
		/* we would need to squeeze the sound, but we won't do that */
		sound.hard.speed = 49170;
		divider = 1;
		sound.trans = &transFalconNormal;
	} else if (sound.hard.speed > 32780) {
		sound.hard.speed = 49170;
		divider = 1;
	} else if (sound.hard.speed > 24585) {
		sound.hard.speed = 32780;
		divider = 2;
	} else if (sound.hard.speed > 19668) {
		sound.hard.speed = 24585;
		divider = 3;
	} else if (sound.hard.speed > 16390) {
		sound.hard.speed = 19668;
		divider = 4;
	} else if (sound.hard.speed > 12292) {
		sound.hard.speed = 16390;
		divider = 5;
	} else if (sound.hard.speed > 9834) {
		sound.hard.speed = 12292;
		divider = 7;
	} else if (sound.hard.speed > 8195) {
		sound.hard.speed = 9834;
		divider = 9;
	} else {
		sound.hard.speed = 8195;
		divider = 11;
	}
	tt_dmasnd.int_div = divider;

	/* Setup Falcon sound DMA for playback */
	tt_dmasnd.int_ctrl = 0x4; /* Timer A int at play end */
	tt_dmasnd.track_select = 0x0; /* play 1 track, track 1 */
	tt_dmasnd.cbar_src = 0x0001; /* DMA(25MHz) --> DAC */
	tt_dmasnd.cbar_dst = 0x0000;
	tt_dmasnd.rec_track_select = 0;
	tt_dmasnd.dac_src = 2; /* connect matrix to DAC */
	tt_dmasnd.adc_src = 0; /* ADC Input = Mic */

	tt_dmasnd.mode = (sound.hard.stereo ?
			  DMASND_MODE_STEREO : DMASND_MODE_MONO) |
		((sound.hard.size == 8) ?
		 DMASND_MODE_8BIT : DMASND_MODE_16BIT) |
		DMASND_MODE_6KHZ;

	sound.bal = -sound.soft.speed;
}


static int FalconSetFormat(int format)
{
	int size;
	/* Falcon sound DMA supports 8bit and 16bit modes */

	switch (format) {
	case AFMT_QUERY:
		return(sound.soft.format);
	case AFMT_MU_LAW:
	case AFMT_A_LAW:
	case AFMT_U8:
	case AFMT_S8:
		size = 8;
		break;
	case AFMT_S16_BE:
	case AFMT_U16_BE:
	case AFMT_S16_LE:
	case AFMT_U16_LE:
		size = 16;
		break;
	default: /* :-) */
		size = 8;
		format = AFMT_S8;
	}

	sound.soft.format = format;
	sound.soft.size = size;
	if (sound.minDev == SND_DEV_DSP) {
		sound.dsp.format = format;
		sound.dsp.size = sound.soft.size;
	}

	FalconInit();

	return(format);
}


/* This is for the Falcon output *attenuation* in 1.5dB steps,
 * i.e. output level from 0 to -22.5dB in -1.5dB steps.
 */
#define VOLUME_VOXWARE_TO_ATT(v) \
	((v) < 0 ? 15 : (v) > 100 ? 0 : 15 - (v) * 3 / 20)
#define VOLUME_ATT_TO_VOXWARE(v) (100 - (v) * 20 / 3)


static int FalconSetVolume(int volume)
{
	sound.volume_left = VOLUME_VOXWARE_TO_ATT(volume & 0xff);
	sound.volume_right = VOLUME_VOXWARE_TO_ATT((volume & 0xff00) >> 8);
	tt_dmasnd.output_atten = sound.volume_left << 8 | sound.volume_right << 4;
	return(VOLUME_ATT_TO_VOXWARE(sound.volume_left) |
	       VOLUME_ATT_TO_VOXWARE(sound.volume_right) << 8);
}


static void ata_sq_play_next_frame(int index)
{
	char *start, *end;

	/* used by AtaPlay() if all doubts whether there really is something
	 * to be played are already wiped out.
	 */
	start = sq_block_address(sq.front);
	end = start+((sq.count == index) ? sq.rear_size : sq.block_size);
	/* end might not be a legal virtual address. */
	DMASNDSetEnd(virt_to_phys(end - 1) + 1);
	DMASNDSetBase(virt_to_phys(start));
	/* Since only an even number of samples per frame can
	   be played, we might lose one byte here. (TO DO) */
	sq.front = (sq.front+1) % sq.max_count;
	sq.active++;
	tt_dmasnd.ctrl = DMASND_CTRL_ON | DMASND_CTRL_REPEAT;
}


static void AtaPlay(void)
{
	/* ++TeSche: Note that sq.active is no longer just a flag but holds
	 * the number of frames the DMA is currently programmed for instead,
	 * may be 0, 1 (currently being played) or 2 (pre-programmed).
	 *
	 * Changes done to sq.count and sq.active are a bit more subtle again
	 * so now I must admit I also prefer disabling the irq here rather
	 * than considering all possible situations. But the point is that
	 * disabling the irq doesn't have any bad influence on this version of
	 * the driver as we benefit from having pre-programmed the DMA
	 * wherever possible: There's no need to reload the DMA at the exact
	 * time of an interrupt but only at some time while the pre-programmed
	 * frame is playing!
	 */
	atari_disable_irq(IRQ_MFP_TIMA);

	if (sq.active == 2 ||	/* DMA is 'full' */
	    sq.count <= 0) {	/* nothing to do */
		atari_enable_irq(IRQ_MFP_TIMA);
		return;
	}

	if (sq.active == 0) {
		/* looks like there's nothing 'in' the DMA yet, so try
		 * to put two frames into it (at least one is available).
		 */
		if (sq.count == 1 && sq.rear_size < sq.block_size && !sq.syncing) {
			/* hmmm, the only existing frame is not
			 * yet filled and we're not syncing?
			 */
			atari_enable_irq(IRQ_MFP_TIMA);
			return;
		}
		ata_sq_play_next_frame(1);
		if (sq.count == 1) {
			/* no more frames */
			atari_enable_irq(IRQ_MFP_TIMA);
			return;
		}
		if (sq.count == 2 && sq.rear_size < sq.block_size && !sq.syncing) {
			/* hmmm, there were two frames, but the second
			 * one is not yet filled and we're not syncing?
			 */
			atari_enable_irq(IRQ_MFP_TIMA);
			return;
		}
		ata_sq_play_next_frame(2);
	} else {
		/* there's already a frame being played so we may only stuff
		 * one new into the DMA, but even if this may be the last
		 * frame existing the previous one is still on sq.count.
		 */
		if (sq.count == 2 && sq.rear_size < sq.block_size && !sq.syncing) {
			/* hmmm, the only existing frame is not
			 * yet filled and we're not syncing?
			 */
			atari_enable_irq(IRQ_MFP_TIMA);
			return;
		}
		ata_sq_play_next_frame(2);
	}
	atari_enable_irq(IRQ_MFP_TIMA);
}


static void ata_sq_interrupt(int irq, void *dummy, struct pt_regs *fp)
{
#if 0
	/* ++TeSche: if you should want to test this... */
	static int cnt = 0;
	if (sq.active == 2)
		if (++cnt == 10) {
			/* simulate losing an interrupt */
			cnt = 0;
			return;
		}
#endif

	if (sq.ignore_int && (sound.mach.type == DMASND_FALCON)) {
		/* ++TeSche: Falcon only: ignore first irq because it comes
		 * immediately after starting a frame. after that, irqs come
		 * (almost) like on the TT.
		 */
		sq.ignore_int = 0;
		return;
	}

	if (!sq.active) {
		/* playing was interrupted and sq_reset() has already cleared
		 * the sq variables, so better don't do anything here.
		 */
		WAKE_UP(sq.sync_queue);
		return;
	}

	/* Probably ;) one frame is finished. Well, in fact it may be that a
	 * pre-programmed one is also finished because there has been a long
	 * delay in interrupt delivery and we've completely lost one, but
	 * there's no way to detect such a situation. In such a case the last
	 * frame will be played more than once and the situation will recover
	 * as soon as the irq gets through.
	 */
	sq.count--;
	sq.active--;

	if (!sq.active) {
		tt_dmasnd.ctrl = DMASND_CTRL_OFF;
		sq.ignore_int = 1;
	}

	WAKE_UP(sq.action_queue);
	/* At least one block of the queue is free now
	   so wake up a writing process blocked because
	   of a full queue. */

	if ((sq.active != 1) || (sq.count != 1))
		/* We must be a bit carefully here: sq.count indicates the
		 * number of buffers used and not the number of frames to
		 * be played. If sq.count==1 and sq.active==1 that means
		 * the only remaining frame was already programmed earlier
		 * (and is currently running) so we mustn't call AtaPlay()
		 * here, otherwise we'll play one frame too much.
		 */
		AtaPlay();

	if (!sq.active) WAKE_UP(sq.sync_queue);
	/* We are not playing after AtaPlay(), so there
	   is nothing to play any more. Wake up a process
	   waiting for audio output to drain. */
}
#endif /* CONFIG_ATARI */


#ifdef CONFIG_AMIGA

/*
 * Amiga
 */


static void *AmiAlloc(unsigned int size, int flags)
{
	return(amiga_chip_alloc((long)size));
}

static void AmiFree(void *obj, unsigned int size)
{
	amiga_chip_free (obj);
}

static int __init AmiIrqInit(void)
{
	/* turn off DMA for audio channels */
	custom.dmacon = AMI_AUDIO_OFF;

	/* Register interrupt handler. */
	if (request_irq(IRQ_AMIGA_AUD0, ami_sq_interrupt, 0,
			"DMA sound", ami_sq_interrupt))
		return(0);
	return(1);
}

#ifdef MODULE
static void AmiIrqCleanUp(void)
{
	/* turn off DMA for audio channels */
	custom.dmacon = AMI_AUDIO_OFF;
	/* release the interrupt */
	free_irq(IRQ_AMIGA_AUD0, ami_sq_interrupt);
}
#endif /* MODULE */

static void AmiSilence(void)
{
	/* turn off DMA for audio channels */
	custom.dmacon = AMI_AUDIO_OFF;
}


static void AmiInit(void)
{
	int period, i;

	AmiSilence();

	if (sound.soft.speed)
		period = amiga_colorclock/sound.soft.speed-1;
	else
		period = amiga_audio_min_period;
	sound.hard = sound.soft;
	sound.trans = &transAmiga;

	if (period < amiga_audio_min_period) {
		/* we would need to squeeze the sound, but we won't do that */
		period = amiga_audio_min_period;
	} else if (period > 65535) {
		period = 65535;
	}
	sound.hard.speed = amiga_colorclock/(period+1);

	for (i = 0; i < 4; i++)
		custom.aud[i].audper = period;
	amiga_audio_period = period;

	AmiSetTreble(50);  /* recommended for newer amiga models */
}


static int AmiSetFormat(int format)
{
	int size;

	/* Amiga sound DMA supports 8bit and 16bit (pseudo 14 bit) modes */

	switch (format) {
	case AFMT_QUERY:
		return(sound.soft.format);
	case AFMT_MU_LAW:
	case AFMT_A_LAW:
	case AFMT_U8:
	case AFMT_S8:
		size = 8;
		break;
	case AFMT_S16_BE:
	case AFMT_U16_BE:
	case AFMT_S16_LE:
	case AFMT_U16_LE:
		size = 16;
		break;
	default: /* :-) */
		size = 8;
		format = AFMT_S8;
	}

	sound.soft.format = format;
	sound.soft.size = size;
	if (sound.minDev == SND_DEV_DSP) {
		sound.dsp.format = format;
		sound.dsp.size = sound.soft.size;
	}
	AmiInit();

	return(format);
}


#define VOLUME_VOXWARE_TO_AMI(v) \
	(((v) < 0) ? 0 : ((v) > 100) ? 64 : ((v) * 64)/100)
#define VOLUME_AMI_TO_VOXWARE(v) ((v)*100/64)

static int AmiSetVolume(int volume)
{
	sound.volume_left = VOLUME_VOXWARE_TO_AMI(volume & 0xff);
	custom.aud[0].audvol = sound.volume_left;
	sound.volume_right = VOLUME_VOXWARE_TO_AMI((volume & 0xff00) >> 8);
	custom.aud[1].audvol = sound.volume_right;
	return(VOLUME_AMI_TO_VOXWARE(sound.volume_left) |
	       (VOLUME_AMI_TO_VOXWARE(sound.volume_right) << 8));
}

static int AmiSetTreble(int treble)
{
	sound.treble = treble;
	if (treble < 50)
		ciaa.pra &= ~0x02;
	else
		ciaa.pra |= 0x02;
	return(treble);
}


#define AMI_PLAY_LOADED		1
#define AMI_PLAY_PLAYING	2
#define AMI_PLAY_MASK		3


static void ami_sq_play_next_frame(int index)
{
	u_char *start, *ch0, *ch1, *ch2, *ch3;
	u_long size;

	/* used by AmiPlay() if all doubts whether there really is something
	 * to be played are already wiped out.
	 */
	start = sq_block_address(sq.front);
	size = (sq.count == index ? sq.rear_size : sq.block_size)>>1;

	if (sound.hard.stereo) {
		ch0 = start;
		ch1 = start+sq.block_size_half;
		size >>= 1;
	} else {
		ch0 = start;
		ch1 = start;
	}
	if (sound.hard.size == 8) {
		custom.aud[0].audlc = (u_short *)ZTWO_PADDR(ch0);
		custom.aud[0].audlen = size;
		custom.aud[1].audlc = (u_short *)ZTWO_PADDR(ch1);
		custom.aud[1].audlen = size;
		custom.dmacon = AMI_AUDIO_8;
	} else {
		size >>= 1;
		custom.aud[0].audlc = (u_short *)ZTWO_PADDR(ch0);
		custom.aud[0].audlen = size;
		custom.aud[1].audlc = (u_short *)ZTWO_PADDR(ch1);
		custom.aud[1].audlen = size;
		if (sound.volume_left == 64 && sound.volume_right == 64) {
			/* We can play pseudo 14-bit only with the maximum volume */
			ch3 = ch0+sq.block_size_quarter;
			ch2 = ch1+sq.block_size_quarter;
			custom.aud[2].audvol = 1;  /* we are being affected by the beeps */
			custom.aud[3].audvol = 1;  /* restoring volume here helps a bit */
			custom.aud[2].audlc = (u_short *)ZTWO_PADDR(ch2);
			custom.aud[2].audlen = size;
			custom.aud[3].audlc = (u_short *)ZTWO_PADDR(ch3);
			custom.aud[3].audlen = size;
			custom.dmacon = AMI_AUDIO_14;
		} else
			custom.dmacon = AMI_AUDIO_8;
	}
	sq.front = (sq.front+1) % sq.max_count;
	sq.active |= AMI_PLAY_LOADED;
}


static void AmiPlay(void)
{
	int minframes = 1;

	custom.intena = IF_AUD0;

	if (sq.active & AMI_PLAY_LOADED) {
		/* There's already a frame loaded */
		custom.intena = IF_SETCLR | IF_AUD0;
		return;
	}

	if (sq.active & AMI_PLAY_PLAYING)
		/* Increase threshold: frame 1 is already being played */
		minframes = 2;

	if (sq.count < minframes) {
		/* Nothing to do */
		custom.intena = IF_SETCLR | IF_AUD0;
		return;
	}

	if (sq.count <= minframes && sq.rear_size < sq.block_size && !sq.syncing) {
		/* hmmm, the only existing frame is not
		 * yet filled and we're not syncing?
		 */
		custom.intena = IF_SETCLR | IF_AUD0;
		return;
	}

	ami_sq_play_next_frame(minframes);

	custom.intena = IF_SETCLR | IF_AUD0;
}


static void ami_sq_interrupt(int irq, void *dummy, struct pt_regs *fp)
{
	int minframes = 1;

	if (!sq.active) {
		/* Playing was interrupted and sq_reset() has already cleared
		 * the sq variables, so better don't do anything here.
		 */
		WAKE_UP(sq.sync_queue);
		return;
	}

	if (sq.active & AMI_PLAY_PLAYING) {
		/* We've just finished a frame */
		sq.count--;
		WAKE_UP(sq.action_queue);
	}

	if (sq.active & AMI_PLAY_LOADED)
		/* Increase threshold: frame 1 is already being played */
		minframes = 2;

	/* Shift the flags */
	sq.active = (sq.active<<1) & AMI_PLAY_MASK;

	if (!sq.active)
		/* No frame is playing, disable audio DMA */
		custom.dmacon = AMI_AUDIO_OFF;

	if (sq.count >= minframes)
		/* Try to play the next frame */
		AmiPlay();

	if (!sq.active)
		/* Nothing to play anymore.
		   Wake up a process waiting for audio output to drain. */
		WAKE_UP(sq.sync_queue);
}
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC

/*
 * PCI PowerMac, with AWACS and DBDMA.
 */

static void *PMacAlloc(unsigned int size, int flags)
{
	return kmalloc(size, flags);
}

static void PMacFree(void *ptr, unsigned int size)
{
	kfree(ptr);
}

static int __init PMacIrqInit(void)
{
	if (request_irq(awacs_irq, pmac_awacs_intr, 0, "AWACS", 0)
	    || request_irq(awacs_tx_irq, pmac_awacs_tx_intr, 0, "AWACS out", 0)
	    || request_irq(awacs_rx_irq, pmac_awacs_rx_intr, 0, "AWACS in", 0))
		return 0;
	return 1;
}

#ifdef MODULE
static void PMacIrqCleanup(void)
{
	/* turn off output dma */
	out_le32(&awacs_txdma->control, RUN<<16);
	/* disable interrupts from awacs interface */
	out_le32(&awacs->control, in_le32(&awacs->control) & 0xfff);
	free_irq(awacs_irq, pmac_awacs_intr);
	free_irq(awacs_tx_irq, pmac_awacs_tx_intr);
	free_irq(awacs_rx_irq, pmac_awacs_rx_intr);
	kfree(awacs_tx_cmd_space);
	if (awacs_rx_cmd_space)
		kfree(awacs_rx_cmd_space);
	if (beep_buf)
		kfree(beep_buf);
	kd_mksound = orig_mksound;
#ifdef CONFIG_PMAC_PBOOK
	pmu_unregister_sleep_notifier(&awacs_sleep_notifier);
#endif
}
#endif /* MODULE */

static void PMacSilence(void)
{
	/* turn off output dma */
	out_le32(&awacs_txdma->control, RUN<<16);
}

static int awacs_freqs[8] = {
	44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350
};
static int awacs_freqs_ok[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

static void PMacInit(void)
{
	int i, tolerance;

	switch (sound.soft.format) {
	case AFMT_S16_LE:
	case AFMT_U16_LE:
		sound.hard.format = AFMT_S16_LE;
		break;
	default:
		sound.hard.format = AFMT_S16_BE;
		break;
	}
	sound.hard.stereo = 1;
	sound.hard.size = 16;

	/*
	 * If we have a sample rate which is within catchRadius percent
	 * of the requested value, we don't have to expand the samples.
	 * Otherwise choose the next higher rate.
	 * N.B.: burgundy awacs (iMac and later) only works at 44100 Hz.
	 */
	i = 8;
	do {
		tolerance = catchRadius * awacs_freqs[--i] / 100;
		if (awacs_freqs_ok[i]
		    && sound.soft.speed <= awacs_freqs[i] + tolerance)
			break;
	} while (i > 0);
	if (sound.soft.speed >= awacs_freqs[i] - tolerance)
		sound.trans = &transAwacsNormal;
	else
		sound.trans = &transAwacsExpand;
	sound.read_trans = &transAwacsNormalRead;
	sound.hard.speed = awacs_freqs[i];
	awacs_rate_index = i;

	/* XXX disable error interrupt on burgundy for now */
	out_le32(&awacs->control, MASK_IEPC | (i << 8) | 0x11
		 | (awacs_revision < AWACS_BURGUNDY? MASK_IEE: 0));
	awacs_reg[1] = (awacs_reg[1] & ~MASK_SAMPLERATE) | (i << 3);
	awacs_write(awacs_reg[1] | MASK_ADDR1);
	out_le32(&awacs->byteswap, sound.hard.format != AFMT_S16_BE);

	/* We really want to execute a DMA stop command, after the AWACS
	 * is initialized.
	 * For reasons I don't understand, it stops the hissing noise
	 * common to many PowerBook G3 systems (like mine :-).  Maybe it
	 * is just the AWACS control register change......
	 */
	out_le32(&awacs_txdma->control, (RUN|WAKE|FLUSH|PAUSE) << 16);
	st_le16(&beep_dbdma_cmd->command, DBDMA_STOP);
	out_le32(&awacs->control, (in_le32(&awacs->control) & ~0x1f00));
	out_le32(&awacs_txdma->cmdptr, virt_to_bus(beep_dbdma_cmd));
	out_le32(&awacs_txdma->control, RUN | (RUN << 16));

	sound.bal = -sound.soft.speed;
}

static int PMacSetFormat(int format)
{
	int size;

	switch (format) {
	case AFMT_QUERY:
		return sound.soft.format;
	case AFMT_MU_LAW:
	case AFMT_A_LAW:
	case AFMT_U8:
	case AFMT_S8:
		size = 8;
		break;
	case AFMT_S16_BE:
	case AFMT_U16_BE:
	case AFMT_S16_LE:
	case AFMT_U16_LE:
		size = 16;
		break;
	default: /* :-) */
		printk(KERN_ERR "dmasound: unknown format 0x%x, using AFMT_U8\n",
		       format);
		size = 8;
		format = AFMT_U8;
	}

	sound.soft.format = format;
	sound.soft.size = size;
	if (sound.minDev == SND_DEV_DSP) {
		sound.dsp.format = format;
		sound.dsp.size = size;
	}

	PMacInit();

	return format;
}

#define AWACS_VOLUME_TO_MASK(x)	(15 - ((((x) - 1) * 15) / 99))
#define AWACS_MASK_TO_VOLUME(y)	(100 - ((y) * 99 / 15))

static int awacs_get_volume(int reg, int lshift)
{
	int volume;

	volume = AWACS_MASK_TO_VOLUME((reg >> lshift) & 0xf);
	volume |= AWACS_MASK_TO_VOLUME(reg & 0xf) << 8;
	return volume;
}

static int awacs_volume_setter(int volume, int n, int mute, int lshift)
{
	int r1, rn;

	if (mute && volume == 0) {
		r1 = awacs_reg[1] | mute;
	} else {
		r1 = awacs_reg[1] & ~mute;
		rn = awacs_reg[n] & ~(0xf | (0xf << lshift));
		rn |= ((AWACS_VOLUME_TO_MASK(volume & 0xff) & 0xf) << lshift);
		rn |= AWACS_VOLUME_TO_MASK((volume >> 8) & 0xff) & 0xf;
		awacs_reg[n] = rn;
		awacs_write((n << 12) | rn);
		volume = awacs_get_volume(rn, lshift);
	}
	if (r1 != awacs_reg[1]) {
		awacs_reg[1] = r1;
		awacs_write(r1 | MASK_ADDR1);
	}
	return volume;
}

static int PMacSetVolume(int volume)
{
	return awacs_volume_setter(volume, 2, MASK_AMUTE, 6);
}

static void PMacPlay(void)
{
	volatile struct dbdma_cmd *cp;
	int i, count;
	unsigned long flags;

	save_flags(flags); cli();
	if (awacs_beep_state) {
		/* sound takes precedence over beeps */
		out_le32(&awacs_txdma->control, (RUN|PAUSE|FLUSH|WAKE) << 16);
		out_le32(&awacs->control,
			 (in_le32(&awacs->control) & ~0x1f00)
			 | (awacs_rate_index << 8));
		out_le32(&awacs->byteswap, sound.hard.format != AFMT_S16_BE);
		out_le32(&awacs_txdma->cmdptr, virt_to_bus(&(awacs_tx_cmds[(sq.front+sq.active) % sq.max_count])));

		beep_playing = 0;
		awacs_beep_state = 0;
	}
	i = sq.front + sq.active;
	if (i >= sq.max_count)
		i -= sq.max_count;
	while (sq.active < 2 && sq.active < sq.count) {
		count = (sq.count == sq.active + 1)?sq.rear_size:sq.block_size;
		if (count < sq.block_size && !sq.syncing)
			/* last block not yet filled, and we're not syncing. */
			break;
		cp = &awacs_tx_cmds[i];
		st_le16(&cp->req_count, count);
		st_le16(&cp->xfer_status, 0);
		if (++i >= sq.max_count)
			i = 0;
		out_le16(&awacs_tx_cmds[i].command, DBDMA_STOP);
		out_le16(&cp->command, OUTPUT_MORE + INTR_ALWAYS);
		if (sq.active == 0)
			out_le32(&awacs_txdma->cmdptr, virt_to_bus(cp));
		out_le32(&awacs_txdma->control, ((RUN|WAKE) << 16) + (RUN|WAKE));
		++sq.active;
	}
	restore_flags(flags);
}


static void PMacRecord(void)
{
	unsigned long flags;

	if (read_sq.active)
		return;

	save_flags(flags); cli();

	/* This is all we have to do......Just start it up.
	*/
	out_le32(&awacs_rxdma->control, ((RUN|WAKE) << 16) + (RUN|WAKE));
	read_sq.active = 1;

        restore_flags(flags);
}


static void
pmac_awacs_tx_intr(int irq, void *devid, struct pt_regs *regs)
{
	int i = sq.front;
	int stat;
	volatile struct dbdma_cmd *cp;

	while (sq.active > 0) {
		cp = &awacs_tx_cmds[i];
		stat = ld_le16(&cp->xfer_status);
		if ((stat & ACTIVE) == 0)
			break;	/* this frame is still going */
		--sq.count;
		--sq.active;
		if (++i >= sq.max_count)
			i = 0;
	}
	if (i != sq.front)
		WAKE_UP(sq.action_queue);
	sq.front = i;

	PMacPlay();

	if (!sq.active)
		WAKE_UP(sq.sync_queue);
}


static void
pmac_awacs_rx_intr(int irq, void *devid, struct pt_regs *regs)
{

	/* For some reason on my PowerBook G3, I get one interrupt
	 * when the interrupt vector is installed (like something is
	 * pending).  This happens before the dbdma is initialize by
	 * us, so I just check the command pointer and if it is zero,
	 * just blow it off.
	 */
	if (in_le32(&awacs_rxdma->cmdptr) == 0)
		return;
	
	/* We also want to blow 'em off when shutting down.
	*/
	if (read_sq.active == 0)
		return;

	/* Check multiple buffers in case we were held off from
	 * interrupt processing for a long time.  Geeze, I really hope
	 * this doesn't happen.
	 */
	while (awacs_rx_cmds[read_sq.rear].xfer_status) {

		/* Clear status and move on to next buffer.
		*/
		awacs_rx_cmds[read_sq.rear].xfer_status = 0;
		read_sq.rear++;

		/* Wrap the buffer ring.
		*/
		if (read_sq.rear >= read_sq.max_active)
			read_sq.rear = 0;

		/* If we have caught up to the front buffer, bump it.
		 * This will cause weird (but not fatal) results if the
		 * read loop is currently using this buffer.  The user is
		 * behind in this case anyway, so weird things are going
		 * to happen.
		 */
		if (read_sq.rear == read_sq.front) {
			read_sq.front++;
			if (read_sq.front >= read_sq.max_active)
				read_sq.front = 0;
		}
	}

	WAKE_UP(read_sq.action_queue);
}


static void
pmac_awacs_intr(int irq, void *devid, struct pt_regs *regs)
{
	int ctrl = in_le32(&awacs->control);

	if (ctrl & MASK_PORTCHG) {
		/* do something when headphone is plugged/unplugged? */
	}
	if (ctrl & MASK_CNTLERR) {
		int err = (in_le32(&awacs->codec_stat) & MASK_ERRCODE) >> 16;
		if (err != 0 && awacs_revision < AWACS_BURGUNDY)
			printk(KERN_ERR "AWACS: error %x\n", err);
	}
	/* Writing 1s to the CNTLERR and PORTCHG bits clears them... */
	out_le32(&awacs->control, ctrl);
}

static void
awacs_write(int val)
{
	if (awacs_revision >= AWACS_BURGUNDY)
		return;
	while (in_le32(&awacs->codec_ctrl) & MASK_NEWECMD)
		;	/* XXX should have timeout */
	out_le32(&awacs->codec_ctrl, val | (awacs_subframe << 22));
}

static void awacs_nosound(unsigned long xx)
{
	unsigned long flags;

	save_flags(flags); cli();
	if (beep_playing) {
		st_le16(&beep_dbdma_cmd->command, DBDMA_STOP);
		beep_playing = 0;
	}
	restore_flags(flags);
}

static struct timer_list beep_timer = {
	NULL, NULL, 0, 0, awacs_nosound
};

static void awacs_mksound(unsigned int hz, unsigned int ticks)
{
	unsigned long flags;
	int beep_speed = 0;
	int srate;
	int period, ncycles, nsamples;
	int i, j, f;
	short *p;
	static int beep_hz_cache;
	static int beep_nsamples_cache;
	static int beep_volume_cache;

	for (i = 0; i < 8 && awacs_freqs[i] >= BEEP_SRATE; ++i)
		if (awacs_freqs_ok[i])
			beep_speed = i;
	srate = awacs_freqs[beep_speed];

	if (hz <= srate / BEEP_BUFLEN || hz > srate / 2) {
#if 1
		/* this is a hack for broken X server code */
		hz = 750;
		ticks = 12;
#else
		/* cancel beep currently playing */
		awacs_nosound(0);
		return;
#endif
	}
	save_flags(flags); cli();
	del_timer(&beep_timer);
	if (ticks) {
		beep_timer.expires = jiffies + ticks;
		add_timer(&beep_timer);
	}
	if (beep_playing || sq.active || beep_buf == NULL) {
		restore_flags(flags);
		return;		/* too hard, sorry :-( */
	}
	beep_playing = 1;
	st_le16(&beep_dbdma_cmd->command, OUTPUT_MORE + BR_ALWAYS);
	restore_flags(flags);

	if (hz == beep_hz_cache && beep_volume == beep_volume_cache) {
		nsamples = beep_nsamples_cache;
	} else {
		period = srate * 256 / hz;	/* fixed point */
		ncycles = BEEP_BUFLEN * 256 / period;
		nsamples = (period * ncycles) >> 8;
		f = ncycles * 65536 / nsamples;
		j = 0;
		p = beep_buf;
		for (i = 0; i < nsamples; ++i, p += 2) {
			p[0] = p[1] = beep_wform[j >> 8] * beep_volume;
			j = (j + f) & 0xffff;
		}
		beep_hz_cache = hz;
		beep_volume_cache = beep_volume;
		beep_nsamples_cache = nsamples;
	}

	st_le16(&beep_dbdma_cmd->req_count, nsamples*4);
	st_le16(&beep_dbdma_cmd->xfer_status, 0);
	st_le32(&beep_dbdma_cmd->cmd_dep, virt_to_bus(beep_dbdma_cmd));
	st_le32(&beep_dbdma_cmd->phy_addr, virt_to_bus(beep_buf));
	awacs_beep_state = 1;

	save_flags(flags); cli();
	if (beep_playing) {	/* i.e. haven't been terminated already */
		out_le32(&awacs_txdma->control, (RUN|WAKE|FLUSH|PAUSE) << 16);
		out_le32(&awacs->control,
			 (in_le32(&awacs->control) & ~0x1f00)
			 | (beep_speed << 8));
		out_le32(&awacs->byteswap, 0);
		out_le32(&awacs_txdma->cmdptr, virt_to_bus(beep_dbdma_cmd));
		out_le32(&awacs_txdma->control, RUN | (RUN << 16));
	}
	restore_flags(flags);
}

#ifdef CONFIG_PMAC_PBOOK
/*
 * Save state when going to sleep, restore it afterwards.
 */
static int awacs_sleep_notify(struct pmu_sleep_notifier *self, int when)
{
	switch (when) {
	case PBOOK_SLEEP_NOW:
		/* XXX we should stop any dma in progress when going to sleep
		   and restart it when we wake. */
		PMacSilence();
		disable_irq(awacs_irq);
		disable_irq(awacs_tx_irq);
		break;
	case PBOOK_WAKE:
		out_le32(&awacs->control, MASK_IEPC
			 | (awacs_rate_index << 8) | 0x11
			 | (awacs_revision < AWACS_BURGUNDY? MASK_IEE: 0));
		awacs_write(awacs_reg[0] | MASK_ADDR0);
		awacs_write(awacs_reg[1] | MASK_ADDR1);
		awacs_write(awacs_reg[2] | MASK_ADDR2);
		awacs_write(awacs_reg[4] | MASK_ADDR4);
		out_le32(&awacs->byteswap, sound.hard.format != AFMT_S16_BE);
		enable_irq(awacs_irq);
		enable_irq(awacs_tx_irq);
		if (awacs_revision == 3) {
			mdelay(100);
			awacs_write(0x6000);
			mdelay(2);
			awacs_write(awacs_reg[1] | MASK_ADDR1);
		}
	}
	return PBOOK_SLEEP_OK;
}
#endif /* CONFIG_PMAC_PBOOK */


/* All the burgundy functions: */

/* Waits for busy flag to clear */
inline static void
awacs_burgundy_busy_wait(void)
{
	while (in_le32(&awacs->codec_ctrl) & MASK_NEWECMD)
		;
}

inline static void
awacs_burgundy_extend_wait(void)
{
	while (!(in_le32(&awacs->codec_stat) & MASK_EXTEND))
		;
	while (in_le32(&awacs->codec_stat) & MASK_EXTEND)
		;
}

static void
awacs_burgundy_wcw(unsigned addr, unsigned val)
{
	out_le32(&awacs->codec_ctrl, addr + 0x200c00 + (val & 0xff));
	awacs_burgundy_busy_wait();
	out_le32(&awacs->codec_ctrl, addr + 0x200d00 +((val>>8) & 0xff));
	awacs_burgundy_busy_wait();	     	 
	out_le32(&awacs->codec_ctrl, addr + 0x200e00 +((val>>16) & 0xff));
	awacs_burgundy_busy_wait();	     	 
	out_le32(&awacs->codec_ctrl, addr + 0x200f00 +((val>>24) & 0xff));
	awacs_burgundy_busy_wait();
}

static unsigned
awacs_burgundy_rcw(unsigned addr)
{
	unsigned val = 0;
	unsigned long flags;

	/* should have timeouts here */
	save_flags(flags); cli();

	out_le32(&awacs->codec_ctrl, addr + 0x100000);
	awacs_burgundy_busy_wait();
	awacs_burgundy_extend_wait();
	val += (in_le32(&awacs->codec_stat) >> 4) & 0xff;

	out_le32(&awacs->codec_ctrl, addr + 0x100100);
	awacs_burgundy_busy_wait();	     	 
	awacs_burgundy_extend_wait();
	val += ((in_le32(&awacs->codec_stat)>>4) & 0xff) <<8;

	out_le32(&awacs->codec_ctrl, addr + 0x100200);
	awacs_burgundy_busy_wait();	     	 
	awacs_burgundy_extend_wait();
	val += ((in_le32(&awacs->codec_stat)>>4) & 0xff) <<16;

	out_le32(&awacs->codec_ctrl, addr + 0x100300);
	awacs_burgundy_busy_wait();
	awacs_burgundy_extend_wait();
	val += ((in_le32(&awacs->codec_stat)>>4) & 0xff) <<24;

	restore_flags(flags);

	return val;
}


static void
awacs_burgundy_wcb(unsigned addr, unsigned val)
{
	out_le32(&awacs->codec_ctrl, addr + 0x300000 + (val & 0xff));
	awacs_burgundy_busy_wait();
}

static unsigned
awacs_burgundy_rcb(unsigned addr)
{
	unsigned val = 0;
	unsigned long flags;

	/* should have timeouts here */
	save_flags(flags); cli();

	out_le32(&awacs->codec_ctrl, addr + 0x100000);
	awacs_burgundy_busy_wait();
	awacs_burgundy_extend_wait();
	val += (in_le32(&awacs->codec_stat) >> 4) & 0xff;

	restore_flags(flags);

	return val;
}

static int
awacs_burgundy_check(void)
{
	/* Checks to see the chip is alive and kicking */
	int error = in_le32(&awacs->codec_ctrl) & MASK_ERRCODE;
  
	return error == 0xf0000;
}

static int
awacs_burgundy_init(void)
{
	if (awacs_burgundy_check()) {
		printk(KERN_WARNING "AWACS: disabled by MacOS :-(\n");
		return 1;
	}

	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_OUTPUTENABLES,
			   DEF_BURGUNDY_OUTPUTENABLES);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES,
			   DEF_BURGUNDY_MORE_OUTPUTENABLES);
	awacs_burgundy_wcw(MASK_ADDR_BURGUNDY_OUTPUTSELECTS,
			   DEF_BURGUNDY_OUTPUTSELECTS);
	
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_INPSEL21,
			   DEF_BURGUNDY_INPSEL21);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_INPSEL3,
			   DEF_BURGUNDY_INPSEL3);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_GAINCD,
			   DEF_BURGUNDY_GAINCD);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_GAINLINE,
			   DEF_BURGUNDY_GAINLINE);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_GAINMIC,
			   DEF_BURGUNDY_GAINMIC);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_GAINMODEM,
			   DEF_BURGUNDY_GAINMODEM);
	
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_ATTENSPEAKER,
			   DEF_BURGUNDY_ATTENSPEAKER);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_ATTENLINEOUT,
			   DEF_BURGUNDY_ATTENLINEOUT);
	awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_ATTENHP,
			   DEF_BURGUNDY_ATTENHP);
	
	awacs_burgundy_wcw(MASK_ADDR_BURGUNDY_MASTER_VOLUME,
			   DEF_BURGUNDY_MASTER_VOLUME);
	awacs_burgundy_wcw(MASK_ADDR_BURGUNDY_VOLCD,
			   DEF_BURGUNDY_VOLCD);
	awacs_burgundy_wcw(MASK_ADDR_BURGUNDY_VOLLINE,
			   DEF_BURGUNDY_VOLLINE);
	awacs_burgundy_wcw(MASK_ADDR_BURGUNDY_VOLMIC,
			   DEF_BURGUNDY_VOLMIC);
	return 0;
}

static void
awacs_burgundy_write_volume(unsigned address, int volume)
{
	int hardvolume,lvolume,rvolume;

	lvolume = (volume & 0xff) ? (volume & 0xff) + 155 : 0;
	rvolume = ((volume >>8)&0xff) ? ((volume >> 8)&0xff ) + 155 : 0;

	hardvolume = lvolume + (rvolume << 16);

	awacs_burgundy_wcw(address, hardvolume);
}

static int
awacs_burgundy_read_volume(unsigned address)
{
	int softvolume,wvolume;

	wvolume = awacs_burgundy_rcw(address);

	softvolume = (wvolume & 0xff) - 155;
	softvolume += (((wvolume >> 16) & 0xff) - 155)<<8;

	return softvolume > 0 ? softvolume : 0;
}




static int
awacs_burgundy_read_mvolume(unsigned address)
{
	int lvolume,rvolume,wvolume;

	wvolume = awacs_burgundy_rcw(address);

	wvolume &= 0xffff;
	
	rvolume = (wvolume & 0xff) - 155;
	lvolume = ((wvolume & 0xff00)>>8) - 155;

	return lvolume + (rvolume << 8);
}


static void
awacs_burgundy_write_mvolume(unsigned address, int volume)
{
	int lvolume,rvolume,hardvolume;

	lvolume = (volume &0xff) ? (volume & 0xff) + 155 :0;
	rvolume = ((volume >>8) & 0xff) ? (volume >> 8) + 155 :0;

	hardvolume = lvolume + (rvolume << 8);
	hardvolume += (hardvolume << 16);

	awacs_burgundy_wcw(address, hardvolume);
}

/* End burgundy functions */





/* Turn on sound output, needed on G3 desktop powermacs */
static void
awacs_enable_amp(int spkr_vol)
{
	struct adb_request req;

	awacs_spkr_vol = spkr_vol;
	if (sys_ctrler != SYS_CTRLER_CUDA)
		return;

	/* turn on headphones */
	cuda_request(&req, NULL, 5, CUDA_PACKET, CUDA_GET_SET_IIC,
		     0x8a, 4, 0);
	while (!req.complete) cuda_poll();
	cuda_request(&req, NULL, 5, CUDA_PACKET, CUDA_GET_SET_IIC,
		     0x8a, 6, 0);
	while (!req.complete) cuda_poll();

	/* turn on speaker */
	cuda_request(&req, NULL, 5, CUDA_PACKET, CUDA_GET_SET_IIC,
		     0x8a, 3, (100 - (spkr_vol & 0xff)) * 32 / 100);
	while (!req.complete) cuda_poll();
	cuda_request(&req, NULL, 5, CUDA_PACKET, CUDA_GET_SET_IIC,
		     0x8a, 5, (100 - ((spkr_vol >> 8) & 0xff)) * 32 / 100);
	while (!req.complete) cuda_poll();

	cuda_request(&req, NULL, 5, CUDA_PACKET,
		     CUDA_GET_SET_IIC, 0x8a, 1, 0x29);
	while (!req.complete) cuda_poll();
}

#endif /* CONFIG_PPC */

/*** Machine definitions *****************************************************/


#ifdef CONFIG_ATARI
static MACHINE machTT = {
	DMASND_TT, AtaAlloc, AtaFree, AtaIrqInit,
#ifdef MODULE
	AtaIrqCleanUp,
#endif /* MODULE */
	TTInit, TTSilence, TTSetFormat, TTSetVolume,
	AtaSetBass, AtaSetTreble, TTSetGain,
	AtaPlay
};

static MACHINE machFalcon = {
	DMASND_FALCON, AtaAlloc, AtaFree, AtaIrqInit,
#ifdef MODULE
	AtaIrqCleanUp,
#endif /* MODULE */
	FalconInit, FalconSilence, FalconSetFormat, FalconSetVolume,
	AtaSetBass, AtaSetTreble, NULL,
	AtaPlay
};
#endif /* CONFIG_ATARI */

#ifdef CONFIG_AMIGA
static MACHINE machAmiga = {
	DMASND_AMIGA, AmiAlloc, AmiFree, AmiIrqInit,
#ifdef MODULE
	AmiIrqCleanUp,
#endif /* MODULE */
	AmiInit, AmiSilence, AmiSetFormat, AmiSetVolume,
	NULL, AmiSetTreble, NULL,
	AmiPlay
};
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
static MACHINE machPMac = {
	DMASND_AWACS, PMacAlloc, PMacFree, PMacIrqInit,
#ifdef MODULE
	PMacIrqCleanup,
#endif /* MODULE */
	PMacInit, PMacSilence, PMacSetFormat, PMacSetVolume,
	NULL, NULL, NULL,	/* bass, treble, gain */
	PMacPlay
};
#endif /* CONFIG_AMIGA */


/*** Mid level stuff *********************************************************/


static void sound_silence(void)
{
	/* update hardware settings one more */
	(*sound.mach.init)();

	(*sound.mach.silence)();
}


static void sound_init(void)
{
	(*sound.mach.init)();
}


static int sound_set_format(int format)
{
	return(*sound.mach.setFormat)(format);
}


static int sound_set_speed(int speed)
{
	if (speed < 0)
		return(sound.soft.speed);

	sound.soft.speed = speed;
	(*sound.mach.init)();
	if (sound.minDev == SND_DEV_DSP)
		sound.dsp.speed = sound.soft.speed;

	return(sound.soft.speed);
}


static int sound_set_stereo(int stereo)
{
	if (stereo < 0)
		return(sound.soft.stereo);

	stereo = !!stereo;    /* should be 0 or 1 now */

	sound.soft.stereo = stereo;
	if (sound.minDev == SND_DEV_DSP)
		sound.dsp.stereo = stereo;
	(*sound.mach.init)();

	return(stereo);
}


static int sound_set_volume(int volume)
{
	return(*sound.mach.setVolume)(volume);
}


#ifdef CONFIG_ATARI
static int sound_set_bass(int bass)
{
	return(sound.mach.setBass ? (*sound.mach.setBass)(bass) : 50);
}

static int sound_set_gain(int gain)
{
	return sound.mach.setGain ? sound.mach.setGain(gain) : 100;
}
#endif /* CONFIG_ATARI */

#if defined(CONFIG_ATARI) || defined(CONFIG_AMIGA)
static int sound_set_treble(int treble)
{
	return(sound.mach.setTreble ? (*sound.mach.setTreble)(treble) : 50);
}
#endif /* CONFIG_ATARI || CONFIG_AMIGA */


static ssize_t sound_copy_translate(const u_char *userPtr,
				    size_t userCount,
				    u_char frame[], ssize_t *frameUsed,
				    ssize_t frameLeft)
{
	ssize_t (*ct_func)(const u_char *, size_t, u_char *, ssize_t *, ssize_t) = NULL;

	switch (sound.soft.format) {
	case AFMT_MU_LAW:
		ct_func = sound.trans->ct_ulaw;
		break;
	case AFMT_A_LAW:
		ct_func = sound.trans->ct_alaw;
		break;
	case AFMT_S8:
		ct_func = sound.trans->ct_s8;
		break;
	case AFMT_U8:
		ct_func = sound.trans->ct_u8;
		break;
	case AFMT_S16_BE:
		ct_func = sound.trans->ct_s16be;
		break;
	case AFMT_U16_BE:
		ct_func = sound.trans->ct_u16be;
		break;
	case AFMT_S16_LE:
		ct_func = sound.trans->ct_s16le;
		break;
	case AFMT_U16_LE:
		ct_func = sound.trans->ct_u16le;
		break;
	}
	if (ct_func)
		return ct_func(userPtr, userCount, frame, frameUsed, frameLeft);
	else
		return 0;
}

#ifdef CONFIG_PPC
static ssize_t sound_copy_translate_read(const u_char *userPtr,
				    size_t userCount,
				    u_char frame[], ssize_t *frameUsed,
				    ssize_t frameLeft)
{
	ssize_t (*ct_func)(const u_char *, size_t, u_char *, ssize_t *, ssize_t) = NULL;

	switch (sound.soft.format) {
	case AFMT_MU_LAW:
		ct_func = sound.read_trans->ct_ulaw;
		break;
	case AFMT_A_LAW:
		ct_func = sound.read_trans->ct_alaw;
		break;
	case AFMT_S8:
		ct_func = sound.read_trans->ct_s8;
		break;
	case AFMT_U8:
		ct_func = sound.read_trans->ct_u8;
		break;
	case AFMT_S16_BE:
		ct_func = sound.read_trans->ct_s16be;
		break;
	case AFMT_U16_BE:
		ct_func = sound.read_trans->ct_u16be;
		break;
	case AFMT_S16_LE:
		ct_func = sound.read_trans->ct_s16le;
		break;
	case AFMT_U16_LE:
		ct_func = sound.read_trans->ct_u16le;
		break;
	}
	if (ct_func)
		return ct_func(userPtr, userCount, frame, frameUsed, frameLeft);
	else
		return 0;
}
#endif


/*
 * /dev/mixer abstraction
 */


#define RECLEVEL_VOXWARE_TO_GAIN(v) \
	((v) < 0 ? 0 : (v) > 100 ? 15 : (v) * 3 / 20)
#define RECLEVEL_GAIN_TO_VOXWARE(v) (((v) * 20 + 2) / 3)


static int mixer_open(struct inode *inode, struct file *file)
{
	MOD_INC_USE_COUNT;
	mixer.busy = 1;
	return 0;
}


static int mixer_release(struct inode *inode, struct file *file)
{
	mixer.busy = 0;
	MOD_DEC_USE_COUNT;
	return 0;
}


static int mixer_ioctl(struct inode *inode, struct file *file, u_int cmd,
		       u_long arg)
{
	int data;
	if (_SIOC_DIR(cmd) & _SIOC_WRITE)
	    mixer.modify_counter++;
	if (cmd == OSS_GETVERSION)
	    return IOCTL_OUT(arg, SOUND_VERSION);
	switch (sound.mach.type) {
#ifdef CONFIG_ATARI
	case DMASND_FALCON:
		switch (cmd) {
		case SOUND_MIXER_INFO: {
		    mixer_info info;
		    strncpy(info.id, "FALCON", sizeof(info.id));
		    strncpy(info.name, "FALCON", sizeof(info.name));
		    info.name[sizeof(info.name)-1] = 0;
		    info.modify_counter = mixer.modify_counter;
		    copy_to_user_ret((int *)arg, &info, sizeof(info), -EFAULT);
		    return 0;
		}
		case SOUND_MIXER_READ_DEVMASK:
			return IOCTL_OUT(arg, SOUND_MASK_VOLUME | SOUND_MASK_MIC | SOUND_MASK_SPEAKER);
		case SOUND_MIXER_READ_RECMASK:
			return IOCTL_OUT(arg, SOUND_MASK_MIC);
		case SOUND_MIXER_READ_STEREODEVS:
			return IOCTL_OUT(arg, SOUND_MASK_VOLUME | SOUND_MASK_MIC);
		case SOUND_MIXER_READ_CAPS:
			return IOCTL_OUT(arg, SOUND_CAP_EXCL_INPUT);
		case SOUND_MIXER_READ_VOLUME:
			return IOCTL_OUT(arg,
				VOLUME_ATT_TO_VOXWARE(sound.volume_left) |
				VOLUME_ATT_TO_VOXWARE(sound.volume_right) << 8);
		case SOUND_MIXER_WRITE_MIC:
			IOCTL_IN(arg, data);
			tt_dmasnd.input_gain =
				RECLEVEL_VOXWARE_TO_GAIN(data & 0xff) << 4 |
				RECLEVEL_VOXWARE_TO_GAIN(data >> 8 & 0xff);
			/* fall thru, return set value */
		case SOUND_MIXER_READ_MIC:
			return IOCTL_OUT(arg,
				RECLEVEL_GAIN_TO_VOXWARE(tt_dmasnd.input_gain >> 4 & 0xf) |
				RECLEVEL_GAIN_TO_VOXWARE(tt_dmasnd.input_gain & 0xf) << 8);
		case SOUND_MIXER_READ_SPEAKER:
			{
				int porta;
				cli();
				sound_ym.rd_data_reg_sel = 14;
				porta = sound_ym.rd_data_reg_sel;
				sti();
				return IOCTL_OUT(arg, porta & 0x40 ? 0 : 100);
			}
		case SOUND_MIXER_WRITE_VOLUME:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_volume(data));
		case SOUND_MIXER_WRITE_SPEAKER:
			{
				int porta;
				IOCTL_IN(arg, data);
				cli();
				sound_ym.rd_data_reg_sel = 14;
				porta = (sound_ym.rd_data_reg_sel & ~0x40) |
					(data < 50 ? 0x40 : 0);
				sound_ym.wd_data = porta;
				sti();
				return IOCTL_OUT(arg, porta & 0x40 ? 0 : 100);
			}
		}
		break;

	case DMASND_TT:
		switch (cmd) {
		case SOUND_MIXER_INFO: {
		    mixer_info info;
		    strncpy(info.id, "TT", sizeof(info.id));
		    strncpy(info.name, "TT", sizeof(info.name));
		    info.name[sizeof(info.name)-1] = 0;
		    info.modify_counter = mixer.modify_counter;
		    copy_to_user_ret((int *)arg, &info, sizeof(info), -EFAULT);
		    return 0;
		}
		case SOUND_MIXER_READ_DEVMASK:
			return IOCTL_OUT(arg,
					 SOUND_MASK_VOLUME | SOUND_MASK_TREBLE | SOUND_MASK_BASS |
					 (MACH_IS_TT ? SOUND_MASK_SPEAKER : 0));
		case SOUND_MIXER_READ_RECMASK:
			return IOCTL_OUT(arg, 0);
		case SOUND_MIXER_READ_STEREODEVS:
			return IOCTL_OUT(arg, SOUND_MASK_VOLUME);
		case SOUND_MIXER_READ_VOLUME:
			return IOCTL_OUT(arg,
					 VOLUME_DB_TO_VOXWARE(sound.volume_left) |
					 (VOLUME_DB_TO_VOXWARE(sound.volume_right) << 8));
		case SOUND_MIXER_READ_BASS:
			return IOCTL_OUT(arg, TONE_DB_TO_VOXWARE(sound.bass));
		case SOUND_MIXER_READ_TREBLE:
			return IOCTL_OUT(arg, TONE_DB_TO_VOXWARE(sound.treble));
		case SOUND_MIXER_READ_OGAIN:
			return IOCTL_OUT(arg, GAIN_DB_TO_VOXWARE(sound.gain));
		case SOUND_MIXER_READ_SPEAKER:
			{
				int porta;
				if (MACH_IS_TT) {
					cli();
					sound_ym.rd_data_reg_sel = 14;
					porta = sound_ym.rd_data_reg_sel;
					sti();
					return IOCTL_OUT(arg, porta & 0x40 ? 0 : 100);
				}
			}
			break;
		case SOUND_MIXER_WRITE_VOLUME:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_volume(data));
		case SOUND_MIXER_WRITE_BASS:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_bass(data));
		case SOUND_MIXER_WRITE_TREBLE:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_treble(data));
		case SOUND_MIXER_WRITE_OGAIN:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_gain(data));
		case SOUND_MIXER_WRITE_SPEAKER:
			if (MACH_IS_TT) {
				int porta;
				IOCTL_IN(arg, data);
				cli();
				sound_ym.rd_data_reg_sel = 14;
				porta = (sound_ym.rd_data_reg_sel & ~0x40) |
					(data < 50 ? 0x40 : 0);
				sound_ym.wd_data = porta;
				sti();
				return IOCTL_OUT(arg, porta & 0x40 ? 0 : 100);
			}
		}
		break;
#endif /* CONFIG_ATARI */

#ifdef CONFIG_AMIGA
	case DMASND_AMIGA:
		switch (cmd) {
		case SOUND_MIXER_INFO: {
		    mixer_info info;
		    strncpy(info.id, "AMIGA", sizeof(info.id));
		    strncpy(info.name, "AMIGA", sizeof(info.name));
		    info.name[sizeof(info.name)-1] = 0;
		    info.modify_counter = mixer.modify_counter;
		    copy_to_user_ret((int *)arg, &info, sizeof(info), -EFAULT);
		    return 0;
		}
		case SOUND_MIXER_READ_DEVMASK:
			return IOCTL_OUT(arg, SOUND_MASK_VOLUME | SOUND_MASK_TREBLE);
		case SOUND_MIXER_READ_RECMASK:
			return IOCTL_OUT(arg, 0);
		case SOUND_MIXER_READ_STEREODEVS:
			return IOCTL_OUT(arg, SOUND_MASK_VOLUME);
		case SOUND_MIXER_READ_VOLUME:
			return IOCTL_OUT(arg,
				VOLUME_AMI_TO_VOXWARE(sound.volume_left) |
				VOLUME_AMI_TO_VOXWARE(sound.volume_right) << 8);
		case SOUND_MIXER_WRITE_VOLUME:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_volume(data));
		case SOUND_MIXER_READ_TREBLE:
			return IOCTL_OUT(arg, sound.treble);
		case SOUND_MIXER_WRITE_TREBLE:
			IOCTL_IN(arg, data);
			return IOCTL_OUT(arg, sound_set_treble(data));
		}
		break;
#endif /* CONFIG_AMIGA */

#ifdef CONFIG_PPC
	case DMASND_AWACS:
		/* Different IOCTLS for burgundy*/
		if (awacs_revision < AWACS_BURGUNDY) {
			switch (cmd) {
			case SOUND_MIXER_INFO: {
			    mixer_info info;
			    strncpy(info.id, "AWACS", sizeof(info.id));
			    strncpy(info.name, "AWACS", sizeof(info.name));
			    info.name[sizeof(info.name)-1] = 0;
			    info.modify_counter = mixer.modify_counter;
			    copy_to_user_ret((int *)arg, &info, 
					     sizeof(info), -EFAULT);
			    return 0;
			}
			case SOUND_MIXER_READ_DEVMASK:
				data = SOUND_MASK_VOLUME | SOUND_MASK_SPEAKER
					| SOUND_MASK_LINE | SOUND_MASK_MIC
					| SOUND_MASK_CD | SOUND_MASK_RECLEV
					| SOUND_MASK_ALTPCM
					| SOUND_MASK_MONITOR;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_RECMASK:
				data = SOUND_MASK_LINE | SOUND_MASK_MIC
					| SOUND_MASK_CD;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_RECSRC:
				data = 0;
				if (awacs_reg[0] & MASK_MUX_AUDIN)
					data |= SOUND_MASK_LINE;
				if (awacs_reg[0] & MASK_MUX_MIC)
					data |= SOUND_MASK_MIC;
				if (awacs_reg[0] & MASK_MUX_CD)
					data |= SOUND_MASK_CD;
				if (awacs_reg[1] & MASK_LOOPTHRU)
					data |= SOUND_MASK_MONITOR;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_RECSRC:
				IOCTL_IN(arg, data);
				data &= (SOUND_MASK_LINE
					 | SOUND_MASK_MIC | SOUND_MASK_CD
					 | SOUND_MASK_MONITOR);
				awacs_reg[0] &= ~(MASK_MUX_CD | MASK_MUX_MIC
						  | MASK_MUX_AUDIN);
				awacs_reg[1] &= ~MASK_LOOPTHRU;
				if (data & SOUND_MASK_LINE)
					awacs_reg[0] |= MASK_MUX_AUDIN;
				if (data & SOUND_MASK_MIC)
					awacs_reg[0] |= MASK_MUX_MIC;
				if (data & SOUND_MASK_CD)
					awacs_reg[0] |= MASK_MUX_CD;
				if (data & SOUND_MASK_MONITOR)
					awacs_reg[1] |= MASK_LOOPTHRU;
				awacs_write(awacs_reg[0] | MASK_ADDR0);
				awacs_write(awacs_reg[1] | MASK_ADDR1);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_STEREODEVS:
				data = SOUND_MASK_VOLUME | SOUND_MASK_SPEAKER
					| SOUND_MASK_RECLEV;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_CAPS:
				return IOCTL_OUT(arg, 0);
			case SOUND_MIXER_READ_VOLUME:
				data = (awacs_reg[1] & MASK_AMUTE)? 0:
					awacs_get_volume(awacs_reg[2], 6);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_VOLUME:
				IOCTL_IN(arg, data);
				return IOCTL_OUT(arg, sound_set_volume(data));
			case SOUND_MIXER_READ_SPEAKER:
				if (awacs_revision == 3
				    && sys_ctrler == SYS_CTRLER_CUDA)
					data = awacs_spkr_vol;
				else
					data = (awacs_reg[1] & MASK_CMUTE)? 0:
						awacs_get_volume(awacs_reg[4], 6);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_SPEAKER:
				IOCTL_IN(arg, data);
				if (awacs_revision == 3
				    && sys_ctrler == SYS_CTRLER_CUDA)
					awacs_enable_amp(data);
				else
					data = awacs_volume_setter(data, 4, MASK_CMUTE, 6);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_ALTPCM:	/* really bell volume */
				IOCTL_IN(arg, data);
				beep_volume = data & 0xff;
				/* fall through */
			case SOUND_MIXER_READ_ALTPCM:
				return IOCTL_OUT(arg, beep_volume);
			case SOUND_MIXER_WRITE_LINE:
				IOCTL_IN(arg, data);
				awacs_reg[0] &= ~MASK_MUX_AUDIN;
				if ((data & 0xff) >= 50)
					awacs_reg[0] |= MASK_MUX_AUDIN;
				awacs_write(MASK_ADDR0 | awacs_reg[0]);
				/* fall through */
			case SOUND_MIXER_READ_LINE:
				data = (awacs_reg[0] & MASK_MUX_AUDIN)? 100: 0;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_MIC:
				IOCTL_IN(arg, data);
				data &= 0xff;
				awacs_reg[0] &= ~(MASK_MUX_MIC | MASK_GAINLINE);
				if (data >= 25) {
					awacs_reg[0] |= MASK_MUX_MIC;
					if (data >= 75)
						awacs_reg[0] |= MASK_GAINLINE;
				}
				awacs_write(MASK_ADDR0 | awacs_reg[0]);
				/* fall through */
			case SOUND_MIXER_READ_MIC:
				data = (awacs_reg[0] & MASK_MUX_MIC)?
					(awacs_reg[0] & MASK_GAINLINE? 100: 50): 0;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_CD:
				IOCTL_IN(arg, data);
				awacs_reg[0] &= ~MASK_MUX_CD;
				if ((data & 0xff) >= 50)
					awacs_reg[0] |= MASK_MUX_CD;
				awacs_write(MASK_ADDR0 | awacs_reg[0]);
				/* fall through */
			case SOUND_MIXER_READ_CD:
				data = (awacs_reg[0] & MASK_MUX_CD)? 100: 0;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_RECLEV:
				IOCTL_IN(arg, data);
				data = awacs_volume_setter(data, 0, 0, 4);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_RECLEV:
				data = awacs_get_volume(awacs_reg[0], 4);
				return IOCTL_OUT(arg, data);
			}
			break;
		} else {
			/* We are, we are, we are... Burgundy or better */
			switch(cmd) {
			case SOUND_MIXER_INFO: {
			    mixer_info info;
			    strncpy(info.id, "AWACS", sizeof(info.id));
			    strncpy(info.name, "AWACS", sizeof(info.name));
			    info.name[sizeof(info.name)-1] = 0;
			    info.modify_counter = mixer.modify_counter;
			    copy_to_user_ret((int *)arg, &info, 
					     sizeof(info), -EFAULT);
			    return 0;
			}
			case SOUND_MIXER_READ_DEVMASK:
				data = SOUND_MASK_VOLUME | SOUND_MASK_CD |
					SOUND_MASK_LINE | SOUND_MASK_MIC |
					SOUND_MASK_SPEAKER | SOUND_MASK_ALTPCM;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_RECMASK:
				data = SOUND_MASK_LINE | SOUND_MASK_MIC
					| SOUND_MASK_CD;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_RECSRC:
				data = 0;
				if (awacs_reg[0] & MASK_MUX_AUDIN)
					data |= SOUND_MASK_LINE;
				if (awacs_reg[0] & MASK_MUX_MIC)
					data |= SOUND_MASK_MIC;
				if (awacs_reg[0] & MASK_MUX_CD)
					data |= SOUND_MASK_CD;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_RECSRC:
				IOCTL_IN(arg, data);
				data &= (SOUND_MASK_LINE
					 | SOUND_MASK_MIC | SOUND_MASK_CD);
				awacs_reg[0] &= ~(MASK_MUX_CD | MASK_MUX_MIC
						  | MASK_MUX_AUDIN);
				if (data & SOUND_MASK_LINE)
					awacs_reg[0] |= MASK_MUX_AUDIN;
				if (data & SOUND_MASK_MIC)
					awacs_reg[0] |= MASK_MUX_MIC;
				if (data & SOUND_MASK_CD)
					awacs_reg[0] |= MASK_MUX_CD;
				awacs_write(awacs_reg[0] | MASK_ADDR0);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_STEREODEVS:
				data = SOUND_MASK_VOLUME | SOUND_MASK_SPEAKER
					| SOUND_MASK_RECLEV | SOUND_MASK_CD
					| SOUND_MASK_LINE;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_CAPS:
				return IOCTL_OUT(arg, 0);
			case SOUND_MIXER_WRITE_VOLUME:
				IOCTL_IN(arg, data);
				awacs_burgundy_write_mvolume(MASK_ADDR_BURGUNDY_MASTER_VOLUME, data);
				/* Fall through */
			case SOUND_MIXER_READ_VOLUME:
				return IOCTL_OUT(arg, awacs_burgundy_read_mvolume(MASK_ADDR_BURGUNDY_MASTER_VOLUME));
			case SOUND_MIXER_WRITE_SPEAKER:
				IOCTL_IN(arg, data);

				if (!(data & 0xff)) {
				  /* Mute the left speaker */
				  awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES,
						     awacs_burgundy_rcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES) & ~0x2);
				} else {
				  /* Unmute the left speaker */
				  awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES,
						     awacs_burgundy_rcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES) | 0x2);
				}
				if (!(data & 0xff00)) {
				  /* Mute the right speaker */
				  awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES,
						     awacs_burgundy_rcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES) & ~0x4);
				} else {
				  /* Unmute the right speaker */
				  awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES,
						     awacs_burgundy_rcb(MASK_ADDR_BURGUNDY_MORE_OUTPUTENABLES) | 0x4);
				}

				data = (((data&0xff)*16)/100 > 0xf ? 0xf :
					(((data&0xff)*16)/100)) + 
					 ((((data>>8)*16)/100 > 0xf ? 0xf :
					((((data>>8)*16)/100)))<<4);

				awacs_burgundy_wcb(MASK_ADDR_BURGUNDY_ATTENSPEAKER, ~data);
				/* Fall through */
			case SOUND_MIXER_READ_SPEAKER:
				data = awacs_burgundy_rcb(MASK_ADDR_BURGUNDY_ATTENSPEAKER);
				data = (((data & 0xf)*100)/16) + ((((data>>4)*100)/16)<<8);
				return IOCTL_OUT(arg, ~data);
			case SOUND_MIXER_WRITE_ALTPCM:	/* really bell volume */
				IOCTL_IN(arg, data);
				beep_volume = data & 0xff;
				/* fall through */
			case SOUND_MIXER_READ_ALTPCM:
				return IOCTL_OUT(arg, beep_volume);
			case SOUND_MIXER_WRITE_LINE:
				IOCTL_IN(arg, data);
				awacs_burgundy_write_volume(MASK_ADDR_BURGUNDY_VOLLINE, data);

				/* fall through */
			case SOUND_MIXER_READ_LINE:
				data = awacs_burgundy_read_volume(MASK_ADDR_BURGUNDY_VOLLINE);				
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_MIC:
				IOCTL_IN(arg, data);
				/* Mic is mono device */
				data = (data << 8) + (data << 24);
				awacs_burgundy_write_volume(MASK_ADDR_BURGUNDY_VOLMIC, data);
				/* fall through */
			case SOUND_MIXER_READ_MIC:
				data = awacs_burgundy_read_volume(MASK_ADDR_BURGUNDY_VOLMIC);				
				data <<= 24;
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_CD:
				IOCTL_IN(arg, data);
				awacs_burgundy_write_volume(MASK_ADDR_BURGUNDY_VOLCD, data);
				/* fall through */
			case SOUND_MIXER_READ_CD:
				data = awacs_burgundy_read_volume(MASK_ADDR_BURGUNDY_VOLCD);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_WRITE_RECLEV:
				IOCTL_IN(arg, data);
				data = awacs_volume_setter(data, 0, 0, 4);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_READ_RECLEV:
				data = awacs_get_volume(awacs_reg[0], 4);
				return IOCTL_OUT(arg, data);
			case SOUND_MIXER_OUTMASK:
				break;
			case SOUND_MIXER_OUTSRC:
				break;
			}
			break;
		}
#endif
	}

	return -EINVAL;
}


static struct file_operations mixer_fops =
{
	sound_lseek,
	NULL,			/* mixer_read */
	NULL,			/* mixer_write */
	NULL,			/* mixer_readdir */
	NULL,			/* mixer_poll */
	mixer_ioctl,
	NULL,			/* mixer_mmap */
	mixer_open,
	NULL,			/* flush */
	mixer_release,
};


static void __init mixer_init(void)
{
#ifndef MODULE
	int mixer_unit;
#endif
	mixer_unit = register_sound_mixer(&mixer_fops, -1);
	if (mixer_unit < 0)
		return;

	mixer.busy = 0;
	sound.treble = 0;
	sound.bass = 0;
	switch (sound.mach.type) {
#ifdef CONFIG_ATARI
	case DMASND_TT:
		atari_microwire_cmd(MW_LM1992_VOLUME(0));
		sound.volume_left = 0;
		atari_microwire_cmd(MW_LM1992_BALLEFT(0));
		sound.volume_right = 0;
		atari_microwire_cmd(MW_LM1992_BALRIGHT(0));
		atari_microwire_cmd(MW_LM1992_TREBLE(0));
		atari_microwire_cmd(MW_LM1992_BASS(0));
		break;
	case DMASND_FALCON:
		sound.volume_left = (tt_dmasnd.output_atten & 0xf00) >> 8;
		sound.volume_right = (tt_dmasnd.output_atten & 0xf0) >> 4;
		break;
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	case DMASND_AMIGA:
		sound.volume_left = 64;
		sound.volume_right = 64;
		custom.aud[0].audvol = sound.volume_left;
		custom.aud[3].audvol = 1;	/* For pseudo 14bit */
		custom.aud[1].audvol = sound.volume_right;
		custom.aud[2].audvol = 1;	/* For pseudo 14bit */
		sound.treble = 50;
		break;
#endif /* CONFIG_AMIGA */
	}
}


/*
 * Sound queue stuff, the heart of the driver
 */


static int sq_allocate_buffers(void)
{
	int i;

	if (sound_buffers)
		return 0;
	sound_buffers = kmalloc (numBufs * sizeof(char *), GFP_KERNEL);
	if (!sound_buffers)
		return -ENOMEM;
	for (i = 0; i < numBufs; i++) {
		sound_buffers[i] = sound.mach.dma_alloc (bufSize << 10, GFP_KERNEL);
		if (!sound_buffers[i]) {
			while (i--)
				sound.mach.dma_free (sound_buffers[i], bufSize << 10);
			kfree (sound_buffers);
			sound_buffers = 0;
			return -ENOMEM;
		}
	}
	return 0;
}


static void sq_release_buffers(void)
{
	int i;

	if (sound_buffers) {
		for (i = 0; i < numBufs; i++)
			sound.mach.dma_free (sound_buffers[i], bufSize << 10);
		kfree (sound_buffers);
		sound_buffers = 0;
	}
}


#ifdef CONFIG_PPC
static int sq_allocate_read_buffers(void)
{
	int i;
	int j;

	if (sound_read_buffers)
		return 0;
	sound_read_buffers = kmalloc(numReadBufs * sizeof(char *), GFP_KERNEL);
	if (!sound_read_buffers)
		return -ENOMEM;
	for (i = 0; i < numBufs; i++) {
		sound_read_buffers[i] = sound.mach.dma_alloc (readbufSize<<10,
							      GFP_KERNEL);
		if (!sound_read_buffers[i]) {
			while (i--)
				sound.mach.dma_free (sound_read_buffers[i],
						     readbufSize << 10);
			kfree (sound_read_buffers);
			sound_read_buffers = 0;
			return -ENOMEM;
		}
		/* XXXX debugging code */
		for (j=0; j<readbufSize; j++) {
			sound_read_buffers[i][j] = 0xef;
		}
	}
	return 0;
}

static void sq_release_read_buffers(void)
{
	int i;
	volatile struct dbdma_cmd *cp;
	

	if (sound_read_buffers) {
		cp = awacs_rx_cmds;
		for (i = 0; i < numReadBufs; i++,cp++) {
			st_le16(&cp->command, DBDMA_STOP);
		}
		/* We should probably wait for the thing to stop before we
		   release the memory */
		for (i = 0; i < numBufs; i++)
			sound.mach.dma_free (sound_read_buffers[i],
					     bufSize << 10);
		kfree (sound_read_buffers);
		sound_read_buffers = 0;
	}
}
#endif


static void sq_setup(int numBufs, int bufSize, char **write_buffers)
{
#ifdef CONFIG_PPC
	int i;
	volatile struct dbdma_cmd *cp;
#endif /* CONFIG_PPC */

	sq.max_count = numBufs;
	sq.max_active = numBufs;
	sq.block_size = bufSize;
	sq.buffers = write_buffers;

	sq.front = sq.count = 0;
	sq.rear = -1;
	sq.syncing = 0;
	sq.active = 0;

#ifdef CONFIG_ATARI
	sq.ignore_int = 0;
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	sq.block_size_half = sq.block_size>>1;
	sq.block_size_quarter = sq.block_size_half>>1;
#endif /* CONFIG_AMIGA */
#ifdef CONFIG_PPC
	cp = awacs_tx_cmds;
	memset((void *) cp, 0, (numBufs + 1) * sizeof(struct dbdma_cmd));
	for (i = 0; i < numBufs; ++i, ++cp) {
		st_le32(&cp->phy_addr, virt_to_bus(write_buffers[i]));
	}
	st_le16(&cp->command, DBDMA_NOP + BR_ALWAYS);
	st_le32(&cp->cmd_dep, virt_to_bus(awacs_tx_cmds));
	out_le32(&awacs_txdma->control, (RUN|PAUSE|FLUSH|WAKE) << 16);
	out_le32(&awacs_txdma->cmdptr, virt_to_bus(awacs_tx_cmds));
#endif /* CONFIG_PPC */
}

#ifdef CONFIG_PPC
static void read_sq_setup(int numBufs, int bufSize, char **read_buffers)
{
	int i;
	volatile struct dbdma_cmd *cp;

	read_sq.max_count = numBufs;
	read_sq.max_active = numBufs;
	read_sq.block_size = bufSize;
	read_sq.buffers = read_buffers;

	read_sq.front = read_sq.count = 0;
	read_sq.rear = 0;
	read_sq.rear_size = 0;
	read_sq.syncing = 0;
	read_sq.active = 0;

	cp = awacs_rx_cmds;
	memset((void *) cp, 0, (numBufs + 1) * sizeof(struct dbdma_cmd));

	/* Set dma buffers up in a loop */
	for (i = 0; i < numBufs; i++,cp++) {
		st_le32(&cp->phy_addr, virt_to_bus(read_buffers[i]));
		st_le16(&cp->command, INPUT_MORE + INTR_ALWAYS);
		st_le16(&cp->req_count, read_sq.block_size);
		st_le16(&cp->xfer_status, 0);
	}

	/* The next two lines make the thing loop around.
	*/
	st_le16(&cp->command, DBDMA_NOP + BR_ALWAYS);
	st_le32(&cp->cmd_dep, virt_to_bus(awacs_rx_cmds));

	/* Don't start until the first read is done.
	 * This will also abort any operations in progress if the DMA
	 * happens to be running (and it shouldn't).
	 */
	out_le32(&awacs_rxdma->control, (RUN|PAUSE|FLUSH|WAKE) << 16);
	out_le32(&awacs_rxdma->cmdptr, virt_to_bus(awacs_rx_cmds));

}
#endif /* CONFIG_PPC */


static void sq_play(void)
{
	(*sound.mach.play)();
}


/* ++TeSche: radically changed this one too */

static ssize_t sq_write(struct file *file, const char *src, size_t uLeft,
			loff_t *ppos)
{
	ssize_t uWritten = 0;
	u_char *dest;
	ssize_t uUsed, bUsed, bLeft;

	/* ++TeSche: Is something like this necessary?
	 * Hey, that's an honest question! Or does any other part of the
	 * filesystem already checks this situation? I really don't know.
	 */
	if (uLeft == 0)
		return 0;

	/* The interrupt doesn't start to play the last, incomplete frame.
	 * Thus we can append to it without disabling the interrupts! (Note
	 * also that sq.rear isn't affected by the interrupt.)
	 */

	if (sq.count > 0 && (bLeft = sq.block_size-sq.rear_size) > 0) {
		dest = sq_block_address(sq.rear);
		bUsed = sq.rear_size;
		uUsed = sound_copy_translate(src, uLeft, dest, &bUsed, bLeft);
		if (uUsed <= 0)
			return uUsed;
		src += uUsed;
		uWritten += uUsed;
		uLeft -= uUsed;
		sq.rear_size = bUsed;
	}

	do {
		while (sq.count == sq.max_active) {
			sq_play();
			if (NON_BLOCKING(sq.open_mode))
				return uWritten > 0 ? uWritten : -EAGAIN;
			SLEEP(sq.action_queue, ONE_SECOND);
			if (SIGNAL_RECEIVED)
				return uWritten > 0 ? uWritten : -EINTR;
		}

		/* Here, we can avoid disabling the interrupt by first
		 * copying and translating the data, and then updating
		 * the sq variables. Until this is done, the interrupt
		 * won't see the new frame and we can work on it
		 * undisturbed.
		 */

		dest = sq_block_address((sq.rear+1) % sq.max_count);
		bUsed = 0;
		bLeft = sq.block_size;
		uUsed = sound_copy_translate(src, uLeft, dest, &bUsed, bLeft);
		if (uUsed <= 0)
			break;
		src += uUsed;
		uWritten += uUsed;
		uLeft -= uUsed;
		if (bUsed) {
			sq.rear = (sq.rear+1) % sq.max_count;
			sq.rear_size = bUsed;
			sq.count++;
		}
	} while (bUsed);   /* uUsed may have been 0 */

	sq_play();

	return uUsed < 0? uUsed: uWritten;
}


/***********/

#ifdef CONFIG_PPC

/* Here is how the values are used for reading.
 * The value 'active' simply indicates the DMA is running.  This is
 * done so the driver semantics are DMA starts when the first read is
 * posted.  The value 'front' indicates the buffer we should next
 * send to the user.  The value 'rear' indicates the buffer the DMA is
 * currently filling.  When 'front' == 'rear' the buffer "ring" is
 * empty (we always have an empty available).  The 'rear_size' is used
 * to track partial offsets into the current buffer.  Right now, I just keep
 * the DMA running.  If the reader can't keep up, the interrupt tosses
 * the oldest buffer.  We could also shut down the DMA in this case.
 */
static ssize_t sq_read(struct file *file, char *dst, size_t uLeft,
                       loff_t *ppos)
{

	ssize_t	uRead, bLeft, bUsed, uUsed;

	if (uLeft == 0)
		return 0;

	if (!read_sq.active)
		PMacRecord();	/* Kick off the record process. */

	uRead = 0;

	/* Move what the user requests, depending upon other options.
	*/
	while (uLeft > 0) {

		/* When front == rear, the DMA is not done yet.
		*/
		while (read_sq.front == read_sq.rear) {
			if (NON_BLOCKING(read_sq.open_mode)) {
			       return uRead > 0 ? uRead : -EAGAIN;
			}
			SLEEP(read_sq.action_queue, ONE_SECOND);
			if (SIGNAL_RECEIVED)
				return uRead > 0 ? uRead : -EINTR;
		}

		/* The amount we move is either what is left in the
		 * current buffer or what the user wants.
		 */
		bLeft = read_sq.block_size - read_sq.rear_size;
		bUsed = read_sq.rear_size;
		uUsed = sound_copy_translate_read(dst, uLeft,
			read_sq.buffers[read_sq.front], &bUsed, bLeft);
		if (uUsed <= 0)
			return uUsed;
		dst += uUsed;
		uRead += uUsed;
		uLeft -= uUsed;
		read_sq.rear_size += bUsed;
		if (read_sq.rear_size >= read_sq.block_size) {
			read_sq.rear_size = 0;
			read_sq.front++;
			if (read_sq.front >= read_sq.max_active)
				read_sq.front = 0;
		}
	}
	return uRead;
}
#endif

static int sq_open(struct inode *inode, struct file *file)
{
	int rc = 0;

	MOD_INC_USE_COUNT;
	if (file->f_mode & FMODE_WRITE) {
		if (sq.busy) {
			rc = -EBUSY;
			if (NON_BLOCKING(file->f_flags))
				goto err_out;
			rc = -EINTR;
			while (sq.busy) {
				SLEEP(sq.open_queue, ONE_SECOND);
				if (SIGNAL_RECEIVED)
					goto err_out;
			}
		}
		sq.busy = 1; /* Let's play spot-the-race-condition */

		if (sq_allocate_buffers()) goto err_out_nobusy;

		sq_setup(numBufs, bufSize<<10,sound_buffers);
		sq.open_mode = file->f_mode;
	}


#ifdef CONFIG_PPC
	if (file->f_mode & FMODE_READ) {
		if (read_sq.busy) {
			rc = -EBUSY;
			if (NON_BLOCKING(file->f_flags))
				goto err_out;
			rc = -EINTR;
			while (read_sq.busy) {
				SLEEP(read_sq.open_queue, ONE_SECOND);
				if (SIGNAL_RECEIVED)
					goto err_out;
			}
			rc = 0;
		}
		read_sq.busy = 1;
		if (sq_allocate_read_buffers()) goto err_out_nobusy;

		read_sq_setup(numReadBufs,readbufSize<<10, sound_read_buffers);
		read_sq.open_mode = file->f_mode;
	}                                                                      
#endif

#ifdef CONFIG_ATARI
	sq.ignore_int = 1;
#endif /* CONFIG_ATARI */
	sound.minDev = MINOR(inode->i_rdev) & 0x0f;
	sound.soft = sound.dsp;
	sound.hard = sound.dsp;
	sound_init();
	if ((MINOR(inode->i_rdev) & 0x0f) == SND_DEV_AUDIO) {
		sound_set_speed(8000);
		sound_set_stereo(0);
		sound_set_format(AFMT_MU_LAW);
	}

#if 0
	if (file->f_mode == FMODE_READ) {
		/* Start dma'ing straight away */
		PMacRecord();
	}
#endif

	return 0;

err_out_nobusy:
	if (file->f_mode & FMODE_WRITE) {
		sq.busy = 0;
		WAKE_UP(sq.open_queue);
	}
#ifdef CONFIG_PPC
	if (file->f_mode & FMODE_READ) {
		read_sq.busy = 0;
		WAKE_UP(read_sq.open_queue);
	}
#endif
err_out:
	MOD_DEC_USE_COUNT;
	return rc;
}


static void sq_reset(void)
{
	sound_silence();
	sq.active = 0;
	sq.count = 0;
	sq.front = (sq.rear+1) % sq.max_count;
}


static int sq_fsync(struct file *filp, struct dentry *dentry)
{
	int rc = 0;

	sq.syncing = 1;
	sq_play();	/* there may be an incomplete frame waiting */

	while (sq.active) {
		SLEEP(sq.sync_queue, ONE_SECOND);
		if (SIGNAL_RECEIVED) {
			/* While waiting for audio output to drain, an
			 * interrupt occurred.  Stop audio output immediately
			 * and clear the queue. */
			sq_reset();
			rc = -EINTR;
			break;
		}
	}

	sq.syncing = 0;
	return rc;
}

static int sq_release(struct inode *inode, struct file *file)
{
	int rc = 0;

	if (sq.busy)
		rc = sq_fsync(file, file->f_dentry);
	sound.soft = sound.dsp;
	sound.hard = sound.dsp;
	sound_silence();

#ifdef CONFIG_PPC
	sq_release_read_buffers();
#endif
	sq_release_buffers();
	MOD_DEC_USE_COUNT;

	/* There is probably a DOS atack here. They change the mode flag. */
	/* XXX add check here */
#ifdef CONFIG_PPC
	if (file->f_mode & FMODE_READ) {
		read_sq.busy = 0;
		WAKE_UP(read_sq.open_queue);
	}
#endif

	if (file->f_mode & FMODE_WRITE) {
		sq.busy = 0;
		WAKE_UP(sq.open_queue);
	}

	/* Wake up a process waiting for the queue being released.
	 * Note: There may be several processes waiting for a call
	 * to open() returning. */

	return rc;
}


static int sq_ioctl(struct inode *inode, struct file *file, u_int cmd,
		    u_long arg)
{
	u_long fmt;
	int data;
	int size, nbufs;

	switch (cmd) {
	case SNDCTL_DSP_RESET:
		sq_reset();
		return 0;
	case SNDCTL_DSP_POST:
	case SNDCTL_DSP_SYNC:
		return sq_fsync(file, file->f_dentry);

		/* ++TeSche: before changing any of these it's
		 * probably wise to wait until sound playing has
		 * settled down. */
	case SNDCTL_DSP_SPEED:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_speed(data));
	case SNDCTL_DSP_STEREO:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_stereo(data));
	case SOUND_PCM_WRITE_CHANNELS:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_stereo(data-1)+1);
	case SNDCTL_DSP_SETFMT:
		sq_fsync(file, file->f_dentry);
		IOCTL_IN(arg, data);
		return IOCTL_OUT(arg, sound_set_format(data));
	case SNDCTL_DSP_GETFMTS:
		fmt = 0;
		if (sound.trans) {
			if (sound.trans->ct_ulaw)
				fmt |= AFMT_MU_LAW;
			if (sound.trans->ct_alaw)
				fmt |= AFMT_A_LAW;
			if (sound.trans->ct_s8)
				fmt |= AFMT_S8;
			if (sound.trans->ct_u8)
				fmt |= AFMT_U8;
			if (sound.trans->ct_s16be)
				fmt |= AFMT_S16_BE;
			if (sound.trans->ct_u16be)
				fmt |= AFMT_U16_BE;
			if (sound.trans->ct_s16le)
				fmt |= AFMT_S16_LE;
			if (sound.trans->ct_u16le)
				fmt |= AFMT_U16_LE;
		}
		return IOCTL_OUT(arg, fmt);
	case SNDCTL_DSP_GETBLKSIZE:
		size = sq.block_size
			* sound.soft.size * (sound.soft.stereo + 1)
			/ (sound.hard.size * (sound.hard.stereo + 1));
		return IOCTL_OUT(arg, size);
	case SNDCTL_DSP_SUBDIVIDE:
		break;
	case SNDCTL_DSP_SETFRAGMENT:
		if (sq.count || sq.active || sq.syncing)
			return -EINVAL;
		IOCTL_IN(arg, size);
		nbufs = size >> 16;
		if (nbufs < 2 || nbufs > numBufs)
			nbufs = numBufs;
		size &= 0xffff;
		if (size >= 8 && size <= 29) {
			size = 1 << size;
			size *= sound.hard.size * (sound.hard.stereo + 1);
			size /= sound.soft.size * (sound.soft.stereo + 1);
			if (size > (bufSize << 10))
				size = bufSize << 10;
		} else
			size = bufSize << 10;
		sq_setup(numBufs, size, sound_buffers);
		sq.max_active = nbufs;
		return 0;

	default:
		return mixer_ioctl(inode, file, cmd, arg);
	}
	return -EINVAL;
}



static struct file_operations sq_fops =
{
	sound_lseek,
#ifdef CONFIG_PPC
	sq_read,			/* sq_read */
#else
	NULL,			/* sq_read */
#endif
	sq_write,
	NULL,			/* sq_readdir */
	NULL,			/* sq_poll */
	sq_ioctl,
	NULL,			/* sq_mmap */
	sq_open,
	NULL,			/* flush */
	sq_release,
};


static void __init sq_init(void)
{
#ifndef MODULE
	int sq_unit;
#endif
	sq_unit = register_sound_dsp(&sq_fops, -1);
	if (sq_unit < 0)
		return;

	init_waitqueue_head(&sq.action_queue);
	init_waitqueue_head(&sq.open_queue);
	init_waitqueue_head(&sq.sync_queue);

#ifdef CONFIG_PPC
	init_waitqueue_head(&read_sq.action_queue);
	init_waitqueue_head(&read_sq.open_queue);
	init_waitqueue_head(&read_sq.sync_queue);
#endif

	sq.busy = 0;
#ifdef CONFIG_PPC
	read_sq.busy = 0;
#endif

	/* whatever you like as startup mode for /dev/dsp,
	 * (/dev/audio hasn't got a startup mode). note that
	 * once changed a new open() will *not* restore these!
	 */
	sound.dsp.format = AFMT_U8;
	sound.dsp.stereo = 0;
	sound.dsp.size = 8;

	/* set minimum rate possible without expanding */
	switch (sound.mach.type) {
#ifdef CONFIG_ATARI
	case DMASND_TT:
		sound.dsp.speed = 6258;
		break;
	case DMASND_FALCON:
		sound.dsp.speed = 8195;
		break;
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	case DMASND_AMIGA:
		sound.dsp.speed = 8000;
		break;
#endif /* CONFIG_AMIGA */
#ifdef CONFIG_PPC
	case DMASND_AWACS:
		sound.dsp.speed = 8000;
		break;
#endif /* CONFIG_PPC */
	}

	/* before the first open to /dev/dsp this wouldn't be set */
	sound.soft = sound.dsp;
	sound.hard = sound.dsp;

	sound_silence();
}

/*
 * /dev/sndstat
 */


/* state.buf should not overflow! */

static int state_open(struct inode *inode, struct file *file)
{
	char *buffer = state.buf, *mach = "";
#ifdef CONFIG_PPC
	char awacs_buf[50];
#endif
	int len = 0;

	if (state.busy)
		return -EBUSY;

	MOD_INC_USE_COUNT;
	state.ptr = 0;
	state.busy = 1;

	switch (sound.mach.type) {
#ifdef CONFIG_ATARI
	case DMASND_TT:
	case DMASND_FALCON:
		mach = "Atari ";
		break;
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	case DMASND_AMIGA:
		mach = "Amiga ";
		break;
#endif /* CONFIG_AMIGA */
#ifdef CONFIG_PPC
	case DMASND_AWACS:
		sprintf(awacs_buf, "PowerMac (AWACS rev %d) ", awacs_revision);
		mach = awacs_buf;
		break;
#endif /* CONFIG_PPC */
	}
	len += sprintf(buffer+len, "%sDMA sound driver:\n", mach);

	len += sprintf(buffer+len, "\tsound.format = 0x%x", sound.soft.format);
	switch (sound.soft.format) {
	case AFMT_MU_LAW:
		len += sprintf(buffer+len, " (mu-law)");
		break;
	case AFMT_A_LAW:
		len += sprintf(buffer+len, " (A-law)");
		break;
	case AFMT_U8:
		len += sprintf(buffer+len, " (unsigned 8 bit)");
		break;
	case AFMT_S8:
		len += sprintf(buffer+len, " (signed 8 bit)");
		break;
	case AFMT_S16_BE:
		len += sprintf(buffer+len, " (signed 16 bit big)");
		break;
	case AFMT_U16_BE:
		len += sprintf(buffer+len, " (unsigned 16 bit big)");
		break;
	case AFMT_S16_LE:
		len += sprintf(buffer+len, " (signed 16 bit little)");
		break;
	case AFMT_U16_LE:
		len += sprintf(buffer+len, " (unsigned 16 bit little)");
		break;
	}
	len += sprintf(buffer+len, "\n");
	len += sprintf(buffer+len, "\tsound.speed = %dHz (phys. %dHz)\n",
		       sound.soft.speed, sound.hard.speed);
	len += sprintf(buffer+len, "\tsound.stereo = 0x%x (%s)\n",
		       sound.soft.stereo, sound.soft.stereo ? "stereo" : "mono");
	switch (sound.mach.type) {
#ifdef CONFIG_ATARI
	case DMASND_TT:
		len += sprintf(buffer+len, "\tsound.volume_left = %ddB [-40...0]\n",
			       sound.volume_left);
		len += sprintf(buffer+len, "\tsound.volume_right = %ddB [-40...0]\n",
			       sound.volume_right);
		len += sprintf(buffer+len, "\tsound.bass = %ddB [-12...+12]\n",
			       sound.bass);
		len += sprintf(buffer+len, "\tsound.treble = %ddB [-12...+12]\n",
			       sound.treble);
		break;
	case DMASND_FALCON:
		len += sprintf(buffer+len, "\tsound.volume_left = %ddB [-22.5...0]\n",
			       sound.volume_left);
		len += sprintf(buffer+len, "\tsound.volume_right = %ddB [-22.5...0]\n",
			       sound.volume_right);
		break;
#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	case DMASND_AMIGA:
		len += sprintf(buffer+len, "\tsound.volume_left = %d [0...64]\n",
			       sound.volume_left);
		len += sprintf(buffer+len, "\tsound.volume_right = %d [0...64]\n",
			       sound.volume_right);
		break;
#endif /* CONFIG_AMIGA */
	}
	len += sprintf(buffer+len, "\tsq.block_size = %d sq.max_count = %d"
		       " sq.max_active = %d\n",
		       sq.block_size, sq.max_count, sq.max_active);
	len += sprintf(buffer+len, "\tsq.count = %d sq.rear_size = %d\n", sq.count,
		       sq.rear_size);
	len += sprintf(buffer+len, "\tsq.active = %d sq.syncing = %d\n",
		       sq.active, sq.syncing);
	state.len = len;
	return 0;
}


static int state_release(struct inode *inode, struct file *file)
{
	state.busy = 0;
	MOD_DEC_USE_COUNT;
	return 0;
}


static ssize_t state_read(struct file *file, char *buf, size_t count,
			  loff_t *ppos)
{
	int n = state.len - state.ptr;
	if (n > count)
		n = count;
	if (n <= 0)
		return 0;
	if (copy_to_user(buf, &state.buf[state.ptr], n))
		return -EFAULT;
	state.ptr += n;
	return n;
}


static struct file_operations state_fops =
{
	sound_lseek,
	state_read,
	NULL,			/* state_write */
	NULL,			/* state_readdir */
	NULL,			/* state_poll */
	NULL,			/* state_ioctl */
	NULL,			/* state_mmap */
	state_open,
	NULL,			/* flush */
	state_release,
};


static void __init state_init(void)
{
#ifndef MODULE
	int state_unit;
#endif
	state_unit = register_sound_special(&state_fops, SND_DEV_STATUS);
	if (state_unit < 0)
		return;
	state.busy = 0;
}


/*** Common stuff ********************************************************/

static long long sound_lseek(struct file *file, long long offset, int orig)
{
	return -ESPIPE;
}


/*** Config & Setup **********************************************************/


void __init dmasound_init(void)
{
	int has_sound = 0;
#ifdef CONFIG_PPC
	struct device_node *np;
#endif

#if defined(__mc68000__) || defined(CONFIG_APUS)
	switch (m68k_machtype) {
#ifdef CONFIG_ATARI
	case MACH_ATARI:
		if (ATARIHW_PRESENT(PCM_8BIT)) {
			if (ATARIHW_PRESENT(CODEC))
				sound.mach = machFalcon;
			else if (ATARIHW_PRESENT(MICROWIRE))
				sound.mach = machTT;
			else
				break;
			if ((mfp.int_en_a & mfp.int_mk_a & 0x20) == 0)
				has_sound = 1;
			else
				printk("DMA sound driver: Timer A interrupt already in use\n");
		}
		break;

#endif /* CONFIG_ATARI */
#ifdef CONFIG_AMIGA
	case MACH_AMIGA:
		if (AMIGAHW_PRESENT(AMI_AUDIO)) {
			sound.mach = machAmiga;
			has_sound = 1;
		}
		break;
#endif /* CONFIG_AMIGA */
	}
#endif /* __mc68000__||CONFIG_APUS */

#ifdef CONFIG_PPC
	awacs_subframe = 0;
	awacs_revision = 0;
	np = find_devices("awacs");
	if (np == 0) {
		/*
		 * powermac G3 models have a node called "davbus"
		 * with a child called "sound".
		 */
		struct device_node *sound;
		np = find_devices("davbus");
		sound = find_devices("sound");
		if (sound != 0 && sound->parent == np) {
			unsigned int *prop, l, i;
			prop = (unsigned int *)
				get_property(sound, "sub-frame", 0);
			if (prop != 0 && *prop >= 0 && *prop < 16)
				awacs_subframe = *prop;
			if (device_is_compatible(sound, "burgundy"))
				awacs_revision = AWACS_BURGUNDY;

			/* look for a property saying what sample rates
			   are available */
			for (i = 0; i < 8; ++i)
				awacs_freqs_ok[i] = 0;
			prop = (unsigned int *) get_property
				(sound, "sample-rates", &l);
			if (prop == 0)
				prop = (unsigned int *) get_property
					(sound, "output-frame-rates", &l);
			if (prop != 0) {
				for (l /= sizeof(int); l > 0; --l) {
					/* sometimes the rate is in the
					   high-order 16 bits (?) */
					unsigned int r = *prop++;
					if (r >= 0x10000)
						r >>= 16;
					for (i = 0; i < 8; ++i) {
						if (r == awacs_freqs[i]) {
							awacs_freqs_ok[i] = 1;
							break;
						}
					}
				}
			} else {
				/* assume just 44.1k is OK */
				awacs_freqs_ok[0] = 1;
			}
		}
	}
	if (np != NULL && np->n_addrs >= 3 && np->n_intrs >= 3) {
		int vol;
		sound.mach = machPMac;
		has_sound = 1;

		awacs = (volatile struct awacs_regs *)
			ioremap(np->addrs[0].address, 0x80);
		awacs_txdma = (volatile struct dbdma_regs *)
			ioremap(np->addrs[1].address, 0x100);
		awacs_rxdma = (volatile struct dbdma_regs *)
			ioremap(np->addrs[2].address, 0x100);

		awacs_irq = np->intrs[0].line;
		awacs_tx_irq = np->intrs[1].line;
		awacs_rx_irq = np->intrs[2].line;

		awacs_tx_cmd_space = kmalloc((numBufs + 4) * sizeof(struct dbdma_cmd),
					     GFP_KERNEL);
		if (awacs_tx_cmd_space == NULL) {
			printk(KERN_ERR "DMA sound driver: Not enough buffer memory, driver disabled!\n");
			return;
		}
		awacs_tx_cmds = (volatile struct dbdma_cmd *)
			DBDMA_ALIGN(awacs_tx_cmd_space);


		awacs_rx_cmd_space = kmalloc((numReadBufs + 4) * sizeof(struct dbdma_cmd),
					     GFP_KERNEL);
		if (awacs_rx_cmd_space == NULL) {
		  printk("DMA sound driver: No memory for input");
		}
		awacs_rx_cmds = (volatile struct dbdma_cmd *)
		  DBDMA_ALIGN(awacs_rx_cmd_space);



		awacs_reg[0] = MASK_MUX_CD;
		awacs_reg[1] = MASK_LOOPTHRU | MASK_PAROUT;
		/* get default volume from nvram */
		vol = (~nvram_read_byte(0x1308) & 7) << 1;
		awacs_reg[2] = vol + (vol << 6);
		awacs_reg[4] = vol + (vol << 6);
		out_le32(&awacs->control, 0x11);
		awacs_write(awacs_reg[0] + MASK_ADDR0);
		awacs_write(awacs_reg[1] + MASK_ADDR1);
		awacs_write(awacs_reg[2] + MASK_ADDR2);
		awacs_write(awacs_reg[4] + MASK_ADDR4);

		/* Initialize recent versions of the awacs */
		if (awacs_revision == 0) {
			awacs_revision =
				(in_le32(&awacs->codec_stat) >> 12) & 0xf;
			if (awacs_revision == 3) {
				awacs_write(0x6000);
				awacs_enable_amp(100 * 0x101);
			}
		}
		if (awacs_revision >= AWACS_BURGUNDY)
			awacs_burgundy_init();

		/* Initialize beep stuff */
		beep_dbdma_cmd = awacs_tx_cmds + (numBufs + 1);
		orig_mksound = kd_mksound;
		kd_mksound = awacs_mksound;
		beep_buf = (short *) kmalloc(BEEP_BUFLEN * 4, GFP_KERNEL);
		if (beep_buf == NULL)
			printk(KERN_WARNING "dmasound: no memory for "
			       "beep buffer\n");
#ifdef CONFIG_PMAC_PBOOK
		pmu_register_sleep_notifier(&awacs_sleep_notifier);
#endif /* CONFIG_PMAC_PBOOK */

		/* Powerbooks have odd ways of enabling inputs such as
		   an expansion-bay CD or sound from an internal modem
		   or a PC-card modem. */
		if (machine_is_compatible("AAPL,3400/2400")) {
			is_pbook_3400 = 1;
			/*
			 * Enable CD and PC-card sound inputs.
			 * This is done by reading from address
			 * f301a000, + 0x10 to enable the expansion-bay
			 * CD sound input, + 0x80 to enable the PC-card
			 * sound input.  The 0x100 seems to enable the
			 * MESH and/or its SCSI bus drivers.
			 */
			in_8((unsigned char *)0xf301a190);
		} else if (machine_is_compatible("PowerBook1,1")) {
			np = find_devices("mac-io");
			if (np && np->n_addrs > 0) {
				is_pbook_G3 = 1;
				macio_base = (unsigned char *)
					ioremap(np->addrs[0].address, 0x40);
				/* enable CD sound input */
				out_8(macio_base + 0x37, 3);
			}
		}
	}
#endif /* CONFIG_PPC */

	if (!has_sound)
		return;

	/* Set up sound queue, /dev/audio and /dev/dsp. */

	/* Set default settings. */
	sq_init();

	/* Set up /dev/sndstat. */
	state_init();

	/* Set up /dev/mixer. */
	mixer_init();

	if (!sound.mach.irqinit()) {
		printk(KERN_ERR "DMA sound driver: Interrupt initialization failed\n");
		return;
	}
#ifdef MODULE
	irq_installed = 1;
#endif

	printk(KERN_INFO "DMA sound driver installed, using %d buffers of %dk.\n",
	       numBufs, bufSize);

	return;
}


#define MAXARGS		8	/* Should be sufficient for now */

void __init dmasound_setup(char *str, int *ints)
{
	/* check the bootstrap parameter for "dmasound=" */

	switch (ints[0]) {
	case 3:
		if ((ints[3] < 0) || (ints[3] > MAX_CATCH_RADIUS))
			printk("dmasound_setup: illegal catch radius, using default = %d\n", catchRadius);
		else
			catchRadius = ints[3];
		/* fall through */
	case 2:
		if (ints[1] < MIN_BUFFERS)
			printk("dmasound_setup: illegal number of buffers, using default = %d\n", numBufs);
		else
			numBufs = ints[1];
		if (ints[2] < MIN_BUFSIZE || ints[2] > MAX_BUFSIZE)
			printk("dmasound_setup: illegal buffer size, using default = %d\n", bufSize);
		else
			bufSize = ints[2];
		break;
	case 0:
		break;
	default:
		printk("dmasound_setup: illegal number of arguments\n");
	}
}


#ifdef MODULE

int init_module(void)
{
	dmasound_init();
	return 0;
}


void cleanup_module(void)
{
	if (irq_installed) {
		sound_silence();
		sound.mach.irqcleanup();
	}

#ifdef CONFIG_PPC
	sq_release_read_buffers();
#endif
	sq_release_buffers();

	if (mixer_unit >= 0)
		unregister_sound_mixer(mixer_unit);
	if (state_unit >= 0)
		unregister_sound_special(state_unit);
	if (sq_unit >= 0)
		unregister_sound_dsp(sq_unit);
}

#endif /* MODULE */
