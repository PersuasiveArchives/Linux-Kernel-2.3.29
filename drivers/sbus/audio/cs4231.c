/*
 * drivers/sbus/audio/cs4231.c
 *
 * Copyright 1996, 1997, 1998, 1999 Derrick J Brashear (shadow@andrew.cmu.edu)
 * The 4231/ebus support was written by David Miller, who didn't bother
 * crediting himself here, so I will.
 *
 * Based on the AMD7930 driver:
 * Copyright 1996 Thomas K. Dyas (tdyas@noc.rutgers.edu)
 *
 * This is the lowlevel driver for the CS4231 audio chip found on some
 * sun4m and sun4u machines.
 * 
 * This was culled from the Crystal docs on the 4231a, and the addendum they
 * faxed me on the 4231.
 * The APC DMA controller support unfortunately is not documented. Thanks, Sun.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/soundcard.h>
#include <linux/version.h>
#include <linux/ioport.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/sbus.h>
#if defined (LINUX_VERSION_CODE) && LINUX_VERSION_CODE > 0x200ff && defined(CONFIG_PCI)
#define EB4231_SUPPORT
#include <asm/ebus.h>
#include <asm/pbm.h>
#endif

#include <asm/audioio.h>
#include "cs4231.h"

#undef __CS4231_DEBUG
#undef __CS4231_TRACE
#define __CS4231_ERROR
#ifdef __CS4231_ERROR
#define eprintk(x) printk x
#else
#define eprintk(x)
#endif
#ifdef __CS4231_TRACE
#define tprintk(x) printk x
#else
#define tprintk(x)
#endif
#ifdef __CS4231_DEBUG
#define dprintk(x) printk x
#else
#define dprintk(x)
#endif

#define MAX_DRIVERS 1
static struct sparcaudio_driver drivers[MAX_DRIVERS];
static int num_drivers;

static int cs4231_record_gain(struct sparcaudio_driver *drv, int value, 
                              unsigned char balance);
static int cs4231_play_gain(struct sparcaudio_driver *drv, int value, 
                            unsigned char balance);
static void cs4231_ready(struct sparcaudio_driver *drv);
static void cs4231_playintr(struct sparcaudio_driver *drv, int);
static int cs4231_recintr(struct sparcaudio_driver *drv);
static int cs4231_output_muted(struct sparcaudio_driver *drv, int value);
static void cs4231_pollinput(struct sparcaudio_driver *drv);
static int cs4231_length_to_samplecount(struct audio_prinfo *thisdir, 
                                        unsigned int length);
static void cs4231_getsamplecount(struct sparcaudio_driver *drv, 
                                  unsigned int length, unsigned int value);
#ifdef EB4231_SUPPORT
static void eb4231_pollinput(struct sparcaudio_driver *drv);
#endif

#define CHIP_READY udelay(100); cs4231_ready(drv); udelay(1000);

/* Enable cs4231 interrupts atomically. */
static __inline__ void cs4231_enable_interrupts(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  register unsigned long flags;

  if (cs4231_chip->status & CS_STATUS_INTS_ON)
    return;

  tprintk(("enabling interrupts\n"));
  save_flags(flags);
  cli();
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0xa);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), INTR_ON);
  restore_flags(flags);

  cs4231_chip->status |= CS_STATUS_INTS_ON;
}

/* Disable cs4231 interrupts atomically. */
static __inline__ void cs4231_disable_interrupts(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  register unsigned long flags;

  if (!(cs4231_chip->status & CS_STATUS_INTS_ON))
    return;

  tprintk(("disabling interrupts\n"));
  save_flags(flags);
  cli();
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0xa);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), INTR_OFF);
  restore_flags(flags);

  cs4231_chip->status &= ~CS_STATUS_INTS_ON;
}

static __inline__ void cs4231_enable_play(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  register unsigned long flags;

  tprintk(("enabling play\n"));
  save_flags(flags);
  cli();
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x9);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | PEN_ENABLE));
  restore_flags(flags);
}

static __inline__ void cs4231_disable_play(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  register unsigned long flags;

  tprintk(("disabling play\n"));
  save_flags(flags);
  cli();
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x9);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & PEN_DISABLE));
  restore_flags(flags);
}

static __inline__ void cs4231_enable_rec(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  register unsigned long flags;

  tprintk(("enabling rec\n"));
  save_flags(flags);
  cli();
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x9);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | CEN_ENABLE));
  restore_flags(flags);
}

static __inline__ void cs4231_disable_rec(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  register unsigned long flags;

  tprintk(("disabling rec\n"));
  save_flags(flags);
  cli();
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x9);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & CEN_DISABLE));
  restore_flags(flags);
}

static struct cs4231_rates {
  int speed, bits;
} cs4231_rate_table[] = {
  { 5512, CS4231_DFR_5512 },
  { 6615, CS4231_DFR_6615 },
  { 8000, CS4231_DFR_8000 },
  { 9600, CS4231_DFR_9600 },
  { 11025, CS4231_DFR_11025 },
  { 16000, CS4231_DFR_16000 },
  { 18900, CS4231_DFR_18900 },
  { 22050, CS4231_DFR_22050 },
  { 27429, CS4231_DFR_27429 },
  { 32000, CS4231_DFR_32000 },
  { 33075, CS4231_DFR_33075 },
  { 37800, CS4231_DFR_37800 },
  { 44100, CS4231_DFR_44100 },
  { 48000, CS4231_DFR_48000 }
};

#define NUM_RATES	(sizeof(cs4231_rate_table) / sizeof(struct cs4231_rates))

static int 
cs4231_rate_to_bits(struct sparcaudio_driver *drv, int *value)
{
  struct cs4231_rates *p = &cs4231_rate_table[0];
  int i, wanted = *value;

  /* We try to be nice and approximate what the user asks for. */
  if(wanted < 5512)
    wanted = 5512;
  if(wanted > 48000)
    wanted = 48000;

  for(i = 0; i < NUM_RATES; i++, p++) {
    /* Exact match? */
    if(wanted == p->speed)
      break;

    /* If we're inbetween two entries, and neither is exact,
     * pick the closest one.
     */
    if(wanted == p[1].speed)
      continue;
    if(wanted > p->speed &&
       wanted < p[1].speed) {
      int diff1, diff2;

      diff1 = wanted - p->speed;
      diff2 = p[1].speed - wanted;
      if(diff2 < diff1)
        p++;
      break;
    }
  }
  *value = p->speed;
  return p->bits;
}

static int 
cs4231_encoding_to_bits(struct sparcaudio_driver *drv, int value)
{
  int set_bits;
  
  switch (value) {
  case AUDIO_ENCODING_ULAW:
    set_bits = CS4231_DFR_ULAW;
    break;
  case AUDIO_ENCODING_ALAW:
    set_bits = CS4231_DFR_ALAW;
    break;
  case AUDIO_ENCODING_DVI:
    set_bits = CS4231_DFR_ADPCM;
    break;
  case AUDIO_ENCODING_LINEARLE:
    set_bits = CS4231_DFR_LINEARLE;
    break;
  case AUDIO_ENCODING_LINEAR:
    set_bits = CS4231_DFR_LINEARBE;
    break;
  case AUDIO_ENCODING_LINEAR8:
    set_bits = CS4231_DFR_LINEAR8;
    break;
  default:
    set_bits = -(EINVAL);
    break;
  }
  
  return set_bits;
}

static int
cs4231_set_output_encoding(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp_bits, set_bits;

  tprintk(("output encoding %d\n", value));
  if (value != 0) {
    set_bits = cs4231_encoding_to_bits(drv, value);
    if (set_bits >= 0) {
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x8);
        tmp_bits = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                      CHANGE_ENCODING(tmp_bits, set_bits));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
    
        CHIP_READY

        cs4231_chip->perchip_info.play.encoding = value;
        return 0;
    }
  }
  dprintk(("output enc failed\n"));
  return -EINVAL;
}

static int cs4231_get_output_encoding(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.play.encoding;
}

static int
cs4231_set_input_encoding(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp_bits, set_bits;

  tprintk(("input encoding %d\n", value));
  if (value != 0) {
    set_bits = cs4231_encoding_to_bits(drv, value);
    if (set_bits >= 0) {
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x1c);
        tmp_bits = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                      CHANGE_ENCODING(tmp_bits, set_bits));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));

        CHIP_READY

        cs4231_chip->perchip_info.record.encoding = value;
        return 0;
    }
  }
  dprintk(("input enc failed\n"));
  return -EINVAL;
}

static int cs4231_get_input_encoding(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.record.encoding;
}

static int
cs4231_set_output_rate(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp_bits, set_bits;

  tprintk(("output rate %d\n", value));
  if (value != 0) {
    set_bits = cs4231_rate_to_bits(drv, &value);
    if (set_bits >= 0) {
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x8);
        tmp_bits = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                      CHANGE_DFR(tmp_bits, set_bits));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));

        CHIP_READY

        cs4231_chip->perchip_info.play.sample_rate = value;
        tprintk(("tmp_bits[%02x] set_bits[%02x] CHANGE_DFR[%02x]\n",
                 tmp_bits, set_bits, CHANGE_DFR(tmp_bits, set_bits)));
        return 0;
    }
  }
  dprintk(("output rate failed\n"));
  return -EINVAL;
}

static int cs4231_get_output_rate(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.play.sample_rate;
}

static int
cs4231_set_input_rate(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp_bits, set_bits;

  tprintk(("input rate %d\n", value));
  if (value != 0) {
    set_bits = cs4231_rate_to_bits(drv, &value);
    if (set_bits >= 0) {
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x1c);
        tmp_bits = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                      CHANGE_DFR(tmp_bits, set_bits));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
        CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
      
        CHIP_READY

        cs4231_chip->perchip_info.record.sample_rate = value;
        return 0;
    }
  }
  dprintk(("input rate failed\n"));
  return -EINVAL;
}

static int cs4231_get_input_rate(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.record.sample_rate;
}

/* Generically we support 4 channels. This hardware does 2 */
static int
cs4231_set_input_channels(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp_bits;

  tprintk(("input channels %d\n", value));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), (IAR_AUTOCAL_BEGIN | 0x1c));
  tmp_bits = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  switch (value) {
  case 1:
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), CS4231_MONO_ON(tmp_bits));
      break;
  case 2:
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), CS4231_STEREO_ON(tmp_bits));
      break;
  default:
      dprintk(("input chan failed\n"));
      return -(EINVAL);
  }  

  CHIP_READY

  cs4231_chip->perchip_info.record.channels = value;
  return 0;
}

static int cs4231_get_input_channels(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.record.channels;
}

/* Generically we support 4 channels. This hardware does 2 */
static int
cs4231_set_output_channels(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp_bits;

  tprintk(("output channels %d\n", value));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x8);
  tmp_bits = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  switch (value) {
  case 1:
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), CS4231_MONO_ON(tmp_bits));
      break;
  case 2:
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), CS4231_STEREO_ON(tmp_bits));
      break;
  default:
      dprintk(("output chan failed\n"));
      return -(EINVAL);
  }  

  CHIP_READY
    
  cs4231_chip->perchip_info.play.channels = value;
  return 0;
}

static int cs4231_get_output_channels(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.play.channels;
}

static int cs4231_get_input_precision(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.record.precision;
}

static int cs4231_get_output_precision(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.play.precision;
}

static int cs4231_set_input_precision(struct sparcaudio_driver *drv, int val)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

      cs4231_chip->perchip_info.record.precision = val;

      return cs4231_chip->perchip_info.record.precision;
}

static int cs4231_set_output_precision(struct sparcaudio_driver *drv, int val)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private; 

      cs4231_chip->perchip_info.play.precision = val;

      return cs4231_chip->perchip_info.play.precision;
}

/* Wait until the auto calibration process has finished */
static void
cs4231_ready(struct sparcaudio_driver *drv) 
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned int x = 0;

  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_END);
  while (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) == IAR_NOT_READY &&
         x <= CS_TIMEOUT) {
    x++;
  }

  x = 0;
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0b);
  while (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) == AUTOCAL_IN_PROGRESS &&
         x <= CS_TIMEOUT) {
    x++;
  }
}

/* Set output mute */
static int cs4231_output_muted(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  tprintk(("in cs4231_output_muted: %d\n", value));
  if (!value) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x7);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & OUTCR_UNMUTE));
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x6);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & OUTCR_UNMUTE));
      cs4231_chip->perchip_info.output_muted = 0;
  } else {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x7);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | OUTCR_MUTE));
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x6);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | OUTCR_MUTE));
      cs4231_chip->perchip_info.output_muted = 1;
  }
  return 0;
}

static int cs4231_get_output_muted(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  return cs4231_chip->perchip_info.output_muted;
}

static int cs4231_get_formats(struct sparcaudio_driver *drv)
{
      return (AFMT_MU_LAW | AFMT_A_LAW | AFMT_U8 | AFMT_IMA_ADPCM | 
              AFMT_S16_LE | AFMT_S16_BE);
}

static int cs4231_get_output_ports(struct sparcaudio_driver *drv)
{
      return (AUDIO_LINE_OUT | AUDIO_SPEAKER | AUDIO_HEADPHONE);
}

static int cs4231_get_input_ports(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  /* This apparently applies only to APC ultras, not ebus ultras */
  if (cs4231_chip->status & CS_STATUS_IS_ULTRA)
    return (AUDIO_LINE_IN | AUDIO_MICROPHONE | AUDIO_ANALOG_LOOPBACK);
  else
    return (AUDIO_INTERNAL_CD_IN | AUDIO_LINE_IN | AUDIO_MICROPHONE | 
	    AUDIO_ANALOG_LOOPBACK);
}

/* Set chip "output" port */
static int cs4231_set_output_port(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int retval = 0;

  tprintk(("output port: %d\n", value));
  /* Aaaaaah! It's all coming so fast! Turn it all off, then selectively
   * enable things.
   */
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1a);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | MONO_IOCR_MUTE));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0a);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | PINCR_LINE_MUTE));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) | PINCR_HDPH_MUTE));

  if (value & AUDIO_SPEAKER) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1a);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & ~MONO_IOCR_MUTE));
      retval |= AUDIO_SPEAKER;
  }

  if (value & AUDIO_HEADPHONE) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0a);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & ~PINCR_HDPH_MUTE));
      retval |= AUDIO_HEADPHONE;
  }

  if (value & AUDIO_LINE_OUT) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0a);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & ~PINCR_LINE_MUTE));
      retval |= AUDIO_LINE_OUT;
  }
  
  cs4231_chip->perchip_info.play.port = retval;

  return (retval);
}

static int cs4231_get_output_port(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.play.port;
}

/* Set chip "input" port */
static int cs4231_set_input_port(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int retval = 0;

  tprintk(("input port: %d\n", value));

  /* You can have one and only one. This is probably wrong, but
   * appears to be how SunOS is doing it. Should be able to mix.
   * More work to be done. CD input mixable, analog loopback may be.
   */

  /* Ultra systems do not support AUDIO_INTERNAL_CD_IN */
  /* This apparently applies only to APC ultras, not ebus ultras */
  if (!(cs4231_chip->status & CS_STATUS_IS_ULTRA)) {
    if (value & AUDIO_INTERNAL_CD_IN) {
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1);
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                      CDROM_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0);
        CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                      CDROM_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
        retval = AUDIO_INTERNAL_CD_IN;
    }
  }
  if ((value & AUDIO_LINE_IN)) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    LINE_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    LINE_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
      retval = AUDIO_LINE_IN;
  } else if (value & AUDIO_MICROPHONE) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    MIC_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    MIC_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
      retval = AUDIO_MICROPHONE;
  } else if (value & AUDIO_ANALOG_LOOPBACK) {
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    OUTPUTLOOP_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0);
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                    OUTPUTLOOP_ENABLE(CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr))));
      retval = AUDIO_ANALOG_LOOPBACK;
  }

  cs4231_chip->perchip_info.record.port = retval;

  return (retval);
}

static int cs4231_get_input_port(struct sparcaudio_driver *drv)
{
      struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
      return cs4231_chip->perchip_info.record.port;
}

/* Set chip "monitor" gain */
static int cs4231_set_monitor_volume(struct sparcaudio_driver *drv, int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int a = 0;

  tprintk(("monitor gain: %d\n", value));

  /* This interpolation really sucks. The question is, be compatible 
   * with ScumOS/Sloaris or not?
   */
  a = CS4231_MON_MAX_ATEN - (value * (CS4231_MON_MAX_ATEN + 1) / 
                             (AUDIO_MAX_GAIN + 1));

  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0d);
  if (a >= CS4231_MON_MAX_ATEN) 
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), LOOPB_OFF);
  else 
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), ((a << 2) | LOOPB_ON));

  if (value == AUDIO_MAX_GAIN) 
    CS4231_WRITE8(cs4231_chip, &(cs4231_chip->perchip_info.monitor_gain), AUDIO_MAX_GAIN);
  else 
    CS4231_WRITE8(cs4231_chip, &(cs4231_chip->perchip_info.monitor_gain),
                  ((CS4231_MAX_DEV_ATEN - a) * 
                   (AUDIO_MAX_GAIN + 1) / 
                   (CS4231_MAX_DEV_ATEN + 1)));

  return 0;
}

static int cs4231_get_monitor_volume(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.monitor_gain;
}

static int cs4231_get_output_error(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.play.error;
}

static int cs4231_get_input_error(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.record.error;
}

#ifdef EB4231_SUPPORT
static int eb4231_get_output_samples(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
        int count = 
          cs4231_length_to_samplecount(&cs4231_chip->perchip_info.play, 
                                       readl(&cs4231_chip->eb2p->dbcr));

        return (cs4231_chip->perchip_info.play.samples - 
                ((count > cs4231_chip->perchip_info.play.samples) 
                 ? 0 : count));
}

static int eb4231_get_input_samples(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
        int count = 
          cs4231_length_to_samplecount(&cs4231_chip->perchip_info.record, 
                                       readl(&cs4231_chip->eb2c->dbcr));

        return (cs4231_chip->perchip_info.record.samples - 
                ((count > cs4231_chip->perchip_info.record.samples) ?
                0 : count));
}
#endif

static int cs4231_get_output_samples(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
        int count = 
          cs4231_length_to_samplecount(&cs4231_chip->perchip_info.play, 
                                       cs4231_chip->regs->dmapc);

        return (cs4231_chip->perchip_info.play.samples - 
                ((count > cs4231_chip->perchip_info.play.samples) 
                 ? 0 : count));
}

static int cs4231_get_input_samples(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
        int count = 
          cs4231_length_to_samplecount(&cs4231_chip->perchip_info.record, 
                                       cs4231_chip->regs->dmacc);

        return (cs4231_chip->perchip_info.record.samples - 
                ((count > cs4231_chip->perchip_info.record.samples) ?
                0 : count));
}

static int cs4231_get_output_pause(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.play.pause;
}

static int cs4231_get_input_pause(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.record.pause;
}

/* But for play/record we have these cheesy jacket routines because of 
 * how this crap gets set */
static int cs4231_set_input_volume(struct sparcaudio_driver *drv, int value)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

	cs4231_record_gain(drv, value, 
                           cs4231_chip->perchip_info.record.balance);
	
        return 0;
}

static int cs4231_get_input_volume(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.record.gain;
}

static int cs4231_set_output_volume(struct sparcaudio_driver *drv, int value)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

	cs4231_play_gain(drv, value, cs4231_chip->perchip_info.play.balance);
	
        return 0;
}

static int cs4231_get_output_volume(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return cs4231_chip->perchip_info.play.gain;
}

/* Likewise for balance */
static int cs4231_set_input_balance(struct sparcaudio_driver *drv, int value)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

	cs4231_chip->perchip_info.record.balance = value;
	cs4231_record_gain(drv, cs4231_chip->perchip_info.record.gain, 
                           cs4231_chip->perchip_info.record.balance);
	
        return 0;
}

static int cs4231_get_input_balance(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.record.balance;
}

static int cs4231_set_output_balance(struct sparcaudio_driver *drv, int value)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

	cs4231_chip->perchip_info.play.balance = value;
	cs4231_play_gain(drv, cs4231_chip->perchip_info.play.gain, 
                         cs4231_chip->perchip_info.play.balance);
	
        return 0;
}

static int cs4231_get_output_balance(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        return (int)cs4231_chip->perchip_info.play.balance;
}

/* Set chip record gain */
static int cs4231_record_gain(struct sparcaudio_driver *drv, int value, unsigned char balance)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp = 0, r, l, r_adj, l_adj;
  unsigned char old_gain;

  r = l = value;

  if (balance < AUDIO_MID_BALANCE) {
    r = (int)(value - ((AUDIO_MID_BALANCE - balance) << AUDIO_BALANCE_SHIFT));
    if (r < 0) r = 0;
  } else if (balance > AUDIO_MID_BALANCE) {
    l = (int)(value - ((balance - AUDIO_MID_BALANCE) << AUDIO_BALANCE_SHIFT));
    if (l < 0) l = 0;
  }

  l_adj = l * (CS4231_MAX_GAIN + 1) / (AUDIO_MAX_GAIN + 1);
  r_adj = r * (CS4231_MAX_GAIN + 1) / (AUDIO_MAX_GAIN + 1);
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0);
  old_gain = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), RECGAIN_SET(old_gain, l_adj));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x1);
  old_gain = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), RECGAIN_SET(old_gain, r_adj));
  
  if (l == value) {
    (l == 0) ? (tmp = 0) : (tmp = ((l_adj + 1) * AUDIO_MAX_GAIN) / 
                            (CS4231_MAX_GAIN + 1));
  } else if (r == value) {
    (r == 0) ? (tmp = 0) : (tmp = ((r_adj + 1) * AUDIO_MAX_GAIN) / 
                            (CS4231_MAX_GAIN + 1));
  }
  cs4231_chip->perchip_info.record.gain = tmp;
  return 0;
}

/* Set chip play gain */
static int cs4231_play_gain(struct sparcaudio_driver *drv, int value, unsigned char balance)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int tmp = 0, r, l, r_adj, l_adj;
  unsigned char old_gain;

  tprintk(("in play_gain: %d %c\n", value, balance));
  r = l = value;
  if (balance < AUDIO_MID_BALANCE) {
    r = (int)(value - ((AUDIO_MID_BALANCE - balance) << AUDIO_BALANCE_SHIFT));
    if (r < 0) r = 0;
  } else if (balance > AUDIO_MID_BALANCE) {
    l = (int)(value - ((balance - AUDIO_MID_BALANCE) << AUDIO_BALANCE_SHIFT));
    if (l < 0) l = 0;
  }

  (l == 0) ? (l_adj = CS4231_MAX_DEV_ATEN) : (l_adj = CS4231_MAX_ATEN - 
                                              (l * (CS4231_MAX_ATEN + 1) / 
                                               (AUDIO_MAX_GAIN + 1)));
  (r == 0) ? (r_adj = CS4231_MAX_DEV_ATEN) : (r_adj = CS4231_MAX_ATEN -
                                              (r * (CS4231_MAX_ATEN + 1) /
                                               (AUDIO_MAX_GAIN + 1)));
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x6);
  old_gain = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), GAIN_SET(old_gain, l_adj));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x7);
  old_gain = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), GAIN_SET(old_gain, r_adj));
  
  if ((value == 0) || (value == AUDIO_MAX_GAIN)) {
    tmp = value;
  } else {
    if (value == l)
      tmp = ((CS4231_MAX_ATEN - l_adj) * (AUDIO_MAX_GAIN + 1) / 
             (CS4231_MAX_ATEN + 1));
    else if (value == r)
      tmp = ((CS4231_MAX_ATEN - r_adj) * (AUDIO_MAX_GAIN + 1) / 
             (CS4231_MAX_ATEN + 1));
  }
  cs4231_chip->perchip_info.play.gain = tmp;

  return 0;
}

/* Reset the audio chip to a sane state. */
static void cs4231_chip_reset(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned char vers;

  tprintk(("in cs4231_chip_reset\n"));

  if (cs4231_chip->status & CS_STATUS_IS_EBUS) {
#ifdef EB4231_SUPPORT
    writel(EBUS_DCSR_RESET, &(cs4231_chip->eb2p->dcsr));
    writel(EBUS_DCSR_RESET, &(cs4231_chip->eb2c->dcsr));
    writel(EBUS_DCSR_BURST_SZ_16, &(cs4231_chip->eb2p->dcsr));
    writel(EBUS_DCSR_BURST_SZ_16, &(cs4231_chip->eb2c->dcsr));
#endif
  } else {
    cs4231_chip->regs->dmacsr = APC_CHIP_RESET;
    cs4231_chip->regs->dmacsr = 0x00;
    cs4231_chip->regs->dmacsr |= APC_CDC_RESET;
  
    udelay(20);
  
    cs4231_chip->regs->dmacsr &= ~(APC_CDC_RESET);
  }

  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->iar)) | IAR_AUTOCAL_BEGIN));
  
  CHIP_READY
    
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x0c);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), MISC_IR_MODE2);

  /* This is the equivalent of DEFAULT_DATA_FMAT */
  cs4231_set_input_encoding(drv, AUDIO_ENCODING_ULAW);
  cs4231_set_input_rate(drv, CS4231_RATE);
  cs4231_set_input_channels(drv, CS4231_CHANNELS);
  cs4231_set_input_precision(drv, CS4231_PRECISION);

  cs4231_set_output_encoding(drv, AUDIO_ENCODING_ULAW);
  cs4231_set_output_rate(drv, CS4231_RATE);
  cs4231_set_output_channels(drv, CS4231_CHANNELS);
  cs4231_set_output_precision(drv, CS4231_PRECISION);

  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x19);

  /* see what we can turn on */
  vers = CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr));
  if (vers & CS4231A) {
    tprintk(("This is a CS4231A\n"));
    cs4231_chip->status |= CS_STATUS_REV_A;
  } else
    cs4231_chip->status &= ~CS_STATUS_REV_A;
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x10);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), OLB_ENABLE);
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x11);
  if (cs4231_chip->status & CS_STATUS_REV_A)
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), (HPF_ON | XTALE_ON));
  else
      CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), HPF_ON);
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x1a);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), 0x00);
  
  /* Now set things up for defaults */
  cs4231_set_input_balance(drv, AUDIO_MID_BALANCE);
  cs4231_set_output_balance(drv, AUDIO_MID_BALANCE);

  cs4231_set_input_volume(drv, CS4231_DEFAULT_RECGAIN);
  cs4231_set_output_volume(drv, CS4231_DEFAULT_PLAYGAIN);

  cs4231_set_input_port(drv, AUDIO_MICROPHONE);
  cs4231_set_output_port(drv, AUDIO_SPEAKER);

  cs4231_set_monitor_volume(drv, LOOPB_OFF);
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_END);
  
  cs4231_ready(drv);
  
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_BEGIN | 0x09);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr),
                (CS4231_READ8(cs4231_chip, &(cs4231_chip->regs->idr)) & ACAL_DISABLE));
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), IAR_AUTOCAL_END);
  
  cs4231_ready(drv);

  cs4231_output_muted(drv, 0);

  cs4231_chip->recording_count = 0;
  cs4231_chip->input_next_dma_handle = 0;
  cs4231_chip->input_dma_handle = 0;
  cs4231_chip->input_next_dma_size = 0;
  cs4231_chip->input_dma_size = 0;

  cs4231_chip->playing_count = 0;
  cs4231_chip->output_next_dma_handle = 0;
  cs4231_chip->output_dma_handle = 0;
  cs4231_chip->output_next_dma_size = 0;
  cs4231_chip->output_dma_size = 0;
}

static int 
cs4231_length_to_samplecount(struct audio_prinfo *thisdir, unsigned int length)
{
  unsigned int count;

  if (thisdir->channels == 2)
    count = (length/2);
  else 
    count = length;
  
  if (thisdir->encoding == AUDIO_ENCODING_LINEAR)
    count = (count/2);
  else if (thisdir->encoding == AUDIO_ENCODING_DVI)
    count = (count/4);
  
  return count;
}

#ifdef EB4231_SUPPORT
static void eb4231_getsamplecount(struct sparcaudio_driver *drv, unsigned int length, unsigned int direction)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  struct audio_prinfo *thisdir;
  unsigned int count, curcount, nextcount, dbcr;

  if(direction == 1) {
    thisdir = &cs4231_chip->perchip_info.record;
    dbcr = readl(&(cs4231_chip->eb2c->dbcr));
    nextcount = cs4231_chip->input_next_dma_size;
  } else {
    thisdir = &cs4231_chip->perchip_info.play;
    dbcr = readl(&(cs4231_chip->eb2p->dbcr));
    nextcount = cs4231_chip->output_next_dma_size;
  }
  curcount = cs4231_length_to_samplecount(thisdir, dbcr);
  count = thisdir->samples;
  length = cs4231_length_to_samplecount(thisdir, length);
  /* normalize for where we are. */
  thisdir->samples = ((count - nextcount) + (length - curcount));
}
#endif

static void cs4231_getsamplecount(struct sparcaudio_driver *drv, unsigned int length, unsigned int direction)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  struct audio_prinfo *thisdir;
  unsigned int count, nextcount, curcount;

  if (direction == 1) /* record */ 
    {
      thisdir = &cs4231_chip->perchip_info.record;
      curcount = 
	cs4231_length_to_samplecount(thisdir, cs4231_chip->regs->dmacc);
      nextcount = 
	cs4231_length_to_samplecount(thisdir, cs4231_chip->regs->dmacnc);
    }
  else /* play */
    {
      thisdir = &cs4231_chip->perchip_info.play;
      curcount = 
	cs4231_length_to_samplecount(thisdir, cs4231_chip->regs->dmapc);
      nextcount = 
	cs4231_length_to_samplecount(thisdir, cs4231_chip->regs->dmapnc);
    }
  count = thisdir->samples;
  length = cs4231_length_to_samplecount(thisdir, length);
  /* normalize for where we are. */
  thisdir->samples = ((count - nextcount) + (length - curcount));
}

static int cs4231_open(struct inode * inode, struct file * file, struct sparcaudio_driver *drv)
{	
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  /* Set the default audio parameters if not already in use. */
  if (file->f_mode & FMODE_WRITE) {
    if (!(drv->flags & SDF_OPEN_WRITE) && 
	(cs4231_chip->perchip_info.play.active == 0)) {
      cs4231_chip->perchip_info.play.open = 1;
      cs4231_chip->perchip_info.play.samples =
        cs4231_chip->perchip_info.play.error = 0;
    }
  }

  if (file->f_mode & FMODE_READ) {
    if (!(drv->flags & SDF_OPEN_READ) && 
	(cs4231_chip->perchip_info.record.active == 0)) {
      cs4231_chip->perchip_info.record.open = 1;
      cs4231_chip->perchip_info.record.samples = 
        cs4231_chip->perchip_info.record.error = 0;
    }
  }  

  cs4231_ready(drv);
  
  CHIP_READY
    
  MOD_INC_USE_COUNT;
  
  return 0;
}

static void cs4231_release(struct inode * inode, struct file * file, struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  /* zero out any info about what data we have as well */

  if (file->f_mode & FMODE_READ) {
    /* stop capture here or midlevel? */
    cs4231_chip->perchip_info.record.open = 0;
    if (cs4231_chip->input_dma_handle) {
      if(!(cs4231_chip->status & CS_STATUS_IS_EBUS))
              mmu_release_scsi_one(cs4231_chip->input_dma_handle,
                                   cs4231_chip->input_dma_size, drv->dev->my_bus);
      cs4231_chip->input_dma_handle = 0;
      cs4231_chip->input_dma_size = 0;
    }
    if (cs4231_chip->input_next_dma_handle) {
      if(!(cs4231_chip->status & CS_STATUS_IS_EBUS))
              mmu_release_scsi_one(cs4231_chip->input_next_dma_handle,
                                   cs4231_chip->input_next_dma_size, drv->dev->my_bus);
      cs4231_chip->input_next_dma_handle = 0;
      cs4231_chip->input_next_dma_size = 0;
    }
  }

  if (file->f_mode & FMODE_WRITE) {
    cs4231_chip->perchip_info.play.active =
      cs4231_chip->perchip_info.play.open = 0;
    if (cs4231_chip->output_dma_handle) {
      if(!(cs4231_chip->status & CS_STATUS_IS_EBUS))
              mmu_release_scsi_one(cs4231_chip->output_dma_handle,
                                   cs4231_chip->output_dma_size, drv->dev->my_bus);
      cs4231_chip->output_dma_handle = 0;
      cs4231_chip->output_dma_size = 0;
    }
    if (cs4231_chip->output_next_dma_handle) {
      if(!(cs4231_chip->status & CS_STATUS_IS_EBUS))
              mmu_release_scsi_one(cs4231_chip->output_next_dma_handle,
                                   cs4231_chip->output_next_dma_size, 
                                   drv->dev->my_bus);
      cs4231_chip->output_next_dma_handle = 0;
      cs4231_chip->output_next_dma_size = 0;
    }
  }

  if (!cs4231_chip->perchip_info.play.open && 
      !cs4231_chip->perchip_info.record.open && 
      (cs4231_chip->status & CS_STATUS_INIT_ON_CLOSE)) {
    cs4231_chip_reset(drv);
    cs4231_chip->status &= ~CS_STATUS_INIT_ON_CLOSE;
  }

  MOD_DEC_USE_COUNT;
}

static void cs4231_playintr(struct sparcaudio_driver *drv, int push)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int status = 0;

  if (!push) {
    if (!cs4231_chip->perchip_info.play.active) {
      cs4231_chip->regs->dmapnva = cs4231_chip->output_next_dma_handle;
      cs4231_chip->regs->dmapnc = cs4231_chip->output_next_dma_size;
    }
    sparcaudio_output_done(drv, 0);
    return;
  }

  if (cs4231_chip->playlen == 0 && cs4231_chip->output_size > 0) 
    cs4231_chip->playlen = cs4231_chip->output_size;

  if (cs4231_chip->output_dma_handle) {
    mmu_release_scsi_one(cs4231_chip->output_dma_handle, 
                         cs4231_chip->output_dma_size, drv->dev->my_bus);
    cs4231_chip->output_dma_handle = 0;
    cs4231_chip->output_dma_size = 0;
    cs4231_chip->playing_count--;
    status++;
  }
  if (cs4231_chip->output_next_dma_handle) {
    cs4231_chip->output_dma_handle = cs4231_chip->output_next_dma_handle;
    cs4231_chip->output_dma_size = cs4231_chip->output_next_dma_size;
    cs4231_chip->output_next_dma_size = 0;
    cs4231_chip->output_next_dma_handle = 0;
  }

  if ((cs4231_chip->output_ptr && cs4231_chip->output_size > 0) && 
      !(cs4231_chip->perchip_info.play.pause)) {
    cs4231_chip->output_next_dma_handle =
      mmu_get_scsi_one((char *) cs4231_chip->output_ptr, 
                       cs4231_chip->output_size, drv->dev->my_bus);
    cs4231_chip->regs->dmapnva = cs4231_chip->output_next_dma_handle;
    cs4231_chip->output_next_dma_size = cs4231_chip->regs->dmapnc = 
      cs4231_chip->output_size;
    cs4231_chip->output_size = 0;
    cs4231_chip->output_ptr = NULL;
    cs4231_chip->playing_count++;
    status += 2;
  } else {
    cs4231_chip->regs->dmapnva = 0;
    cs4231_chip->regs->dmapnc = 0;
  }

  sparcaudio_output_done(drv, status);

  return;
}

#ifdef EB4231_SUPPORT
static void eb4231_playintr(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int status = 0;

  if (cs4231_chip->playlen == 0 && cs4231_chip->output_size > 0) 
    cs4231_chip->playlen = cs4231_chip->output_size;

  if (cs4231_chip->output_dma_handle) {
    cs4231_chip->output_dma_handle = 0;
    cs4231_chip->output_dma_size = 0;
    cs4231_chip->playing_count--;
    status++;
  }
  if(cs4231_chip->output_next_dma_handle) {
    cs4231_chip->output_dma_handle = cs4231_chip->output_next_dma_handle;
    cs4231_chip->output_dma_size = cs4231_chip->output_next_dma_size;
    cs4231_chip->output_next_dma_handle = 0;
    cs4231_chip->output_next_dma_size = 0;
  }

  if ((cs4231_chip->output_ptr && cs4231_chip->output_size > 0) && 
      !(cs4231_chip->perchip_info.play.pause)) {
    cs4231_chip->output_next_dma_handle = virt_to_bus(cs4231_chip->output_ptr);
    cs4231_chip->output_next_dma_size = cs4231_chip->output_size;

    writel(cs4231_chip->output_next_dma_size, &(cs4231_chip->eb2p->dbcr));
    writel(cs4231_chip->output_next_dma_handle, &(cs4231_chip->eb2p->dacr));
    cs4231_chip->output_size = 0;
    cs4231_chip->output_ptr = NULL;
    cs4231_chip->playing_count++;
    status += 2;
  }

  sparcaudio_output_done(drv, status);

  return;
}
#endif

static void cs4231_recclear(int fmt, char *dmabuf, int length)
{
  switch (fmt) {
  case AUDIO_ENCODING_LINEAR:
    memset(dmabuf, 0x00, length);
    break;
  case AUDIO_ENCODING_ALAW:
    memset(dmabuf, 0xd5, length);
    break;
  case AUDIO_ENCODING_ULAW:
    memset(dmabuf, 0xff, length);
    break;
  }
}

static int cs4231_recintr(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int status = 0;

  if (cs4231_chip->perchip_info.record.active == 0) {
    dprintk(("going inactive\n"));
    cs4231_pollinput(drv);
    cs4231_disable_rec(drv);    
  } 

  if (cs4231_chip->input_dma_handle) {
    mmu_release_scsi_one(cs4231_chip->input_dma_handle, 
                         cs4231_chip->input_dma_size, drv->dev->my_bus);
    cs4231_chip->input_dma_handle = 0;
    cs4231_chip->input_dma_size = 0;
    cs4231_chip->recording_count--;
    status++;
  }
  if (cs4231_chip->input_next_dma_handle) {
    cs4231_chip->input_dma_handle = cs4231_chip->input_next_dma_handle;
    cs4231_chip->input_dma_size = cs4231_chip->input_next_dma_size;
    cs4231_chip->input_next_dma_size = 0;
    cs4231_chip->input_next_dma_handle = 0;
  }

  if ((cs4231_chip->input_ptr && cs4231_chip->input_size > 0) && 
      !(cs4231_chip->perchip_info.record.pause)) {
    cs4231_recclear(cs4231_chip->perchip_info.record.encoding, 
                    (char *)cs4231_chip->input_ptr, cs4231_chip->input_size);
    cs4231_chip->input_next_dma_handle =
      mmu_get_scsi_one((char *) cs4231_chip->input_ptr, 
                       cs4231_chip->input_size, drv->dev->my_bus);
    cs4231_chip->regs->dmacnva = cs4231_chip->input_next_dma_handle;
    cs4231_chip->input_next_dma_size = cs4231_chip->regs->dmacnc = 
      cs4231_chip->input_size;
    cs4231_chip->input_size = 0;
    cs4231_chip->input_ptr = NULL;
    cs4231_chip->recording_count++;
    status += 2;
  } else {
    cs4231_chip->regs->dmacnva = 0;
    cs4231_chip->regs->dmacnc = 0;
  }

  sparcaudio_input_done(drv, status);

  return 1;
}

#ifdef EB4231_SUPPORT
static int eb4231_recintr(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int status = 0;

  if (cs4231_chip->perchip_info.record.active == 0) {
    dprintk(("going inactive\n"));
    eb4231_pollinput(drv);
    cs4231_disable_rec(drv);    
  } 

  if (cs4231_chip->input_dma_handle) {
    cs4231_chip->input_dma_handle = 0;
    cs4231_chip->input_dma_size = 0;
    cs4231_chip->recording_count--;
    status++;
  }
  if (cs4231_chip->input_next_dma_handle) {
    cs4231_chip->input_dma_handle = cs4231_chip->input_next_dma_handle;
    cs4231_chip->input_dma_size = cs4231_chip->input_next_dma_size;
    cs4231_chip->input_next_dma_size = 0;
    cs4231_chip->input_next_dma_handle = 0;
  }

  if ((cs4231_chip->input_ptr && cs4231_chip->input_size > 0) && 
      !(cs4231_chip->perchip_info.record.pause)) {
    cs4231_recclear(cs4231_chip->perchip_info.record.encoding, 
                    (char *)cs4231_chip->input_ptr, cs4231_chip->input_size);

    cs4231_chip->input_next_dma_handle = virt_to_bus(cs4231_chip->input_ptr);
    cs4231_chip->input_next_dma_size = cs4231_chip->input_size;

    writel(cs4231_chip->input_next_dma_size, &(cs4231_chip->eb2c->dbcr));
    writel(cs4231_chip->input_next_dma_handle, &(cs4231_chip->eb2c->dacr));

    cs4231_chip->input_size = 0;
    cs4231_chip->input_ptr = NULL;
    cs4231_chip->recording_count++;
    status += 2;
  }

  sparcaudio_input_done(drv, status);

  return 1;
}
#endif

#ifdef EB4231_SUPPORT
static void eb4231_start_output(struct sparcaudio_driver *drv, __u8 * buffer,
                                unsigned long count)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned int dcsr;

  cs4231_chip->output_ptr = buffer;
  cs4231_chip->output_size = count;

  if (cs4231_chip->perchip_info.play.active || 
      (cs4231_chip->perchip_info.play.pause))
    return;

  cs4231_ready(drv);

  cs4231_chip->perchip_info.play.active = 1;
  cs4231_chip->playing_count = 0;

  dcsr = readl(&cs4231_chip->eb2p->dcsr);
  if (!(dcsr & EBUS_DCSR_EN_DMA)) {
    writel(EBUS_DCSR_RESET, &(cs4231_chip->eb2p->dcsr));
    writel(EBUS_DCSR_BURST_SZ_16, &(cs4231_chip->eb2p->dcsr));

    eb4231_playintr(drv);

    writel(EBUS_DCSR_BURST_SZ_16 |
           (EBUS_DCSR_EN_DMA | EBUS_DCSR_INT_EN | EBUS_DCSR_EN_CNT | EBUS_DCSR_EN_NEXT),
           &(cs4231_chip->eb2p->dcsr));

    cs4231_enable_play(drv);

    cs4231_ready(drv);
  } else {
    eb4231_playintr(drv);
  }
}
#endif

static void cs4231_start_output(struct sparcaudio_driver *drv, __u8 * buffer,
                                unsigned long count)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  tprintk(("in 4231 start output\n"));
  cs4231_chip->output_ptr = buffer;
  cs4231_chip->output_size = count;

  if (cs4231_chip->perchip_info.play.active || 
      (cs4231_chip->perchip_info.play.pause))
    return;

  cs4231_ready(drv);

  cs4231_chip->perchip_info.play.active = 1;
  cs4231_chip->playing_count = 0;

  if ((cs4231_chip->regs->dmacsr & APC_PPAUSE) || 
      !(cs4231_chip->regs->dmacsr & APC_PDMA_READY)) {
    cs4231_chip->regs->dmacsr &= ~APC_XINT_PLAY;
    cs4231_chip->regs->dmacsr &= ~APC_PPAUSE;
    
    cs4231_playintr(drv, cs4231_chip->regs->dmapnva == 0 ? 1 : 0);

    cs4231_chip->regs->dmacsr |= APC_PLAY_SETUP;
    cs4231_enable_play(drv);

    cs4231_ready(drv);
  }
}

#ifdef EB4231_SUPPORT
static void eb4231_stop_output(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned int dcsr;

  dprintk(("eb4231_stop_output: dcsr 0x%x dacr 0x%x dbcr %d\n",
           readl(&cs4231_chip->eb2p->dcsr),
           readl(&cs4231_chip->eb2p->dacr),
           readl(&cs4231_chip->eb2p->dbcr)));
  cs4231_chip->output_ptr = NULL;
  cs4231_chip->output_size = 0;
  if (cs4231_chip->output_dma_handle) {
    cs4231_chip->output_dma_handle = 0;
    cs4231_chip->output_dma_size = 0;
  }
  if (cs4231_chip->output_next_dma_handle) {
    cs4231_chip->output_next_dma_handle = 0;
    cs4231_chip->output_next_dma_size = 0;
  }
  dcsr = readl(&(cs4231_chip->eb2p->dcsr));
  if(dcsr & EBUS_DCSR_EN_DMA)
    writel(dcsr & ~EBUS_DCSR_EN_DMA, &(cs4231_chip->eb2p->dcsr));

  /* Else subsequent speed setting changes are ignored by the chip. */
  cs4231_disable_play(drv);
}
#endif

static void cs4231_stop_output(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  tprintk(("in cs4231_stop_output\n"));
  cs4231_chip->output_ptr = NULL;
  cs4231_chip->output_size = 0;
  if (cs4231_chip->output_dma_handle) {
    mmu_release_scsi_one(cs4231_chip->output_dma_handle,
                         cs4231_chip->output_dma_size, drv->dev->my_bus);
    cs4231_chip->output_dma_handle = 0;
    cs4231_chip->output_dma_size = 0;
  }
  if (cs4231_chip->output_next_dma_handle) {
    mmu_release_scsi_one(cs4231_chip->output_next_dma_handle,
                         cs4231_chip->output_next_dma_size, drv->dev->my_bus);
    cs4231_chip->output_next_dma_handle = 0;
    cs4231_chip->output_next_dma_size = 0;
  }
#if 0 /* Not safe without shutting off the DMA controller as well. -DaveM */
  /* Else subsequent speed setting changes are ignored by the chip. */
  cs4231_disable_play(drv);
#endif
}

#ifdef EB4231_SUPPORT
static void eb4231_pollinput(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int x = 0, dcsr;

  while (!((dcsr = readl(&(cs4231_chip->eb2c->dcsr))) & EBUS_DCSR_TC) &&
         x <= CS_TIMEOUT) {
    x++;
  }

  writel(dcsr | EBUS_DCSR_TC, &(cs4231_chip->eb2c->dcsr));
}
#endif

static void cs4231_pollinput(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int x = 0;

  while (!(cs4231_chip->regs->dmacsr & APC_XINT_COVF) && x <= CS_TIMEOUT) {
    x++;
  }
  cs4231_chip->regs->dmacsr |= APC_XINT_CEMP;
}

static void cs4231_start_input(struct sparcaudio_driver *drv, __u8 * buffer, 
                               unsigned long count)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  cs4231_chip->input_ptr = buffer;
  cs4231_chip->input_size = count;

  if (cs4231_chip->perchip_info.record.active || 
      (cs4231_chip->perchip_info.record.pause))
    return;

  cs4231_ready(drv);

  cs4231_chip->perchip_info.record.active = 1;
  cs4231_chip->recording_count = 0;

  if ((cs4231_chip->regs->dmacsr & APC_CPAUSE) || 
      !(cs4231_chip->regs->dmacsr & APC_CDMA_READY)) {
    cs4231_chip->regs->dmacsr &= ~APC_XINT_CAPT;
    cs4231_chip->regs->dmacsr &= ~APC_CPAUSE;

    cs4231_recintr(drv);

    cs4231_chip->regs->dmacsr |= APC_CAPT_SETUP;
    cs4231_enable_rec(drv);

    cs4231_ready(drv);
  } else
    cs4231_recintr(drv);
}

static void cs4231_stop_input(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  cs4231_chip->perchip_info.record.active = 0;
  cs4231_chip->regs->dmacsr |= (APC_CPAUSE);

  cs4231_chip->input_ptr = NULL;
  cs4231_chip->input_size = 0;
  if (cs4231_chip->input_dma_handle) {
    mmu_release_scsi_one(cs4231_chip->input_dma_handle,
                         cs4231_chip->input_dma_size, drv->dev->my_bus);
    cs4231_chip->input_dma_handle = 0;
    cs4231_chip->input_dma_size = 0;
  }
  if (cs4231_chip->input_next_dma_handle) {
    mmu_release_scsi_one(cs4231_chip->input_next_dma_handle,
                         cs4231_chip->input_next_dma_size, drv->dev->my_bus);
    cs4231_chip->input_next_dma_handle = 0;
    cs4231_chip->input_next_dma_size = 0;
  }
  cs4231_pollinput(drv);
}

#ifdef EB4231_SUPPORT
static void eb4231_start_input(struct sparcaudio_driver *drv, __u8 * buffer, 
                               unsigned long count)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned int dcsr;

  cs4231_chip->input_ptr = buffer;
  cs4231_chip->input_size = count;

  if (cs4231_chip->perchip_info.record.active || 
      (cs4231_chip->perchip_info.record.pause))
    return;

  cs4231_ready(drv);

  cs4231_chip->perchip_info.record.active = 1;
  cs4231_chip->recording_count = 0;

  dcsr = readl(&cs4231_chip->eb2c->dcsr);
  if (!(dcsr & EBUS_DCSR_EN_DMA)) {
    writel(EBUS_DCSR_RESET, &(cs4231_chip->eb2c->dcsr));
    writel(EBUS_DCSR_BURST_SZ_16, &(cs4231_chip->eb2c->dcsr));

    eb4231_recintr(drv);

    writel(EBUS_DCSR_BURST_SZ_16 |
           (EBUS_DCSR_EN_DMA | EBUS_DCSR_INT_EN | EBUS_DCSR_EN_CNT | EBUS_DCSR_EN_NEXT),
           &(cs4231_chip->eb2c->dcsr));

    cs4231_enable_rec(drv);
    cs4231_ready(drv);
  } else
    eb4231_recintr(drv);
}

static void eb4231_stop_input(struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned int dcsr;

  cs4231_chip->perchip_info.record.active = 0;

  cs4231_chip->input_ptr = NULL;
  cs4231_chip->input_size = 0;
  if (cs4231_chip->input_dma_handle) {
    cs4231_chip->input_dma_handle = 0;
    cs4231_chip->input_dma_size = 0;
  }
  if (cs4231_chip->input_next_dma_handle) {
    cs4231_chip->input_next_dma_handle = 0;
    cs4231_chip->input_next_dma_size = 0;
  }

  dcsr = readl(&(cs4231_chip->eb2c->dcsr));
  if (dcsr & EBUS_DCSR_EN_DMA)
    writel(dcsr & ~EBUS_DCSR_EN_DMA, &(cs4231_chip->eb2c->dcsr));

  cs4231_disable_rec(drv);
}
#endif

static int cs4231_set_output_pause(struct sparcaudio_driver *drv, int value)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        cs4231_chip->perchip_info.play.pause = value;

        if (!value)
          sparcaudio_output_done(drv, 0);

        return value;
}

static int cs4231_set_output_error(struct sparcaudio_driver *drv, int value)
{
  int i;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  
  i = cs4231_chip->perchip_info.play.error;
  cs4231_chip->perchip_info.play.error = value;
  
  return i;
}

static int cs4231_set_input_error(struct sparcaudio_driver *drv, int value)
{
  int i;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  
  i = cs4231_chip->perchip_info.record.error;
  cs4231_chip->perchip_info.record.error = value;
  
  return i;
}

static int cs4231_set_output_samples(struct sparcaudio_driver *drv, int value)
{
  int i;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  
  i = cs4231_chip->perchip_info.play.samples;
  cs4231_chip->perchip_info.play.samples = value;
  
  return i;
}

static int cs4231_set_input_samples(struct sparcaudio_driver *drv, int value)
{
  int i;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  
  i = cs4231_chip->perchip_info.record.samples;
  cs4231_chip->perchip_info.record.samples = value;
  
  return i;
}

static int cs4231_set_input_pause(struct sparcaudio_driver *drv, int value)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

        cs4231_chip->perchip_info.record.pause = value;
	
        if (value)
          cs4231_stop_input(drv);
	
        return value;
}

static void cs4231_audio_getdev(struct sparcaudio_driver *drv,
                                 audio_device_t * audinfo)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  strncpy(audinfo->name, "SUNW,CS4231", sizeof(audinfo->name) - 1);
  /* versions */
  /* a: SPARCstation 4/5          b: Ultra 1/2 (electron)       */
  /* c: Ultra 1/2 PCI? (positron) d: ppc                        */
  /* e: x86                       f: Ultra Enterprise? (tazmo)  */
  /* g: Ultra 30? (quark)         h: Ultra 5/10? (darwin)       */
  /* apparently Ultra 1, Ultra 2 don't have internal CD input */
  if (cs4231_chip->status & CS_STATUS_IS_ULTRA)
    strncpy(audinfo->version, "b", sizeof(audinfo->version) - 1);
  else
    strncpy(audinfo->version, "a", sizeof(audinfo->version) - 1);
  strncpy(audinfo->config, "onboard1", sizeof(audinfo->config) - 1);
}


static int cs4231_audio_getdev_sunos(struct sparcaudio_driver *drv)
{
  return AUDIO_DEV_CS4231;
}

static void cs4231_loopback(struct sparcaudio_driver *drv, unsigned int value)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->iar), 0x0d);
  CS4231_WRITE8(cs4231_chip, &(cs4231_chip->regs->idr), (value ? LOOPB_ON : 0));
}

static int cs4231_ioctl(struct inode * inode, struct file * file,
			unsigned int cmd, unsigned long arg, 
			struct sparcaudio_driver *drv)
{
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int retval = 0;
  
  switch (cmd) {
  case AUDIO_DIAG_LOOPBACK:
    cs4231_chip->status |= CS_STATUS_INIT_ON_CLOSE;
    cs4231_loopback(drv, (unsigned int)arg);
    break;
  default:
    retval = -EINVAL;
  }

  return retval;
}

#ifdef EB4231_SUPPORT
/* ebus audio capture interrupt handler. */
void eb4231_cinterrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  struct sparcaudio_driver *drv = (struct sparcaudio_driver *)dev_id;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  int dummy;
  
  /* Clear the interrupt. */
  dummy = readl(&(cs4231_chip->eb2c->dcsr));
  writel(dummy, &(cs4231_chip->eb2c->dcsr));

  if ((dummy & EBUS_DCSR_TC) != 0
      /*&& (dummy & EBUS_DCSR_A_LOADED) != 0*/) {
    cs4231_chip->perchip_info.record.samples += 
      cs4231_length_to_samplecount(&(cs4231_chip->perchip_info.record), 
                                   cs4231_chip->reclen);
    eb4231_recintr(drv);
  }

  if ((dummy & EBUS_DCSR_A_LOADED) == 0) {
    cs4231_chip->perchip_info.record.active = 0;
    eb4231_recintr(drv);
#if 1
    eb4231_getsamplecount(drv, cs4231_chip->reclen, 1);
#endif
  }
}

/* ebus audio play interrupt handler. */
void eb4231_pinterrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  struct sparcaudio_driver *drv = (struct sparcaudio_driver *)dev_id;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  unsigned int dummy;
  
  /* Clear the interrupt.  Bleh, when not using the next-address
   * feature, TC can only be cleared by a reset.
   */
  dummy = readl(&(cs4231_chip->eb2p->dcsr));
  writel(dummy, &(cs4231_chip->eb2p->dcsr));

  /* If we get a terminal count and address loaded condition,
   * this means the DNAR was copied into DACR.
   */
  if((dummy & EBUS_DCSR_TC) != 0
     /*&& (dummy & EBUS_DCSR_A_LOADED) != 0*/) {
    cs4231_chip->perchip_info.play.samples += 
      cs4231_length_to_samplecount(&(cs4231_chip->perchip_info.play), 
                                   cs4231_chip->playlen); 
    eb4231_playintr(drv);
  }

  if((dummy & EBUS_DCSR_A_LOADED) == 0) {
    cs4231_chip->perchip_info.play.active = 0;
    eb4231_playintr(drv);

    eb4231_getsamplecount(drv, cs4231_chip->playlen, 0);
  }
}
#endif

/* Audio interrupt handler. */
void cs4231_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  struct sparcaudio_driver *drv = (struct sparcaudio_driver *)dev_id;
  struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;
  __u32 dummy;
  
  dprintk(("in cs4231_interrupt\n"));

  /* Clear the interrupt. */
  dummy = cs4231_chip->regs->dmacsr;
  cs4231_chip->regs->dmacsr = dummy;

  /* now go through and figure out what gets to claim the interrupt 
   * if anything since we may be doing shared interrupts 
   */

  if (dummy & APC_PLAY_INT) {
    if (dummy & APC_XINT_PNVA) {
      cs4231_chip->perchip_info.play.samples += 
	cs4231_length_to_samplecount(&(cs4231_chip->perchip_info.play), 
				     cs4231_chip->playlen); 
      if (!(dummy & APC_XINT_EMPT))
        cs4231_playintr(drv, 1);
    } 
    /* Any other conditions we need worry about? */
  }

  if (dummy & APC_CAPT_INT) {
    if (dummy & APC_XINT_CNVA) {
      cs4231_chip->perchip_info.record.samples += 
	cs4231_length_to_samplecount(&(cs4231_chip->perchip_info.record), 
				     cs4231_chip->reclen);
      cs4231_recintr(drv);
    }
    /* Any other conditions we need worry about? */
  }

  
  if (dummy & APC_XINT_CEMP) {
    if (cs4231_chip->perchip_info.record.active == 0) {
      /* Fix me */
      cs4231_chip->perchip_info.record.active = 0;
      cs4231_chip->perchip_info.record.error = 1;
      cs4231_recintr(drv);
    }
  }

  if (dummy & APC_XINT_EMPT) {
    if (!cs4231_chip->output_next_dma_handle) {
      cs4231_chip->regs->dmacsr |= (APC_PPAUSE);
      cs4231_disable_play(drv);
      cs4231_chip->perchip_info.play.error = 1;
    }
    cs4231_chip->perchip_info.play.active = 0;
    cs4231_playintr(drv, 0);

    cs4231_getsamplecount(drv, cs4231_chip->playlen, 0);
  }

  if (dummy & APC_GENL_INT) {
    /* If we get here we must be sharing an interrupt, but I haven't code 
       to handle this right now */
  }

}

static struct sparcaudio_operations cs4231_ops = {
	cs4231_open,
	cs4231_release,
	cs4231_ioctl,
	cs4231_start_output,
	cs4231_stop_output,
	cs4231_start_input,
        cs4231_stop_input,
	cs4231_audio_getdev,
        cs4231_set_output_volume,
        cs4231_get_output_volume,
        cs4231_set_input_volume,
        cs4231_get_input_volume,
        cs4231_set_monitor_volume,
        cs4231_get_monitor_volume,
	cs4231_set_output_balance,
	cs4231_get_output_balance,
        cs4231_set_input_balance,
        cs4231_get_input_balance,
        cs4231_set_output_channels,
        cs4231_get_output_channels,
        cs4231_set_input_channels,
        cs4231_get_input_channels,
        cs4231_set_output_precision,
        cs4231_get_output_precision,
        cs4231_set_input_precision,
        cs4231_get_input_precision,
        cs4231_set_output_port,
        cs4231_get_output_port,
        cs4231_set_input_port,
        cs4231_get_input_port,
        cs4231_set_output_encoding,
        cs4231_get_output_encoding,
        cs4231_set_input_encoding,
        cs4231_get_input_encoding,
        cs4231_set_output_rate,
        cs4231_get_output_rate,
        cs4231_set_input_rate,
	cs4231_get_input_rate,
	cs4231_audio_getdev_sunos,
	cs4231_get_output_ports,
	cs4231_get_input_ports,
	cs4231_output_muted,
	cs4231_get_output_muted,
	cs4231_set_output_pause,
	cs4231_get_output_pause,
	cs4231_set_input_pause,
	cs4231_get_input_pause,
	cs4231_set_output_samples,
	cs4231_get_output_samples,
	cs4231_set_input_samples,
	cs4231_get_input_samples,
	cs4231_set_output_error,
	cs4231_get_output_error,
	cs4231_set_input_error,
	cs4231_get_input_error,
        cs4231_get_formats,
};

#ifdef EB4231_SUPPORT
static struct sparcaudio_operations eb4231_ops = {
	cs4231_open,
	cs4231_release,
	cs4231_ioctl,
	eb4231_start_output,
	eb4231_stop_output,
	eb4231_start_input,
        eb4231_stop_input,
	cs4231_audio_getdev,
        cs4231_set_output_volume,
        cs4231_get_output_volume,
        cs4231_set_input_volume,
        cs4231_get_input_volume,
        cs4231_set_monitor_volume,
        cs4231_get_monitor_volume,
	cs4231_set_output_balance,
	cs4231_get_output_balance,
        cs4231_set_input_balance,
        cs4231_get_input_balance,
        cs4231_set_output_channels,
        cs4231_get_output_channels,
        cs4231_set_input_channels,
        cs4231_get_input_channels,
        cs4231_set_output_precision,
        cs4231_get_output_precision,
        cs4231_set_input_precision,
        cs4231_get_input_precision,
        cs4231_set_output_port,
        cs4231_get_output_port,
        cs4231_set_input_port,
        cs4231_get_input_port,
        cs4231_set_output_encoding,
        cs4231_get_output_encoding,
        cs4231_set_input_encoding,
        cs4231_get_input_encoding,
        cs4231_set_output_rate,
        cs4231_get_output_rate,
        cs4231_set_input_rate,
	cs4231_get_input_rate,
	cs4231_audio_getdev_sunos,
	cs4231_get_output_ports,
	cs4231_get_input_ports,
	cs4231_output_muted,
	cs4231_get_output_muted,
	cs4231_set_output_pause,
	cs4231_get_output_pause,
	cs4231_set_input_pause,
	cs4231_get_input_pause,
	cs4231_set_output_samples,
	eb4231_get_output_samples,
	cs4231_set_input_samples,
	eb4231_get_input_samples,
	cs4231_set_output_error,
	cs4231_get_output_error,
	cs4231_set_input_error,
	cs4231_get_input_error,
        cs4231_get_formats,
};
#endif

/* Attach to an cs4231 chip given its PROM node. */
static int cs4231_attach(struct sparcaudio_driver *drv, 
			 struct linux_sbus_device *sdev)
{
#if defined (LINUX_VERSION_CODE) && LINUX_VERSION_CODE < 0x20100
  struct linux_prom_irqs irq;
#endif
  struct linux_sbus *sbus = sdev->my_bus;
  struct cs4231_chip *cs4231_chip;
  int err;

  /* Allocate our private information structure. */
  drv->private = kmalloc(sizeof(struct cs4231_chip), GFP_KERNEL);
  if (!drv->private)
    return -ENOMEM;

  /* Point at the information structure and initialize it. */
  drv->ops = &cs4231_ops;
  cs4231_chip = (struct cs4231_chip *)drv->private;
  cs4231_chip->input_ptr = cs4231_chip->output_ptr = NULL;
  cs4231_chip->input_size = cs4231_chip->output_size = 0;
  cs4231_chip->status = 0;

  drv->dev = sdev;

  /* Map the registers into memory. */
#if defined (LINUX_VERSION_CODE) && LINUX_VERSION_CODE < 0x20100
  prom_apply_sbus_ranges(sbus, &sdev->reg_addrs[0], 
			 sdev->num_registers, sdev);
#else
  prom_apply_sbus_ranges(sbus, sdev->reg_addrs, 1, sdev);
#endif

  cs4231_chip->regs_size = sdev->reg_addrs[0].reg_size;

  cs4231_chip->regs = sparc_alloc_io(sdev->reg_addrs[0].phys_addr, 0,
				     sdev->reg_addrs[0].reg_size, 
				     "cs4231", sdev->reg_addrs[0].which_io, 
				     0);

  if (!cs4231_chip->regs) {
    printk(KERN_ERR "cs4231: could not allocate registers\n");
    kfree(drv->private);
    return -EIO;
  }

  /* Attach the interrupt handler to the audio interrupt. */
#if defined (LINUX_VERSION_CODE) && LINUX_VERSION_CODE < 0x20100
  prom_getproperty(sdev->prom_node, "intr", (char *)&irq, sizeof(irq));

  if (irq.pri < 0) {
    sparc_free_io(cs4231_chip->regs, cs4231_chip->regs_size);
    kfree(drv->private);
    return -EIO;
  }

  cs4231_chip->irq = irq.pri;

#else
  cs4231_chip->irq = sdev->irqs[0];
#endif

  request_irq(cs4231_chip->irq, cs4231_interrupt, SA_SHIRQ, "cs4231", drv);

  enable_irq(cs4231_chip->irq);

  cs4231_chip->nirqs = 1;

  cs4231_enable_interrupts(drv);

  /* Reset the audio chip. */
  cs4231_chip_reset(drv);

  /* Register ourselves with the midlevel audio driver. */
  err = register_sparcaudio_driver(drv, 1);

  if (err < 0) {
    printk(KERN_ERR "cs4231: unable to register\n");
    cs4231_disable_interrupts(drv);
    disable_irq(cs4231_chip->irq);
    free_irq(cs4231_chip->irq, drv);
    sparc_free_io(cs4231_chip->regs, cs4231_chip->regs_size);
    kfree(drv->private);
    return -EIO;
  }

  cs4231_chip->perchip_info.play.active = 
    cs4231_chip->perchip_info.play.pause = 0;

  cs4231_chip->perchip_info.record.active = 
    cs4231_chip->perchip_info.record.pause = 0;

  cs4231_chip->perchip_info.play.avail_ports = (AUDIO_HEADPHONE |
						AUDIO_SPEAKER |
						AUDIO_LINE_OUT);

  cs4231_chip->perchip_info.record.avail_ports = (AUDIO_INTERNAL_CD_IN |
						  AUDIO_LINE_IN | 
						  AUDIO_MICROPHONE |
						  AUDIO_ANALOG_LOOPBACK);

  /* Announce the hardware to the user. */
#if defined (LINUX_VERSION_CODE) && LINUX_VERSION_CODE < 0x20100
  printk(KERN_INFO "audio%d: cs4231%c at 0x%x irq %d\n",
         drv->index, (cs4231_chip->status & CS_STATUS_REV_A) ? 'a' : ' ', 
	 (unsigned long)cs4231_chip->regs, cs4231_chip->irq);
#else	 
  printk(KERN_INFO "audio%d: cs4231%c at %p irq %s\n",
         drv->index, (cs4231_chip->status & CS_STATUS_REV_A) ? 'a' : ' ', 
	 cs4231_chip->regs, __irq_itoa(cs4231_chip->irq));
#endif
  
  /* Success! */
  return 0;
}

#ifdef EB4231_SUPPORT
/* Attach to an cs4231 chip given its PROM node. */
static int eb4231_attach(struct sparcaudio_driver *drv, 
			 struct linux_ebus_device *edev)
{
  struct cs4231_chip *cs4231_chip;
  int len, err, nregs;
  struct linux_prom_registers regs[4];

  /* Allocate our private information structure. */
  drv->private = kmalloc(sizeof(struct cs4231_chip), GFP_KERNEL);
  if (!drv->private)
    return -ENOMEM;

  /* Point at the information structure and initialize it. */
  drv->ops = &eb4231_ops;
  cs4231_chip = (struct cs4231_chip *)drv->private;
  cs4231_chip->input_ptr = cs4231_chip->output_ptr = NULL;
  cs4231_chip->input_size = cs4231_chip->output_size = 0;
  cs4231_chip->status = 0;

  len = prom_getproperty(edev->prom_node, "reg", (void *)regs, sizeof(regs));

  if ((len % sizeof(regs[0])) != 0) {
    printk("eb4231: Strange reg property size %d\n", len);
    return -ENODEV;
  }

  nregs = len / sizeof(regs[0]);

  cs4231_chip->regs = (struct cs4231_regs *)edev->resource[0].start;
  cs4231_chip->eb2p = (struct linux_ebus_dma *)edev->resource[1].start;
  cs4231_chip->eb2c = (struct linux_ebus_dma *)edev->resource[2].start;

  cs4231_chip->status |= CS_STATUS_IS_EBUS;

  /* Attach the interrupt handler to the audio interrupt. */
  cs4231_chip->irq = edev->irqs[0];
  cs4231_chip->irq2 = edev->irqs[1];

  if(request_irq(cs4231_chip->irq, eb4231_cinterrupt, SA_SHIRQ, "cs4231", drv) ||
     request_irq(cs4231_chip->irq2, eb4231_pinterrupt, SA_SHIRQ, "cs4231", drv))
          goto bail;

  cs4231_chip->nirqs = 2;

  cs4231_enable_interrupts(drv);

  /* Reset the audio chip. */
  cs4231_chip_reset(drv);

  /* Register ourselves with the midlevel audio driver. */
  err = register_sparcaudio_driver(drv, 1);

  if (err < 0) {
  bail:
    printk(KERN_ERR "cs4231: unable to register\n");
    cs4231_disable_interrupts(drv);
    disable_irq(cs4231_chip->irq);
    free_irq(cs4231_chip->irq, drv);
    disable_irq(cs4231_chip->irq2);
    free_irq(cs4231_chip->irq2, drv);
    kfree(drv->private);
    return -EIO;
  }

  cs4231_chip->perchip_info.play.active = 
    cs4231_chip->perchip_info.play.pause = 0;

  cs4231_chip->perchip_info.record.active = 
    cs4231_chip->perchip_info.record.pause = 0;

  cs4231_chip->perchip_info.play.avail_ports = (AUDIO_HEADPHONE |
						AUDIO_SPEAKER |
						AUDIO_LINE_OUT);

  cs4231_chip->perchip_info.record.avail_ports = (AUDIO_INTERNAL_CD_IN |
						  AUDIO_LINE_IN | 
						  AUDIO_MICROPHONE |
						  AUDIO_ANALOG_LOOPBACK);

  /* Announce the hardware to the user. */
  printk(KERN_INFO "audio%d: cs4231%c(eb2) at %p irq %s\n",
         drv->index, (cs4231_chip->status & CS_STATUS_REV_A) ? 'a' : ' ', 
	 cs4231_chip->regs, __irq_itoa(cs4231_chip->irq));
  
  /* Success! */
  return 0;
}
#endif

/* Probe for the cs4231 chip and then attach the driver. */
#ifdef MODULE
int init_module(void)
#else
int __init cs4231_init(void)
#endif
{
  struct linux_sbus *sbus;
  struct linux_sbus_device *sdev;
#ifdef EB4231_SUPPORT
  struct linux_ebus *ebus;
  struct linux_ebus_device *edev;
#endif

  num_drivers = 0;
  
  /* Probe each SBUS for cs4231 chips. */
  for_all_sbusdev(sdev,sbus) {
    if (!strcmp(sdev->prom_name, "SUNW,CS4231")) {
      /* Don't go over the max number of drivers. */
      if (num_drivers >= MAX_DRIVERS)
	continue;
      
      if (cs4231_attach(&drivers[num_drivers], sdev) == 0)
	num_drivers++;
    }
  }
  
#ifdef EB4231_SUPPORT
  for_each_ebus(ebus) {
    for_each_ebusdev(edev, ebus) {
      if (!strcmp(edev->prom_name, "SUNW,CS4231")) {
        /* Don't go over the max number of drivers. */
        if (num_drivers >= MAX_DRIVERS)
          continue;
      
        if (eb4231_attach(&drivers[num_drivers], edev) == 0)
          num_drivers++;
      }
    }
  }
#endif

  /* Only return success if we found some cs4231 chips. */
  return (num_drivers > 0) ? 0 : -EIO;
}

#ifdef MODULE
/* Detach from an cs4231 chip given the device structure. */
static void cs4231_detach(struct sparcaudio_driver *drv)
{
        struct cs4231_chip *cs4231_chip = (struct cs4231_chip *)drv->private;

	cs4231_disable_interrupts(drv);
        unregister_sparcaudio_driver(drv, 1);
        disable_irq(cs4231_chip->irq);
        free_irq(cs4231_chip->irq, drv);
        if (!(cs4231_chip->status & CS_STATUS_IS_EBUS)) {
          sparc_free_io(cs4231_chip->regs, cs4231_chip->regs_size);
        } else {
#ifdef EB4231_SUPPORT
          disable_irq(cs4231_chip->irq2);
          free_irq(cs4231_chip->irq2, drv);
#endif
        }
        kfree(drv->private);
}

void cleanup_module(void)
{
        register int i;

        for (i = 0; i < num_drivers; i++) {
                cs4231_detach(&drivers[i]);
                num_drivers--;
        }
}
#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 4
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -4
 * c-argdecl-indent: 4
 * c-label-offset: -4
 * c-continued-statement-offset: 4
 * c-continued-brace-offset: 0
 * indent-tabs-mode: nil
 * tab-width: 8
 * End:
 */
