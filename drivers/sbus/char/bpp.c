/*
 * drivers/sbus/char/bpp.c
 *
 * Copyright (c) 1995 Picture Elements
 *      Stephen Williams (steve@icarus.com)
 *      Gus Baldauf (gbaldauf@ix.netcom.com)
 *
 * Linux/SPARC port by Peter Zaitcev.
 * Integration into SPARC tree by Tom Dyas.
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/ioport.h>
#include <linux/major.h>

#include <asm/uaccess.h>
#include <asm/io.h>

#if defined(__i386__)
# include <asm/system.h>
# include <asm/segment.h>
#endif

#if defined(__sparc__)
# include <linux/init.h>
# include <linux/delay.h>         /* udelay() */

# include <asm/oplib.h>           /* OpenProm Library */
# include <asm/sbus.h>            /* struct linux_sbus *SBus_chain */
#endif

#include <asm/bpp.h>

#define BPP_PROBE_CODE 0x55
#define BPP_DELAY 100

static const unsigned  BPP_MAJOR = LP_MAJOR;
static const char* dev_name = "bpp";

/* When switching from compatibility to a mode where I can read, try
   the following mode first. */

/* const unsigned char DEFAULT_ECP = 0x10; */
static const unsigned char DEFAULT_ECP = 0x30;
static const unsigned char DEFAULT_NIBBLE = 0x00;

/*
 * These are 1284 time constraints, in units of jiffies.
 */

static const unsigned long TIME_PSetup = 1;
static const unsigned long TIME_PResponse = 6;
static const unsigned long TIME_IDLE_LIMIT = 2000;

/*
 * One instance per supported subdevice...
 */
# define BPP_NO 3

enum IEEE_Mode { COMPATIBILITY, NIBBLE, ECP, ECP_RLE, EPP };

struct inst {
      unsigned present  : 1; /* True if the hardware exists */
      unsigned enhanced : 1; /* True if the hardware in "enhanced" */
      unsigned opened   : 1; /* True if the device is opened already */
      unsigned run_flag : 1; /* True if waiting for a repeate byte */

      unsigned char direction; /* 0 --> out, 0x20 --> IN */
      unsigned char pp_state; /* State of host controlled pins. */
      enum IEEE_Mode mode;

      unsigned char run_length;
      unsigned char repeat_byte;

      /* These members manage timeouts for programmed delays */
      wait_queue_head_t wait_queue;
      struct timer_list timer_list;
};

static struct inst instances[BPP_NO];

#if defined(__i386__)

const unsigned short base_addrs[BPP_NO] = { 0x278, 0x378, 0x3bc };

/*
 * These are for data access.
 * Control lines accesses are hidden in set_bits() and get_bits().
 * The exeption is the probe procedure, which is system-dependent.
 */
#define bpp_outb_p(data, base)  outb_p((data), (base))
#define bpp_inb(base)  inb(base)
#define bpp_inb_p(base)  inb_p(base)

/*
 * This method takes the pin values mask and sets the hardware pins to
 * the requested value: 1 == high voltage, 0 == low voltage. This
 * burries the annoying PC bit inversion and preserves the direction
 * flag.
 */
static void set_pins(unsigned short pins, unsigned minor)
{
      unsigned char bits = instances[minor].direction;  /* == 0x20 */

      if (! (pins & BPP_PP_nStrobe))   bits |= 1;
      if (! (pins & BPP_PP_nAutoFd))   bits |= 2;
      if (   pins & BPP_PP_nInit)      bits |= 4;
      if (! (pins & BPP_PP_nSelectIn)) bits |= 8;

      instances[minor].pp_state = bits;

      outb_p(bits, base_addrs[minor]+2);
}

static unsigned short get_pins(unsigned minor)
{
      unsigned short bits = 0;

      unsigned value = instances[minor].pp_state;
      if (! (value & 0x01)) bits |= BPP_PP_nStrobe;
      if (! (value & 0x02)) bits |= BPP_PP_nAutoFd;
      if (value & 0x04)     bits |= BPP_PP_nInit;
      if (! (value & 0x08)) bits |= BPP_PP_nSelectIn;

      value = inb_p(base_addrs[minor]+1);
      if (value & 0x08)     bits |= BPP_GP_nFault;
      if (value & 0x10)     bits |= BPP_GP_Select;
      if (value & 0x20)     bits |= BPP_GP_PError;
      if (value & 0x40)     bits |= BPP_GP_nAck;
      if (! (value & 0x80)) bits |= BPP_GP_Busy;

      return bits;
}

#endif /* __i386__ */

#if defined(__sparc__)

/*
 * Register block
 */
struct bpp_regs {
      /* DMA registers */
      __volatile__ __u32 p_csr;		/* DMA Control/Status Register */
      __volatile__ __u32 p_addr;	/* Address Register */
      __volatile__ __u32 p_bcnt;	/* Byte Count Register */
      __volatile__ __u32 p_tst_csr;	/* Test Control/Status (DMA2 only) */
      /* Parallel Port registers */
      __volatile__ __u16 p_hcr;		/* Hardware Configuration Register */
      __volatile__ __u16 p_ocr;		/* Operation Configuration Register */
      __volatile__ __u8 p_dr;		/* Parallel Data Register */
      __volatile__ __u8 p_tcr;		/* Transfer Control Register */
      __volatile__ __u8 p_or;		/* Output Register */
      __volatile__ __u8 p_ir;		/* Input Register */
      __volatile__ __u16 p_icr;		/* Interrupt Control Register */
};

/* P_CSR.  Bits of type RW1 are cleared with writting '1'. */
#define P_DEV_ID_MASK   0xf0000000      /* R   */
#define P_DEV_ID_ZEBRA  0x40000000
#define P_DEV_ID_L64854 0xa0000000      /*      == NCR 89C100+89C105. Pity. */
#define P_NA_LOADED     0x08000000      /* R    NA wirtten but was not used */
#define P_A_LOADED      0x04000000      /* R    */
#define P_DMA_ON        0x02000000      /* R    DMA is not disabled */
#define P_EN_NEXT       0x01000000      /* RW   */
#define P_TCI_DIS       0x00800000      /* RW   TCI forbidden from interrupts */
#define P_DIAG          0x00100000      /* RW   Disables draining and resetting
                                                of P-FIFO on loading of P_ADDR*/
#define P_BURST_SIZE    0x000c0000      /* RW   SBus burst size */
#define P_BURST_8       0x00000000
#define P_BURST_4       0x00040000
#define P_BURST_1       0x00080000      /*      "No burst" write */
#define P_TC            0x00004000      /* RW1  Term Count, can be cleared when
                                           P_EN_NEXT=1 */
#define P_EN_CNT        0x00002000      /* RW   */
#define P_EN_DMA        0x00000200      /* RW   */
#define P_WRITE         0x00000100      /* R    DMA dir, 1=to ram, 0=to port */
#define P_RESET         0x00000080      /* RW   */
#define P_SLAVE_ERR     0x00000040      /* RW1  Access size error */
#define P_INVALIDATE    0x00000020      /* W    Drop P-FIFO */
#define P_INT_EN        0x00000010      /* RW   OK to P_INT_PEND||P_ERR_PEND */
#define P_DRAINING      0x0000000c      /* R    P-FIFO is draining to memory */
#define P_ERR_PEND      0x00000002      /* R    */
#define P_INT_PEND      0x00000001      /* R    */

/* P_HCR. Time is in increments of SBus clock. */
#define P_HCR_TEST      0x8000      /* Allows buried counters to be read */
#define P_HCR_DSW       0x7f00      /* Data strobe width (in ticks) */
#define P_HCR_DDS       0x007f      /* Data setup before strobe (in ticks) */

/* P_OCR. */
#define P_OCR_MEM_CLR   0x8000
#define P_OCR_DATA_SRC  0x4000      /* )                  */
#define P_OCR_DS_DSEL   0x2000      /* )  Bidirectional      */
#define P_OCR_BUSY_DSEL 0x1000      /* )    selects            */
#define P_OCR_ACK_DSEL  0x0800      /* )                  */
#define P_OCR_EN_DIAG   0x0400
#define P_OCR_BUSY_OP   0x0200      /* Busy operation */
#define P_OCR_ACK_OP    0x0100      /* Ack operation */
#define P_OCR_SRST      0x0080      /* Reset state machines. Not selfcleaning. */
#define P_OCR_IDLE      0x0008      /* PP data transfer state machine is idle */
#define P_OCR_V_ILCK    0x0002      /* Versatec faded. Zebra only. */
#define P_OCR_EN_VER    0x0001      /* Enable Versatec (0 - enable). Zebra only. */

/* P_TCR */
#define P_TCR_DIR       0x08
#define P_TCR_BUSY      0x04
#define P_TCR_ACK       0x02
#define P_TCR_DS        0x01        /* Strobe */

/* P_OR */
#define P_OR_V3         0x20        /* )                 */
#define P_OR_V2         0x10        /* ) on Zebra only   */
#define P_OR_V1         0x08        /* )                 */
#define P_OR_INIT       0x04
#define P_OR_AFXN       0x02        /* Auto Feed */
#define P_OR_SLCT_IN    0x01

/* P_IR */
#define P_IR_PE         0x04
#define P_IR_SLCT       0x02
#define P_IR_ERR        0x01

/* P_ICR */
#define P_DS_IRQ        0x8000      /* RW1  */
#define P_ACK_IRQ       0x4000      /* RW1  */
#define P_BUSY_IRQ      0x2000      /* RW1  */
#define P_PE_IRQ        0x1000      /* RW1  */
#define P_SLCT_IRQ      0x0800      /* RW1  */
#define P_ERR_IRQ       0x0400      /* RW1  */
#define P_DS_IRQ_EN     0x0200      /* RW   Always on rising edge */
#define P_ACK_IRQ_EN    0x0100      /* RW   Always on rising edge */
#define P_BUSY_IRP      0x0080      /* RW   1= rising edge */
#define P_BUSY_IRQ_EN   0x0040      /* RW   */
#define P_PE_IRP        0x0020      /* RW   1= rising edge */
#define P_PE_IRQ_EN     0x0010      /* RW   */
#define P_SLCT_IRP      0x0008      /* RW   1= rising edge */
#define P_SLCT_IRQ_EN   0x0004      /* RW   */
#define P_ERR_IRP       0x0002      /* RW1  1= rising edge */
#define P_ERR_IRQ_EN    0x0001      /* RW   */

volatile struct bpp_regs *base_addrs[BPP_NO];

static inline void bpp_outb_p(__u8 data, volatile struct bpp_regs *base){
      base->p_dr = data;
}

#define bpp_inb_p(base)  bpp_inb(base)

static inline __u8 bpp_inb(volatile struct bpp_regs *base){
      return base->p_dr;
}


static void set_pins(unsigned short pins, unsigned minor)
{
      volatile struct bpp_regs *base = base_addrs[minor];
      unsigned char bits_tcr = 0, bits_or = 0;

      if (instances[minor].direction & 0x20) bits_tcr |= P_TCR_DIR;
      if (   pins & BPP_PP_nStrobe)          bits_tcr |= P_TCR_DS;

      if (   pins & BPP_PP_nAutoFd)          bits_or |= P_OR_AFXN;
      if (! (pins & BPP_PP_nInit))           bits_or |= P_OR_INIT;
      if (! (pins & BPP_PP_nSelectIn))       bits_or |= P_OR_SLCT_IN;

      base->p_or = bits_or;
      base->p_tcr = bits_tcr;
}

/*
 * i386 people read output pins from a software image.
 * We may get them back from hardware.
 * Again, inversion of pins must he buried here.
 */
static unsigned short get_pins(unsigned minor)
{
      volatile struct bpp_regs *base = base_addrs[minor];
      unsigned short bits = 0;
      unsigned value_tcr = base->p_tcr;
      unsigned value_ir = base->p_ir;
      unsigned value_or = base->p_or;

      if (value_tcr & P_TCR_DS)         bits |= BPP_PP_nStrobe;
      if (value_or & P_OR_AFXN)         bits |= BPP_PP_nAutoFd;
      if (! (value_or & P_OR_INIT))     bits |= BPP_PP_nInit;
      if (! (value_or & P_OR_SLCT_IN))  bits |= BPP_PP_nSelectIn;

      if (value_ir & P_IR_ERR)          bits |= BPP_GP_nFault;
      if (! (value_ir & P_IR_SLCT))     bits |= BPP_GP_Select;
      if (! (value_ir & P_IR_PE))       bits |= BPP_GP_PError;
      if (! (value_tcr & P_TCR_ACK))    bits |= BPP_GP_nAck;
      if (value_tcr & P_TCR_BUSY)       bits |= BPP_GP_Busy;

      return bits;
}

#if 0
/* P3 */
static inline void bpp_snap(const char *msg, unsigned minor)
{
	volatile struct bpp_regs *r = base_addrs[minor];
	printk("bpp.%s: c=%02x o=%02x i=%02x\n", msg, r->p_tcr, r->p_or, r->p_ir);
}
#endif

#endif /* __sparc__ */

static void bpp_wake_up(unsigned long val)
{ wake_up(&instances[val].wait_queue); }

static void snooze(unsigned long snooze_time, unsigned minor)
{
      instances[minor].timer_list.expires = jiffies + snooze_time + 1;
      instances[minor].timer_list.data    = minor;
      add_timer(&instances[minor].timer_list);
      sleep_on (&instances[minor].wait_queue);
}

static int wait_for(unsigned short set, unsigned short clr,
               unsigned long delay, unsigned minor)
{
      unsigned short pins = get_pins(minor);

      unsigned long extime = 0;

      /*
       * Try a real fast scan for the first jiffy, in case the device
       * responds real good. The first while loop guesses an expire
       * time accounting for possible wraparound of jiffies.
       */
      while (time_after_eq(jiffies, extime)) extime = jiffies + 1;
      while ( (time_before(jiffies, extime))
              && (((pins & set) != set) || ((pins & clr) != 0)) ) {
            pins = get_pins(minor);
      }

      delay -= 1;

      /*
       * If my delay expired or the pins are still not where I want
       * them, then resort to using the timer and greatly reduce my
       * sample rate. If the peripheral is going to be slow, this will
       * give the CPU up to some more worthy process.
       */
      while ( delay && (((pins & set) != set) || ((pins & clr) != 0)) ) {

            snooze(1, minor);
            pins = get_pins(minor);
            delay -= 1;
      }

      if (delay == 0) return -1;
      else return pins;
}

/*
 * Return ZERO(0) If the negotiation succeeds, an errno otherwise. An
 * errno means something broke, and I do not yet know how to fix it.
 */
static int negotiate(unsigned char mode, unsigned minor)
{
      int rc;
      unsigned short pins = get_pins(minor);
      if (pins & BPP_PP_nSelectIn) return -EIO;


        /* Event 0: Write the mode to the data lines */
      bpp_outb_p(mode, base_addrs[minor]);

      snooze(TIME_PSetup, minor);

        /* Event 1: Strobe the mode code into the peripheral */
      set_pins(BPP_PP_nSelectIn|BPP_PP_nStrobe|BPP_PP_nInit, minor);

        /* Wait for Event 2: Peripheral responds as a 1284 device. */
      rc = wait_for(BPP_GP_PError|BPP_GP_Select|BPP_GP_nFault,
                BPP_GP_nAck,
                TIME_PResponse,
                minor);

      if (rc == -1) return -ETIMEDOUT;

        /* Event 3: latch extensibility request */
      set_pins(BPP_PP_nSelectIn|BPP_PP_nInit, minor);

        /* ... quick nap while peripheral ponders the byte i'm sending...*/
      snooze(1, minor);

        /* Event 4: restore strobe, to ACK peripheral's response. */
      set_pins(BPP_PP_nSelectIn|BPP_PP_nAutoFd|BPP_PP_nStrobe|BPP_PP_nInit, minor);

        /* Wait for Event 6: Peripheral latches response bits */
      rc = wait_for(BPP_GP_nAck, 0, TIME_PSetup+TIME_PResponse, minor);
      if (rc == -1) return -EIO;

        /* A 1284 device cannot refuse nibble mode */
      if (mode == DEFAULT_NIBBLE) return 0;

      if (pins & BPP_GP_Select) return 0;

      return -EPROTONOSUPPORT;
}

static int terminate(unsigned minor)
{
      int rc;

        /* Event 22: Request termination of 1284 mode */
      set_pins(BPP_PP_nAutoFd|BPP_PP_nStrobe|BPP_PP_nInit, minor);

        /* Wait for Events 23 and 24: ACK termination request. */
      rc = wait_for(BPP_GP_Busy|BPP_GP_nFault,
                BPP_GP_nAck,
                TIME_PSetup+TIME_PResponse,
                minor);

      instances[minor].direction = 0;
      instances[minor].mode = COMPATIBILITY;

      if (rc == -1) {
          return -EIO;
      }

        /* Event 25: Handshake by lowering nAutoFd */
      set_pins(BPP_PP_nStrobe|BPP_PP_nInit, minor);

        /* Event 26: Peripheral wiggles lines... */

        /* Event 27: Peripheral sets nAck HIGH to ack handshake */
      rc = wait_for(BPP_GP_nAck, 0, TIME_PResponse, minor);
      if (rc == -1) {
          set_pins(BPP_PP_nAutoFd|BPP_PP_nStrobe|BPP_PP_nInit, minor);
          return -EIO;
      }

        /* Event 28: Finish phase by raising nAutoFd */
      set_pins(BPP_PP_nAutoFd|BPP_PP_nStrobe|BPP_PP_nInit, minor);

      return 0;
}


/*
 * Allow only one process to open the device at a time.
 */
static int bpp_open(struct inode *inode, struct file *f)
{
      unsigned minor = MINOR(inode->i_rdev);
      if (minor >= BPP_NO) return -ENODEV;
      if (! instances[minor].present) return -ENODEV;
      if (instances[minor].opened) return -EBUSY;

      instances[minor].opened = 1;

      return 0;
}

/*
 * When the process closes the device, this method is called to clean
 * up and reset the hardware. Always leave the device in compatibility
 * mode as this is a reasonable place to clean up from messes made by
 * ioctls, or other mayhem.
 */
static int bpp_release(struct inode *inode, struct file *f)
{
      unsigned minor = MINOR(inode->i_rdev);
      instances[minor].opened = 0;

      if (instances[minor].mode != COMPATIBILITY)
      terminate(minor);
      return 0;
}

static long read_nibble(unsigned minor, char *c, unsigned long cnt)
{
      unsigned long remaining = cnt;
      long rc;

      while (remaining > 0) {
          unsigned char byte = 0;
          int pins;

          /* Event 7: request nibble */
          set_pins(BPP_PP_nSelectIn|BPP_PP_nStrobe, minor);

          /* Wait for event 9: Peripher strobes first nibble */
          pins = wait_for(0, BPP_GP_nAck, TIME_IDLE_LIMIT, minor);
          if (pins == -1) return -ETIMEDOUT;

          /* Event 10: I handshake nibble */
          set_pins(BPP_PP_nSelectIn|BPP_PP_nStrobe|BPP_PP_nAutoFd, minor);
          if (pins & BPP_GP_nFault) byte |= 0x01;
          if (pins & BPP_GP_Select) byte |= 0x02;
          if (pins & BPP_GP_PError) byte |= 0x04;
          if (pins & BPP_GP_Busy)   byte |= 0x08;

          /* Wait for event 11: Peripheral handshakes nibble */
          rc = wait_for(BPP_GP_nAck, 0, TIME_PResponse, minor);

          /* Event 7: request nibble */
          set_pins(BPP_PP_nSelectIn|BPP_PP_nStrobe, minor);

          /* Wait for event 9: Peripher strobes first nibble */
          pins = wait_for(0, BPP_GP_nAck, TIME_PResponse, minor);
          if (rc == -1) return -ETIMEDOUT;

          /* Event 10: I handshake nibble */
          set_pins(BPP_PP_nSelectIn|BPP_PP_nStrobe|BPP_PP_nAutoFd, minor);
          if (pins & BPP_GP_nFault) byte |= 0x10;
          if (pins & BPP_GP_Select) byte |= 0x20;
          if (pins & BPP_GP_PError) byte |= 0x40;
          if (pins & BPP_GP_Busy)   byte |= 0x80;

          put_user_ret(byte, c, -EFAULT);
          c += 1;
          remaining -= 1;

          /* Wait for event 11: Peripheral handshakes nibble */
          rc = wait_for(BPP_GP_nAck, 0, TIME_PResponse, minor);
          if (rc == -1) return -EIO;
      }

      return cnt - remaining;
}

static long read_ecp(unsigned minor, char *c, unsigned long cnt)
{
      unsigned long remaining;
      long rc;

        /* Turn ECP mode from forward to reverse if needed. */
      if (! instances[minor].direction) {
          unsigned short pins = get_pins(minor);

            /* Event 38: Turn the bus around */
          instances[minor].direction = 0x20;
          pins &= ~BPP_PP_nAutoFd;
          set_pins(pins, minor);

            /* Event 39: Set pins for reverse mode. */
          snooze(TIME_PSetup, minor);
          set_pins(BPP_PP_nStrobe|BPP_PP_nSelectIn, minor);

            /* Wait for event 40: Peripheral ready to be strobed */
          rc = wait_for(0, BPP_GP_PError, TIME_PResponse, minor);
          if (rc == -1) return -ETIMEDOUT;
      }

      remaining = cnt;

      while (remaining > 0) {

            /* If there is a run length for a repeated byte, repeat */
            /* that byte a few times. */
          if (instances[minor].run_length && !instances[minor].run_flag) {

              char buffer[128];
              unsigned idx;
              unsigned repeat = remaining < instances[minor].run_length
                                     ? remaining
                               : instances[minor].run_length;

              for (idx = 0 ;  idx < repeat ;  idx += 1)
                buffer[idx] = instances[minor].repeat_byte;

              copy_to_user_ret(c, buffer, repeat, -EFAULT);
              remaining -= repeat;
              c += repeat;
              instances[minor].run_length -= repeat;
          }

          if (remaining == 0) break;


            /* Wait for Event 43: Data active on the bus. */
          rc = wait_for(0, BPP_GP_nAck, TIME_IDLE_LIMIT, minor);
          if (rc == -1) break;

          if (rc & BPP_GP_Busy) {
                /* OK, this is data. read it in. */
              unsigned char byte = bpp_inb(base_addrs[minor]);
              put_user_ret(byte, c, -EFAULT);
              c += 1;
              remaining -= 1;

              if (instances[minor].run_flag) {
                  instances[minor].repeat_byte = byte;
                  instances[minor].run_flag = 0;
              }

          } else {
              unsigned char byte = bpp_inb(base_addrs[minor]);
              if (byte & 0x80) {
                  printk("bpp%d: "
                         "Ignoring ECP channel %u from device.\n",
                         minor, byte & 0x7f);
              } else {
                  instances[minor].run_length = byte;
                  instances[minor].run_flag = 1;
              }
          }

            /* Event 44: I got it. */
          set_pins(BPP_PP_nStrobe|BPP_PP_nAutoFd|BPP_PP_nSelectIn, minor);

            /* Wait for event 45: peripheral handshake */
          rc = wait_for(BPP_GP_nAck, 0, TIME_PResponse, minor);
          if (rc == -1) return -ETIMEDOUT;

             /* Event 46: Finish handshake */
          set_pins(BPP_PP_nStrobe|BPP_PP_nSelectIn, minor);

      }


      return cnt - remaining;
}

static ssize_t bpp_read(struct file *f, char *c, size_t cnt, loff_t * ppos)
{
      long rc;
      const unsigned minor = MINOR(f->f_dentry->d_inode->i_rdev);
      if (minor >= BPP_NO) return -ENODEV;
      if (!instances[minor].present) return -ENODEV;

      switch (instances[minor].mode) {

        default:
          if (instances[minor].mode != COMPATIBILITY)
            terminate(minor);

          if (instances[minor].enhanced) {
              /* For now, do all reads with ECP-RLE mode */
              unsigned short pins;

              rc = negotiate(DEFAULT_ECP, minor);
              if (rc < 0) break;

              instances[minor].mode = ECP_RLE;

              /* Event 30: set nAutoFd low to setup for ECP mode */
              pins = get_pins(minor);
              pins &= ~BPP_PP_nAutoFd;
              set_pins(pins, minor);

              /* Wait for Event 31: peripheral ready */
              rc = wait_for(BPP_GP_PError, 0, TIME_PResponse, minor);
              if (rc == -1) return -ETIMEDOUT;

              rc = read_ecp(minor, c, cnt);

          } else {
              rc = negotiate(DEFAULT_NIBBLE, minor);
              if (rc < 0) break;

              instances[minor].mode = NIBBLE;

              rc = read_nibble(minor, c, cnt);
          }
          break;

        case NIBBLE:
          rc = read_nibble(minor, c, cnt);
          break;

        case ECP:
        case ECP_RLE:
          rc = read_ecp(minor, c, cnt);
          break;

      }


      return rc;
}

/*
 * Compatibility mode handshaking is a matter of writing data,
 * strobing it, and waiting for the printer to stop being busy.
 */
static long write_compat(unsigned minor, const char *c, unsigned long cnt)
{
      long rc;
      unsigned short pins = get_pins(minor);

      unsigned long remaining = cnt;


      while (remaining > 0) {
            unsigned char byte;

            get_user_ret(byte, c, -EFAULT);
            c += 1;

            rc = wait_for(BPP_GP_nAck, BPP_GP_Busy, TIME_IDLE_LIMIT, minor);
            if (rc == -1) return -ETIMEDOUT;

            bpp_outb_p(byte, base_addrs[minor]);
            remaining -= 1;
          /* snooze(1, minor); */

          pins &= ~BPP_PP_nStrobe;
          set_pins(pins, minor);

          rc = wait_for(BPP_GP_Busy, 0, TIME_PResponse, minor);

          pins |= BPP_PP_nStrobe;
          set_pins(pins, minor);
      }

      return cnt - remaining;
}

/*
 * Write data using ECP mode. Watch out that the port may be set up
 * for reading. If so, turn the port around.
 */
static long write_ecp(unsigned minor, const char *c, unsigned long cnt)
{
      unsigned short pins = get_pins(minor);
      unsigned long remaining = cnt;

      if (instances[minor].direction) {
          int rc;

            /* Event 47 Request bus be turned around */
          pins |= BPP_PP_nInit;
          set_pins(pins, minor);

            /* Wait for Event 49: Peripheral relinquished bus */
          rc = wait_for(BPP_GP_PError, 0, TIME_PResponse, minor);

          pins |= BPP_PP_nAutoFd;
          instances[minor].direction = 0;
          set_pins(pins, minor);
      }

      while (remaining > 0) {
          unsigned char byte;
          int rc;

          get_user_ret(byte, c, -EFAULT);

          rc = wait_for(0, BPP_GP_Busy, TIME_PResponse, minor);
          if (rc == -1) return -ETIMEDOUT;

          c += 1;

          bpp_outb_p(byte, base_addrs[minor]);

          pins &= ~BPP_PP_nStrobe;
          set_pins(pins, minor);

          pins |= BPP_PP_nStrobe;
          rc = wait_for(BPP_GP_Busy, 0, TIME_PResponse, minor);
          if (rc == -1) return -EIO;

          set_pins(pins, minor);
      }

      return cnt - remaining;
}

/*
 * Write to the peripheral. Be sensitive of the current mode. If I'm
 * in a mode that can be turned around (ECP) then just do
 * that. Otherwise, terminate and do my writing in compat mode. This
 * is the safest course as any device can handle it.
 */
static ssize_t bpp_write(struct file *f, const char *c, size_t cnt, loff_t * ppos)
{
      long errno = 0;
      const unsigned minor = MINOR(f->f_dentry->d_inode->i_rdev);
      if (minor >= BPP_NO) return -ENODEV;
      if (!instances[minor].present) return -ENODEV;

      switch (instances[minor].mode) {

        case ECP:
        case ECP_RLE:
          errno = write_ecp(minor, c, cnt);
          break;
        case COMPATIBILITY:
          errno = write_compat(minor, c, cnt);
          break;
        default:
          terminate(minor);
          errno = write_compat(minor, c, cnt);
      }

      return errno;
}

static int bpp_ioctl(struct inode *inode, struct file *f, unsigned int cmd,
		 unsigned long arg)
{
      int errno = 0;

      unsigned minor = MINOR(inode->i_rdev);
      if (minor >= BPP_NO) return -ENODEV;
      if (!instances[minor].present) return -ENODEV;


      switch (cmd) {

        case BPP_PUT_PINS:
          set_pins(arg, minor);
          break;

        case BPP_GET_PINS:
          errno = get_pins(minor);
          break;

        case BPP_PUT_DATA:
          bpp_outb_p(arg, base_addrs[minor]);
          break;

        case BPP_GET_DATA:
          errno = bpp_inb_p(base_addrs[minor]);
          break;

        case BPP_SET_INPUT:
          if (arg)
            if (instances[minor].enhanced) {
                unsigned short bits = get_pins(minor);
                instances[minor].direction = 0x20;
                set_pins(bits, minor);
            } else {
                errno = -ENOTTY;
            }
          else {
              unsigned short bits = get_pins(minor);
              instances[minor].direction = 0x00;
              set_pins(bits, minor);
          }
          break;

        default:
            errno = -EINVAL;
      }

      return errno;
}

static struct file_operations bpp_fops = {
	NULL,		/* bpp_lseek */
	bpp_read,
	bpp_write,
	NULL,		/* bpp_readdir */
	NULL,		/* bpp_select */
	bpp_ioctl,
	NULL,		/* bpp_mmap */
	bpp_open,
	NULL,		/* flush */
	bpp_release,
  NULL,   /* fsync */
  NULL,   /* fasync */
  NULL,   /* check media change */
  NULL,   /* revalidate */
  NULL,   /* lock */
};

#if defined(__i386__)

#define collectLptPorts()  {}

static void probeLptPort(unsigned idx)
{
      unsigned int testvalue;
      const unsigned short lpAddr = base_addrs[idx];

      instances[idx].present = 0;
      instances[idx].enhanced = 0;
      instances[idx].direction = 0;
      instances[idx].mode = COMPATIBILITY;
      instances[idx].wait_queue = 0;
      instances[idx].run_length = 0;
      instances[idx].run_flag = 0;
      init_timer(&instances[idx].timer_list);
      instances[idx].timer_list.function = bpp_wake_up;
      if (check_region(lpAddr,3)) return;

      /*
       * First, make sure the instance exists. Do this by writing to
       * the data latch and reading the value back. If the port *is*
       * present, test to see if it supports extended-mode
       * operation. This will be required for IEEE1284 reverse
       * transfers.
       */

      outb_p(BPP_PROBE_CODE, lpAddr);
      for (testvalue=0; testvalue<BPP_DELAY; testvalue++)
            ;
      testvalue = inb_p(lpAddr);
      if (testvalue == BPP_PROBE_CODE) {
            unsigned save;
            instances[idx].present = 1;

            request_region(lpAddr,3, dev_name);
            save = inb_p(lpAddr+2);
            for (testvalue=0; testvalue<BPP_DELAY; testvalue++)
                  ;
            outb_p(save|0x20, lpAddr+2);
            for (testvalue=0; testvalue<BPP_DELAY; testvalue++)
                  ;
            outb_p(~BPP_PROBE_CODE, lpAddr);
            for (testvalue=0; testvalue<BPP_DELAY; testvalue++)
                  ;
            testvalue = inb_p(lpAddr);
            if ((testvalue&0xff) == (0xff&~BPP_PROBE_CODE))
                  instances[idx].enhanced = 0;
            else
                  instances[idx].enhanced = 1;
            outb_p(save, lpAddr+2);
      }

      /*
       * Leave the port in compat idle mode.
       */
      set_pins(BPP_PP_nAutoFd|BPP_PP_nStrobe|BPP_PP_nInit, idx);

      printk("bpp%d: Port at 0x%03x: Enhanced mode %s\n", idx, base_addrs[idx],
            instances[idx].enhanced? "SUPPORTED" : "UNAVAILABLE");
}

static inline void freeLptPort(int idx)
{
      release_region(base_addrs[idx], 3);
}

#endif

#if defined(__sparc__)

static volatile struct bpp_regs *map_bpp(struct linux_sbus_device *dev, int idx)
{
      volatile struct bpp_regs *regs;

      /*
       * PROM reports different numbers on Zebra and on DMA2.
       * We need to figure out when to apply parent ranges.
       * printk will show this on different machines.
       */

      /* IPC Zebra   1.fa200000[1c] i=2  */
      prom_apply_sbus_ranges(dev->my_bus, &dev->reg_addrs[0],
			     dev->num_registers, dev);

      regs = sparc_alloc_io(dev->reg_addrs[0].phys_addr, 0,
			    dev->reg_addrs[0].reg_size, "bpp",
			    dev->reg_addrs[0].which_io, 0x0);
      printk("bpp%d.map_bpp: 0x%x.%p[0x%x] i=%d\n", idx,
	     dev->reg_addrs[0].which_io, dev->reg_addrs[0].phys_addr,
	     dev->reg_addrs[0].reg_size, dev->irqs[0]);

      return regs;
}

static int collectLptPorts(void)
{
	struct linux_sbus *bus;
	struct linux_sbus_device *dev;
	int count;

	count = 0;
	for_all_sbusdev(dev, bus) {
		if (strcmp(dev->prom_name, "SUNW,bpp") == 0) {
			if (count >= BPP_NO) {
				printk(KERN_NOTICE
				       "bpp: More than %d bpp ports,"
				       " rest is ignored\n", BPP_NO);
				return count;
			}
			base_addrs[count] = map_bpp(dev, count);
			count++;
		}
	}
	return count;
}

static void probeLptPort(unsigned idx)
{
      volatile struct bpp_regs *rp = base_addrs[idx];
      __u32 csr;
      char *brand;

      instances[idx].present = 0;
      instances[idx].enhanced = 0;
      instances[idx].direction = 0;
      instances[idx].mode = COMPATIBILITY;
      init_waitqueue_head(&instances[idx].wait_queue);
      instances[idx].run_length = 0;
      instances[idx].run_flag = 0;
      init_timer(&instances[idx].timer_list);
      instances[idx].timer_list.function = bpp_wake_up;

      if (rp == 0) return;

      instances[idx].present = 1;
      instances[idx].enhanced = 1;   /* Sure */

      if (((csr = rp->p_csr) & P_DRAINING) != 0 && (csr & P_ERR_PEND) == 0) {
            udelay(20);
            csr = rp->p_csr;
            if ((csr & P_DRAINING) != 0 && (csr & P_ERR_PEND) == 0) {
                  printk("bpp%d: DRAINING still active (0x%08x)\n", idx, csr);
            }
      }
      printk("bpp%d: reset with 0x%08x ..", idx, csr);
      rp->p_csr = (csr | P_RESET) & ~P_INT_EN;
      udelay(500);
      rp->p_csr &= ~P_RESET;
      printk(" done with csr=0x%08x ocr=0x%04x\n", rp->p_csr, rp->p_ocr);

      switch (rp->p_csr & P_DEV_ID_MASK) {
      case P_DEV_ID_ZEBRA:
            brand = "Zebra";
            break;
      case P_DEV_ID_L64854:
            brand = "DMA2";
            break;
      default:
            brand = "Unknown";
      }
      printk("bpp%d: %s at 0x%p\n", idx, brand, rp);

      /*
       * Leave the port in compat idle mode.
       */
      set_pins(BPP_PP_nAutoFd|BPP_PP_nStrobe|BPP_PP_nInit, idx);

      return;
}

static inline void freeLptPort(int idx)
{
      sparc_free_io ((void *)base_addrs[idx], sizeof(struct bpp_regs));
}

#endif

#ifdef MODULE
int init_module(void)
#else
int __init bpp_init(void)
#endif
{
	int rc;
	unsigned idx;

	rc = collectLptPorts();
	if (rc == 0)
		return -ENODEV;

	rc = register_chrdev(BPP_MAJOR, dev_name, &bpp_fops);
	if (rc < 0)
		return rc;

	for (idx = 0; idx < BPP_NO; idx += 1) {
		instances[idx].opened = 0;
		probeLptPort(idx);
	}

	return 0;
}

#ifdef MODULE
void cleanup_module(void)
{
	unsigned idx;

	unregister_chrdev(BPP_MAJOR, dev_name);

	for (idx = 0 ;  idx < BPP_NO ;  idx += 1) {
		if (instances[idx].present)
			freeLptPort(idx);
	}
}
#endif
