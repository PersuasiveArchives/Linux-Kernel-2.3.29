/* cm206.c. A linux-driver for the cm206 cdrom player with cm260 adapter card.
   Copyright (c) 1995--1997 David A. van Leeuwen.
   $Id: cm206.c,v 1.5 1997/12/26 11:02:51 david Exp $
   
     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published by
     the Free Software Foundation; either version 2 of the License, or
     (at your option) any later version.
     
     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.
     
     You should have received a copy of the GNU General Public License
     along with this program; if not, write to the Free Software
     Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

History:
 Started 25 jan 1994. Waiting for documentation...
 22 feb 1995: 0.1a first reasonably safe polling driver.
	      Two major bugs, one in read_sector and one in 
	      do_cm206_request, happened to cancel!
 25 feb 1995: 0.2a first reasonable interrupt driven version of above.
              uart writes are still done in polling mode. 
 25 feb 1995: 0.21a writes also in interrupt mode, still some
	      small bugs to be found... Larger buffer. 
  2 mrt 1995: 0.22 Bug found (cd-> nowhere, interrupt was called in
              initialization), read_ahead of 16. Timeouts implemented.
	      unclear if they do something...
  7 mrt 1995: 0.23 Start of background read-ahead.
 18 mrt 1995: 0.24 Working background read-ahead. (still problems)
 26 mrt 1995: 0.25 Multi-session ioctl added (kernel v1.2).
              Statistics implemented, though separate stats206.h.
	      Accessible trough ioctl 0x1000 (just a number).
	      Hard to choose between v1.2 development and 1.1.75.
	      Bottom-half doesn't work with 1.2...
	      0.25a: fixed... typo. Still problems...
  1 apr 1995: 0.26 Module support added. Most bugs found. Use kernel 1.2.n.
  5 apr 1995: 0.27 Auto-probe for the adapter card base address.
              Auto-probe for the adaptor card irq line.
  7 apr 1995: 0.28 Added lilo setup support for base address and irq.
              Use major number 32 (not in this source), officially
	      assigned to this driver.
  9 apr 1995: 0.29 Added very limited audio support. Toc_header, stop, pause,
              resume, eject. Play_track ignores track info, because we can't 
	      read a table-of-contents entry. Toc_entry is implemented
	      as a `placebo' function: always returns start of disc. 
  3 may 1995: 0.30 Audio support completed. The get_toc_entry function
              is implemented as a binary search. 
 15 may 1995: 0.31 More work on audio stuff. Workman is not easy to 
              satisfy; changed binary search into linear search.
	      Auto-probe for base address somewhat relaxed.
  1 jun 1995: 0.32 Removed probe_irq_on/off for module version.
 10 jun 1995: 0.33 Workman still behaves funny, but you should be
              able to eject and substitute another disc.

 An adaptation of 0.33 is included in linux-1.3.7 by Eberhard Moenkeberg

 18 jul 1995: 0.34 Patch by Heiko Eissfeldt included, mainly considering 
              verify_area's in the ioctls. Some bugs introduced by 
	      EM considering the base port and irq fixed. 

 18 dec 1995: 0.35 Add some code for error checking... no luck...

 We jump to reach our goal: version 1.0 in the next stable linux kernel.

 19 mar 1996: 0.95 Different implementation of CDROM_GET_UPC, on
	      request of Thomas Quinot. 
 25 mar 1996: 0.96 Interpretation of opening with O_WRONLY or O_RDWR:
	      open only for ioctl operation, e.g., for operation of
	      tray etc.
 4 apr 1996:  0.97 First implementation of layer between VFS and cdrom
              driver, a generic interface. Much of the functionality
	      of cm206_open() and cm206_ioctl() is transferred to a
	      new file cdrom.c and its header ucdrom.h. 

	      Upgrade to Linux kernel 1.3.78. 

 11 apr 1996  0.98 Upgrade to Linux kernel 1.3.85
              More code moved to cdrom.c
 
 	      0.99 Some more small changes to decrease number
 	      of oopses at module load; 
 
 27 jul 1996  0.100 Many hours of debugging, kernel change from 1.2.13
	      to 2.0.7 seems to have introduced some weird behavior
	      in (interruptible_)sleep_on(&cd->data): the process
	      seems to be woken without any explicit wake_up in my own
	      code. Patch to try 100x in case such untriggered wake_up's 
	      occur. 

 28 jul 1996  0.101 Rewriting of the code that receives the command echo,
	      using a fifo to store echoed bytes. 

 	      Branch from 0.99:
 
 	      0.99.1.0 Update to kernel release 2.0.10 dev_t -> kdev_t
 	      (emoenke) various typos found by others.  extra
 	      module-load oops protection.
 
 	      0.99.1.1 Initialization constant cdrom_dops.speed
 	      changed from float (2.0) to int (2); Cli()-sti() pair
 	      around cm260_reset() in module initialization code.
 
 	      0.99.1.2 Changes literally as proposed by Scott Snyder
 	      <snyder@d0sgif.fnal.gov> for the 2.1 kernel line, which
 	      have to do mainly with the poor minor support i had. The
 	      major new concept is to change a cdrom driver's
 	      operations struct from the capabilities struct. This
 	      reflects the fact that there is one major for a driver,
 	      whilst there can be many minors whith completely
 	      different capabilities.

	      0.99.1.3 More changes for operations/info separation.

	      0.99.1.4 Added speed selection (someone had to do this
	      first).

  23 jan 1997 0.99.1.5 MODULE_PARMS call added.

  23 jan 1997 0.100.1.2--0.100.1.5 following similar lines as 
  	      0.99.1.1--0.99.1.5. I get too many complaints about the
	      drive making read errors. What't wrong with the 2.0+
	      kernel line? Why get i (and othe cm206 owners) weird
	      results? Why were things good in the good old 1.1--1.2 
	      era? Why don't i throw away the drive?

 2 feb 1997   0.102 Added `volatile' to values in cm206_struct. Seems to 
 	      reduce many of the problems. Rewrote polling routines
	      to use fixed delays between polls. 
	      0.103 Changed printk behavior. 
	      0.104 Added a 0.100 -> 0.100.1.1 change

11 feb 1997   0.105 Allow auto_probe during module load, disable
              with module option "auto_probe=0". Moved some debugging
	      statements to lower priority. Implemented select_speed()
	      function. 

13 feb 1997   1.0 Final version for 2.0 kernel line. 

	      All following changes will be for the 2.1 kernel line. 

15 feb 1997   1.1 Keep up with kernel 2.1.26, merge in changes from 
              cdrom.c 0.100.1.1--1.0. Add some more MODULE_PARMS. 

14 sep 1997   1.2 Upgrade to Linux 2.1.55.  Added blksize_size[], patch
              sent by James Bottomley <James.Bottomley@columbiasc.ncr.com>.

21 dec 1997   1.4 Upgrade to Linux 2.1.72.  

24 jan 1998   Removed the cm206_disc_status() function, as it was now dead
              code.  The Uniform CDROM driver now provides this functionality.
 * 
 * Parts of the code are based upon lmscd.c written by Kai Petzke,
 * sbpcd.c written by Eberhard Moenkeberg, and mcd.c by Martin
 * Harriss, but any off-the-shelf dynamic programming algorithm won't
 * be able to find them.
 *
 * The cm206 drive interface and the cm260 adapter card seem to be 
 * sufficiently different from their cm205/cm250 counterparts
 * in order to write a complete new driver.
 * 
 * I call all routines connected to the Linux kernel something
 * with `cm206' in it, as this stuff is too series-dependent. 
 * 
 * Currently, my limited knowledge is based on:
 * - The Linux Kernel Hacker's guide, v. 0.5, by Michael K. Johnson
 * - Linux Kernel Programmierung, by Michael Beck and others
 * - Philips/LMS cm206 and cm226 product specification
 * - Philips/LMS cm260 product specification
 *
 * David van Leeuwen, david@tm.tno.nl.  */
#define REVISION "$Revision: 1.5 $"

#include <linux/module.h>	

#include <linux/errno.h>	/* These include what we really need */
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/cdrom.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/init.h>

/* #include <linux/ucdrom.h> */

#include <asm/io.h>

#define MAJOR_NR CM206_CDROM_MAJOR
#include <linux/blk.h>

#undef DEBUG
#define STATISTICS		/* record times and frequencies of events */
#define AUTO_PROBE_MODULE
#define USE_INSW

#include "cm206.h"

/* This variable defines whether or not to probe for adapter base port 
   address and interrupt request. It can be overridden by the boot 
   parameter `auto'.
*/
static int auto_probe=1;	/* Yes, why not? */

static int cm206_base = CM206_BASE;
static int cm206_irq = CM206_IRQ; 
MODULE_PARM(cm206_base, "i");	/* base */
MODULE_PARM(cm206_irq, "i");	/* irq */
MODULE_PARM(cm206, "1-2i");	/* base,irq or irq,base */
MODULE_PARM(auto_probe, "i");	/* auto probe base and irq */

#define POLLOOP 100		/* milliseconds */
#define READ_AHEAD 1		/* defines private buffer, waste! */
#define BACK_AHEAD 1		/* defines adapter-read ahead */
#define DATA_TIMEOUT (3*HZ)	/* measured in jiffies (10 ms) */
#define UART_TIMEOUT (5*HZ/100)
#define DSB_TIMEOUT (7*HZ)	/* time for the slowest command to finish */
#define UR_SIZE 4		/* uart receive buffer fifo size */

#define LINUX_BLOCK_SIZE 512	/* WHERE is this defined? */
#define RAW_SECTOR_SIZE 2352	/* ok, is also defined in cdrom.h */
#define ISO_SECTOR_SIZE 2048
#define BLOCKS_ISO (ISO_SECTOR_SIZE/LINUX_BLOCK_SIZE) /* 4 */
#define CD_SYNC_HEAD 16		/* CD_SYNC + CD_HEAD */

#ifdef STATISTICS		/* keep track of errors in counters */
#define stats(i) { ++cd->stats[st_ ## i]; \
		     cd->last_stat[st_ ## i] = cd->stat_counter++; \
		 }
#else
#define stats(i) (void) 0;
#endif

#define Debug(a) {printk (KERN_DEBUG); printk a;}
#ifdef DEBUG
#define debug(a) Debug(a)
#else
#define debug(a) (void) 0;
#endif

typedef unsigned char uch;	/* 8-bits */
typedef unsigned short ush;	/* 16-bits */

struct toc_struct{		/* private copy of Table of Contents */
  uch track, fsm[3], q0;
};

static int cm206_blocksizes[1] = { 2048 };

struct cm206_struct {
  volatile ush intr_ds;		/* data status read on last interrupt */
  volatile ush intr_ls;		/* uart line status read on last interrupt*/
  volatile uch ur[UR_SIZE];	/* uart receive buffer fifo */
  volatile uch ur_w, ur_r;	/* write/read buffer index */
  volatile uch dsb, cc;	 /* drive status byte and condition (error) code */
  int command;			/* command to be written to the uart */
  int openfiles;
  ush sector[READ_AHEAD*RAW_SECTOR_SIZE/2]; /* buffered cd-sector */
  int sector_first, sector_last; /* range of these sectors */
  wait_queue_head_t uart;	/* wait queues for interrupt */
  wait_queue_head_t data;
  struct timer_list timer;	/* time-out */
  char timed_out;
  signed char max_sectors;	/* number of sectors that fit in adapter mem */
  char wait_back;		/* we're waiting for a background-read */
  char background;		/* is a read going on in the background? */
  int adapter_first;		/* if so, that's the starting sector */
  int adapter_last;
  char fifo_overflowed;
  uch disc_status[7];		/* result of get_disc_status command */
#ifdef STATISTICS
  int stats[NR_STATS];
  int last_stat[NR_STATS];	/* `time' at which stat was stat */
  int stat_counter;
#endif  
  struct toc_struct toc[101];	/* The whole table of contents + lead-out */
  uch q[10];			/* Last read q-channel info */
  uch audio_status[5];		/* last read position on pause */
  uch media_changed;		/* record if media changed */
};

#define DISC_STATUS cd->disc_status[0]
#define FIRST_TRACK cd->disc_status[1]
#define LAST_TRACK cd->disc_status[2]
#define PAUSED cd->audio_status[0] /* misuse this memory byte! */
#define PLAY_TO cd->toc[0]	/* toc[0] records end-time in play */

static struct cm206_struct * cd; /* the main memory structure */

/* First, we define some polling functions. These are actually
   only being used in the initialization. */

void send_command_polled(int command)
{
  int loop=POLLOOP;
  while (!(inw(r_line_status) & ls_transmitter_buffer_empty) && loop>0) {
    mdelay(1);		/* one millisec delay */
    --loop;
  }
  outw(command, r_uart_transmit);
}

uch receive_echo_polled(void)
{
  int loop=POLLOOP;
  while (!(inw(r_line_status) & ls_receive_buffer_full) && loop>0) {
    mdelay(1);
    --loop;
  }
  return ((uch) inw(r_uart_receive));
}

uch send_receive_polled(int command)
{
  send_command_polled(command);
  return receive_echo_polled();
}

inline void clear_ur(void) {
  if (cd->ur_r != cd->ur_w) {
    debug(("Deleting bytes from fifo:"));
    for(;cd->ur_r != cd->ur_w; cd->ur_r++, cd->ur_r %= UR_SIZE)
      debug((" 0x%x", cd->ur[cd->ur_r]));
    debug(("\n"));
  }
}

/* The interrupt handler. When the cm260 generates an interrupt, very
   much care has to be taken in reading out the registers in the right
   order; in case of a receive_buffer_full interrupt, first the
   uart_receive must be read, and then the line status again to
   de-assert the interrupt line. It took me a couple of hours to find
   this out:-( 

   The function reset_cm206 appears to cause an interrupt, because
   pulling up the INIT line clears both the uart-write-buffer /and/
   the uart-write-buffer-empty mask. We call this a `lost interrupt,'
   as there seems so reason for this to happen.
*/

static void cm206_interrupt(int sig, void *dev_id, struct pt_regs * regs) 
/* you rang? */
{
  volatile ush fool;
  cd->intr_ds = inw(r_data_status); /* resets data_ready, data_error,
				       crc_error, sync_error, toc_ready 
				       interrupts */
  cd->intr_ls = inw(r_line_status); /* resets overrun bit */
  debug(("Intr, 0x%x 0x%x, %d\n", cd->intr_ds, cd->intr_ls, cd->background));
  if (cd->intr_ls & ls_attention) stats(attention);
  /* receive buffer full? */
  if (cd->intr_ls & ls_receive_buffer_full) {	
    cd->ur[cd->ur_w] = inb(r_uart_receive); /* get order right! */
    cd->intr_ls = inw(r_line_status); /* resets rbf interrupt */
    debug(("receiving #%d: 0x%x\n", cd->ur_w, cd->ur[cd->ur_w]));
    cd->ur_w++; cd->ur_w %= UR_SIZE;
    if (cd->ur_w == cd->ur_r) debug(("cd->ur overflow!\n"));
    if (waitqueue_active(&cd->uart) && cd->background < 2) { 
      del_timer(&cd->timer);
      wake_up_interruptible(&cd->uart);
    }
  }
  /* data ready in fifo? */
  else if (cd->intr_ds & ds_data_ready) { 
    if (cd->background) ++cd->adapter_last;
    if (waitqueue_active(&cd->data) && (cd->wait_back || !cd->background)) {
      del_timer(&cd->timer);
      wake_up_interruptible(&cd->data);
    }
    stats(data_ready);
  }
  /* ready to issue a write command? */
  else if (cd->command && cd->intr_ls & ls_transmitter_buffer_empty) {
    outw(dc_normal | (inw(r_data_status) & 0x7f), r_data_control);
    outw(cd->command, r_uart_transmit);
    cd->command=0;
    if (!cd->background) wake_up_interruptible(&cd->uart);
  }
  /* now treat errors (at least, identify them for debugging) */
  else if (cd->intr_ds & ds_fifo_overflow) {
    debug(("Fifo overflow at sectors 0x%x\n", cd->sector_first));
    fool = inw(r_fifo_output_buffer);	/* de-assert the interrupt */
    cd->fifo_overflowed=1;	/* signal one word less should be read */
    stats(fifo_overflow);
  }
  else if (cd->intr_ds & ds_data_error) {
    debug(("Data error at sector 0x%x\n", cd->sector_first));
    stats(data_error);
  }
  else if (cd->intr_ds & ds_crc_error) {
    debug(("CRC error at sector 0x%x\n", cd->sector_first));
    stats(crc_error);
  }
  else if (cd->intr_ds & ds_sync_error) {
    debug(("Sync at sector 0x%x\n", cd->sector_first));
    stats(sync_error);
  }
  else if (cd->intr_ds & ds_toc_ready) {
    /* do something appropriate */
  }
  /* couldn't see why this interrupt, maybe due to init */
  else {			
    outw(dc_normal | READ_AHEAD, r_data_control);
    stats(lost_intr);
  }
  if (cd->background && (cd->adapter_last-cd->adapter_first == cd->max_sectors
			 || cd->fifo_overflowed))
    mark_bh(CM206_BH);	/* issue a stop read command */
  stats(interrupt);
}

/* we have put the address of the wait queue in who */
void cm206_timeout(unsigned long who)
{
  cd->timed_out = 1;
  debug(("Timing out\n"));
  wake_up_interruptible((wait_queue_head_t *)who);
}

/* This function returns 1 if a timeout occurred, 0 if an interrupt
   happened */
int sleep_or_timeout(wait_queue_head_t *wait, int timeout)
{
  cd->timed_out=0;
  cd->timer.data=(unsigned long) wait;
  cd->timer.expires = jiffies + timeout;
  add_timer(&cd->timer);
  debug(("going to sleep\n"));
  interruptible_sleep_on(wait);
  del_timer(&cd->timer);
  if (cd->timed_out) {
    cd->timed_out = 0;
    return 1;
  }
  else return 0;
}

void cm206_delay(int nr_jiffies) 
{
  DECLARE_WAIT_QUEUE_HEAD(wait);
  sleep_or_timeout(&wait, nr_jiffies);
}

void send_command(int command)
{
  debug(("Sending 0x%x\n", command));
  if (!(inw(r_line_status) & ls_transmitter_buffer_empty)) {
    cd->command = command;
    cli();			/* don't interrupt before sleep */
    outw(dc_mask_sync_error | dc_no_stop_on_error | 
	 (inw(r_data_status) & 0x7f), r_data_control);
    /* interrupt routine sends command */
    if (sleep_or_timeout(&cd->uart, UART_TIMEOUT)) {
      debug(("Time out on write-buffer\n"));
      stats(write_timeout);
      outw(command, r_uart_transmit);
    }
    debug(("Write commmand delayed\n"));
  }
  else outw(command, r_uart_transmit);
}

uch receive_byte(int timeout)
{
  uch ret;
  cli();
  debug(("cli\n"));
  ret = cd->ur[cd->ur_r];
  if (cd->ur_r != cd->ur_w) {
    sti();
    debug(("returning #%d: 0x%x\n", cd->ur_r, cd->ur[cd->ur_r]));
    cd->ur_r++; cd->ur_r %= UR_SIZE;
    return ret;
  } 
  else if (sleep_or_timeout(&cd->uart, timeout)) { /* does sti() */
    debug(("Time out on receive-buffer\n"));
#ifdef STATISTICS
    if (timeout==UART_TIMEOUT) stats(receive_timeout) /* no `;'! */
    else stats(dsb_timeout);
#endif
    return 0xda;
  }
  ret = cd->ur[cd->ur_r];  
  debug(("slept; returning #%d: 0x%x\n", cd->ur_r, cd->ur[cd->ur_r]));
  cd->ur_r++; cd->ur_r %= UR_SIZE;
  return ret;
}

inline uch receive_echo(void)
{
  return receive_byte(UART_TIMEOUT);
}

inline uch send_receive(int command)
{
  send_command(command);
  return receive_echo();
}

inline uch wait_dsb(void)
{
  return receive_byte(DSB_TIMEOUT);
}

int type_0_command(int command, int expect_dsb)
{
  int e;
  clear_ur();
  if (command != (e=send_receive(command))) {
    debug(("command 0x%x echoed as 0x%x\n", command, e));
    stats(echo);
    return -1;
  }
  if (expect_dsb) {
    cd->dsb = wait_dsb();	/* wait for command to finish */
  }
  return 0;
}

int type_1_command(int command, int bytes, uch * status) /* returns info */
{
  int i;
  if (type_0_command(command,0)) return -1;
  for(i=0; i<bytes; i++) 
    status[i] = send_receive(c_gimme);
  return 0;
}  

/* This function resets the adapter card. We'd better not do this too
 * often, because it tends to generate `lost interrupts.' */
void reset_cm260(void)
{
  outw(dc_normal | dc_initialize | READ_AHEAD, r_data_control);
  udelay(10);			/* 3.3 mu sec minimum */
  outw(dc_normal | READ_AHEAD, r_data_control);
}

/* fsm: frame-sec-min from linear address; one of many */
void fsm(int lba, uch * fsm) 
{
  fsm[0] = lba % 75;
  lba /= 75; lba += 2;
  fsm[1] = lba % 60; fsm[2] = lba / 60;
}

inline int fsm2lba(uch * fsm) 
{
  return fsm[0] + 75*(fsm[1]-2 + 60*fsm[2]);
}

inline int f_s_m2lba(uch f, uch s, uch m)
{
  return f + 75*(s-2 + 60*m);
}

int start_read(int start) 
{
  uch read_sector[4] = {c_read_data, };
  int i, e;

  fsm(start, &read_sector[1]);
  clear_ur();
  for (i=0; i<4; i++) 
    if (read_sector[i] != (e=send_receive(read_sector[i]))) {
      debug(("read_sector: %x echoes %x\n", read_sector[i], e));
      stats(echo);
      if (e==0xff) {		/* this seems to happen often */
	e = receive_echo();
	debug(("Second try %x\n", e));
	if (e!=read_sector[i]) return -1;
      }
    }
  return 0;
}

int stop_read(void)
{
  int e;
  type_0_command(c_stop,0);
  if((e=receive_echo()) != 0xff) {
    debug(("c_stop didn't send 0xff, but 0x%x\n", e));
    stats(stop_0xff);
    return -1;
  }
  return 0;
}  

/* This function starts to read sectors in adapter memory, the
   interrupt routine should stop the read. In fact, the bottom_half
   routine takes care of this. Set a flag `background' in the cd
   struct to indicate the process. */

int read_background(int start, int reading)
{
  if (cd->background) return -1; /* can't do twice */
  outw(dc_normal | BACK_AHEAD, r_data_control);
  if (!reading && start_read(start)) return -2;
  cd->adapter_first = cd->adapter_last = start; 
  cd->background = 1;		/* flag a read is going on */
  return 0;
}

#ifdef USE_INSW
#define transport_data insw
#else
/* this routine implements insw(,,). There was a time i had the
   impression that there would be any difference in error-behaviour. */
void transport_data(int port, ush * dest, int count) 
{
  int i;
  ush * d;
  for (i=0, d=dest; i<count; i++, d++) 
    *d = inw(port);
}
#endif


#define MAX_TRIES 100
int read_sector(int start)
{
  int tries=0;
  if (cd->background) {
    cd->background=0;
    cd->adapter_last = -1;	/* invalidate adapter memory */
    stop_read();
  }
  cd->fifo_overflowed=0;
  reset_cm260();		/* empty fifo etc. */
  if (start_read(start)) return -1;
  do {
    if (sleep_or_timeout(&cd->data, DATA_TIMEOUT)) {
      debug(("Read timed out sector 0x%x\n", start));
      stats(read_timeout);
      stop_read();
      return -3;		
    } 
    tries++;
  } while (cd->intr_ds & ds_fifo_empty && tries < MAX_TRIES);
  if (tries>1) debug(("Took me some tries\n"))
  else if (tries == MAX_TRIES) 
    debug(("MAX_TRIES tries for read sector\n"));
  transport_data(r_fifo_output_buffer, cd->sector, 
		 READ_AHEAD*RAW_SECTOR_SIZE/2);
  if (read_background(start+READ_AHEAD,1)) stats(read_background);
  cd->sector_first = start; cd->sector_last = start+READ_AHEAD;
  stats(read_restarted);
  return 0;
}

/* The function of bottom-half is to send a stop command to the drive
   This isn't easy because the routine is not `owned' by any process;
   we can't go to sleep! The variable cd->background gives the status:
   0 no read pending
   1 a read is pending
   2 c_stop waits for write_buffer_empty
   3 c_stop waits for receive_buffer_full: echo
   4 c_stop waits for receive_buffer_full: 0xff
*/

void cm206_bh(void)
{
  debug(("bh: %d\n", cd->background));
  switch (cd->background) {
  case 1:
    stats(bh);
    if (!(cd->intr_ls & ls_transmitter_buffer_empty)) {
      cd->command = c_stop;
      outw(dc_mask_sync_error | dc_no_stop_on_error | 
	   (inw(r_data_status) & 0x7f), r_data_control);
      cd->background=2;
      break;			/* we'd better not time-out here! */
    }
    else outw(c_stop, r_uart_transmit);
    /* fall into case 2: */
  case 2:			
    /* the write has been satisfied by interrupt routine */
    cd->background=3;
    break;
  case 3:
    if (cd->ur_r != cd->ur_w) {
      if (cd->ur[cd->ur_r] != c_stop) {
	debug(("cm206_bh: c_stop echoed 0x%x\n", cd->ur[cd->ur_r]));
	stats(echo);
      }
      cd->ur_r++; cd->ur_r %= UR_SIZE;
    }
    cd->background++;
    break;
  case 4:
    if (cd->ur_r != cd->ur_w) {
      if (cd->ur[cd->ur_r] != 0xff) {
	debug(("cm206_bh: c_stop reacted with 0x%x\n", cd->ur[cd->ur_r]));
	stats(stop_0xff);
      }
      cd->ur_r++; cd->ur_r %= UR_SIZE;
    }
    cd->background=0;
  }
}

/* This command clears the dsb_possible_media_change flag, so we must 
 * retain it.
 */
void get_drive_status(void)
{
  uch status[2];
  type_1_command(c_drive_status, 2, status); /* this might be done faster */
  cd->dsb=status[0];
  cd->cc=status[1];
  cd->media_changed |= 
    !!(cd->dsb & (dsb_possible_media_change | 
		  dsb_drive_not_ready | dsb_tray_not_closed));
}

void get_disc_status(void)
{
  if (type_1_command(c_disc_status, 7, cd->disc_status)) {
    debug(("get_disc_status: error\n"));
  }
}

/* The new open. The real opening strategy is defined in cdrom.c. */

static int cm206_open(struct cdrom_device_info * cdi, int purpose) 
{
  if (!cd->openfiles) {		/* reset only first time */
    cd->background=0;
    reset_cm260();
    cd->adapter_last = -1;	/* invalidate adapter memory */
    cd->sector_last = -1;
  }
  ++cd->openfiles; MOD_INC_USE_COUNT;
  stats(open);
  return 0;
}

static void cm206_release(struct cdrom_device_info * cdi)
{
  if (cd->openfiles==1) {
    if (cd->background) {
      cd->background=0;
      stop_read();
    }
    cd->sector_last = -1;	/* Make our internal buffer invalid */
    FIRST_TRACK = 0;		/* No valid disc status */
  }
  --cd->openfiles; MOD_DEC_USE_COUNT;
}

/* Empty buffer empties $sectors$ sectors of the adapter card buffer,
 * and then reads a sector in kernel memory.  */
void empty_buffer(int sectors) 
{
  while (sectors>=0) {
    transport_data(r_fifo_output_buffer, cd->sector + cd->fifo_overflowed, 
	 RAW_SECTOR_SIZE/2 - cd->fifo_overflowed);
    --sectors;
    ++cd->adapter_first;	/* update the current adapter sector */
    cd->fifo_overflowed=0;	/* reset overflow bit */
    stats(sector_transferred);
  } 
  cd->sector_first=cd->adapter_first-1;
  cd->sector_last=cd->adapter_first; /* update the buffer sector */
}

/* try_adapter. This function determines if the requested sector is
   in adapter memory, or will appear there soon. Returns 0 upon
   success */
int try_adapter(int sector)
{
  if (cd->adapter_first <= sector && sector < cd->adapter_last) { 
    /* sector is in adapter memory */
    empty_buffer(sector - cd->adapter_first);
    return 0;
  }
  else if (cd->background==1 && cd->adapter_first <= sector
	   && sector < cd->adapter_first+cd->max_sectors) {
    /* a read is going on, we can wait for it */
    cd->wait_back=1;
    while (sector >= cd->adapter_last) {
      if (sleep_or_timeout(&cd->data, DATA_TIMEOUT)) {
	debug(("Timed out during background wait: %d %d %d %d\n", sector, 
	       cd->adapter_last, cd->adapter_first, cd->background));
	stats(back_read_timeout);
	cd->wait_back=0;
	return -1;
      }
    }
    cd->wait_back=0;
    empty_buffer(sector - cd->adapter_first);
    return 0;
  }
  else return -2;
}

/* This is not a very smart implementation. We could optimize for 
   consecutive block numbers. I'm not convinced this would really
   bring down the processor load. */
static void do_cm206_request(void)
{
  long int i, cd_sec_no;
  int quarter, error; 
  uch * source, * dest;
  
  while(1) {	 /* repeat until all requests have been satisfied */
    INIT_REQUEST;
    if (CURRENT == NULL || CURRENT->rq_status == RQ_INACTIVE)
      return;
    if (CURRENT->cmd != READ) {
      debug(("Non-read command %d on cdrom\n", CURRENT->cmd));
      end_request(0);
      continue;
    }
    spin_unlock_irq(&io_request_lock);
    error=0;
    for (i=0; i<CURRENT->nr_sectors; i++) {
      int e1, e2;
      cd_sec_no = (CURRENT->sector+i)/BLOCKS_ISO; /* 4 times 512 bytes */
      quarter = (CURRENT->sector+i) % BLOCKS_ISO; 
      dest = CURRENT->buffer + i*LINUX_BLOCK_SIZE;
      /* is already in buffer memory? */
      if (cd->sector_first <= cd_sec_no && cd_sec_no < cd->sector_last) {
	source = ((uch *) cd->sector) + 16 + quarter*LINUX_BLOCK_SIZE 
	  + (cd_sec_no-cd->sector_first)*RAW_SECTOR_SIZE;
 	memcpy(dest, source, LINUX_BLOCK_SIZE); 
      }
      else if (!(e1=try_adapter(cd_sec_no)) || 
	       !(e2=read_sector(cd_sec_no))) {
	source =  ((uch *) cd->sector)+16+quarter*LINUX_BLOCK_SIZE;
	memcpy(dest, source, LINUX_BLOCK_SIZE); 
      }
      else {
	error=1;
	debug(("cm206_request: %d %d\n", e1, e2));
      }
    }
    spin_lock_irq(&io_request_lock);
    end_request(!error);
  }
}

/* Audio support. I've tried very hard, but the cm206 drive doesn't 
   seem to have a get_toc (table-of-contents) function, while i'm
   pretty sure it must read the toc upon disc insertion. Therefore
   this function has been implemented through a binary search 
   strategy. All track starts that happen to be found are stored in
   cd->toc[], for future use. 

   I've spent a whole day on a bug that only shows under Workman---
   I don't get it. Tried everything, nothing works. If workman asks
   for track# 0xaa, it'll get the wrong time back. Any other program
   receives the correct value. I'm stymied.
*/

/* seek seeks to address lba. It does wait to arrive there. */
void seek(int lba)
{
  int i;
  uch seek_command[4]={c_seek, };
  
  fsm(lba, &seek_command[1]);
  for (i=0; i<4; i++) type_0_command(seek_command[i], 0);
  cd->dsb = wait_dsb();
}

uch bcdbin(unsigned char bcd)	/* stolen from mcd.c! */
{
  return (bcd >> 4)*10 + (bcd & 0xf);
} 

inline uch normalize_track(uch track) 
{
  if (track<1) return 1;
  if (track>LAST_TRACK) return LAST_TRACK+1;
  return track;
}

/* This function does a binary search for track start. It records all
 * tracks seen in the process. Input $track$ must be between 1 and
 * #-of-tracks+1.  Note that the start of the disc must be in toc[1].fsm. 
 */
int get_toc_lba(uch track)
{
  int max=74*60*75-150, min=fsm2lba(cd->toc[1].fsm);
  int i, lba, l, old_lba=0;
  uch * q = cd->q;
  uch ct;			/* current track */
  int binary=0;
  const int skip = 3*60*75;		/* 3 minutes */

  for (i=track; i>0; i--) if (cd->toc[i].track) {
    min = fsm2lba(cd->toc[i].fsm);
    break;
  }
  lba = min + skip;
  do {
    seek(lba); 
    type_1_command(c_read_current_q, 10, q);
    ct = normalize_track(q[1]);
    if (!cd->toc[ct].track) {
      l = q[9]-bcdbin(q[5]) + 75*(q[8]-bcdbin(q[4])-2 + 
				  60*(q[7]-bcdbin(q[3])));
      cd->toc[ct].track=q[1];	/* lead out still 0xaa */
      fsm(l, cd->toc[ct].fsm);
      cd->toc[ct].q0 = q[0];	/* contains adr and ctrl info */
      if (ct==track) return l;
    }
    old_lba=lba;
    if (binary) {
      if (ct < track) min = lba; else max = lba;
      lba = (min+max)/2; 
    } else {
      if(ct < track) lba += skip;
      else {
	binary=1;
	max = lba; min = lba - skip;
	lba = (min+max)/2;
      }
    }
  } while (lba!=old_lba);
  return lba;
}

void update_toc_entry(uch track) 
{
  track = normalize_track(track);
  if (!cd->toc[track].track) get_toc_lba(track);
}

/* return 0 upon success */
int read_toc_header(struct cdrom_tochdr * hp)
{
  if (!FIRST_TRACK) get_disc_status();
  if (hp) { 
    int i;
    hp->cdth_trk0 = FIRST_TRACK;
    hp->cdth_trk1 = LAST_TRACK; 
				/* fill in first track position */
    for (i=0; i<3; i++) cd->toc[1].fsm[i] = cd->disc_status[3+i];
    update_toc_entry(LAST_TRACK+1);		/* find most entries */
    return 0;
  }
  return -1;
}  

void play_from_to_msf(struct cdrom_msf* msfp)
{
  uch play_command[] = {c_play, 
	   msfp->cdmsf_frame0, msfp->cdmsf_sec0, msfp->cdmsf_min0,
	   msfp->cdmsf_frame1, msfp->cdmsf_sec1, msfp->cdmsf_min1, 2, 2};
  int i;
  for (i=0; i<9; i++) type_0_command(play_command[i], 0);
  for (i=0; i<3; i++) 
    PLAY_TO.fsm[i] = play_command[i+4];
  PLAY_TO.track = 0;		/* say no track end */
  cd->dsb = wait_dsb();
}  

void play_from_to_track(int from, int to)
{
  uch play_command[8] = {c_play, };
  int i;

  if (from==0) {		/* continue paused play */
    for (i=0; i<3; i++) { 
      play_command[i+1] = cd->audio_status[i+2];
      play_command[i+4] = PLAY_TO.fsm[i];
    }
  } else {
    update_toc_entry(from); update_toc_entry(to+1);
    for (i=0; i<3; i++) {
      play_command[i+1] = cd->toc[from].fsm[i];
      PLAY_TO.fsm[i] = play_command[i+4] = cd->toc[to+1].fsm[i];
    }
    PLAY_TO.track = to; 
  }
  for (i=0; i<7; i++) type_0_command(play_command[i],0);
  for (i=0; i<2; i++) type_0_command(0x2, 0); /* volume */
  cd->dsb = wait_dsb();
}

int get_current_q(struct cdrom_subchnl * qp)
{
  int i;
  uch * q = cd->q;
  if (type_1_command(c_read_current_q, 10, q)) return 0;
/*  q[0] = bcdbin(q[0]); Don't think so! */
  for (i=2; i<6; i++) q[i]=bcdbin(q[i]); 
  qp->cdsc_adr = q[0] & 0xf; qp->cdsc_ctrl = q[0] >> 4;	/* from mcd.c */
  qp->cdsc_trk = q[1];  qp->cdsc_ind = q[2];
  if (qp->cdsc_format == CDROM_MSF) {
    qp->cdsc_reladdr.msf.minute = q[3];
    qp->cdsc_reladdr.msf.second = q[4];
    qp->cdsc_reladdr.msf.frame = q[5];
    qp->cdsc_absaddr.msf.minute = q[7];
    qp->cdsc_absaddr.msf.second = q[8];
    qp->cdsc_absaddr.msf.frame = q[9];
  } else {
    qp->cdsc_reladdr.lba = f_s_m2lba(q[5], q[4], q[3]);
    qp->cdsc_absaddr.lba = f_s_m2lba(q[9], q[8], q[7]);
  }
  get_drive_status();
  if (cd->dsb & dsb_play_in_progress) 
    qp->cdsc_audiostatus = CDROM_AUDIO_PLAY ;
  else if (PAUSED) 
    qp->cdsc_audiostatus = CDROM_AUDIO_PAUSED;
  else qp->cdsc_audiostatus = CDROM_AUDIO_NO_STATUS;
  return 0;
}

void invalidate_toc(void)
{
  memset(cd->toc, 0, sizeof(cd->toc));
  memset(cd->disc_status, 0, sizeof(cd->disc_status));
}

/* cdrom.c guarantees that cdte_format == CDROM_MSF */
void get_toc_entry(struct cdrom_tocentry * ep)
{
  uch track = normalize_track(ep->cdte_track);
  update_toc_entry(track);
  ep->cdte_addr.msf.frame = cd->toc[track].fsm[0];
  ep->cdte_addr.msf.second = cd->toc[track].fsm[1];
  ep->cdte_addr.msf.minute = cd->toc[track].fsm[2];
  ep->cdte_adr = cd->toc[track].q0 & 0xf; 
  ep->cdte_ctrl = cd->toc[track].q0 >> 4;
  ep->cdte_datamode=0;
}

/* Audio ioctl.  Ioctl commands connected to audio are in such an
 * idiosyncratic i/o format, that we leave these untouched. Return 0
 * upon success. Memory checking has been done by cdrom_ioctl(), the
 * calling function, as well as LBA/MSF sanitization.
*/
int cm206_audio_ioctl(struct cdrom_device_info * cdi, unsigned int cmd, 
		      void * arg)  
{
  switch (cmd) {
  case CDROMREADTOCHDR: 
    return read_toc_header((struct cdrom_tochdr *) arg);
  case CDROMREADTOCENTRY: 	
    get_toc_entry((struct cdrom_tocentry *) arg);
    return 0;
  case CDROMPLAYMSF: 
    play_from_to_msf((struct cdrom_msf *) arg);
    return 0;
  case CDROMPLAYTRKIND:		/* admittedly, not particularly beautiful */
    play_from_to_track(((struct cdrom_ti *)arg)->cdti_trk0, 
		       ((struct cdrom_ti *)arg)->cdti_trk1);
    return 0;
  case CDROMSTOP: 
    PAUSED=0;
    if (cd->dsb & dsb_play_in_progress) return type_0_command(c_stop, 1);
    else return 0;
  case CDROMPAUSE: 
    get_drive_status();
    if (cd->dsb & dsb_play_in_progress) {
      type_0_command(c_stop, 1);
      type_1_command(c_audio_status, 5, cd->audio_status);
      PAUSED=1;	/* say we're paused */
    }
    return 0;
  case CDROMRESUME:
    if (PAUSED) play_from_to_track(0,0);
    PAUSED=0;
    return 0;
  case CDROMSTART:
  case CDROMVOLCTRL:
    return 0;
  case CDROMSUBCHNL: 
    return get_current_q((struct cdrom_subchnl *)arg);
  default:
    return -EINVAL;
  }
}

/* Ioctl. These ioctls are specific to the cm206 driver. I have made
   some driver statistics accessible through ioctl calls.
 */

static int cm206_ioctl(struct cdrom_device_info * cdi, unsigned int cmd, 
		       unsigned long arg)
{
  switch (cmd) {
#ifdef STATISTICS
  case CM206CTL_GET_STAT:
    if (arg >= NR_STATS) return -EINVAL;
    else return cd->stats[arg];
  case CM206CTL_GET_LAST_STAT:
    if (arg >= NR_STATS) return -EINVAL;
    else return cd->last_stat[arg];
#endif    
  default:
    debug(("Unknown ioctl call 0x%x\n", cmd));
    return -EINVAL;
  }
}     

int cm206_media_changed(struct cdrom_device_info * cdi, int disc_nr) 
{
  if (cd != NULL) {
    int r;
    get_drive_status();		/* ensure cd->media_changed OK */
    r = cd->media_changed;
    cd->media_changed = 0;	/* clear bit */
    return r;
  }
  else return -EIO;
}

/* The new generic cdrom support. Routines should be concise, most of
   the logic should be in cdrom.c */

/* returns number of times device is in use */
int cm206_open_files(struct cdrom_device_info * cdi)	
{
  if (cd) return cd->openfiles;
  return -1;
}

/* controls tray movement */
int cm206_tray_move(struct cdrom_device_info * cdi, int position) 
{
  if (position) {		/* 1: eject */
    type_0_command(c_open_tray,1);
    invalidate_toc();
  } 
  else type_0_command(c_close_tray, 1);	/* 0: close */
  return 0;
}

/* gives current state of the drive */
int cm206_drive_status(struct cdrom_device_info * cdi, int slot_nr)
{
  get_drive_status();
  if (cd->dsb & dsb_tray_not_closed) return CDS_TRAY_OPEN;
  if (!(cd->dsb & dsb_disc_present)) return CDS_NO_DISC; 
  if (cd->dsb & dsb_drive_not_ready) return CDS_DRIVE_NOT_READY;
  return CDS_DISC_OK;
}
 
/* locks or unlocks door lock==1: lock; return 0 upon success */
int cm206_lock_door(struct cdrom_device_info * cdi, int lock)
{
  uch command = (lock) ? c_lock_tray : c_unlock_tray;
  type_0_command(command, 1);	/* wait and get dsb */
  /* the logic calculates the success, 0 means successful */
  return lock ^ ((cd->dsb & dsb_tray_locked) != 0);
}
  
/* Although a session start should be in LBA format, we return it in 
   MSF format because it is slightly easier, and the new generic ioctl
   will take care of the necessary conversion. */
int cm206_get_last_session(struct cdrom_device_info * cdi, 
			   struct cdrom_multisession * mssp) 
{
  if (!FIRST_TRACK) get_disc_status();
  if (mssp != NULL) {
    if (DISC_STATUS & cds_multi_session) { /* multi-session */
      mssp->addr.msf.frame = cd->disc_status[3];
      mssp->addr.msf.second = cd->disc_status[4];
      mssp->addr.msf.minute = cd->disc_status[5];
      mssp->addr_format = CDROM_MSF;
      mssp->xa_flag = 1;
    } else {
      mssp->xa_flag = 0;
    }
    return 1;
  }
  return 0;
}

int cm206_get_upc(struct cdrom_device_info * cdi, struct cdrom_mcn * mcn)
{
  uch upc[10];
  char * ret = mcn->medium_catalog_number;
  int i;
  
  if (type_1_command(c_read_upc, 10, upc)) return -EIO;
  for (i=0; i<13; i++) {
    int w=i/2+1, r=i%2;
    if (r) ret[i] = 0x30 | (upc[w] & 0x0f);
    else ret[i] = 0x30 | ((upc[w] >> 4) & 0x0f);
  }
  ret[13] = '\0';
  return 0;
} 

int cm206_reset(struct cdrom_device_info * cdi)
{
  stop_read();
  reset_cm260();
  outw(dc_normal | dc_break | READ_AHEAD, r_data_control);
  mdelay(1);			/* 750 musec minimum */
  outw(dc_normal | READ_AHEAD, r_data_control);
  cd->sector_last = -1;		/* flag no data buffered */
  cd->adapter_last = -1;    
  invalidate_toc();
  return 0;
}

int cm206_select_speed(struct cdrom_device_info * cdi, int speed)
{
  int r;
  switch (speed) {
  case 0: 
    r = type_0_command(c_auto_mode, 1);
    break;
  case 1:
    r = type_0_command(c_force_1x, 1);
    break;
  case 2:
    r = type_0_command(c_force_2x, 1);
    break;
  default:
    return -1;
  }
  if (r<0) return r;
  else return 1;
}

static struct cdrom_device_ops cm206_dops = {
  cm206_open,			/* open */
  cm206_release,		/* release */
  cm206_drive_status,		/* drive status */
  cm206_media_changed,		/* media changed */
  cm206_tray_move,		/* tray move */
  cm206_lock_door,		/* lock door */
  cm206_select_speed,		/* select speed */
  NULL,				/* select disc */
  cm206_get_last_session,	/* get last session */
  cm206_get_upc,		/* get universal product code */
  cm206_reset,			/* hard reset */
  cm206_audio_ioctl,		/* audio ioctl */
  cm206_ioctl,			/* device-specific ioctl */
  CDC_CLOSE_TRAY | CDC_OPEN_TRAY | CDC_LOCK | CDC_MULTI_SESSION |
    CDC_MEDIA_CHANGED | CDC_MCN | CDC_PLAY_AUDIO | CDC_SELECT_SPEED |
    CDC_IOCTLS | CDC_DRIVE_STATUS, 
    /* capability */
  1,				/* number of minor devices */
};


static struct cdrom_device_info cm206_info = {
  &cm206_dops,                  /* device operations */
  NULL,				/* link */
  NULL,				/* handle (not used by cm206) */
  0,				/* dev */
  0,				/* mask */
  2,				/* maximum speed */
  1,				/* number of discs */
  0,				/* options, not owned */
  0,				/* mc_flags, not owned */
  0,				/* use count, not owned */
  "cm206"			/* name of the device type */
};

/* This routine gets called during initialization if things go wrong,
 * can be used in cleanup_module as well. */
static void cleanup(int level)
{
  switch (level) {
  case 4: 
    if (unregister_cdrom(&cm206_info)) {
      printk("Can't unregister cdrom cm206\n");
      return;
    }
    if (unregister_blkdev(MAJOR_NR, "cm206")) {
      printk("Can't unregister major cm206\n");
      return;
    }
  case 3: 
    free_irq(cm206_irq, NULL);
  case 2: 
  case 1: 
    kfree(cd);
    release_region(cm206_base, 16);
  default:
  }
}

/* This function probes for the adapter card. It returns the base
   address if it has found the adapter card. One can specify a base 
   port to probe specifically, or 0 which means span all possible
   bases. 

   Linus says it is too dangerous to use writes for probing, so we
   stick with pure reads for a while. Hope that 8 possible ranges,
   check_region, 15 bits of one port and 6 of another make things
   likely enough to accept the region on the first hit...
 */
int __init probe_base_port(int base)
{
  int b=0x300, e=0x370;		/* this is the range of start addresses */
  volatile int fool, i;

  if (base) b=e=base;
  for (base=b; base<=e; base += 0x10) {
    if (check_region(base, 0x10)) continue;
    for (i=0; i<3; i++) 
      fool = inw(base+2); /* empty possibly uart_receive_buffer */
    if((inw(base+6) & 0xffef) != 0x0001 || /* line_status */
       (inw(base) & 0xad00) != 0) /* data status */
      continue;
    return(base);
  }
  return 0;
}

#if !defined(MODULE) || defined(AUTO_PROBE_MODULE)
/* Probe for irq# nr. If nr==0, probe for all possible irq's. */
int __init probe_irq(int nr){
  int irqs, irq;
  outw(dc_normal | READ_AHEAD, r_data_control);	/* disable irq-generation */
  sti(); 
  irqs = probe_irq_on();
  reset_cm260();		/* causes interrupt */
  udelay(100);			/* wait for it */
  irq = probe_irq_off(irqs);
  outw(dc_normal | READ_AHEAD, r_data_control);	/* services interrupt */
  if (nr && irq!=nr && irq>0) return 0;	/* wrong interrupt happened */
  else return irq;
}
#endif

int __init cm206_init(void)
{
  uch e=0;
  long int size=sizeof(struct cm206_struct);

  printk(KERN_INFO "cm206 cdrom driver " REVISION);
  cm206_base = probe_base_port(auto_probe ? 0 : cm206_base);
  if (!cm206_base) {
    printk(" can't find adapter!\n");
    return -EIO;
  }
  printk(" adapter at 0x%x", cm206_base);
  request_region(cm206_base, 16, "cm206");
  cd = (struct cm206_struct *) kmalloc(size, GFP_KERNEL);
  if (!cd) return -EIO;
  /* Now we have found the adaptor card, try to reset it. As we have
   * found out earlier, this process generates an interrupt as well,
   * so we might just exploit that fact for irq probing! */
#if !defined(MODULE) || defined(AUTO_PROBE_MODULE)
  cm206_irq = probe_irq(auto_probe ? 0 : cm206_irq);	
  if (cm206_irq<=0) {
    printk("can't find IRQ!\n");
    cleanup(1);
    return -EIO;
  }
  else printk(" IRQ %d found\n", cm206_irq);
#else
  cli();
  reset_cm260();
  /* Now, the problem here is that reset_cm260 can generate an
     interrupt. It seems that this can cause a kernel oops some time
     later. So we wait a while and `service' this interrupt. */
  mdelay(1);
  outw(dc_normal | READ_AHEAD, r_data_control);
  sti();
  printk(" using IRQ %d\n", cm206_irq);
#endif
  if (send_receive_polled(c_drive_configuration) != c_drive_configuration) 
    {
      printk(KERN_INFO " drive not there\n");
      cleanup(1);
      return -EIO;
    }
  e = send_receive_polled(c_gimme);
  printk(KERN_INFO "Firmware revision %d", e & dcf_revision_code);
  if (e & dcf_transfer_rate) printk(" double");
  else printk(" single");
  printk(" speed drive");
  if (e & dcf_motorized_tray) printk(", motorized tray");
  if (request_irq(cm206_irq, cm206_interrupt, 0, "cm206", NULL)) {
    printk("\nUnable to reserve IRQ---aborted\n");
    cleanup(2);
    return -EIO;
  }
  printk(".\n");
  if (register_blkdev(MAJOR_NR, "cm206", &cdrom_fops) != 0) {
    printk(KERN_INFO "Cannot register for major %d!\n", MAJOR_NR);
    cleanup(3);
    return -EIO;
  }
  cm206_info.dev = MKDEV(MAJOR_NR,0);
  if (register_cdrom(&cm206_info) != 0) {
    printk(KERN_INFO "Cannot register for cdrom %d!\n", MAJOR_NR);
    cleanup(3);
    return -EIO;
  }    
  blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
  blksize_size[MAJOR_NR] = cm206_blocksizes;
  read_ahead[MAJOR_NR] = 16;	/* reads ahead what? */
  init_bh(CM206_BH, cm206_bh);

  memset(cd, 0, sizeof(*cd));	/* give'm some reasonable value */
  cd->sector_last = -1;		/* flag no data buffered */
  cd->adapter_last = -1;
  cd->timer.function = cm206_timeout;
  cd->max_sectors = (inw(r_data_status) & ds_ram_size) ? 24 : 97;
  printk(KERN_INFO "%d kB adapter memory available, "  
	 " %ld bytes kernel memory used.\n", cd->max_sectors*2, size);
  return 0;
}

#ifdef MODULE

static int cm206[2] = {0,0};	/* for compatible `insmod' parameter passing */

void __init parse_options(void)
{
  int i;
  for (i=0; i<2; i++) {
    if (0x300 <= cm206[i] && i<= 0x370 && cm206[i] % 0x10 == 0) {
      cm206_base = cm206[i];
      auto_probe=0;
    }
    else if (3 <= cm206[i] && cm206[i] <= 15) {
      cm206_irq = cm206[i];
      auto_probe=0;
    }
  }
}

int init_module(void)
{
	parse_options();
#if !defined(AUTO_PROBE_MODULE)
	auto_probe=0;
#endif
	return cm206_init();
}

void cleanup_module(void)
{
  cleanup(4);
  printk(KERN_INFO "cm206 removed\n");
}
      
#else /* !MODULE */

/* This setup function accepts either `auto' or numbers in the range
 * 3--11 (for irq) or 0x300--0x370 (for base port) or both. */
void __init cm206_setup(char *s, int *p)
{
  int i;
  if (!strcmp(s, "auto")) auto_probe=1;
  for(i=1; i<=p[0]; i++) {
    if (0x300 <= p[i] && i<= 0x370 && p[i] % 0x10 == 0) {
      cm206_base = p[i];
      auto_probe = 0;
    }
    else if (3 <= p[i] && p[i] <= 15) {
      cm206_irq = p[i];
      auto_probe = 0;
    }
  }
}
#endif /* MODULE */
/*
 * Local variables:
 * compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/include -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -D__SMP__ -pipe -fno-strength-reduce -m486 -DCPU=486 -D__SMP__ -DMODULE -DMODVERSIONS -include /usr/src/linux/include/linux/modversions.h  -c -o cm206.o cm206.c"
 * End:
 */
