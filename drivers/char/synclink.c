/*
 * linux/drivers/char/synclink.c
 *
 * ==FILEDATE 19990901==
 *
 * Device driver for Microgate SyncLink ISA and PCI
 * high speed multiprotocol serial adapters.
 *
 * written by Paul Fulghum for Microgate Corporation
 * paulkf@microgate.com
 *
 * Microgate and SyncLink are trademarks of Microgate Corporation
 *
 * Derived from serial.c written by Theodore Ts'o and Linus Torvalds
 *
 * Original release 01/11/99
 *
 * This code is released under the GNU General Public License (GPL)
 *
 * This driver is primarily intended for use in synchronous
 * HDLC mode. Asynchronous mode is also provided.
 *
 * When operating in synchronous mode, each call to mgsl_write()
 * contains exactly one complete HDLC frame. Calling mgsl_put_char
 * will start assembling an HDLC frame that will not be sent until
 * mgsl_flush_chars or mgsl_write is called.
 * 
 * Synchronous receive data is reported as complete frames. To accomplish
 * this, the TTY flip buffer is bypassed (too small to hold largest
 * frame and may fragment frames) and the line discipline
 * receive entry point is called directly.
 *
 * This driver has been tested with a slightly modified ppp.c driver
 * for synchronous PPP.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define VERSION(ver,rel,seq) (((ver)<<16) | ((rel)<<8) | (seq))
#define BREAKPOINT() asm("   int $3");

#define MAX_ISA_DEVICES 10
#define MAX_PCI_DEVICES 10
#define MAX_TOTAL_DEVICES 20

#include <linux/config.h>	
#include <linux/module.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/malloc.h>

#if LINUX_VERSION_CODE >= VERSION(2,1,0) 
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <asm/serial.h>
#else
#include <linux/bios32.h>
#endif

#include <linux/delay.h>
#include <linux/ioctl.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <asm/bitops.h>
#include <asm/types.h>
#include <linux/termios.h>
#include <linux/tqueue.h>

#if LINUX_VERSION_CODE < VERSION(2,3,0) 
typedef struct wait_queue *wait_queue_head_t;
#define DECLARE_WAITQUEUE(name,task) struct wait_queue (name) = {(task),NULL}
#define init_waitqueue_head(head) *(head) = NULL
#define DECLARE_MUTEX(name) struct semaphore (name) = MUTEX
#define set_current_state(a) current->state = (a)
#endif

#if LINUX_VERSION_CODE >= VERSION(2,1,4)
#include <asm/segment.h>
#define GET_USER(error,value,addr) error = get_user(value,addr)
#define COPY_FROM_USER(error,dest,src,size) error = copy_from_user(dest,src,size) ? -EFAULT : 0
#define PUT_USER(error,value,addr) error = put_user(value,addr)
#define COPY_TO_USER(error,dest,src,size) error = copy_to_user(dest,src,size) ? -EFAULT : 0

#if LINUX_VERSION_CODE >= VERSION(2,1,5)
#include <asm/uaccess.h>
#endif

#else  /* 2.0.x and 2.1.x before 2.1.4 */

#define GET_USER(error,value,addr)					  \
do {									  \
	error = verify_area (VERIFY_READ, (void *) addr, sizeof (value)); \
	if (error == 0)							  \
		value = get_user(addr);					  \
} while (0)

#define COPY_FROM_USER(error,dest,src,size)				  \
do {									  \
	error = verify_area (VERIFY_READ, (void *) src, size);		  \
	if (error == 0)							  \
		memcpy_fromfs (dest, src, size);			  \
} while (0)

#define PUT_USER(error,value,addr)					   \
do {									   \
	error = verify_area (VERIFY_WRITE, (void *) addr, sizeof (value)); \
	if (error == 0)							   \
		put_user (value, addr);					   \
} while (0)

#define COPY_TO_USER(error,dest,src,size)				  \
do {									  \
	error = verify_area (VERIFY_WRITE, (void *) dest, size);		  \
	if (error == 0)							  \
		memcpy_tofs (dest, src, size);				  \
} while (0)

#endif

#if LINUX_VERSION_CODE < VERSION(2,1,0)
/*
 * This is used to figure out the divisor speeds and the timeouts
 */
static int baud_table[] = {
	0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800,
	9600, 19200, 38400, 57600, 115200, 230400, 460800, 0 };

#define __init
#define ioremap(a,b) vremap((a),(b))
#define iounmap(a) vfree((a))
#define SERIAL_TYPE_NORMAL	1
#define SERIAL_TYPE_CALLOUT	2
typedef int spinlock_t;
#define spin_lock_irqsave(a,b) {save_flags((b));cli();}
#define spin_unlock_irqrestore(a,b) {restore_flags((b));}
#define spin_lock(a)
#define spin_unlock(a)
#define schedule_timeout(a){current->timeout = jiffies + (a); schedule();}
#define signal_pending(a) ((a)->signal & ~(a)->blocked)
#endif



#include "linux/synclink.h"

#define RCLRVALUE 0xffff

MGSL_PARAMS default_params = {
	MGSL_MODE_HDLC,			/* unsigned long mode */
	0,				/* unsigned char loopback; */
	HDLC_FLAG_UNDERRUN_ABORT15,	/* unsigned short flags; */
	HDLC_ENCODING_NRZI_SPACE,	/* unsigned char encoding; */
	0,				/* unsigned long clock_speed; */
	0xff,				/* unsigned char addr_filter; */
	HDLC_CRC_16_CCITT,		/* unsigned short crc_type; */
	HDLC_PREAMBLE_LENGTH_8BITS,	/* unsigned char preamble_length; */
	HDLC_PREAMBLE_PATTERN_NONE,	/* unsigned char preamble; */
	9600,				/* unsigned long data_rate; */
	8,				/* unsigned char data_bits; */
	1,				/* unsigned char stop_bits; */
	ASYNC_PARITY_NONE		/* unsigned char parity; */
};

#define SHARED_MEM_ADDRESS_SIZE 0x40000
#define BUFFERLISTSIZE (PAGE_SIZE)
#define DMABUFFERSIZE (PAGE_SIZE)
#define MAXRXFRAMES 7

typedef struct _DMABUFFERENTRY
{
	u32 phys_addr;	/* 32-bit flat physical address of data buffer */
	u16 count;	/* buffer size/data count */
	u16 status;	/* Control/status field */
	u16 rcc;	/* character count field */
	u16 reserved;	/* padding required by 16C32 */
	u32 link;	/* 32-bit flat link to next buffer entry */
	char *virt_addr;	/* virtual address of data buffer */
	u32 phys_entry;	/* physical address of this buffer entry */
} DMABUFFERENTRY, *DMAPBUFFERENTRY;

/* The queue of BH actions to be performed */

#define BH_TYPE_RECEIVE_DATA	1
#define BH_TYPE_RECEIVE_STATUS	2
#define BH_TYPE_RECEIVE_DMA	3
#define BH_TYPE_TRANSMIT_DATA	4
#define BH_TYPE_TRANSMIT_STATUS	5
#define BH_TYPE_STATUS		6

typedef struct _BH_EVENT {
	unsigned char type;  /* Set by interrupt routines to reqst */
	u16 status;
	struct _BH_EVENT *link;
	
} BH_EVENT, *BH_QUEUE;     /* Queue of BH actions to be done.  */

#define MAX_BH_QUEUE_ENTRIES 200
#define IO_PIN_SHUTDOWN_LIMIT (MAX_BH_QUEUE_ENTRIES/4)

#define RELEVANT_IFLAG(iflag) (iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

struct	_input_signal_events {
	int	ri_up;	
	int	ri_down;
	int	dsr_up;
	int	dsr_down;
	int	dcd_up;
	int	dcd_down;
	int	cts_up;
	int	cts_down;
};

/*
 * Device instance data structure
 */
 
struct mgsl_struct {
	int			magic;
	int			flags;
	int			count;		/* count of opens */
	int			line;
	unsigned short		close_delay;
	unsigned short		closing_wait;	/* time to wait before closing */
	
	struct mgsl_icount	icount;
	
	struct termios		normal_termios;
	struct termios		callout_termios;
	
	struct tty_struct 	*tty;
	int			timeout;
	int			x_char;		/* xon/xoff character */
	int			blocked_open;	/* # of blocked opens */
	long			session;	/* Session of opening process */
	long			pgrp;		/* pgrp of opening process */
	u16			read_status_mask;
	u16			ignore_status_mask;	
	unsigned char 		*xmit_buf;
	int			xmit_head;
	int			xmit_tail;
	int			xmit_cnt;
	
	wait_queue_head_t	open_wait;
	wait_queue_head_t	close_wait;
	
	wait_queue_head_t	status_event_wait_q;
	wait_queue_head_t	event_wait_q;
	struct timer_list	tx_timer;	/* HDLC transmit timeout timer */
	struct mgsl_struct	*next_device;	/* device list link */
	
	spinlock_t irq_spinlock;		/* spinlock for synchronizing with ISR */
	struct tq_struct task;		/* task structure for scheduling bh */

	u32 EventMask;			/* event trigger mask */
	u32 RecordedEvents;		/* pending events */

	u32 max_frame_size;		/* as set by device config */

	BH_EVENT bh_queue[MAX_BH_QUEUE_ENTRIES];		/* Pointer to alloc'ed block */
	BH_QUEUE bh_queue_head;	/* Queue of BH actions */
	BH_QUEUE bh_queue_tail;	/* Tail of above for perf. */
	BH_QUEUE free_bh_queue_head;	/* Queue of Free BH */
	BH_QUEUE free_bh_queue_tail;	/* Tail of above for perf. */
	BH_QUEUE bh_action;	/* Action for BH */
	int bh_running;		/* Protection from multiple */
	int isr_overflow;
	int bh_requested;
	
	int dcd_chkcount;		/* check counts to prevent */
	int cts_chkcount;		/* too many IRQs if a signal */
	int dsr_chkcount;		/* is floating */
	int ri_chkcount;

	char *buffer_list;		/* virtual address of Rx & Tx buffer lists */
	unsigned long buffer_list_phys;

	unsigned int rx_buffer_count;	/* count of total allocated Rx buffers */
	DMABUFFERENTRY *rx_buffer_list;	/* list of receive buffer entries */
	unsigned int current_rx_buffer;

	unsigned int tx_buffer_count;	/* count of total allocated Tx buffers */
	DMABUFFERENTRY *tx_buffer_list;	/* list of transmit buffer entries */
	
	unsigned char *intermediate_rxbuffer;

	int rx_enabled;
	int rx_overflow;

	int tx_enabled;
	int tx_active;
	u32 idle_mode;

	u16 cmr_value;
	u16 tcsr_value;

	char device_name[25];		/* device instance name */

	unsigned int bus_type;	/* expansion bus type (ISA,EISA,PCI) */
	unsigned char bus;		/* expansion bus number (zero based) */
	unsigned char function;		/* PCI device number */

	unsigned int io_base;		/* base I/O address of adapter */
	unsigned int io_addr_size;	/* size of the I/O address range */
	int io_addr_requested;		/* nonzero if I/O address requested */
	
	unsigned int irq_level;		/* interrupt level */
	unsigned long irq_flags;
	int irq_requested;		/* nonzero if IRQ requested */
	
	unsigned int dma_level;		/* DMA channel */
	int dma_requested;		/* nonzero if dma channel requested */

	u16 mbre_bit;
	u16 loopback_bits;
	u16 usc_idle_mode;

	MGSL_PARAMS params;		/* communications parameters */

	unsigned char serial_signals;	/* current serial signal states */

	int irq_occurred;		/* for diagnostics use */
	unsigned int init_error;	/* Initialization startup error 		(DIAGS)	*/
	int	fDiagnosticsmode;	/* Driver in Diagnostic mode?			(DIAGS)	*/

	u32 last_mem_alloc;
	unsigned char* memory_base;	/* shared memory address (PCI only) */
	u32 phys_memory_base;

	unsigned char* lcr_base;	/* local config registers (PCI only) */
	u32 phys_lcr_base;
	u32 lcr_offset;

	u32 misc_ctrl_value;
	char flag_buf[MAX_ASYNC_BUFFER_SIZE];
	char char_buf[MAX_ASYNC_BUFFER_SIZE];	
	BOOLEAN drop_rts_on_tx_done;

	BOOLEAN loopmode_insert_requested;
	BOOLEAN	loopmode_send_done_requested;
	
	struct	_input_signal_events	input_signal_events;
};

#define MGSL_MAGIC 0x5401

/*
 * The size of the serial xmit buffer is 1 page, or 4096 bytes
 */
#define SERIAL_XMIT_SIZE 4096


/*
 * These macros define the offsets used in calculating the
 * I/O address of the specified USC registers.
 */


#define DCPIN 2		/* Bit 1 of I/O address */
#define SDPIN 4		/* Bit 2 of I/O address */

#define DCAR 0		/* DMA command/address register */
#define CCAR SDPIN		/* channel command/address register */
#define DATAREG DCPIN + SDPIN	/* serial data register */
#define MSBONLY 0x41
#define LSBONLY 0x40

/*
 * These macros define the register address (ordinal number)
 * used for writing address/value pairs to the USC.
 */

#define CMR	0x02	/* Channel mode Register */
#define CCSR	0x04	/* Channel Command/status Register */
#define CCR	0x06	/* Channel Control Register */
#define PSR	0x08	/* Port status Register */
#define PCR	0x0a	/* Port Control Register */
#define TMDR	0x0c	/* Test mode Data Register */
#define TMCR	0x0e	/* Test mode Control Register */
#define CMCR	0x10	/* Clock mode Control Register */
#define HCR	0x12	/* Hardware Configuration Register */
#define IVR	0x14	/* Interrupt Vector Register */
#define IOCR	0x16	/* Input/Output Control Register */
#define ICR	0x18	/* Interrupt Control Register */
#define DCCR	0x1a	/* Daisy Chain Control Register */
#define MISR	0x1c	/* Misc Interrupt status Register */
#define SICR	0x1e	/* status Interrupt Control Register */
#define RDR	0x20	/* Receive Data Register */
#define RMR	0x22	/* Receive mode Register */
#define RCSR	0x24	/* Receive Command/status Register */
#define RICR	0x26	/* Receive Interrupt Control Register */
#define RSR	0x28	/* Receive Sync Register */
#define RCLR	0x2a	/* Receive count Limit Register */
#define RCCR	0x2c	/* Receive Character count Register */
#define TC0R	0x2e	/* Time Constant 0 Register */
#define TDR	0x30	/* Transmit Data Register */
#define TMR	0x32	/* Transmit mode Register */
#define TCSR	0x34	/* Transmit Command/status Register */
#define TICR	0x36	/* Transmit Interrupt Control Register */
#define TSR	0x38	/* Transmit Sync Register */
#define TCLR	0x3a	/* Transmit count Limit Register */
#define TCCR	0x3c	/* Transmit Character count Register */
#define TC1R	0x3e	/* Time Constant 1 Register */


/*
 * MACRO DEFINITIONS FOR DMA REGISTERS
 */

#define DCR	0x06	/* DMA Control Register (shared) */
#define DACR	0x08	/* DMA Array count Register (shared) */
#define BDCR	0x12	/* Burst/Dwell Control Register (shared) */
#define DIVR	0x14	/* DMA Interrupt Vector Register (shared) */	
#define DICR	0x18	/* DMA Interrupt Control Register (shared) */
#define CDIR	0x1a	/* Clear DMA Interrupt Register (shared) */
#define SDIR	0x1c	/* Set DMA Interrupt Register (shared) */

#define TDMR	0x02	/* Transmit DMA mode Register */
#define TDIAR	0x1e	/* Transmit DMA Interrupt Arm Register */
#define TBCR	0x2a	/* Transmit Byte count Register */
#define TARL	0x2c	/* Transmit Address Register (low) */
#define TARU	0x2e	/* Transmit Address Register (high) */
#define NTBCR	0x3a	/* Next Transmit Byte count Register */
#define NTARL	0x3c	/* Next Transmit Address Register (low) */
#define NTARU	0x3e	/* Next Transmit Address Register (high) */

#define RDMR	0x82	/* Receive DMA mode Register (non-shared) */
#define RDIAR	0x9e	/* Receive DMA Interrupt Arm Register */
#define RBCR	0xaa	/* Receive Byte count Register */
#define RARL	0xac	/* Receive Address Register (low) */
#define RARU	0xae	/* Receive Address Register (high) */
#define NRBCR	0xba	/* Next Receive Byte count Register */
#define NRARL	0xbc	/* Next Receive Address Register (low) */
#define NRARU	0xbe	/* Next Receive Address Register (high) */


/*
 * MACRO DEFINITIONS FOR MODEM STATUS BITS
 */

#define MODEMSTATUS_DTR 0x80
#define MODEMSTATUS_DSR 0x40
#define MODEMSTATUS_RTS 0x20
#define MODEMSTATUS_CTS 0x10
#define MODEMSTATUS_RI  0x04
#define MODEMSTATUS_DCD 0x01


/*
 * Channel Command/Address Register (CCAR) Command Codes
 */

#define RTCmd_Null			0x0000
#define RTCmd_ResetHighestIus		0x1000
#define RTCmd_TriggerChannelLoadDma	0x2000
#define RTCmd_TriggerRxDma		0x2800
#define RTCmd_TriggerTxDma		0x3000
#define RTCmd_TriggerRxAndTxDma		0x3800
#define RTCmd_PurgeRxFifo		0x4800
#define RTCmd_PurgeTxFifo		0x5000
#define RTCmd_PurgeRxAndTxFifo		0x5800
#define RTCmd_LoadRcc			0x6800
#define RTCmd_LoadTcc			0x7000
#define RTCmd_LoadRccAndTcc		0x7800
#define RTCmd_LoadTC0			0x8800
#define RTCmd_LoadTC1			0x9000
#define RTCmd_LoadTC0AndTC1		0x9800
#define RTCmd_SerialDataLSBFirst	0xa000
#define RTCmd_SerialDataMSBFirst	0xa800
#define RTCmd_SelectBigEndian		0xb000
#define RTCmd_SelectLittleEndian	0xb800


/*
 * DMA Command/Address Register (DCAR) Command Codes
 */

#define DmaCmd_Null			0x0000
#define DmaCmd_ResetTxChannel		0x1000
#define DmaCmd_ResetRxChannel		0x1200
#define DmaCmd_StartTxChannel		0x2000
#define DmaCmd_StartRxChannel		0x2200
#define DmaCmd_ContinueTxChannel	0x3000
#define DmaCmd_ContinueRxChannel	0x3200
#define DmaCmd_PauseTxChannel		0x4000
#define DmaCmd_PauseRxChannel		0x4200
#define DmaCmd_AbortTxChannel		0x5000
#define DmaCmd_AbortRxChannel		0x5200
#define DmaCmd_InitTxChannel		0x7000
#define DmaCmd_InitRxChannel		0x7200
#define DmaCmd_ResetHighestDmaIus	0x8000
#define DmaCmd_ResetAllChannels		0x9000
#define DmaCmd_StartAllChannels		0xa000
#define DmaCmd_ContinueAllChannels	0xb000
#define DmaCmd_PauseAllChannels		0xc000
#define DmaCmd_AbortAllChannels		0xd000
#define DmaCmd_InitAllChannels		0xf000

#define TCmd_Null			0x0000
#define TCmd_ClearTxCRC			0x2000
#define TCmd_SelectTicrTtsaData		0x4000
#define TCmd_SelectTicrTxFifostatus	0x5000
#define TCmd_SelectTicrIntLevel		0x6000
#define TCmd_SelectTicrdma_level		0x7000
#define TCmd_SendFrame			0x8000
#define TCmd_SendAbort			0x9000
#define TCmd_EnableDleInsertion		0xc000
#define TCmd_DisableDleInsertion	0xd000
#define TCmd_ClearEofEom		0xe000
#define TCmd_SetEofEom			0xf000

#define RCmd_Null			0x0000
#define RCmd_ClearRxCRC			0x2000
#define RCmd_EnterHuntmode		0x3000
#define RCmd_SelectRicrRtsaData		0x4000
#define RCmd_SelectRicrRxFifostatus	0x5000
#define RCmd_SelectRicrIntLevel		0x6000
#define RCmd_SelectRicrdma_level		0x7000

/*
 * Bits for enabling and disabling IRQs in Interrupt Control Register (ICR)
 */
 
#define RECEIVE_STATUS		BIT5
#define RECEIVE_DATA		BIT4
#define TRANSMIT_STATUS		BIT3
#define TRANSMIT_DATA		BIT2
#define IO_PIN			BIT1
#define MISC			BIT0


/*
 * Receive status Bits in Receive Command/status Register RCSR
 */

#define RXSTATUS_SHORT_FRAME		BIT8
#define RXSTATUS_CODE_VIOLATION		BIT8
#define RXSTATUS_EXITED_HUNT		BIT7
#define RXSTATUS_IDLE_RECEIVED		BIT6
#define RXSTATUS_BREAK_RECEIVED		BIT5
#define RXSTATUS_ABORT_RECEIVED		BIT5
#define RXSTATUS_RXBOUND		BIT4
#define RXSTATUS_CRC_ERROR		BIT3
#define RXSTATUS_FRAMING_ERROR		BIT3
#define RXSTATUS_ABORT			BIT2
#define RXSTATUS_PARITY_ERROR		BIT2
#define RXSTATUS_OVERRUN		BIT1
#define RXSTATUS_DATA_AVAILABLE		BIT0
#define RXSTATUS_ALL			0x01f6
#define usc_UnlatchRxstatusBits(a,b) usc_OutReg( (a), RCSR, (u16)((b) & RXSTATUS_ALL) )

/*
 * Values for setting transmit idle mode in 
 * Transmit Control/status Register (TCSR)
 */
#define IDLEMODE_FLAGS			0x0000
#define IDLEMODE_ALT_ONE_ZERO		0x0100
#define IDLEMODE_ZERO			0x0200
#define IDLEMODE_ONE			0x0300
#define IDLEMODE_ALT_MARK_SPACE		0x0500
#define IDLEMODE_SPACE			0x0600
#define IDLEMODE_MARK			0x0700
#define IDLEMODE_MASK			0x0700

/*
 * IUSC revision identifiers
 */
#define	IUSC_SL1660			0x4d44
#define IUSC_PRE_SL1660			0x4553

/*
 * Transmit status Bits in Transmit Command/status Register (TCSR)
 */

#define TCSR_PRESERVE			0x0F00

#define TCSR_UNDERWAIT			BIT11
#define TXSTATUS_PREAMBLE_SENT		BIT7
#define TXSTATUS_IDLE_SENT		BIT6
#define TXSTATUS_ABORT_SENT		BIT5
#define TXSTATUS_EOF_SENT		BIT4
#define TXSTATUS_EOM_SENT		BIT4
#define TXSTATUS_CRC_SENT		BIT3
#define TXSTATUS_ALL_SENT		BIT2
#define TXSTATUS_UNDERRUN		BIT1
#define TXSTATUS_FIFO_EMPTY		BIT0
#define TXSTATUS_ALL			0x00fa
#define usc_UnlatchTxstatusBits(a,b) usc_OutReg( (a), TCSR, (u16)((a)->tcsr_value + ((b) & 0x00FF)) )
				

#define MISCSTATUS_RXC_LATCHED		BIT15
#define MISCSTATUS_RXC			BIT14
#define MISCSTATUS_TXC_LATCHED		BIT13
#define MISCSTATUS_TXC			BIT12
#define MISCSTATUS_RI_LATCHED		BIT11
#define MISCSTATUS_RI			BIT10
#define MISCSTATUS_DSR_LATCHED		BIT9
#define MISCSTATUS_DSR			BIT8
#define MISCSTATUS_DCD_LATCHED		BIT7
#define MISCSTATUS_DCD			BIT6
#define MISCSTATUS_CTS_LATCHED		BIT5
#define MISCSTATUS_CTS			BIT4
#define MISCSTATUS_RCC_UNDERRUN		BIT3
#define MISCSTATUS_DPLL_NO_SYNC		BIT2
#define MISCSTATUS_BRG1_ZERO		BIT1
#define MISCSTATUS_BRG0_ZERO		BIT0

#define usc_UnlatchIostatusBits(a,b) usc_OutReg((a),MISR,(u16)((b) & 0xaaa0))
#define usc_UnlatchMiscstatusBits(a,b) usc_OutReg((a),MISR,(u16)((b) & 0x000f))

#define SICR_RXC_ACTIVE			BIT15
#define SICR_RXC_INACTIVE		BIT14
#define SICR_RXC			(BIT15+BIT14)
#define SICR_TXC_ACTIVE			BIT13
#define SICR_TXC_INACTIVE		BIT12
#define SICR_TXC			(BIT13+BIT12)
#define SICR_RI_ACTIVE			BIT11
#define SICR_RI_INACTIVE		BIT10
#define SICR_RI				(BIT11+BIT10)
#define SICR_DSR_ACTIVE			BIT9
#define SICR_DSR_INACTIVE		BIT8
#define SICR_DSR			(BIT9+BIT8)
#define SICR_DCD_ACTIVE			BIT7
#define SICR_DCD_INACTIVE		BIT6
#define SICR_DCD			(BIT7+BIT6)
#define SICR_CTS_ACTIVE			BIT5
#define SICR_CTS_INACTIVE		BIT4
#define SICR_CTS			(BIT5+BIT4)
#define SICR_RCC_UNDERFLOW		BIT3
#define SICR_DPLL_NO_SYNC		BIT2
#define SICR_BRG1_ZERO			BIT1
#define SICR_BRG0_ZERO			BIT0

void usc_DisableMasterIrqBit( struct mgsl_struct *info );
void usc_EnableMasterIrqBit( struct mgsl_struct *info );
void usc_EnableInterrupts( struct mgsl_struct *info, u16 IrqMask );
void usc_DisableInterrupts( struct mgsl_struct *info, u16 IrqMask );
void usc_ClearIrqPendingBits( struct mgsl_struct *info, u16 IrqMask );

#define usc_EnableInterrupts( a, b ) \
	usc_OutReg( (a), ICR, (u16)((usc_InReg((a),ICR) & 0xff00) + 0xc0 + (b)) )

#define usc_DisableInterrupts( a, b ) \
	usc_OutReg( (a), ICR, (u16)((usc_InReg((a),ICR) & 0xff00) + 0x80 + (b)) )

#define usc_EnableMasterIrqBit(a) \
	usc_OutReg( (a), ICR, (u16)((usc_InReg((a),ICR) & 0x0f00) + 0xb000) )

#define usc_DisableMasterIrqBit(a) \
	usc_OutReg( (a), ICR, (u16)(usc_InReg((a),ICR) & 0x7f00) )

#define usc_ClearIrqPendingBits( a, b ) usc_OutReg( (a), DCCR, 0x40 + (b) )

/*
 * Transmit status Bits in Transmit Control status Register (TCSR)
 * and Transmit Interrupt Control Register (TICR) (except BIT2, BIT0)
 */

#define TXSTATUS_PREAMBLE_SENT	BIT7
#define TXSTATUS_IDLE_SENT	BIT6
#define TXSTATUS_ABORT_SENT	BIT5
#define TXSTATUS_EOF		BIT4
#define TXSTATUS_CRC_SENT	BIT3
#define TXSTATUS_ALL_SENT	BIT2
#define TXSTATUS_UNDERRUN	BIT1
#define TXSTATUS_FIFO_EMPTY	BIT0

#define DICR_MASTER		BIT15
#define DICR_TRANSMIT		BIT0
#define DICR_RECEIVE		BIT1

#define usc_EnableDmaInterrupts(a,b) \
	usc_OutDmaReg( (a), DICR, (u16)(usc_InDmaReg((a),DICR) | (b)) )

#define usc_DisableDmaInterrupts(a,b) \
	usc_OutDmaReg( (a), DICR, (u16)(usc_InDmaReg((a),DICR) & ~(b)) )

#define usc_EnableStatusIrqs(a,b) \
	usc_OutReg( (a), SICR, (u16)(usc_InReg((a),SICR) | (b)) )

#define usc_DisablestatusIrqs(a,b) \
	usc_OutReg( (a), SICR, (u16)(usc_InReg((a),SICR) & ~(b)) )

/* Transmit status Bits in Transmit Control status Register (TCSR) */
/* and Transmit Interrupt Control Register (TICR) (except BIT2, BIT0) */


#define DISABLE_UNCONDITIONAL    0
#define DISABLE_END_OF_FRAME     1
#define ENABLE_UNCONDITIONAL     2
#define ENABLE_AUTO_CTS          3
#define ENABLE_AUTO_DCD          3
#define usc_EnableTransmitter(a,b) \
	usc_OutReg( (a), TMR, (u16)((usc_InReg((a),TMR) & 0xfffc) | (b)) )
#define usc_EnableReceiver(a,b) \
	usc_OutReg( (a), RMR, (u16)((usc_InReg((a),RMR) & 0xfffc) | (b)) )

u16  usc_InDmaReg( struct mgsl_struct *info, u16 Port );
void usc_OutDmaReg( struct mgsl_struct *info, u16 Port, u16 Value );
void usc_DmaCmd( struct mgsl_struct *info, u16 Cmd );

u16  usc_InReg( struct mgsl_struct *info, u16 Port );
void usc_OutReg( struct mgsl_struct *info, u16 Port, u16 Value );
void usc_RTCmd( struct mgsl_struct *info, u16 Cmd );
void usc_RCmd( struct mgsl_struct *info, u16 Cmd );
void usc_TCmd( struct mgsl_struct *info, u16 Cmd );

#define usc_TCmd(a,b) usc_OutReg((a), TCSR, (u16)((a)->tcsr_value + (b)))
#define usc_RCmd(a,b) usc_OutReg((a), RCSR, (b))

void usc_process_rxoverrun_sync( struct mgsl_struct *info );
void usc_start_receiver( struct mgsl_struct *info );
void usc_stop_receiver( struct mgsl_struct *info );

void usc_start_transmitter( struct mgsl_struct *info );
void usc_stop_transmitter( struct mgsl_struct *info );
void usc_set_txidle( struct mgsl_struct *info );
void usc_load_txfifo( struct mgsl_struct *info );

void usc_enable_aux_clock( struct mgsl_struct *info, u32 DataRate );
void usc_enable_loopback( struct mgsl_struct *info, int enable );

void usc_get_serial_signals( struct mgsl_struct *info );
void usc_set_serial_signals( struct mgsl_struct *info );

void usc_reset( struct mgsl_struct *info );

void usc_set_sync_mode( struct mgsl_struct *info );
void usc_set_sdlc_mode( struct mgsl_struct *info );
void usc_set_async_mode( struct mgsl_struct *info );
void usc_enable_async_clock( struct mgsl_struct *info, u32 DataRate );

void usc_loopback_frame( struct mgsl_struct *info );

void mgsl_tx_timeout(unsigned long context);


void usc_loopmode_cancel_transmit( struct mgsl_struct * info );
void usc_loopmode_insert_request( struct mgsl_struct * info );
int usc_loopmode_active( struct mgsl_struct * info);
void usc_loopmode_send_done( struct mgsl_struct * info );
int usc_loopmode_send_active( struct mgsl_struct * info );

/*
 * Defines a BUS descriptor value for the PCI adapter
 * local bus address ranges.
 */

#define BUS_DESCRIPTOR( WrHold, WrDly, RdDly, Nwdd, Nwad, Nxda, Nrdd, Nrad ) \
(0x00400020 + \
((WrHold) << 30) + \
((WrDly)  << 28) + \
((RdDly)  << 26) + \
((Nwdd)   << 20) + \
((Nwad)   << 15) + \
((Nxda)   << 13) + \
((Nrdd)   << 11) + \
((Nrad)   <<  6) )

void mgsl_trace_block(struct mgsl_struct *info,const char* data, int count, int xmit);

/*
 * Adapter diagnostic routines
 */
BOOLEAN mgsl_register_test( struct mgsl_struct *info );
BOOLEAN mgsl_irq_test( struct mgsl_struct *info );
BOOLEAN mgsl_dma_test( struct mgsl_struct *info );
BOOLEAN mgsl_memory_test( struct mgsl_struct *info );
int mgsl_adapter_test( struct mgsl_struct *info );

/*
 * device and resource management routines
 */
int mgsl_claim_resources(struct mgsl_struct *info);
void mgsl_release_resources(struct mgsl_struct *info);
void mgsl_add_device(struct mgsl_struct *info);
struct mgsl_struct* mgsl_allocate_device(void);
int mgsl_enumerate_devices(void);

/*
 * DMA buffer manupulation functions.
 */
void mgsl_free_rx_frame_buffers( struct mgsl_struct *info, unsigned int StartIndex, unsigned int EndIndex );
int  mgsl_get_rx_frame( struct mgsl_struct *info );
void mgsl_reset_rx_dma_buffers( struct mgsl_struct *info );
void mgsl_load_tx_dma_buffer( struct mgsl_struct *info, const char *Buffer, unsigned int BufferSize);
void mgsl_load_pci_memory(char* TargetPtr, const char* SourcePtr, unsigned short count);

/*
 * DMA and Shared Memory buffer allocation and formatting
 */
int  mgsl_allocate_dma_buffers(struct mgsl_struct *info);
void mgsl_free_dma_buffers(struct mgsl_struct *info);
int  mgsl_alloc_frame_memory(struct mgsl_struct *info, DMABUFFERENTRY *BufferList,int Buffercount);
void mgsl_free_frame_memory(struct mgsl_struct *info, DMABUFFERENTRY *BufferList,int Buffercount);
int  mgsl_alloc_buffer_list_memory(struct mgsl_struct *info);
void mgsl_free_buffer_list_memory(struct mgsl_struct *info);
int mgsl_alloc_intermediate_rxbuffer_memory(struct mgsl_struct *info);
void mgsl_free_intermediate_rxbuffer_memory(struct mgsl_struct *info);

/*
 * Bottom half interrupt handlers
 */
void mgsl_bh_handler(void* Context);
void mgsl_bh_receive_dma( struct mgsl_struct *info, unsigned short status );
void mgsl_bh_transmit_data( struct mgsl_struct *info, unsigned short Datacount );
void mgsl_bh_status_handler( struct mgsl_struct *info, unsigned short status );

void mgsl_format_bh_queue( struct mgsl_struct *info );
void mgsl_bh_queue_put( struct mgsl_struct *info, unsigned char type, unsigned short status );
int mgsl_bh_queue_get( struct mgsl_struct *info );


/*
 * Interrupt handler routines and dispatch table.
 */
void mgsl_isr_null( struct mgsl_struct *info );
void mgsl_isr_transmit_data( struct mgsl_struct *info );
void mgsl_isr_receive_data( struct mgsl_struct *info );
void mgsl_isr_receive_status( struct mgsl_struct *info );
void mgsl_isr_transmit_status( struct mgsl_struct *info );
void mgsl_isr_io_pin( struct mgsl_struct *info );
void mgsl_isr_misc( struct mgsl_struct *info );
void mgsl_isr_receive_dma( struct mgsl_struct *info );

typedef void (*isr_dispatch_func)(struct mgsl_struct *);

isr_dispatch_func UscIsrTable[7] =
{
	mgsl_isr_null,
	mgsl_isr_misc,
	mgsl_isr_io_pin,
	mgsl_isr_transmit_data,
	mgsl_isr_transmit_status,
	mgsl_isr_receive_data,
	mgsl_isr_receive_status
};

/*
 * ioctl call handlers
 */
static int set_modem_info(struct mgsl_struct * info, unsigned int cmd,
			  unsigned int *value);
static int get_modem_info(struct mgsl_struct * info, unsigned int *value);
static int mgsl_get_stats(struct mgsl_struct * info, struct mgsl_icount
	*user_icount);
static int mgsl_get_params(struct mgsl_struct * info, MGSL_PARAMS *user_params);
static int mgsl_set_params(struct mgsl_struct * info, MGSL_PARAMS *new_params);
static int mgsl_get_txidle(struct mgsl_struct * info, int*idle_mode);
static int mgsl_set_txidle(struct mgsl_struct * info, int idle_mode);
static int mgsl_txenable(struct mgsl_struct * info, int enable);
static int mgsl_txabort(struct mgsl_struct * info);
static int mgsl_rxenable(struct mgsl_struct * info, int enable);
static int mgsl_wait_event(struct mgsl_struct * info, int * mask);
static int mgsl_loopmode_send_done( struct mgsl_struct * info );

#define jiffies_from_ms(a) ((((a) * HZ)/1000)+1)

/*
 * Global linked list of SyncLink devices
 */
struct mgsl_struct *mgsl_device_list = NULL;
int mgsl_device_count = 0;

/*
 * Set this param to non-zero to load eax with the
 * .text section address and breakpoint on module load.
 * This is useful for use with gdb and add-symbol-file command.
 */
int break_on_load=0;

/*
 * Driver major number, defaults to zero to get auto
 * assigned major number. May be forced as module parameter.
 */
int ttymajor=0;

int cuamajor=0;

/*
 * Array of user specified options for ISA adapters.
 */
static int io[MAX_ISA_DEVICES] = {0,};
static int irq[MAX_ISA_DEVICES] = {0,};
static int dma[MAX_ISA_DEVICES] = {0,};
static int debug_level = 0;
static int maxframe[MAX_TOTAL_DEVICES] = {0,};

	
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
MODULE_PARM(break_on_load,"i");
MODULE_PARM(ttymajor,"i");
MODULE_PARM(cuamajor,"i");
MODULE_PARM(io,"1-" __MODULE_STRING(MAX_ISA_DEVICES) "i");
MODULE_PARM(irq,"1-" __MODULE_STRING(MAX_ISA_DEVICES) "i");
MODULE_PARM(dma,"1-" __MODULE_STRING(MAX_ISA_DEVICES) "i");
MODULE_PARM(debug_level,"i");
MODULE_PARM(maxframe,"1-" __MODULE_STRING(MAX_TOTAL_DEVICES) "i");
#endif

static char *driver_name = "SyncLink serial driver";
static char *driver_version = "1.14";

static struct tty_driver serial_driver, callout_driver;
static int serial_refcount;

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS 256


static void mgsl_change_params(struct mgsl_struct *info);
static void mgsl_wait_until_sent(struct tty_struct *tty, int timeout);

static struct tty_struct **serial_table = NULL;
static struct termios **serial_termios = NULL;
static struct termios **serial_termios_locked = NULL;

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

/*
 * 1st function defined in .text section. Calling this function in
 * init_module() followed by a breakpoint allows a remote debugger
 * (gdb) to get the .text address for the add-symbol-file command.
 * This allows remote debugging of dynamically loadable modules.
 */
void* mgsl_get_text_ptr(void);
void* mgsl_get_text_ptr() {return mgsl_get_text_ptr;}

/*
 * tmp_buf is used as a temporary buffer by mgsl_write.  We need to
 * lock it in case the COPY_FROM_USER blocks while swapping in a page,
 * and some other program tries to do a serial write at the same time.
 * Since the lock will only come under contention when the system is
 * swapping and available memory is low, it makes sense to share one
 * buffer across all the serial ioports, since it significantly saves
 * memory if large numbers of serial ports are open.
 */
static unsigned char *tmp_buf;
static DECLARE_MUTEX(tmp_buf_sem);

static inline int mgsl_paranoia_check(struct mgsl_struct *info,
					kdev_t device, const char *routine)
{
#ifdef MGSL_PARANOIA_CHECK
	static const char *badmagic =
		"Warning: bad magic number for mgsl struct (%s) in %s\n";
	static const char *badinfo =
		"Warning: null mgsl_struct for (%s) in %s\n";

	if (!info) {
		printk(badinfo, kdevname(device), routine);
		return 1;
	}
	if (info->magic != MGSL_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}
#endif
	return 0;
}

/* mgsl_stop()		throttle (stop) transmitter
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static void mgsl_stop(struct tty_struct *tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_stop"))
		return;
	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("mgsl_stop(%s)\n",info->device_name);	
		
	spin_lock_irqsave(&info->irq_spinlock,flags);
	if (info->tx_enabled)
	 	usc_stop_transmitter(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
}	/* end of mgsl_stop() */

/* mgsl_start()		release (start) transmitter
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static void mgsl_start(struct tty_struct *tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_start"))
		return;
	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("mgsl_start(%s)\n",info->device_name);	
		
	spin_lock_irqsave(&info->irq_spinlock,flags);
	if (!info->tx_enabled)
	 	usc_start_transmitter(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
}	/* end of mgsl_start() */

/*
 * Bottom half work queue access functions
 */

/* mgsl_format_bh_queue()
 * 
 * 	Initialize the bottom half processing queue
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_format_bh_queue( struct mgsl_struct *info )
{
	BH_QUEUE bh_queue = info->bh_queue;
	int i;

	/* go through sequentially tacking the little bits together */

	for ( i=0; i < MAX_BH_QUEUE_ENTRIES; i++ ) {
		if ( info->free_bh_queue_tail == NULL )
			info->free_bh_queue_head = bh_queue;
		else
			info->free_bh_queue_tail->link = bh_queue;
		info->free_bh_queue_tail = bh_queue++;
	}

	/* As a safety measure, mark the end of the chain with a NULL */
	info->free_bh_queue_tail->link = NULL;
	info->isr_overflow=0;

}	/* end of mgsl_format_bh_queue() */

/* mgsl_bh_queue_put()
 * 
 * 	Add a BH event to the BH queue
 * 
 * Arguments:		info		pointer to device instance data
 * 			type	BH event type
 * 			status		BH event status
 * 
 * Return Value:	None
 */
void mgsl_bh_queue_put( struct mgsl_struct *info, unsigned char type, unsigned short status )
{
	BH_EVENT *event = info->free_bh_queue_head;

	if ( event != NULL ) {
		/* remove free element from head of free list */
		info->free_bh_queue_head = event->link;
		event->link = NULL;

		/* file out new BH event */
		event->type = type;
		event->status = status;

		/* add element to tail of pending list */
		if ( info->bh_queue_head != NULL ){
			/* BH queue is not empty, add current element to tail */
			info->bh_queue_tail->link = event;
		} else {
			/* the BH queue is empty so this element becomes the head of queue */
			info->bh_queue_head = event;
		}

		/* the new element becomes tail of queue */
		info->bh_queue_tail = event;
	} else {
		/* No more free BH action elements in queue. */
		/* This happens when too many interrupts are occuring */
		/* for the mgsl_bh_handler to process so set a flag. */

		info->isr_overflow = 1;
	}

}	/* end of mgsl_bh_queue_put() */

/* mgsl_bh_queue_get()
 * 
 *	Free the current work item (if any) and get the
 * 	next work item from the head of the pending work item queue.
 *
 * Effects:
 * 
 * 	If a BH action element is available on the BH action queue
 * 	then the head of the queue is removed and bh_action
 * 	is set to point to the removed element.
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	1 if BH action removed from queue
 */
int mgsl_bh_queue_get( struct mgsl_struct *info )
{
	unsigned long flags;
	
	spin_lock_irqsave(&info->irq_spinlock,flags);

	if ( info->bh_action ) {
		/* free the current work item */
		if ( info->free_bh_queue_head != NULL ){
			/* free queue is not empty, add current element to tail */
			info->free_bh_queue_tail->link = info->bh_action;
		} else {
			/* free queue is empty so this element becomes the head of queue */
			info->free_bh_queue_head = info->bh_action;
		}

		/* add element to tail of free queue */
		info->free_bh_queue_tail = info->bh_action;
		info->free_bh_queue_tail->link = NULL;
	}
	
	/* attempt to remove element from head of queue */
	info->bh_action = info->bh_queue_head;

	if ( info->bh_action != NULL ){
		/* BH queue is not empty, remove element from queue head */
		info->bh_queue_head = info->bh_action->link;
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
		return 1;
	}
	
	if ( info->isr_overflow ) {
		if (debug_level >= DEBUG_LEVEL_BH)
			printk("ISR overflow cleared.\n");
		info->isr_overflow=0;
		usc_EnableMasterIrqBit(info);
		usc_EnableDmaInterrupts(info,DICR_MASTER);
	}

	/* Mark BH routine as complete */
	info->bh_running   = 0;
	info->bh_requested = 0;
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	return 0;

}	/* end of mgsl_bh_queue_get() */

/* mgsl_bh_handler()
 * 
 * 	Perform bottom half processing of work items queued by ISR.
 *
 * Arguments:		Context		pointer to device instance data
 * Return Value:	None
 */
void mgsl_bh_handler(void* Context)
{
	struct mgsl_struct *info = (struct mgsl_struct*)Context;
	
	if (!info)
		return;
		
	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):mgsl_bh_handler(%s) entry\n",
			__FILE__,__LINE__,info->device_name);
	
	info->bh_running = 1;

	/* Attempt to clear out the BH queue */

	while( mgsl_bh_queue_get(info) ) {
	
		/* Process work item */
		if ( debug_level >= DEBUG_LEVEL_BH )
			printk( "%s(%d):mgsl_bh_handler() work item action=%d\n",
				__FILE__,__LINE__,info->bh_action->type);

		switch ( info->bh_action->type ) {
		
		case BH_TYPE_RECEIVE_DMA:
			mgsl_bh_receive_dma( info, info->bh_action->status );
			break;

		case BH_TYPE_TRANSMIT_STATUS:
		case BH_TYPE_TRANSMIT_DATA:
			mgsl_bh_transmit_data( info, info->bh_action->status );
			break;

		case BH_TYPE_STATUS:
			mgsl_bh_status_handler( info, info->bh_action->status );
			break;

		default:
			/* unknown work item ID */
			printk("Unknown work item ID=%08X!\n",
				info->bh_action->type );
			break;
		}
	}

	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):mgsl_bh_handler(%s) exit\n",
			__FILE__,__LINE__,info->device_name);
	
}	/* end of mgsl_bh_handler() */

/* mgsl_bh_receive_dma()
 * 
 * 	Perform bottom half processing for a receive DMA interrupt
 * 	This occurs in HDLC mode after a DMA buffer has terminated
 * 	or the DMA buffers have been exhausted.
 * 
 * Arguments:
 * 
 * 	info		pointer to device instance data
 * 	status		status word
 * 
 * Return Value:	None
 */
void mgsl_bh_receive_dma( struct mgsl_struct *info, unsigned short status )
{
	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):mgsl_bh_receive_dma(%s)\n",
			__FILE__,__LINE__,info->device_name);
	
	while( mgsl_get_rx_frame(info) );

}	/* end of mgsl_bh_receive_dma() */

/* mgsl_bh_transmit_data()
 * 
 * 	Process a transmit data interrupt event
 * 	This occurs in asynchronous communications mode.
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_bh_transmit_data( struct mgsl_struct *info, unsigned short Datacount )
{
	struct tty_struct *tty = info->tty;
	unsigned long flags;
	
	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):mgsl_bh_transmit_data() entry on %s\n",
			__FILE__,__LINE__,info->device_name);
			
	/* wakeup any waiting write requests */
 	if (tty) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup) {
			if ( debug_level >= DEBUG_LEVEL_BH )
				printk( "%s(%d):calling ldisc.write_wakeup on %s\n",
					__FILE__,__LINE__,info->device_name);
			(tty->ldisc.write_wakeup)(tty);
		}
		wake_up_interruptible(&tty->write_wait);
	}

	/* if transmitter idle and loopmode_send_done_requested
	 * then start echoing RxD to TxD
	 */
	spin_lock_irqsave(&info->irq_spinlock,flags);
 	if ( !info->tx_active && info->loopmode_send_done_requested )
 		usc_loopmode_send_done( info );
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

}	/* End Of mgsl_bh_transmit_data() */

/* mgsl_bh_status_handler()
 * 
 * 	Peform bottom half processing for a status interrupt
 * 
 * 	This event is generated when a I/O pin (serial signal)
 * 	has a transition. If there is a pending WaitEvent call
 * 	and the status transition is identified in the EventMast
 * 	of the pending call then complete the pending call.
 * 
 * Arguments:
 * 
 * 	info		pointer to device instance data
 * 	status		status word
 * 
 * Return Value:	None
 */
void mgsl_bh_status_handler( struct mgsl_struct *info, unsigned short status )
{
	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):mgsl_bh_status_handler() entry on %s\n",
			__FILE__,__LINE__,info->device_name);

	if (status & MISCSTATUS_RI_LATCHED) {
		if (info->ri_chkcount)
			(info->ri_chkcount)--;
	}
	if (status & MISCSTATUS_DSR_LATCHED) {
		if (info->dsr_chkcount)
			(info->dsr_chkcount)--;
	}
	if (status & MISCSTATUS_DCD_LATCHED) {
		if (info->dcd_chkcount)
			(info->dcd_chkcount)--;
	}
	if (status & MISCSTATUS_CTS_LATCHED) {
		if (info->cts_chkcount)
			(info->cts_chkcount)--;
	}
	
}	/* End Of mgsl_bh_status_handler() */

/* mgsl_isr_receive_status()
 * 
 *	Service a receive status interrupt. The type of status
 *	interrupt is indicated by the state of the RCSR.
 *	This is only used for HDLC mode.
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_isr_receive_status( struct mgsl_struct *info )
{
	u16 status = usc_InReg( info, RCSR );

	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_receive_status status=%04X\n",
			__FILE__,__LINE__,status);
			
 	if ( (status & RXSTATUS_ABORT_RECEIVED) && 
		info->loopmode_insert_requested &&
 		usc_loopmode_active(info) )
 	{
		++info->icount.rxabort;
	 	info->loopmode_insert_requested = FALSE;
 
 		/* clear CMR:13 to start echoing RxD to TxD */
		info->cmr_value &= ~BIT13;
 		usc_OutReg(info, CMR, info->cmr_value);
 
		/* disable received abort irq (no longer required) */
	 	usc_OutReg(info, RICR,
 			(usc_InReg(info, RICR) & ~RXSTATUS_ABORT_RECEIVED));
 	}

	if (status & (RXSTATUS_EXITED_HUNT + RXSTATUS_IDLE_RECEIVED)) {
		if (status & RXSTATUS_EXITED_HUNT)
			info->icount.exithunt++;
		if (status & RXSTATUS_IDLE_RECEIVED)
			info->icount.rxidle++;
		wake_up_interruptible(&info->event_wait_q);
	}

	if (status & RXSTATUS_OVERRUN){
//		/* Purge receive FIFO to allow DMA buffer completion
//		 * with overrun status stored in the receive status block.
//		 */
//		usc_RCmd( info, RCmd_EnterHuntmode );
//		usc_RTCmd( info, RTCmd_PurgeRxFifo );
		
		info->icount.rxover++;
		usc_process_rxoverrun_sync( info );
	}

	usc_ClearIrqPendingBits( info, RECEIVE_STATUS );
	usc_UnlatchRxstatusBits( info, status );

}	/* end of mgsl_isr_receive_status() */

/* mgsl_isr_transmit_status()
 * 
 * 	Service a transmit status interrupt
 *	HDLC mode :end of transmit frame
 *	Async mode:all data is sent
 * 	transmit status is indicated by bits in the TCSR.
 * 
 * Arguments:		info	       pointer to device instance data
 * Return Value:	None
 */
void mgsl_isr_transmit_status( struct mgsl_struct *info )
{
	u16 status = usc_InReg( info, TCSR );

	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_transmit_status status=%04X\n",
			__FILE__,__LINE__,status);
	
	usc_ClearIrqPendingBits( info, TRANSMIT_STATUS );
	usc_UnlatchTxstatusBits( info, status );
	
	if ( status & (TXSTATUS_UNDERRUN | TXSTATUS_ABORT_SENT) )
	{
		/* finished sending HDLC abort. This may leave	*/
		/* the TxFifo with data from the aborted frame	*/
		/* so purge the TxFifo. Also shutdown the DMA	*/
		/* channel in case there is data remaining in 	*/
		/* the DMA buffer				*/
 		usc_DmaCmd( info, DmaCmd_ResetTxChannel );
 		usc_RTCmd( info, RTCmd_PurgeTxFifo );
	}
 
	if ( status & TXSTATUS_EOF_SENT )
		info->icount.txok++;
	else if ( status & TXSTATUS_UNDERRUN )
		info->icount.txunder++;
	else if ( status & TXSTATUS_ABORT_SENT )
		info->icount.txabort++;
	else
		info->icount.txunder++;
			
	info->tx_active = 0;
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	del_timer(&info->tx_timer);	
	
	if ( info->drop_rts_on_tx_done ) {
		usc_get_serial_signals( info );
		if ( info->serial_signals & SerialSignal_RTS ) {
			info->serial_signals &= ~SerialSignal_RTS;
			usc_set_serial_signals( info );
		}
		info->drop_rts_on_tx_done = 0;
	}
		
	if (info->tty->stopped || info->tty->hw_stopped) {
		usc_stop_transmitter(info);
		return;
	}
	
	mgsl_bh_queue_put(info, BH_TYPE_TRANSMIT_STATUS, status);

}	/* end of mgsl_isr_transmit_status() */

/* mgsl_isr_io_pin()
 * 
 * 	Service an Input/Output pin interrupt. The type of
 * 	interrupt is indicated by bits in the MISR
 * 	
 * Arguments:		info	       pointer to device instance data
 * Return Value:	None
 */
void mgsl_isr_io_pin( struct mgsl_struct *info )
{
 	struct	mgsl_icount *icount;
	u16 status = usc_InReg( info, MISR );

	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_io_pin status=%04X\n",
			__FILE__,__LINE__,status);
			
	usc_ClearIrqPendingBits( info, IO_PIN );
	usc_UnlatchIostatusBits( info, status );

	if (status & (MISCSTATUS_CTS_LATCHED | MISCSTATUS_DCD_LATCHED |
	              MISCSTATUS_DSR_LATCHED | MISCSTATUS_RI_LATCHED) ) {
		icount = &info->icount;
		/* update input line counters */
		if (status & MISCSTATUS_RI_LATCHED) {
			if ((info->ri_chkcount)++ >= IO_PIN_SHUTDOWN_LIMIT)
				usc_DisablestatusIrqs(info,SICR_RI);
			icount->rng++;
			if ( status & MISCSTATUS_RI )
				info->input_signal_events.ri_up++;	
			else
				info->input_signal_events.ri_down++;	
		}
		if (status & MISCSTATUS_DSR_LATCHED) {
			if ((info->dsr_chkcount)++ >= IO_PIN_SHUTDOWN_LIMIT)
				usc_DisablestatusIrqs(info,SICR_DSR);
			icount->dsr++;
			if ( status & MISCSTATUS_DSR )
				info->input_signal_events.dsr_up++;
			else
				info->input_signal_events.dsr_down++;
		}
		if (status & MISCSTATUS_DCD_LATCHED) {
			if ((info->dcd_chkcount)++ >= IO_PIN_SHUTDOWN_LIMIT)
				usc_DisablestatusIrqs(info,SICR_DCD);
			icount->dcd++;
			if ( status & MISCSTATUS_DCD )
				info->input_signal_events.dcd_up++;
			else
				info->input_signal_events.dcd_down++;
#ifdef CONFIG_HARD_PPS
			if ((info->flags & ASYNC_HARDPPS_CD) &&
			    (status & MISCSTATUS_DCD_LATCHED))
				hardpps();
#endif
		}
		if (status & MISCSTATUS_CTS_LATCHED)
		{
			if ((info->cts_chkcount)++ >= IO_PIN_SHUTDOWN_LIMIT)
				usc_DisablestatusIrqs(info,SICR_CTS);
			icount->cts++;
			if ( status & MISCSTATUS_CTS )
				info->input_signal_events.cts_up++;
			else
				info->input_signal_events.cts_down++;
		}
		wake_up_interruptible(&info->status_event_wait_q);
		wake_up_interruptible(&info->event_wait_q);

		if ( (info->flags & ASYNC_CHECK_CD) && 
		     (status & MISCSTATUS_DCD_LATCHED) ) {
			if ( debug_level >= DEBUG_LEVEL_ISR )
				printk("%s CD now %s...", info->device_name,
				       (status & MISCSTATUS_DCD) ? "on" : "off");
			if (status & MISCSTATUS_DCD)
				wake_up_interruptible(&info->open_wait);
			else if (!((info->flags & ASYNC_CALLOUT_ACTIVE) &&
				   (info->flags & ASYNC_CALLOUT_NOHUP))) {
				if ( debug_level >= DEBUG_LEVEL_ISR )
					printk("doing serial hangup...");
				if (info->tty)
					tty_hangup(info->tty);
			}
		}
	
		if ( (info->flags & ASYNC_CTS_FLOW) && 
		     (status & MISCSTATUS_CTS_LATCHED) ) {
			if (info->tty->hw_stopped) {
				if (status & MISCSTATUS_CTS) {
					if ( debug_level >= DEBUG_LEVEL_ISR )
						printk("CTS tx start...");
					info->tty->hw_stopped = 0;
					usc_start_transmitter(info);
					mgsl_bh_queue_put( info, BH_TYPE_TRANSMIT_DATA, status );
					return;
				}
			} else {
				if (!(status & MISCSTATUS_CTS)) {
					if ( debug_level >= DEBUG_LEVEL_ISR )
						printk("CTS tx stop...");
					info->tty->hw_stopped = 1;
					usc_stop_transmitter(info);
				}
			}
		}
	}

	mgsl_bh_queue_put(info, BH_TYPE_STATUS, status);
	
	/* for diagnostics set IRQ flag */
	if ( status & MISCSTATUS_TXC_LATCHED ){
		usc_OutReg( info, SICR,
			(unsigned short)(usc_InReg(info,SICR) & ~(SICR_TXC_ACTIVE+SICR_TXC_INACTIVE)) );
		usc_UnlatchIostatusBits( info, MISCSTATUS_TXC_LATCHED );
		info->irq_occurred = 1;
	}

}	/* end of mgsl_isr_io_pin() */

/* mgsl_isr_transmit_data()
 * 
 * 	Service a transmit data interrupt (async mode only).
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_isr_transmit_data( struct mgsl_struct *info )
{
	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_transmit_data xmit_cnt=%d\n",
			__FILE__,__LINE__,info->xmit_cnt);
			
	usc_ClearIrqPendingBits( info, TRANSMIT_DATA );
	
	if (info->tty->stopped || info->tty->hw_stopped) {
		usc_stop_transmitter(info);
		return;
	}
	
	if ( info->xmit_cnt )
		usc_load_txfifo( info );
	else
		info->tx_active = 0;
		
	if (info->xmit_cnt < WAKEUP_CHARS)
		mgsl_bh_queue_put(info, BH_TYPE_TRANSMIT_DATA, (unsigned short)(info->xmit_cnt));

}	/* end of mgsl_isr_transmit_data() */

/* mgsl_isr_receive_data()
 * 
 * 	Service a receive data interrupt. This occurs
 * 	when operating in asynchronous interrupt transfer mode.
 *	The receive data FIFO is flushed to the receive data buffers. 
 * 
 * Arguments:		info		pointer to device instance data
 * Return Value:	None
 */
void mgsl_isr_receive_data( struct mgsl_struct *info )
{
	int Fifocount;
	u16 status;
	unsigned char DataByte;
 	struct tty_struct *tty = info->tty;
 	struct	mgsl_icount *icount = &info->icount;
	
	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_receive_data\n",
			__FILE__,__LINE__);

	usc_ClearIrqPendingBits( info, RECEIVE_DATA );
	
	/* select FIFO status for RICR readback */
	usc_RCmd( info, RCmd_SelectRicrRxFifostatus );

	/* clear the Wordstatus bit so that status readback */
	/* only reflects the status of this byte */
	usc_OutReg( info, RICR+LSBONLY, (u16)(usc_InReg(info, RICR+LSBONLY) & ~BIT3 ));

	/* flush the receive FIFO */

	while( (Fifocount = (usc_InReg(info,RICR) >> 8)) ) {
		/* read one byte from RxFIFO */
		outw( (inw(info->io_base + CCAR) & 0x0780) | (RDR+LSBONLY),
		      info->io_base + CCAR );
		DataByte = inb( info->io_base + CCAR );

		/* get the status of the received byte */
		status = usc_InReg(info, RCSR);
		if ( status & (RXSTATUS_FRAMING_ERROR + RXSTATUS_PARITY_ERROR +
				RXSTATUS_OVERRUN + RXSTATUS_BREAK_RECEIVED) )
			usc_UnlatchRxstatusBits(info,RXSTATUS_ALL);
		
		if (tty->flip.count >= TTY_FLIPBUF_SIZE)
			continue;
			
		*tty->flip.char_buf_ptr = DataByte;
		icount->rx++;
		
		*tty->flip.flag_buf_ptr = 0;
		if ( status & (RXSTATUS_FRAMING_ERROR + RXSTATUS_PARITY_ERROR +
				RXSTATUS_OVERRUN + RXSTATUS_BREAK_RECEIVED) ) {
			printk("rxerr=%04X\n",status);					
			/* update error statistics */
			if ( status & RXSTATUS_BREAK_RECEIVED ) {
				status &= ~(RXSTATUS_FRAMING_ERROR + RXSTATUS_PARITY_ERROR);
				icount->brk++;
			} else if (status & RXSTATUS_PARITY_ERROR) 
				icount->parity++;
			else if (status & RXSTATUS_FRAMING_ERROR)
				icount->frame++;
			else if (status & RXSTATUS_OVERRUN) {
				/* must issue purge fifo cmd before */
				/* 16C32 accepts more receive chars */
				usc_RTCmd(info,RTCmd_PurgeRxFifo);
				icount->overrun++;
			}

			/* discard char if tty control flags say so */					
			if (status & info->ignore_status_mask)
				continue;
				
			status &= info->read_status_mask;
		
			if (status & RXSTATUS_BREAK_RECEIVED) {
				*tty->flip.flag_buf_ptr = TTY_BREAK;
				if (info->flags & ASYNC_SAK)
					do_SAK(tty);
			} else if (status & RXSTATUS_PARITY_ERROR)
				*tty->flip.flag_buf_ptr = TTY_PARITY;
			else if (status & RXSTATUS_FRAMING_ERROR)
				*tty->flip.flag_buf_ptr = TTY_FRAME;
			if (status & RXSTATUS_OVERRUN) {
				/* Overrun is special, since it's
				 * reported immediately, and doesn't
				 * affect the current character
				 */
				if (tty->flip.count < TTY_FLIPBUF_SIZE) {
					tty->flip.count++;
					tty->flip.flag_buf_ptr++;
					tty->flip.char_buf_ptr++;
					*tty->flip.flag_buf_ptr = TTY_OVERRUN;
				}
			}
		}	/* end of if (error) */
		
		tty->flip.flag_buf_ptr++;
		tty->flip.char_buf_ptr++;
		tty->flip.count++;
	}

	if ( debug_level >= DEBUG_LEVEL_ISR ) {
		printk("%s(%d):mgsl_isr_receive_data flip count=%d\n",
			__FILE__,__LINE__,tty->flip.count);
		printk("%s(%d):rx=%d brk=%d parity=%d frame=%d overrun=%d\n",
			__FILE__,__LINE__,icount->rx,icount->brk,
			icount->parity,icount->frame,icount->overrun);
	}
			
	if ( tty->flip.count ) {
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
		tty_flip_buffer_push(tty);
#else		
		queue_task(&tty->flip.tqueue, &tq_timer); 
#endif		
	}
	

}	/* end of mgsl_isr_receive_data() */

/* mgsl_isr_misc()
 * 
 * 	Service a miscellaneos interrupt source.
 * 	
 * Arguments:		info		pointer to device extension (instance data)
 * Return Value:	None
 */
void mgsl_isr_misc( struct mgsl_struct *info )
{
	u16 status = usc_InReg( info, MISR );

	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_misc status=%04X\n",
			__FILE__,__LINE__,status);
			
	usc_ClearIrqPendingBits( info, MISC );
	usc_UnlatchMiscstatusBits( info, status );

}	/* end of mgsl_isr_misc() */

/* mgsl_isr_null()
 *
 * 	Services undefined interrupt vectors from the
 * 	USC. (hence this function SHOULD never be called)
 * 
 * Arguments:		info		pointer to device extension (instance data)
 * Return Value:	None
 */
void mgsl_isr_null( struct mgsl_struct *info )
{

}	/* end of mgsl_isr_null() */

/* mgsl_isr_receive_dma()
 * 
 * 	Service a receive DMA channel interrupt.
 * 	For this driver there are two sources of receive DMA interrupts
 * 	as identified in the Receive DMA mode Register (RDMR):
 * 
 * 	BIT3	EOA/EOL		End of List, all receive buffers in receive
 * 				buffer list have been filled (no more free buffers
 * 				available). The DMA controller has shut down.
 * 
 * 	BIT2	EOB		End of Buffer. This interrupt occurs when a receive
 * 				DMA buffer is terminated in response to completion
 * 				of a good frame or a frame with errors. The status
 * 				of the frame is stored in the buffer entry in the
 * 				list of receive buffer entries.
 * 
 * Arguments:		info		pointer to device instance data
 * Return Value:	None
 */
void mgsl_isr_receive_dma( struct mgsl_struct *info )
{
	u16 status;
	
	/* clear interrupt pending and IUS bit for Rx DMA IRQ */
	usc_OutDmaReg( info, CDIR, BIT9+BIT1 );

	/* Read the receive DMA status to identify interrupt type. */
	/* This also clears the status bits. */
	status = usc_InDmaReg( info, RDMR );

	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_isr_receive_dma(%s) status=%04X\n",
			__FILE__,__LINE__,info->device_name,status);
			
	/* Post a receive event for BH processing. */
	mgsl_bh_queue_put( info, BH_TYPE_RECEIVE_DMA, status );
	
	if ( status & BIT3 ) {
		info->rx_overflow = 1;
		info->icount.buf_overrun++;
	}

}	/* end of mgsl_isr_receive_dma() */

/* mgsl_interrupt()
 * 
 * 	Interrupt service routine entry point.
 * 	
 * Arguments:
 * 
 * 	irq		interrupt number that caused interrupt
 * 	dev_id		device ID supplied during interrupt registration
 * 	regs		interrupted processor context
 * 	
 * Return Value: None
 */
static void mgsl_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
	struct mgsl_struct * info;
	u16 UscVector;
	u16 DmaVector;

	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_interrupt(%d)entry.\n",
			__FILE__,__LINE__,irq);

	info = (struct mgsl_struct *)dev_id;	
	if (!info)
		return;
		
	spin_lock(&info->irq_spinlock);

	for(;;) {
		/* Read the interrupt vectors from hardware. */
		UscVector = usc_InReg(info, IVR) >> 9;
		DmaVector = usc_InDmaReg(info, DIVR);
		
		if ( debug_level >= DEBUG_LEVEL_ISR )	
			printk("%s(%d):%s UscVector=%08X DmaVector=%08X\n",
				__FILE__,__LINE__,info->device_name,UscVector,DmaVector);
			
		if ( !UscVector && !DmaVector )
			break;
			
		/* Dispatch interrupt vector */
		if ( UscVector )
			(*UscIsrTable[UscVector])(info);
		else
			mgsl_isr_receive_dma(info);

		if ( info->isr_overflow ) {
			printk(KERN_ERR"%s(%d):%s isr overflow irq=%d\n",
				__FILE__,__LINE__,info->device_name, irq);
			usc_DisableMasterIrqBit(info);
			usc_DisableDmaInterrupts(info,DICR_MASTER);
			break;
		}
	}
	
	/* Request bottom half processing if there's something 
	 * for it to do and the bh is not already running
	 */

	if ( info->bh_queue_head && !info->bh_running && !info->bh_requested ) {
		if ( debug_level >= DEBUG_LEVEL_ISR )	
			printk("%s(%d):%s queueing bh task.\n",
				__FILE__,__LINE__,info->device_name);
		queue_task(&info->task, &tq_immediate);
		mark_bh(IMMEDIATE_BH);
		info->bh_requested = 1;
	}

	spin_unlock(&info->irq_spinlock);
	
	if ( debug_level >= DEBUG_LEVEL_ISR )	
		printk("%s(%d):mgsl_interrupt(%d)exit.\n",
			__FILE__,__LINE__,irq);

}	/* end of mgsl_interrupt() */

/* startup()
 * 
 * 	Initialize and start device.
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	0 if success, otherwise error code
 */
static int startup(struct mgsl_struct * info)
{
	int retval = 0;
	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("%s(%d):mgsl_startup(%s)\n",__FILE__,__LINE__,info->device_name);
		
	if (info->flags & ASYNC_INITIALIZED)
		return 0;
	
	if (!info->xmit_buf) {
		/* allocate a page of memory for a transmit buffer */
		info->xmit_buf = (unsigned char *)get_free_page(GFP_KERNEL);
		if (!info->xmit_buf) {
			printk(KERN_ERR"%s(%d):%s can't allocate transmit buffer\n",
				__FILE__,__LINE__,info->device_name);
			return -ENOMEM;
		}
	}

	mgsl_format_bh_queue(info);
	
	init_timer(&info->tx_timer);
	info->tx_timer.data = (unsigned long)info;
	info->tx_timer.function = mgsl_tx_timeout;
	
	/* Allocate and claim adapter resources */
	retval = mgsl_claim_resources(info);
	
	/* perform existance check and diagnostics */
	if ( !retval )
		retval = mgsl_adapter_test(info);
		
	if ( retval ) {
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
  		if (capable(CAP_SYS_ADMIN) && info->tty)
#else
  		if (suser() && info->tty)
#endif		
			set_bit(TTY_IO_ERROR, &info->tty->flags);
		mgsl_release_resources(info);
  		return retval;
  	}

	/* program hardware for current parameters */
	mgsl_change_params(info);
	
	if (info->tty)
		clear_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags |= ASYNC_INITIALIZED;
	
	return 0;
	
}	/* end of startup() */

/* shutdown()
 *
 * Called by mgsl_close() and mgsl_hangup() to shutdown hardware
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
static void shutdown(struct mgsl_struct * info)
{
	unsigned long flags;
	
	if (!(info->flags & ASYNC_INITIALIZED))
		return;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_shutdown(%s)\n",
			 __FILE__,__LINE__, info->device_name );

	/* clear status wait queue because status changes */
	/* can't happen after shutting down the hardware */
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);

	if (info->xmit_buf) {
		free_page((unsigned long) info->xmit_buf);
		info->xmit_buf = 0;
	}

	spin_lock_irqsave(&info->irq_spinlock,flags);
	usc_DisableMasterIrqBit(info);
	usc_stop_receiver(info);
	usc_stop_transmitter(info);
	usc_DisableInterrupts(info,RECEIVE_DATA + RECEIVE_STATUS +
		TRANSMIT_DATA + TRANSMIT_STATUS + IO_PIN + MISC );
	usc_DisableDmaInterrupts(info,DICR_MASTER + DICR_TRANSMIT + DICR_RECEIVE);
	
	/* Disable DMAEN (Port 7, Bit 14) */
	/* This disconnects the DMA request signal from the ISA bus */
	/* on the ISA adapter. This has no effect for the PCI adapter */
	usc_OutReg(info, PCR, (u16)((usc_InReg(info, PCR) | BIT15) | BIT14));
	
	/* Disable INTEN (Port 6, Bit12) */
	/* This disconnects the IRQ request signal to the ISA bus */
	/* on the ISA adapter. This has no effect for the PCI adapter */
	usc_OutReg(info, PCR, (u16)((usc_InReg(info, PCR) | BIT13) | BIT12));
	
 	if (!info->tty || info->tty->termios->c_cflag & HUPCL) {
 		info->serial_signals &= ~(SerialSignal_DTR + SerialSignal_RTS);
		usc_set_serial_signals(info);
	}
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

	mgsl_release_resources(info);	
	
	if (info->tty)
		set_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags &= ~ASYNC_INITIALIZED;
	
}	/* end of shutdown() */

/* mgsl_change_params()
 *
 *	Reconfigure adapter based on new parameters
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
static void mgsl_change_params(struct mgsl_struct *info)
{
	unsigned cflag;
	unsigned long flags;
	int bits_per_char;

	if (!info->tty || !info->tty->termios)
		return;
		
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_change_params(%s)\n",
			 __FILE__,__LINE__, info->device_name );
			 
	cflag = info->tty->termios->c_cflag;

	/* if B0 rate (hangup) specified then negate DTR and RTS */
	/* otherwise assert DTR and RTS */
 	if (cflag & CBAUD)
		info->serial_signals |= SerialSignal_RTS + SerialSignal_DTR;
	else
		info->serial_signals &= ~(SerialSignal_RTS + SerialSignal_DTR);
	
	/* byte size and parity */
	
	switch (cflag & CSIZE) {
	      case CS5: info->params.data_bits = 5; break;
	      case CS6: info->params.data_bits = 6; break;
	      case CS7: info->params.data_bits = 7; break;
	      case CS8: info->params.data_bits = 8; break;
	      /* Never happens, but GCC is too dumb to figure it out */
	      default:  info->params.data_bits = 7; break;
	      }
	      
	if (cflag & CSTOPB)
		info->params.stop_bits = 2;
	else
		info->params.stop_bits = 1;

	info->params.parity = ASYNC_PARITY_NONE;
	if (cflag & PARENB) {
		if (cflag & PARODD)
			info->params.parity = ASYNC_PARITY_ODD;
		else
			info->params.parity = ASYNC_PARITY_EVEN;
#ifdef CMSPAR
		if (cflag & CMSPAR)
			info->params.parity = ASYNC_PARITY_SPACE;
#endif
	}

	/* calculate number of jiffies to transmit a full
	 * FIFO (32 bytes) at specified data rate
	 */
	bits_per_char = info->params.data_bits + 
			info->params.stop_bits + 1;

	/* if port data rate is set to 460800 or less then
	 * allow tty settings to override, otherwise keep the
	 * current data rate.
	 */
	if (info->params.data_rate <= 460800) {
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
		info->params.data_rate = tty_get_baud_rate(info->tty);
#else
		int i = cflag & CBAUD;
		if (i & CBAUDEX) {
			i &= ~CBAUDEX;
			if (i < 1 || i > 4) 
				info->tty->termios->c_cflag &= ~CBAUDEX;
			else
				i += 15;
		}
		info->params.data_rate = baud_table[i];
#endif	
	}
	
	if ( info->params.data_rate ) {
		info->timeout = (32*HZ*bits_per_char) / 
				info->params.data_rate;
	}
	info->timeout += HZ/50;		/* Add .02 seconds of slop */

	if (cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;
		
	if (cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	/* process tty input control flags */
	
	info->read_status_mask = RXSTATUS_OVERRUN;
	if (I_INPCK(info->tty))
		info->read_status_mask |= RXSTATUS_PARITY_ERROR | RXSTATUS_FRAMING_ERROR;
 	if (I_BRKINT(info->tty) || I_PARMRK(info->tty))
 		info->read_status_mask |= RXSTATUS_BREAK_RECEIVED;
	
	if (I_IGNPAR(info->tty))
		info->ignore_status_mask |= RXSTATUS_PARITY_ERROR | RXSTATUS_FRAMING_ERROR;
	if (I_IGNBRK(info->tty)) {
		info->ignore_status_mask |= RXSTATUS_BREAK_RECEIVED;
		/* If ignoring parity and break indicators, ignore 
		 * overruns too.  (For real raw support).
		 */
		if (I_IGNPAR(info->tty))
			info->ignore_status_mask |= RXSTATUS_OVERRUN;
	}

	/* reprogram the hardware */
	
	spin_lock_irqsave(&info->irq_spinlock,flags);
	
	usc_stop_receiver(info);
	usc_stop_transmitter(info);
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	
	if ( info->params.mode == MGSL_MODE_HDLC )
		usc_set_sync_mode(info);
	else
		usc_set_async_mode(info);
		
	usc_set_serial_signals(info);
	
	info->dcd_chkcount = 0;
	info->cts_chkcount = 0;
	info->ri_chkcount = 0;
	info->dsr_chkcount = 0;

	/* enable modem signal IRQs and read initial signal states */
	usc_EnableStatusIrqs(info,SICR_CTS+SICR_DSR+SICR_DCD+SICR_RI);		
	usc_EnableInterrupts(info, IO_PIN);
	usc_get_serial_signals(info);
		
	if ( cflag & CREAD )
		usc_start_receiver(info);
		
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

}	/* end of mgsl_change_params() */

/* mgsl_put_char()
 * 
 * 	Add a character to the transmit buffer.
 * 	
 * Arguments:		tty	pointer to tty information structure
 * 			ch	character to add to transmit buffer
 * 		
 * Return Value:	None
 */
static void mgsl_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;

	if ( debug_level >= DEBUG_LEVEL_INFO ) {
		printk( "%s(%d):mgsl_put_char(%d) on %s\n",
			__FILE__,__LINE__,ch,info->device_name);
	}		
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_put_char"))
		return;

	if (!tty || !info->xmit_buf)
		return;

	spin_lock_irqsave(&info->irq_spinlock,flags);
	
	if ( (info->params.mode != MGSL_MODE_HDLC) || 
	     !info->tx_active ) {
	
		if (info->xmit_cnt < SERIAL_XMIT_SIZE - 1) {
			info->xmit_buf[info->xmit_head++] = ch;
			info->xmit_head &= SERIAL_XMIT_SIZE-1;
			info->xmit_cnt++;
		}
	}
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
}	/* end of mgsl_put_char() */

/* mgsl_flush_chars()
 * 
 * 	Enable transmitter so remaining characters in the
 * 	transmit buffer are sent.
 * 	
 * Arguments:		tty	pointer to tty information structure
 * Return Value:	None
 */
static void mgsl_flush_chars(struct tty_struct *tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
				
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_flush_chars() entry on %s xmit_cnt=%d\n",
			__FILE__,__LINE__,info->device_name,info->xmit_cnt);
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_flush_chars"))
		return;

	if (info->xmit_cnt <= 0 || tty->stopped || tty->hw_stopped ||
	    !info->xmit_buf)
		return;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_flush_chars() entry on %s starting transmitter\n",
			__FILE__,__LINE__,info->device_name );

	spin_lock_irqsave(&info->irq_spinlock,flags);
	
	if (!info->tx_active) {
		if ( (info->params.mode == MGSL_MODE_HDLC) &&
			info->xmit_cnt ) {
			/* operating in synchronous (frame oriented) mode */
			/* copy data from circular xmit_buf to */
			/* transmit DMA buffer. */
			mgsl_load_tx_dma_buffer(info,
				 info->xmit_buf,info->xmit_cnt);
		}
	 	usc_start_transmitter(info);
	}
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
}	/* end of mgsl_flush_chars() */

/* mgsl_write()
 * 
 * 	Send a block of data
 * 	
 * Arguments:
 * 
 * 	tty		pointer to tty information structure
 * 	from_user	flag: 1 = from user process
 * 	buf		pointer to buffer containing send data
 * 	count		size of send data in bytes
 * 	
 * Return Value:	number of characters written
 */
static int mgsl_write(struct tty_struct * tty, int from_user,
		    const unsigned char *buf, int count)
{
	int	c, ret = 0, err;
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_write(%s) count=%d\n",
			__FILE__,__LINE__,info->device_name,count);
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_write"))
		goto cleanup;

	if (!tty || !info->xmit_buf || !tmp_buf)
		goto cleanup;

	if ( info->params.mode == MGSL_MODE_HDLC ) {
		/* operating in synchronous (frame oriented) mode */

		if (info->tx_active) {
			ret = 0; goto cleanup; 
		}
	
		/* if operating in HDLC LoopMode and the adapter  */
		/* has yet to be inserted into the loop, we can't */
		/* transmit					  */

		if ( (info->params.flags & HDLC_FLAG_HDLC_LOOPMODE) &&
			!usc_loopmode_active(info) )
		{
			ret = 0;
			goto cleanup;
		}

		if ( info->xmit_cnt ) {
			/* Send accumulated from send_char() calls */
			/* as frame and wait before accepting more data. */
			ret = 0;
			
			/* copy data from circular xmit_buf to */
			/* transmit DMA buffer. */
			mgsl_load_tx_dma_buffer(info,
				info->xmit_buf,info->xmit_cnt);
			if ( debug_level >= DEBUG_LEVEL_INFO )
				printk( "%s(%d):mgsl_write(%s) sync xmit_cnt flushing\n",
					__FILE__,__LINE__,info->device_name);
		} else {
			if ( debug_level >= DEBUG_LEVEL_INFO )
				printk( "%s(%d):mgsl_write(%s) sync transmit accepted\n",
					__FILE__,__LINE__,info->device_name);
			ret = count;
			info->xmit_cnt = count;
			if (from_user) {
				down(&tmp_buf_sem);
				COPY_FROM_USER(err,tmp_buf, buf, count);
				if (err) {
					if ( debug_level >= DEBUG_LEVEL_INFO )
						printk( "%s(%d):mgsl_write(%s) sync user buf copy failed\n",
							__FILE__,__LINE__,info->device_name);
					ret = -EFAULT;
				} else
					mgsl_load_tx_dma_buffer(info,tmp_buf,count);
				up(&tmp_buf_sem);
			}
			else
				mgsl_load_tx_dma_buffer(info,buf,count);
		}
	} else {
		if (from_user) {
			down(&tmp_buf_sem);
			while (1) {
				c = MIN(count,
					MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
					    SERIAL_XMIT_SIZE - info->xmit_head));
				if (c <= 0)
					break;

				COPY_FROM_USER(err,tmp_buf, buf, c);
				c -= err;
				if (!c) {
					if (!ret)
						ret = -EFAULT;
					break;
				}
				spin_lock_irqsave(&info->irq_spinlock,flags);
				c = MIN(c, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
					       SERIAL_XMIT_SIZE - info->xmit_head));
				memcpy(info->xmit_buf + info->xmit_head, tmp_buf, c);
				info->xmit_head = ((info->xmit_head + c) &
						   (SERIAL_XMIT_SIZE-1));
				info->xmit_cnt += c;
				spin_unlock_irqrestore(&info->irq_spinlock,flags);
				buf += c;
				count -= c;
				ret += c;
			}
			up(&tmp_buf_sem);
		} else {
			while (1) {
				spin_lock_irqsave(&info->irq_spinlock,flags);
				c = MIN(count,
					MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
					    SERIAL_XMIT_SIZE - info->xmit_head));
				if (c <= 0) {
					spin_unlock_irqrestore(&info->irq_spinlock,flags);
					break;
				}
				memcpy(info->xmit_buf + info->xmit_head, buf, c);
				info->xmit_head = ((info->xmit_head + c) &
						   (SERIAL_XMIT_SIZE-1));
				info->xmit_cnt += c;
				spin_unlock_irqrestore(&info->irq_spinlock,flags);
				buf += c;
				count -= c;
				ret += c;
			}
		}
	}	
	
 	if (info->xmit_cnt && !tty->stopped && !tty->hw_stopped) {
		spin_lock_irqsave(&info->irq_spinlock,flags);
		if (!info->tx_active)
		 	usc_start_transmitter(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
 	}
cleanup:	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_write(%s) returning=%d\n",
			__FILE__,__LINE__,info->device_name,ret);
			
	return ret;
	
}	/* end of mgsl_write() */

/* mgsl_write_room()
 *
 *	Return the count of free bytes in transmit buffer
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static int mgsl_write_room(struct tty_struct *tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	int	ret;
				
	if (mgsl_paranoia_check(info, tty->device, "mgsl_write_room"))
		return 0;
	ret = SERIAL_XMIT_SIZE - info->xmit_cnt - 1;
	if (ret < 0)
		ret = 0;
		
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_write_room(%s)=%d\n",
			 __FILE__,__LINE__, info->device_name,ret );
			 
	if ( info->params.mode == MGSL_MODE_HDLC ) {
		/* operating in synchronous (frame oriented) mode */
		if ( info->tx_active )
			return 0;
		else
			return HDLC_MAX_FRAME_SIZE;
	}
	
	return ret;
	
}	/* end of mgsl_write_room() */

/* mgsl_chars_in_buffer()
 *
 *	Return the count of bytes in transmit buffer
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static int mgsl_chars_in_buffer(struct tty_struct *tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
			 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_chars_in_buffer(%s)\n",
			 __FILE__,__LINE__, info->device_name );
			 
	if (mgsl_paranoia_check(info, tty->device, "mgsl_chars_in_buffer"))
		return 0;
		
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_chars_in_buffer(%s)=%d\n",
			 __FILE__,__LINE__, info->device_name,info->xmit_cnt );
			 
	if ( info->params.mode == MGSL_MODE_HDLC ) {
		/* operating in synchronous (frame oriented) mode */
		if ( info->tx_active )
			return info->tx_buffer_list[0].rcc;
		else
			return 0;
	}
			 
	return info->xmit_cnt;
}	/* end of mgsl_chars_in_buffer() */

/* mgsl_flush_buffer()
 *
 *	Discard all data in the send buffer
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static void mgsl_flush_buffer(struct tty_struct *tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_flush_buffer(%s) entry\n",
			 __FILE__,__LINE__, info->device_name );
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_flush_buffer"))
		return;
		
	spin_lock_irqsave(&info->irq_spinlock,flags); 
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	del_timer(&info->tx_timer);	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
		
}	/* end of mgsl_flush_buffer() */

/* mgsl_send_xchar()
 *
 *	Send a high-priority XON/XOFF character
 * 	
 * Arguments:		tty	pointer to tty info structure
 *			ch	character to send
 * Return Value:	None
 */
static void mgsl_send_xchar(struct tty_struct *tty, char ch)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_send_xchar(%s,%d)\n",
			 __FILE__,__LINE__, info->device_name, ch );
			 
	if (mgsl_paranoia_check(info, tty->device, "mgsl_send_xchar"))
		return;

	info->x_char = ch;
	if (ch) {
		/* Make sure transmit interrupts are on */
		spin_lock_irqsave(&info->irq_spinlock,flags);
		if (!info->tx_enabled)
		 	usc_start_transmitter(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
	}
}	/* end of mgsl_send_xchar() */

/* mgsl_throttle()
 * 
 * 	Signal remote device to throttle send data (our receive data)
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static void mgsl_throttle(struct tty_struct * tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_throttle(%s) entry\n",
			 __FILE__,__LINE__, info->device_name );

	if (mgsl_paranoia_check(info, tty->device, "mgsl_throttle"))
		return;
	
	if (I_IXOFF(tty))
		mgsl_send_xchar(tty, STOP_CHAR(tty));
 
 	if (tty->termios->c_cflag & CRTSCTS) {
		spin_lock_irqsave(&info->irq_spinlock,flags);
		info->serial_signals &= ~SerialSignal_RTS;
	 	usc_set_serial_signals(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
	}
}	/* end of mgsl_throttle() */

/* mgsl_unthrottle()
 * 
 * 	Signal remote device to stop throttling send data (our receive data)
 * 	
 * Arguments:		tty	pointer to tty info structure
 * Return Value:	None
 */
static void mgsl_unthrottle(struct tty_struct * tty)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_unthrottle(%s) entry\n",
			 __FILE__,__LINE__, info->device_name );

	if (mgsl_paranoia_check(info, tty->device, "mgsl_unthrottle"))
		return;
	
	if (I_IXOFF(tty)) {
		if (info->x_char)
			info->x_char = 0;
		else
			mgsl_send_xchar(tty, START_CHAR(tty));
	}
	
 	if (tty->termios->c_cflag & CRTSCTS) {
		spin_lock_irqsave(&info->irq_spinlock,flags);
		info->serial_signals |= SerialSignal_RTS;
	 	usc_set_serial_signals(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
	}
	
}	/* end of mgsl_unthrottle() */

/* mgsl_get_stats()
 * 
 * 	get the current serial parameters information
 *
 * Arguments:	info		pointer to device instance data
 * 		user_icount	pointer to buffer to hold returned stats
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_get_stats(struct mgsl_struct * info, struct mgsl_icount *user_icount)
{
	int err;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_get_params(%s)\n",
			 __FILE__,__LINE__, info->device_name);
			
	COPY_TO_USER(err,user_icount, &info->icount, sizeof(struct mgsl_icount));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):mgsl_get_stats(%s) user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}
	
	return 0;
	
}	/* end of mgsl_get_stats() */

/* mgsl_get_params()
 * 
 * 	get the current serial parameters information
 *
 * Arguments:	info		pointer to device instance data
 * 		user_params	pointer to buffer to hold returned params
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_get_params(struct mgsl_struct * info, MGSL_PARAMS *user_params)
{
	int err;
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_get_params(%s)\n",
			 __FILE__,__LINE__, info->device_name);
			
	COPY_TO_USER(err,user_params, &info->params, sizeof(MGSL_PARAMS));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):mgsl_get_params(%s) user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}
	
	return 0;
	
}	/* end of mgsl_get_params() */

/* mgsl_set_params()
 * 
 * 	set the serial parameters
 * 	
 * Arguments:
 * 
 * 	info		pointer to device instance data
 * 	new_params	user buffer containing new serial params
 *
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_set_params(struct mgsl_struct * info, MGSL_PARAMS *new_params)
{
 	unsigned long flags;
	MGSL_PARAMS tmp_params;
	int err;
 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_set_params %s\n", __FILE__,__LINE__,
			info->device_name );
	COPY_FROM_USER(err,&tmp_params, new_params, sizeof(MGSL_PARAMS));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):mgsl_set_params(%s) user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}
	
	spin_lock_irqsave(&info->irq_spinlock,flags);
	memcpy(&info->params,&tmp_params,sizeof(MGSL_PARAMS));
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
 	mgsl_change_params(info);
	
	return 0;
	
}	/* end of mgsl_set_params() */

/* mgsl_get_txidle()
 * 
 * 	get the current transmit idle mode
 *
 * Arguments:	info		pointer to device instance data
 * 		idle_mode	pointer to buffer to hold returned idle mode
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_get_txidle(struct mgsl_struct * info, int*idle_mode)
{
	int err;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_get_txidle(%s)=%d\n",
			 __FILE__,__LINE__, info->device_name, info->idle_mode);
			
	COPY_TO_USER(err,idle_mode, &info->idle_mode, sizeof(int));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):mgsl_get_txidle(%s) user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}
	
	return 0;
	
}	/* end of mgsl_get_txidle() */

/* mgsl_set_txidle()	service ioctl to set transmit idle mode
 * 	
 * Arguments:	 	info		pointer to device instance data
 * 			idle_mode	new idle mode
 *
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_set_txidle(struct mgsl_struct * info, int idle_mode)
{
 	unsigned long flags;
 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_set_txidle(%s,%d)\n", __FILE__,__LINE__,
			info->device_name, idle_mode );
			
	spin_lock_irqsave(&info->irq_spinlock,flags);
	info->idle_mode = idle_mode;
	usc_set_txidle( info );
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	return 0;
	
}	/* end of mgsl_set_txidle() */

/* mgsl_txenable()
 * 
 * 	enable or disable the transmitter
 * 	
 * Arguments:
 * 
 * 	info		pointer to device instance data
 * 	enable		1 = enable, 0 = disable
 *
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_txenable(struct mgsl_struct * info, int enable)
{
 	unsigned long flags;
 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_txenable(%s,%d)\n", __FILE__,__LINE__,
			info->device_name, enable);
			
	spin_lock_irqsave(&info->irq_spinlock,flags);
	if ( enable ) {
		if ( !info->tx_enabled ) {

			usc_start_transmitter(info);
			/*--------------------------------------------------
			 * if HDLC/SDLC Loop mode, attempt to insert the
			 * station in the 'loop' by setting CMR:13. Upon
			 * receipt of the next GoAhead (RxAbort) sequence,
			 * the OnLoop indicator (CCSR:7) should go active
			 * to indicate that we are on the loop
			 *--------------------------------------------------*/
			if ( info->params.flags & HDLC_FLAG_HDLC_LOOPMODE )
				usc_loopmode_insert_request( info );
		}
	} else {
		if ( info->tx_enabled )
			usc_stop_transmitter(info);
	}
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	return 0;
	
}	/* end of mgsl_txenable() */

/* mgsl_txabort()	abort send HDLC frame
 * 	
 * Arguments:	 	info		pointer to device instance data
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_txabort(struct mgsl_struct * info)
{
 	unsigned long flags;
 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_txabort(%s)\n", __FILE__,__LINE__,
			info->device_name);
			
	spin_lock_irqsave(&info->irq_spinlock,flags);
	if ( info->tx_active && info->params.mode == MGSL_MODE_HDLC )
	{
		if ( info->params.flags & HDLC_FLAG_HDLC_LOOPMODE )
			usc_loopmode_cancel_transmit( info );
		else
			usc_TCmd(info,TCmd_SendAbort);
	}
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	return 0;
	
}	/* end of mgsl_txabort() */

/* mgsl_rxenable() 	enable or disable the receiver
 * 	
 * Arguments:	 	info		pointer to device instance data
 * 			enable		1 = enable, 0 = disable
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_rxenable(struct mgsl_struct * info, int enable)
{
 	unsigned long flags;
 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_rxenable(%s,%d)\n", __FILE__,__LINE__,
			info->device_name, enable);
			
	spin_lock_irqsave(&info->irq_spinlock,flags);
	if ( enable ) {
		if ( !info->rx_enabled )
			usc_start_receiver(info);
	} else {
		if ( info->rx_enabled )
			usc_stop_receiver(info);
	}
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	return 0;
	
}	/* end of mgsl_rxenable() */

/* mgsl_wait_event() 	wait for specified event to occur
 * 	
 * Arguments:	 	info	pointer to device instance data
 * 			mask	pointer to bitmask of events to wait for
 * Return Value:	0 	if successful and bit mask updated with
 *				of events triggerred,
 * 			otherwise error code
 */
static int mgsl_wait_event(struct mgsl_struct * info, int * mask_ptr)
{
 	unsigned long flags;
	int s;
	int rc=0;
	u16 regval;
	struct mgsl_icount cprev, cnow;
	int events = 0;
	int mask;
	struct	_input_signal_events signal_events_prev, signal_events_now;

	COPY_FROM_USER(rc,&mask, mask_ptr, sizeof(int));
	if (rc) {
		return  -EFAULT;
	}
		 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_wait_event(%s,%d)\n", __FILE__,__LINE__,
			info->device_name, mask);

	spin_lock_irqsave(&info->irq_spinlock,flags);

	usc_get_serial_signals(info);
	s = info->serial_signals;

	/* note the counters on entry */
	cprev = info->icount;
	signal_events_prev = info->input_signal_events;
	
	if (mask & MgslEvent_ExitHuntMode) {
		/* enable exit hunt mode IRQ */
		regval = usc_InReg(info,RICR);
		if (!(regval & RXSTATUS_EXITED_HUNT))
			usc_OutReg(info, RICR, regval | RXSTATUS_EXITED_HUNT);
	}
	
	if (mask & MgslEvent_IdleReceived) {
		/* enable idle mode received IRQ */
		regval = usc_InReg(info,RICR);
		if (!(regval & RXSTATUS_IDLE_RECEIVED))
			usc_OutReg(info, RICR, regval | RXSTATUS_IDLE_RECEIVED);
	}
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	/* Determine if any user requested events for input signals is currently TRUE */
	
	events |= (mask & ((s & SerialSignal_DSR) ?
			MgslEvent_DsrActive:MgslEvent_DsrInactive));

	events |= (mask & ((s & SerialSignal_DCD) ?
			MgslEvent_DcdActive:MgslEvent_DcdInactive));
		
	events |= (mask & ((s & SerialSignal_CTS) ?
			MgslEvent_CtsActive:MgslEvent_CtsInactive));
		
	events |= (mask & ((s & SerialSignal_RI) ?
			MgslEvent_RiActive:MgslEvent_RiInactive));
	

	while(!events) {
		/* sleep until event occurs */
		interruptible_sleep_on(&info->event_wait_q);
		
		/* see if a signal woke us */
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}
			
		spin_lock_irqsave(&info->irq_spinlock,flags);

		/* get icount and serial signal states */
		cnow = info->icount;
		signal_events_now = info->input_signal_events;
		spin_unlock_irqrestore(&info->irq_spinlock,flags);

		if (signal_events_now.dsr_up != signal_events_prev.dsr_up && 
				mask & MgslEvent_DsrActive )
			events |= MgslEvent_DsrActive;
		
		if (signal_events_now.dsr_down != signal_events_prev.dsr_down && 
				mask & MgslEvent_DsrInactive )
			events |= MgslEvent_DsrInactive;

		if (signal_events_now.dcd_up != signal_events_prev.dcd_up &&
				mask & MgslEvent_DcdActive )
			events |= MgslEvent_DcdActive;
		
		if (signal_events_now.dcd_down != signal_events_prev.dcd_down &&
				mask & MgslEvent_DcdInactive )
			events |= MgslEvent_DcdInactive;
		
		if (signal_events_now.cts_up != signal_events_prev.cts_up &&
				mask & MgslEvent_CtsActive )
			events |= MgslEvent_CtsActive;
		
		if (signal_events_now.cts_down != signal_events_prev.cts_down &&
				mask & MgslEvent_CtsInactive )
			events |= MgslEvent_CtsInactive;
		
		if (signal_events_now.ri_up != signal_events_prev.ri_up &&
				mask & MgslEvent_RiActive )
			events |= MgslEvent_RiActive;
		
		if (signal_events_now.ri_down != signal_events_prev.ri_down &&
				mask & MgslEvent_RiInactive )
			events |= MgslEvent_RiInactive;
		
		if (cnow.exithunt != cprev.exithunt)
			events |= (mask & MgslEvent_ExitHuntMode);

		if (cnow.rxidle != cprev.rxidle)
			events |= (mask & MgslEvent_IdleReceived);
		
		cprev = cnow;
		signal_events_prev = signal_events_now;
	}
	
	if (mask & (MgslEvent_ExitHuntMode + MgslEvent_IdleReceived)) {
		spin_lock_irqsave(&info->irq_spinlock,flags);
		if (!waitqueue_active(&info->event_wait_q)) {
			/* disable enable exit hunt mode/idle rcvd IRQs */
			regval = usc_InReg(info,RICR);
			usc_OutReg(info, RICR, regval & 
				~(RXSTATUS_EXITED_HUNT + RXSTATUS_IDLE_RECEIVED));
		}
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
	}

	if ( rc == 0 )
		PUT_USER(rc, events, mask_ptr);
		
	return rc;
	
}	/* end of mgsl_wait_event() */

/* get_modem_info()
 * 
 * 	Read the state of the serial control and
 * 	status signals and return to caller.
 * 	
 * Arguments:	 	info	pointer to device instance data
 * 			value	pointer to int to hold returned info
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int get_modem_info(struct mgsl_struct * info, unsigned int *value)
{
	unsigned int result = 0;
 	unsigned long flags;
	int err;
 
	spin_lock_irqsave(&info->irq_spinlock,flags);
 	usc_get_serial_signals(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

	if (info->serial_signals & SerialSignal_RTS)
		result |= TIOCM_RTS;
	if (info->serial_signals & SerialSignal_DTR)
		result |= TIOCM_DTR;
	if (info->serial_signals & SerialSignal_DCD)
		result |= TIOCM_CAR;
	if (info->serial_signals & SerialSignal_RI)
		result |= TIOCM_RNG;
	if (info->serial_signals & SerialSignal_DSR)
		result |= TIOCM_DSR;
	if (info->serial_signals & SerialSignal_CTS)
		result |= TIOCM_CTS;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_get_modem_info %s value=%08X\n",
			 __FILE__,__LINE__, info->device_name, result );
			
	PUT_USER(err,result,value);
	return err;
}	/* end of get_modem_info() */

/* set_modem_info()
 * 
 * 	Set the state of the modem control signals (DTR/RTS)
 * 	
 * Arguments:
 * 
 * 	info	pointer to device instance data
 * 	cmd	signal command: TIOCMBIS = set bit TIOCMBIC = clear bit
 *		TIOCMSET = set/clear signal values
 * 	value	bit mask for command
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int set_modem_info(struct mgsl_struct * info, unsigned int cmd,
			  unsigned int *value)
{
 	int error;
 	unsigned int arg;
 	unsigned long flags;
 
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_set_modem_info %s\n", __FILE__,__LINE__,
			info->device_name );
			
 	GET_USER(error,arg,value);
 	if (error)
 		return error;
		
 	switch (cmd) {
 	case TIOCMBIS: 
 		if (arg & TIOCM_RTS)
 			info->serial_signals |= SerialSignal_RTS;
 		if (arg & TIOCM_DTR)
 			info->serial_signals |= SerialSignal_DTR;
 		break;
 	case TIOCMBIC:
 		if (arg & TIOCM_RTS)
 			info->serial_signals &= ~SerialSignal_RTS;
 		if (arg & TIOCM_DTR)
 			info->serial_signals &= ~SerialSignal_DTR;
 		break;
 	case TIOCMSET:
 		if (arg & TIOCM_RTS)
 			info->serial_signals |= SerialSignal_RTS;
		else
 			info->serial_signals &= ~SerialSignal_RTS;
		
 		if (arg & TIOCM_DTR)
 			info->serial_signals |= SerialSignal_DTR;
		else
 			info->serial_signals &= ~SerialSignal_DTR;
 		break;
 	default:
 		return -EINVAL;
 	}
	
	spin_lock_irqsave(&info->irq_spinlock,flags);
 	usc_set_serial_signals(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	return 0;
	
}	/* end of set_modem_info() */

#if LINUX_VERSION_CODE >= VERSION(2,1,0)
/* mgsl_break()		Set or clear transmit break condition
 *
 * Arguments:		tty		pointer to tty instance data
 *			break_state	-1=set break condition, 0=clear
 * Return Value:	None
 */
static void mgsl_break(struct tty_struct *tty, int break_state)
{
	struct mgsl_struct * info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_break(%s,%d)\n",
			 __FILE__,__LINE__, info->device_name, break_state);
			 
	if (mgsl_paranoia_check(info, tty->device, "mgsl_break"))
		return;

	spin_lock_irqsave(&info->irq_spinlock,flags);
 	if (break_state == -1)
		usc_OutReg(info,IOCR,(u16)(usc_InReg(info,IOCR) | BIT7));
	else 
		usc_OutReg(info,IOCR,(u16)(usc_InReg(info,IOCR) & ~BIT7));
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
}	/* end of mgsl_break() */
#endif

/* mgsl_ioctl()	Service an IOCTL request
 * 	
 * Arguments:
 * 
 * 	tty	pointer to tty instance data
 * 	file	pointer to associated file object for device
 * 	cmd	IOCTL command code
 * 	arg	command argument/context
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_ioctl(struct tty_struct *tty, struct file * file,
		    unsigned int cmd, unsigned long arg)
{
	int error;
	struct mgsl_struct * info = (struct mgsl_struct *)tty->driver_data;
	struct mgsl_icount cprev, cnow;	/* kernel counter temps */
	struct serial_icounter_struct *p_cuser;	/* user space */
	unsigned long flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_ioctl %s cmd=%08X\n", __FILE__,__LINE__,
			info->device_name, cmd );
	
	if (mgsl_paranoia_check(info, tty->device, "mgsl_ioctl"))
		return -ENODEV;

	if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
	    (cmd != TIOCMIWAIT) && (cmd != TIOCGICOUNT)) {
		if (tty->flags & (1 << TTY_IO_ERROR))
		    return -EIO;
	}
	
	switch (cmd) {
		case TIOCMGET:
			return get_modem_info(info, (unsigned int *) arg);
		case TIOCMBIS:
		case TIOCMBIC:
		case TIOCMSET:
			return set_modem_info(info, cmd, (unsigned int *) arg);
		case MGSL_IOCGPARAMS:
			return mgsl_get_params(info,(MGSL_PARAMS *)arg);
		case MGSL_IOCSPARAMS:
			return mgsl_set_params(info,(MGSL_PARAMS *)arg);
		case MGSL_IOCGTXIDLE:
			return mgsl_get_txidle(info,(int*)arg);
		case MGSL_IOCSTXIDLE:
			return mgsl_set_txidle(info,(int)arg);
		case MGSL_IOCTXENABLE:
			return mgsl_txenable(info,(int)arg);
		case MGSL_IOCRXENABLE:
			return mgsl_rxenable(info,(int)arg);
		case MGSL_IOCTXABORT:
			return mgsl_txabort(info);
		case MGSL_IOCGSTATS:
			return mgsl_get_stats(info,(struct mgsl_icount*)arg);
		case MGSL_IOCWAITEVENT:
			return mgsl_wait_event(info,(int*)arg);
		case MGSL_IOCLOOPTXDONE:
			return mgsl_loopmode_send_done(info);
		case MGSL_IOCCLRMODCOUNT:
			while(MOD_IN_USE)
				MOD_DEC_USE_COUNT;
			return 0;
		/*
		 * Wait for any of the 4 modem inputs (DCD,RI,DSR,CTS) to change
		 * - mask passed in arg for lines of interest
 		 *   (use |'ed TIOCM_RNG/DSR/CD/CTS for masking)
		 * Caller should use TIOCGICOUNT to see which one it was
		 */
		case TIOCMIWAIT:
			spin_lock_irqsave(&info->irq_spinlock,flags);
			/* note the counters on entry */
			cprev = info->icount;
			spin_unlock_irqrestore(&info->irq_spinlock,flags);
			while (1) {
				interruptible_sleep_on(&info->status_event_wait_q);
				/* see if a signal did it */
				if (signal_pending(current))
					return -ERESTARTSYS;
				save_flags(flags); cli();
				cnow = info->icount; /* atomic copy */
				restore_flags(flags);
				if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr && 
				    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
					return -EIO; /* no change => error */
				if ( ((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
				     ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
				     ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
				     ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
					return 0;
				}
				cprev = cnow;
			}
			/* NOTREACHED */

		/* 
		 * Get counter of input serial line interrupts (DCD,RI,DSR,CTS)
		 * Return: write counters to the user passed counter struct
		 * NB: both 1->0 and 0->1 transitions are counted except for
		 *     RI where only 0->1 is counted.
		 */
		case TIOCGICOUNT:
			spin_lock_irqsave(&info->irq_spinlock,flags);
			cnow = info->icount;
			spin_unlock_irqrestore(&info->irq_spinlock,flags);
			p_cuser = (struct serial_icounter_struct *) arg;
			PUT_USER(error,cnow.cts, &p_cuser->cts);
			if (error) return error;
			PUT_USER(error,cnow.dsr, &p_cuser->dsr);
			if (error) return error;
			PUT_USER(error,cnow.rng, &p_cuser->rng);
			if (error) return error;
			PUT_USER(error,cnow.dcd, &p_cuser->dcd);
			if (error) return error;
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
			PUT_USER(error,cnow.rx, &p_cuser->rx);
			if (error) return error;
			PUT_USER(error,cnow.tx, &p_cuser->tx);
			if (error) return error;
			PUT_USER(error,cnow.frame, &p_cuser->frame);
			if (error) return error;
			PUT_USER(error,cnow.overrun, &p_cuser->overrun);
			if (error) return error;
			PUT_USER(error,cnow.parity, &p_cuser->parity);
			if (error) return error;
			PUT_USER(error,cnow.brk, &p_cuser->brk);
			if (error) return error;
			PUT_USER(error,cnow.buf_overrun, &p_cuser->buf_overrun);
			if (error) return error;
#endif			
			return 0;

		default:
			return -ENOIOCTLCMD;
		}
	return 0;
}

/* mgsl_set_termios()
 * 
 * 	Set new termios settings
 * 	
 * Arguments:
 * 
 * 	tty		pointer to tty structure
 * 	termios		pointer to buffer to hold returned old termios
 * 	
 * Return Value:		None
 */
static void mgsl_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	struct mgsl_struct *info = (struct mgsl_struct *)tty->driver_data;
	unsigned long flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_set_termios %s\n", __FILE__,__LINE__,
			tty->driver.name );
	
	/* just return if nothing has changed */
	if ((tty->termios->c_cflag == old_termios->c_cflag)
	    && (RELEVANT_IFLAG(tty->termios->c_iflag) 
		== RELEVANT_IFLAG(old_termios->c_iflag)))
	  return;

	mgsl_change_params(info);

	/* Handle transition to B0 status */
	if (old_termios->c_cflag & CBAUD &&
	    !(tty->termios->c_cflag & CBAUD)) {
		info->serial_signals &= ~(SerialSignal_RTS + SerialSignal_DTR);
		spin_lock_irqsave(&info->irq_spinlock,flags);
	 	usc_set_serial_signals(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
	}
	
	/* Handle transition away from B0 status */
	if (!(old_termios->c_cflag & CBAUD) &&
	    tty->termios->c_cflag & CBAUD) {
		info->serial_signals |= SerialSignal_DTR;
 		if (!(tty->termios->c_cflag & CRTSCTS) || 
 		    !test_bit(TTY_THROTTLED, &tty->flags)) {
			info->serial_signals |= SerialSignal_RTS;
 		}
		spin_lock_irqsave(&info->irq_spinlock,flags);
	 	usc_set_serial_signals(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
	}
	
	/* Handle turning off CRTSCTS */
	if (old_termios->c_cflag & CRTSCTS &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		mgsl_start(tty);
	}

}	/* end of mgsl_set_termios() */

/* mgsl_close()
 * 
 * 	Called when port is closed. Wait for remaining data to be
 * 	sent. Disable port and free resources.
 * 	
 * Arguments:
 * 
 * 	tty	pointer to open tty structure
 * 	filp	pointer to open file object
 * 	
 * Return Value:	None
 */
static void mgsl_close(struct tty_struct *tty, struct file * filp)
{
	struct mgsl_struct * info = (struct mgsl_struct *)tty->driver_data;

	if (!info || mgsl_paranoia_check(info, tty->device, "mgsl_close"))
		return;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_close(%s) entry, count=%d\n",
			 __FILE__,__LINE__, info->device_name, info->count);
			 
	if (!info->count || tty_hung_up_p(filp))
		goto cleanup;
			
	if ((tty->count == 1) && (info->count != 1)) {
		/*
		 * tty->count is 1 and the tty structure will be freed.
		 * info->count should be one in this case.
		 * if it's not, correct it so that the port is shutdown.
		 */
		printk("mgsl_close: bad refcount; tty->count is 1, "
		       "info->count is %d\n", info->count);
		info->count = 1;
	}
	
	info->count--;
	
	/* if at least one open remaining, leave hardware active */
	if (info->count)
		goto cleanup;
	
	info->flags |= ASYNC_CLOSING;
	
	/* Save the termios structure, since this port may have
	 * separate termios for callout and dialin.
	 */
	if (info->flags & ASYNC_NORMAL_ACTIVE)
		info->normal_termios = *tty->termios;
	if (info->flags & ASYNC_CALLOUT_ACTIVE)
		info->callout_termios = *tty->termios;
		
	/* set tty->closing to notify line discipline to 
	 * only process XON/XOFF characters. Only the N_TTY
	 * discipline appears to use this (ppp does not).
	 */
	tty->closing = 1;
	
	/* wait for transmit data to clear all layers */
	
	if (info->closing_wait != ASYNC_CLOSING_WAIT_NONE) {
		if (debug_level >= DEBUG_LEVEL_INFO)
			printk("%s(%d):mgsl_close(%s) calling tty_wait_until_sent\n",
				 __FILE__,__LINE__, info->device_name );
		tty_wait_until_sent(tty, info->closing_wait);
	}
		
 	if (info->flags & ASYNC_INITIALIZED)
 		mgsl_wait_until_sent(tty, info->timeout);

	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
		
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
		
	shutdown(info);
	
	tty->closing = 0;
	info->tty = 0;
	
	if (info->blocked_open) {
		if (info->close_delay) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(info->close_delay);
		}
		wake_up_interruptible(&info->open_wait);
	}
	
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);
			 
	wake_up_interruptible(&info->close_wait);
	
cleanup:			
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_close(%s) exit, count=%d\n", __FILE__,__LINE__,
			tty->driver.name, info->count);
	if(MOD_IN_USE)
		MOD_DEC_USE_COUNT;
			
}	/* end of mgsl_close() */

/* mgsl_wait_until_sent()
 *
 *	Wait until the transmitter is empty.
 *
 * Arguments:
 *
 *	tty		pointer to tty info structure
 *	timeout		time to wait for send completion
 *
 * Return Value:	None
 */
static void mgsl_wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct mgsl_struct * info = (struct mgsl_struct *)tty->driver_data;
	unsigned long orig_jiffies, char_time;

	if (!info )
		return;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_wait_until_sent(%s) entry\n",
			 __FILE__,__LINE__, info->device_name );
      
	if (mgsl_paranoia_check(info, tty->device, "mgsl_wait_until_sent"))
		return;

	if (!(info->flags & ASYNC_INITIALIZED))
		goto exit;
	 
	orig_jiffies = jiffies;
      
	/* Set check interval to 1/5 of estimated time to
	 * send a character, and make it at least 1. The check
	 * interval should also be less than the timeout.
	 * Note: use tight timings here to satisfy the NIST-PCTS.
	 */ 
       
	if ( info->params.data_rate ) {
	       	char_time = info->timeout/(32 * 5);
		if (!char_time)
			char_time++;
	} else
		char_time = 1;
		
	if (timeout)
		char_time = MIN(char_time, timeout);
		
	if ( info->params.mode == MGSL_MODE_HDLC ) {
		while (info->tx_active) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(char_time);
			if (signal_pending(current))
				break;
			if (timeout && ((orig_jiffies + timeout) < jiffies))
				break;
		}
	} else {
		while (!(usc_InReg(info,TCSR) & TXSTATUS_ALL_SENT) &&
			info->tx_enabled) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(char_time);
			if (signal_pending(current))
				break;
			if (timeout && ((orig_jiffies + timeout) < jiffies))
				break;
		}
	}
      
	set_current_state(TASK_RUNNING);
exit:
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_wait_until_sent(%s) exit\n",
			 __FILE__,__LINE__, info->device_name );
			 
}	/* end of mgsl_wait_until_sent() */

/* mgsl_hangup()
 *
 *	Called by tty_hangup() when a hangup is signaled.
 *	This is the same as to closing all open files for the port.
 *
 * Arguments:		tty	pointer to associated tty object
 * Return Value:	None
 */
static void mgsl_hangup(struct tty_struct *tty)
{
	struct mgsl_struct * info = (struct mgsl_struct *)tty->driver_data;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_hangup(%s)\n",
			 __FILE__,__LINE__, info->device_name );
			 
	if (mgsl_paranoia_check(info, tty->device, "mgsl_hangup"))
		return;

	mgsl_flush_buffer(tty);
	shutdown(info);
	
	info->count = 0;	
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	info->tty = 0;

	wake_up_interruptible(&info->open_wait);
	
}	/* end of mgsl_hangup() */

/* block_til_ready()
 * 
 * 	Block the current process until the specified port
 * 	is ready to be opened.
 * 	
 * Arguments:
 * 
 * 	tty		pointer to tty info structure
 * 	filp		pointer to open file object
 * 	info		pointer to device instance data
 * 	
 * Return Value:	0 if success, otherwise error code
 */
static int block_til_ready(struct tty_struct *tty, struct file * filp,
			   struct mgsl_struct *info)
{
	DECLARE_WAITQUEUE(wait, current);
	int		retval;
	int		do_clocal = 0, extra_count = 0;
	unsigned long	flags;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):block_til_ready on %s\n",
			 __FILE__,__LINE__, tty->driver.name );

	if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
		/* this is a callout device */
		/* just verify that normal device is not in use */
		if (info->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_SESSION_LOCKOUT) &&
		    (info->session != current->session))
		    return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_PGRP_LOCKOUT) &&
		    (info->pgrp != current->pgrp))
		    return -EBUSY;
		info->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}
	
	if (filp->f_flags & O_NONBLOCK || tty->flags & (1 << TTY_IO_ERROR)){
		/* nonblock mode is set or port is not enabled */
		/* just verify that callout device is not active */
		if (info->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		info->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	if (info->flags & ASYNC_CALLOUT_ACTIVE) {
		if (info->normal_termios.c_cflag & CLOCAL)
			do_clocal = 1;
	} else {
		if (tty->termios->c_cflag & CLOCAL)
			do_clocal = 1;
	}
	
	/* Wait for carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, info->count is dropped by one, so that
	 * mgsl_close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */
	 
	retval = 0;
	add_wait_queue(&info->open_wait, &wait);
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):block_til_ready before block on %s count=%d\n",
			 __FILE__,__LINE__, tty->driver.name, info->count );

	save_flags(flags); cli();
	if (!tty_hung_up_p(filp)) {
		extra_count = 1;
		info->count--;
	}
	restore_flags(flags);
	info->blocked_open++;
	
	while (1) {
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
 		    (tty->termios->c_cflag & CBAUD)) {
			spin_lock_irqsave(&info->irq_spinlock,flags);
			info->serial_signals |= SerialSignal_RTS + SerialSignal_DTR;
		 	usc_set_serial_signals(info);
			spin_unlock_irqrestore(&info->irq_spinlock,flags);
		}
		
		set_current_state(TASK_INTERRUPTIBLE);
		
		if (tty_hung_up_p(filp) || !(info->flags & ASYNC_INITIALIZED)){
			retval = (info->flags & ASYNC_HUP_NOTIFY) ?
					-EAGAIN : -ERESTARTSYS;
			break;
		}
		
		spin_lock_irqsave(&info->irq_spinlock,flags);
	 	usc_get_serial_signals(info);
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
		
 		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
 		    !(info->flags & ASYNC_CLOSING) &&
 		    (do_clocal || (info->serial_signals & SerialSignal_DCD)) ) {
 			break;
		}
			
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}
		
		if (debug_level >= DEBUG_LEVEL_INFO)
			printk("%s(%d):block_til_ready blocking on %s count=%d\n",
				 __FILE__,__LINE__, tty->driver.name, info->count );
				 
		schedule();
	}
	
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&info->open_wait, &wait);
	
	if (extra_count)
		info->count++;
	info->blocked_open--;
	
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):block_til_ready after blocking on %s count=%d\n",
			 __FILE__,__LINE__, tty->driver.name, info->count );
			 
	if (!retval)
		info->flags |= ASYNC_NORMAL_ACTIVE;
		
	return retval;
	
}	/* end of block_til_ready() */

/* mgsl_open()
 *
 *	Called when a port is opened.  Init and enable port.
 *	Perform serial-specific initialization for the tty structure.
 *
 * Arguments:		tty	pointer to tty info structure
 *			filp	associated file pointer
 *
 * Return Value:	0 if success, otherwise error code
 */
static int mgsl_open(struct tty_struct *tty, struct file * filp)
{
	struct mgsl_struct	*info;
	int 			retval, line;
	unsigned long		page;

	/* verify range of specified line number */	
	line = MINOR(tty->device) - tty->driver.minor_start;
	if ((line < 0) || (line >= mgsl_device_count)) {
		printk("%s(%d):mgsl_open with illegal line #%d.\n",
			__FILE__,__LINE__,line);
		return -ENODEV;
	}

	/* find the info structure for the specified line */
	info = mgsl_device_list;
	while(info && info->line != line)
		info = info->next_device;
	if ( !info ){
		printk("%s(%d):Can't find specified device on open (line=%d)\n",
			__FILE__,__LINE__,line);
		return -ENODEV;
	}
	
	tty->driver_data = info;
	info->tty = tty;
	if (mgsl_paranoia_check(info, tty->device, "mgsl_open"))
		return -ENODEV;
		
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_open(%s), old ref count = %d\n",
			 __FILE__,__LINE__,tty->driver.name, info->count);

	MOD_INC_USE_COUNT;
	
	/* If port is closing, signal caller to try again */
	if (tty_hung_up_p(filp) || info->flags & ASYNC_CLOSING){
		if (info->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&info->close_wait);
		retval = ((info->flags & ASYNC_HUP_NOTIFY) ?
			-EAGAIN : -ERESTARTSYS);
		goto cleanup;
	}
	
	if (!tmp_buf) {
		page = get_free_page(GFP_KERNEL);
		if (!page) {
			retval = -ENOMEM;
			goto cleanup;
		}
		if (tmp_buf)
			free_page(page);
		else
			tmp_buf = (unsigned char *) page;
	}
	
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
	info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;
#endif

	info->count++;
	if (info->count == 1) {
		/* 1st open on this device, init hardware */
		retval = startup(info);
		if (retval < 0)
			goto cleanup;
	}

	retval = block_til_ready(tty, filp, info);
	if (retval) {
		if (debug_level >= DEBUG_LEVEL_INFO)
			printk("%s(%d):block_til_ready(%s) returned %d\n",
				 __FILE__,__LINE__, info->device_name, retval);
		goto cleanup;
	}

	if ((info->count == 1) &&
	    info->flags & ASYNC_SPLIT_TERMIOS) {
		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
			*tty->termios = info->normal_termios;
		else 
			*tty->termios = info->callout_termios;
		mgsl_change_params(info);
	}
	
	info->session = current->session;
	info->pgrp    = current->pgrp;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):mgsl_open(%s) success\n",
			 __FILE__,__LINE__, info->device_name);
	retval = 0;
	
cleanup:			
	if (retval) {
		if(MOD_IN_USE)
			MOD_DEC_USE_COUNT;
		if(info->count)
			info->count--;
	}
	
	return retval;
	
}	/* end of mgsl_open() */

/*
 * /proc fs routines....
 */

static inline int line_info(char *buf, struct mgsl_struct *info)
{
	char	stat_buf[30];
	int	ret;
	unsigned long flags;

	if (info->bus_type == MGSL_BUS_TYPE_PCI) {
		ret = sprintf(buf, "%s:PCI io:%04X irq:%d mem:%08X lcr:%08X",
			info->device_name, info->io_base, info->irq_level,
			info->phys_memory_base, info->phys_lcr_base);
	} else {
		ret = sprintf(buf, "%s:(E)ISA io:%04X irq:%d dma:%d",
			info->device_name, info->io_base, 
			info->irq_level, info->dma_level);
	}

	/* output current serial signal states */
	spin_lock_irqsave(&info->irq_spinlock,flags);
 	usc_get_serial_signals(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	stat_buf[0] = 0;
	stat_buf[1] = 0;
	if (info->serial_signals & SerialSignal_RTS)
		strcat(stat_buf, "|RTS");
	if (info->serial_signals & SerialSignal_CTS)
		strcat(stat_buf, "|CTS");
	if (info->serial_signals & SerialSignal_DTR)
		strcat(stat_buf, "|DTR");
	if (info->serial_signals & SerialSignal_DSR)
		strcat(stat_buf, "|DSR");
	if (info->serial_signals & SerialSignal_DCD)
		strcat(stat_buf, "|CD");
	if (info->serial_signals & SerialSignal_RI)
		strcat(stat_buf, "|RI");

	if (info->params.mode == MGSL_MODE_HDLC) {
		ret += sprintf(buf+ret, " HDLC txok:%d rxok:%d",
			      info->icount.txok, info->icount.rxok);
		if (info->icount.txunder)
			ret += sprintf(buf+ret, " txunder:%d", info->icount.txunder);
		if (info->icount.txabort)
			ret += sprintf(buf+ret, " txabort:%d", info->icount.txabort);
		if (info->icount.rxshort)
			ret += sprintf(buf+ret, " rxshort:%d", info->icount.rxshort);	
		if (info->icount.rxlong)
			ret += sprintf(buf+ret, " rxlong:%d", info->icount.rxlong);
		if (info->icount.rxover)
			ret += sprintf(buf+ret, " rxover:%d", info->icount.rxover);
		if (info->icount.rxcrc)
			ret += sprintf(buf+ret, " rxlong:%d", info->icount.rxcrc);
	} else {
		ret += sprintf(buf+ret, " ASYNC tx:%d rx:%d",
			      info->icount.tx, info->icount.rx);
		if (info->icount.frame)
			ret += sprintf(buf+ret, " fe:%d", info->icount.frame);
		if (info->icount.parity)
			ret += sprintf(buf+ret, " pe:%d", info->icount.parity);
		if (info->icount.brk)
			ret += sprintf(buf+ret, " brk:%d", info->icount.brk);	
		if (info->icount.overrun)
			ret += sprintf(buf+ret, " oe:%d", info->icount.overrun);
	}
	
	/* Append serial signal status to end */
	ret += sprintf(buf+ret, " %s\n", stat_buf+1);
	
	ret += sprintf(buf+ret, "txactive=%d bh_req=%d bh_run=%d bh_q=%p\n",
	 info->tx_active,info->bh_requested,info->bh_running,
	 info->bh_queue_head);
	 
	spin_lock_irqsave(&info->irq_spinlock,flags);
	{	
	u16 Tcsr = usc_InReg( info, TCSR );
	u16 Tdmr = usc_InDmaReg( info, TDMR );
	u16 Ticr = usc_InReg( info, TICR );
	u16 Rscr = usc_InReg( info, RCSR );
	u16 Rdmr = usc_InDmaReg( info, RDMR );
	u16 Ricr = usc_InReg( info, RICR );
	u16 Icr = usc_InReg( info, ICR );
	u16 Dccr = usc_InReg( info, DCCR );
	u16 Tmr = usc_InReg( info, TMR );
	u16 Tccr = usc_InReg( info, TCCR );
	u16 Ccar = inw( info->io_base + CCAR );
	ret += sprintf(buf+ret, "tcsr=%04X tdmr=%04X ticr=%04X rcsr=%04X rdmr=%04X\n"
                        "ricr=%04X icr =%04X dccr=%04X tmr=%04X tccr=%04X ccar=%04X\n",
	 		Tcsr,Tdmr,Ticr,Rscr,Rdmr,Ricr,Icr,Dccr,Tmr,Tccr,Ccar );
	}
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	return ret;
	
}	/* end of line_info() */

/* mgsl_read_proc()
 * 
 * Called to print information about devices
 * 
 * Arguments:
 * 	page	page of memory to hold returned info
 * 	start	
 * 	off
 * 	count
 * 	eof
 * 	data
 * 	
 * Return Value:
 */
int mgsl_read_proc(char *page, char **start, off_t off, int count,
		 int *eof, void *data)
{
	int len = 0, l;
	off_t	begin = 0;
	struct mgsl_struct *info;
	
	len += sprintf(page, "synclink driver:%s\n", driver_version);
	
	info = mgsl_device_list;
	while( info ) {
		l = line_info(page + len, info);
		len += l;
		if (len+begin > off+count)
			goto done;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
		info = info->next_device;
	}

	*eof = 1;
done:
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
	
}	/* end of mgsl_read_proc() */

/* mgsl_allocate_dma_buffers()
 * 
 * 	Allocate and format DMA buffers (ISA adapter)
 * 	or format shared memory buffers (PCI adapter).
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	0 if success, otherwise error
 */
int mgsl_allocate_dma_buffers(struct mgsl_struct *info)
{
	unsigned short BuffersPerFrame;

	info->last_mem_alloc = 0;

	/* Calculate the number of DMA buffers necessary to hold the */
	/* largest allowable frame size. Note: If the max frame size is */
	/* not an even multiple of the DMA buffer size then we need to */
	/* round the buffer count per frame up one. */

	BuffersPerFrame = (unsigned short)(info->max_frame_size/DMABUFFERSIZE);
	if ( info->max_frame_size % DMABUFFERSIZE )
		BuffersPerFrame++;

	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		/*
		 * The PCI adapter has 256KBytes of shared memory to use.
		 * This is 64 PAGE_SIZE buffers.
		 *
		 * The first page is used for padding at this time so the
		 * buffer list does not begin at offset 0 of the PCI
		 * adapter's shared memory.
		 *
		 * The 2nd page is used for the buffer list. A 4K buffer
		 * list can hold 128 DMA_BUFFER structures at 32 bytes
		 * each.
		 *
		 * This leaves 62 4K pages.
		 *
		 * The next N pages are used for a transmit frame. We
		 * reserve enough 4K page blocks to hold the configured
		 * MaxFrameSize
		 *
		 * Of the remaining pages (62-N), determine how many can
		 * be used to receive full MaxFrameSize inbound frames
		 */
		
		info->tx_buffer_count = BuffersPerFrame;
		info->rx_buffer_count = 62 - info->tx_buffer_count;
	} else {
		/* Calculate the number of PAGE_SIZE buffers needed for */
		/* receive and transmit DMA buffers. */


		/* Calculate the number of DMA buffers necessary to */
		/* hold 7 max size receive frames and one max size transmit frame. */
		/* The receive buffer count is bumped by one so we avoid an */
		/* End of List condition if all receive buffers are used when */
		/* using linked list DMA buffers. */

		info->tx_buffer_count = BuffersPerFrame;
		info->rx_buffer_count = (BuffersPerFrame * MAXRXFRAMES) + 6;
		
		/* 
		 * limit total TxBuffers & RxBuffers to 62 4K total 
		 * (ala PCI Allocation) 
		 */
		
		if ( (info->tx_buffer_count + info->rx_buffer_count) > 62 )
			info->rx_buffer_count = 62 - info->tx_buffer_count;

	}

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("%s(%d):Allocating %d TX and %d RX DMA buffers.\n",
			__FILE__,__LINE__, info->tx_buffer_count,info->rx_buffer_count);
	
	if ( mgsl_alloc_buffer_list_memory( info ) < 0 ||
		  mgsl_alloc_frame_memory(info, info->rx_buffer_list, info->rx_buffer_count) < 0 || 
		  mgsl_alloc_frame_memory(info, info->tx_buffer_list, info->tx_buffer_count) < 0 || 
		  mgsl_alloc_intermediate_rxbuffer_memory(info) < 0 ) {
		printk("%s(%d):Can't allocate DMA buffer memory\n",__FILE__,__LINE__);
		return -ENOMEM;
	}
	
	mgsl_reset_rx_dma_buffers( info );

	return 0;

}	/* end of mgsl_allocate_dma_buffers() */

/*
 * mgsl_alloc_buffer_list_memory()
 * 
 * Allocate a common DMA buffer for use as the
 * receive and transmit buffer lists.
 * 
 * A buffer list is a set of buffer entries where each entry contains
 * a pointer to an actual buffer and a pointer to the next buffer entry
 * (plus some other info about the buffer).
 * 
 * The buffer entries for a list are built to form a circular list so
 * that when the entire list has been traversed you start back at the
 * beginning.
 * 
 * This function allocates memory for just the buffer entries.
 * The links (pointer to next entry) are filled in with the physical
 * address of the next entry so the adapter can navigate the list
 * using bus master DMA. The pointers to the actual buffers are filled
 * out later when the actual buffers are allocated.
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	0 if success, otherwise error
 */
int mgsl_alloc_buffer_list_memory( struct mgsl_struct *info )
{
	unsigned int i;

	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		/* PCI adapter uses shared memory. */
		info->buffer_list = info->memory_base + info->last_mem_alloc;
		info->buffer_list_phys = info->last_mem_alloc;
		info->last_mem_alloc += BUFFERLISTSIZE;
	} else {
		/* ISA adapter uses system memory. */
		/* The buffer lists are allocated as a common buffer that both */
		/* the processor and adapter can access. This allows the driver to */
		/* inspect portions of the buffer while other portions are being */
		/* updated by the adapter using Bus Master DMA. */

		info->buffer_list = kmalloc(BUFFERLISTSIZE, GFP_KERNEL | GFP_DMA);
		if ( info->buffer_list == NULL )
			return -ENOMEM;
			
		info->buffer_list_phys = virt_to_bus(info->buffer_list);
	}

	/* We got the memory for the buffer entry lists. */
	/* Initialize the memory block to all zeros. */
	memset( info->buffer_list, 0, BUFFERLISTSIZE );

	/* Save virtual address pointers to the receive and */
	/* transmit buffer lists. (Receive 1st). These pointers will */
	/* be used by the processor to access the lists. */
	info->rx_buffer_list = (DMABUFFERENTRY *)info->buffer_list;
	info->tx_buffer_list = (DMABUFFERENTRY *)info->buffer_list;
	info->tx_buffer_list += info->rx_buffer_count;

	/*
	 * Build the links for the buffer entry lists such that
	 * two circular lists are built. (Transmit and Receive).
	 *
	 * Note: the links are physical addresses
	 * which are read by the adapter to determine the next
	 * buffer entry to use.
	 */

	for ( i = 0; i < info->rx_buffer_count; i++ ) {
		/* calculate and store physical address of this buffer entry */
		info->rx_buffer_list[i].phys_entry =
			info->buffer_list_phys + (i * sizeof(DMABUFFERENTRY));

		/* calculate and store physical address of */
		/* next entry in cirular list of entries */

		info->rx_buffer_list[i].link = info->buffer_list_phys;

		if ( i < info->rx_buffer_count - 1 )
			info->rx_buffer_list[i].link += (i + 1) * sizeof(DMABUFFERENTRY);
	}

	for ( i = 0; i < info->tx_buffer_count; i++ ) {
		/* calculate and store physical address of this buffer entry */
		info->tx_buffer_list[i].phys_entry = info->buffer_list_phys +
			((info->rx_buffer_count + i) * sizeof(DMABUFFERENTRY));

		/* calculate and store physical address of */
		/* next entry in cirular list of entries */

		info->tx_buffer_list[i].link = info->buffer_list_phys +
			info->rx_buffer_count * sizeof(DMABUFFERENTRY);

		if ( i < info->tx_buffer_count - 1 )
			info->tx_buffer_list[i].link += (i + 1) * sizeof(DMABUFFERENTRY);
	}

	return 0;

}	/* end of mgsl_alloc_buffer_list_memory() */

/*
 * mgsl_free_buffer_list_memory()
 * 
 * 	Free the common DMA buffer allocated for use as the
 * 	receive and transmit buffer lists. The associated Memory
 * 	Descriptor List (MDL) is also freed.
 * 
 * Warning:
 * 
 * 	The data transfer buffers associated with the buffer list
 * 	MUST be freed before freeing the buffer list itself because
 * 	the buffer list contains the information necessary to free
 * 	the individual buffers!
 * 
 * Arguments:		info	pointer to device extension
 * Return Value:	None
 */
void mgsl_free_buffer_list_memory( struct mgsl_struct *info )
{
	if ( info->buffer_list && info->bus_type != MGSL_BUS_TYPE_PCI )
		kfree_s(info->buffer_list, BUFFERLISTSIZE);
		
	info->buffer_list = NULL;
	info->rx_buffer_list = NULL;
	info->tx_buffer_list = NULL;

}	/* end of mgsl_free_buffer_list_memory() */

/*
 * mgsl_alloc_frame_memory()
 * 
 * 	Allocate the frame DMA buffers used by the specified buffer list.
 * 	Each DMA buffer will be one memory page in size. This is necessary
 * 	because memory can fragment enough that it may be impossible
 * 	contiguous pages.
 * 
 * Arguments:
 * 
 *	info		pointer to device instance data
 * 	BufferList	pointer to list of buffer entries
 * 	Buffercount	count of buffer entries in buffer list
 * 
 * Return Value:	0 if success, otherwise -ENOMEM
 */
int mgsl_alloc_frame_memory(struct mgsl_struct *info,DMABUFFERENTRY *BufferList,int Buffercount)
{
	int i;
	unsigned long phys_addr;

	/* Allocate page sized buffers for the receive buffer list */

	for ( i = 0; i < Buffercount; i++ ) {
		if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
			/* PCI adapter uses shared memory buffers. */
			BufferList[i].virt_addr = info->memory_base + info->last_mem_alloc;
			phys_addr = info->last_mem_alloc;
			info->last_mem_alloc += DMABUFFERSIZE;
		} else {
			/* ISA adapter uses system memory. */
			BufferList[i].virt_addr = 
				kmalloc(DMABUFFERSIZE, GFP_KERNEL | GFP_DMA);
			if ( BufferList[i].virt_addr == NULL )
				return -ENOMEM;
			phys_addr = virt_to_bus(BufferList[i].virt_addr);
		}
		BufferList[i].phys_addr = phys_addr;
	}

	return 0;

}	/* end of mgsl_alloc_frame_memory() */

/*
 * mgsl_free_frame_memory()
 * 
 * 	Free the buffers associated with
 * 	each buffer entry of a buffer list.
 * 
 * Arguments:
 * 
 *	info		pointer to device instance data
 * 	BufferList	pointer to list of buffer entries
 * 	Buffercount	count of buffer entries in buffer list
 * 
 * Return Value:	None
 */
void mgsl_free_frame_memory(struct mgsl_struct *info, DMABUFFERENTRY *BufferList, int Buffercount)
{
	int i;

	if ( BufferList ) {
		for ( i = 0 ; i < Buffercount ; i++ ) {
			if ( BufferList[i].virt_addr ) {
				if ( info->bus_type != MGSL_BUS_TYPE_PCI )
					kfree_s(BufferList[i].virt_addr, DMABUFFERSIZE);
				BufferList[i].virt_addr = NULL;
			}
		}
	}

}	/* end of mgsl_free_frame_memory() */

/* mgsl_free_dma_buffers()
 * 
 * 	Free DMA buffers
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_free_dma_buffers( struct mgsl_struct *info )
{
	mgsl_free_frame_memory( info, info->rx_buffer_list, info->rx_buffer_count );
	mgsl_free_frame_memory( info, info->tx_buffer_list, info->tx_buffer_count );
	mgsl_free_buffer_list_memory( info );

}	/* end of mgsl_free_dma_buffers() */


/*
 * mgsl_alloc_intermediate_rxbuffer_memory()
 * 
 * 	Allocate a buffer large enough to hold max_frame_size. This buffer
 *	is used to pass an assembled frame to the line discipline.
 * 
 * Arguments:
 * 
 *	info		pointer to device instance data
 * 
 * Return Value:	0 if success, otherwise -ENOMEM
 */
int mgsl_alloc_intermediate_rxbuffer_memory(struct mgsl_struct *info)
{
	info->intermediate_rxbuffer = kmalloc(info->max_frame_size, GFP_KERNEL | GFP_DMA);
	if ( info->intermediate_rxbuffer == NULL )
		return -ENOMEM;

	return 0;

}	/* end of mgsl_alloc_intermediate_rxbuffer_memory() */

/*
 * mgsl_free_intermediate_rxbuffer_memory()
 * 
 * 
 * Arguments:
 * 
 *	info		pointer to device instance data
 * 
 * Return Value:	None
 */
void mgsl_free_intermediate_rxbuffer_memory(struct mgsl_struct *info)
{
	if ( info->intermediate_rxbuffer )
		kfree_s( info->intermediate_rxbuffer, info->max_frame_size);

	info->intermediate_rxbuffer = NULL;

}	/* end of mgsl_free_intermediate_rxbuffer_memory() */

/* mgsl_claim_resources()
 * 
 * 	Claim all resources used by a device
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	0 if success, otherwise -ENODEV
 */
int mgsl_claim_resources(struct mgsl_struct *info)
{
	/* claim 16C32 I/O base address */
	
	if ( check_region(info->io_base,info->io_addr_size) < 0 ) {
		printk( "%s(%d):I/O address conflict on device %s Addr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->io_base );
		return -ENODEV;
	}
	request_region(info->io_base,info->io_addr_size,"synclink.o");
	info->io_addr_requested = 1;
	
	/* claim interrupt level */
	
	if ( request_irq(info->irq_level,mgsl_interrupt,info->irq_flags,
		info->device_name, info ) < 0 ) {
		printk( "%s(%d):Cant request interrupt on device %s IRQ=%d\n",
			__FILE__,__LINE__,info->device_name, info->irq_level );
		mgsl_release_resources( info );
		return -ENODEV;
	}
	info->irq_requested = 1;
	
	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		/* claim shared memory range */
		info->memory_base = ioremap(info->phys_memory_base,0x40000);
		if (!info->memory_base) {
			printk( "%s(%d):Cant map shared memory on device %s MemAddr=%08X\n",
				__FILE__,__LINE__,info->device_name, info->phys_memory_base );
			mgsl_release_resources( info );
			return -ENODEV;
		}
		
		/* test the shared memory range */
		if ( !mgsl_memory_test(info) ) {
			printk( "%s(%d):Failed shared memory test %s MemAddr=%08X\n",
				__FILE__,__LINE__,info->device_name, info->phys_memory_base );
			mgsl_release_resources( info );
			return -ENODEV;
		}
		
		/* claim LCR memory range */
		info->lcr_base = ioremap(info->phys_lcr_base,PAGE_SIZE) + info->lcr_offset;
		if (!info->lcr_base) {
			printk( "%s(%d):Cant map LCR memory on device %s MemAddr=%08X\n",
				__FILE__,__LINE__,info->device_name, info->phys_lcr_base );
			mgsl_release_resources( info );
			return -ENODEV;
		}
		
	} else {
		/* claim DMA channel */
		
		if (request_dma(info->dma_level,info->device_name) < 0){
			printk( "%s(%d):Cant request DMA channel on device %s DMA=%d\n",
				__FILE__,__LINE__,info->device_name, info->dma_level );
			mgsl_release_resources( info );
			return -ENODEV;
		}
		info->dma_requested = 1;

		/* ISA adapter uses bus master DMA */		
		set_dma_mode(info->dma_level,DMA_MODE_CASCADE);
		enable_dma(info->dma_level);
	}
	
	if ( mgsl_allocate_dma_buffers(info) < 0 ) {
		printk( "%s(%d):Cant allocate DMA buffers on device %s DMA=%d\n",
			__FILE__,__LINE__,info->device_name, info->dma_level );
		mgsl_release_resources( info );
		return -ENODEV;
	}	
	
	return 0;
		
}	/* end of mgsl_claim_resources() */

/* mgsl_release_resources()
 * 
 * 	Release all resources used by a device
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_release_resources(struct mgsl_struct *info)
{
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_release_resources(%s) entry\n",
			__FILE__,__LINE__,info->device_name );
			
	if ( info->irq_requested ) {
		free_irq(info->irq_level, info);
		info->irq_requested = 0;
	}
	
	if ( info->dma_requested ) {
		disable_dma(info->dma_level);
		free_dma(info->dma_level);
		info->dma_requested = 0;
	}
	mgsl_free_dma_buffers(info);
	mgsl_free_intermediate_rxbuffer_memory(info);
	
	if ( info->io_addr_requested ) {
		release_region(info->io_base,info->io_addr_size);
		info->io_addr_requested = 0;
	}
		
	if (info->memory_base){
		iounmap(info->memory_base);
		info->memory_base = 0;
	}
		
	if (info->lcr_base){
		iounmap(info->lcr_base - info->lcr_offset);
		info->lcr_base = 0;
	}
	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_release_resources(%s) exit\n",
			__FILE__,__LINE__,info->device_name );
			
}	/* end of mgsl_release_resources() */

/* mgsl_add_device()
 * 
 * 	Add the specified device instance data structure to the
 * 	global linked list of devices and increment the device count.
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_add_device( struct mgsl_struct *info )
{
	info->next_device = NULL;
	info->line = mgsl_device_count;
	sprintf(info->device_name,"ttySL%d",info->line);
	
	mgsl_device_count++;
	
	if ( !mgsl_device_list )
		mgsl_device_list = info;
	else {	
		struct mgsl_struct *current_dev = mgsl_device_list;
		while( current_dev->next_device )
			current_dev = current_dev->next_device;
		current_dev->next_device = info;
	}
	
	if ( info->max_frame_size < 4096 )
		info->max_frame_size = 4096;
	else if ( info->max_frame_size > 65535 )
		info->max_frame_size = 65535;
	
	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		printk( "SyncLink device %s added:PCI bus IO=%04X IRQ=%d Mem=%08X LCR=%08X MaxFrameSize=%u\n",
			info->device_name, info->io_base, info->irq_level,
			info->phys_memory_base, info->phys_lcr_base,
		     	info->max_frame_size );
	} else {
		printk( "SyncLink device %s added:ISA bus IO=%04X IRQ=%d DMA=%d MaxFrameSize=%u\n",
			info->device_name, info->io_base, info->irq_level, info->dma_level,
		     	info->max_frame_size );
	}
	
}	/* end of mgsl_add_device() */

/* mgsl_allocate_device()
 * 
 * 	Allocate and initialize a device instance structure
 * 	
 * Arguments:		none
 * Return Value:	pointer to mgsl_struct if success, otherwise NULL
 */
struct mgsl_struct* mgsl_allocate_device()
{
	struct mgsl_struct *info;
	
	info = (struct mgsl_struct *)kmalloc(sizeof(struct mgsl_struct),
		 GFP_KERNEL);
		 
	if (!info) {
		printk("Error can't allocate device instance data\n");
	} else {
		memset(info, 0, sizeof(struct mgsl_struct));
		info->magic = MGSL_MAGIC;
		info->task.sync = 0;
		info->task.routine = mgsl_bh_handler;
		info->task.data    = info;
		info->max_frame_size = 4096;
		info->close_delay = 5*HZ/10;
		info->closing_wait = 30*HZ;
		init_waitqueue_head(&info->open_wait);
		init_waitqueue_head(&info->close_wait);
		init_waitqueue_head(&info->status_event_wait_q);
		init_waitqueue_head(&info->event_wait_q);

		memcpy(&info->params,&default_params,sizeof(MGSL_PARAMS));
		info->idle_mode = HDLC_TXIDLE_FLAGS;		
	}
	
	return info;

}	/* end of mgsl_allocate_device()*/

/* mgsl_enumerate_devices()
 * 
 * 	Enumerate SyncLink serial devices based on user specified
 *	options for ISA adapters and autodetected PCI adapters.
 * 	
 * Arguments:		None
 * Return Value:	0 if success, otherwise error code
 */
int mgsl_enumerate_devices()
{
	struct mgsl_struct *info;
	int i;
	int num_devices = 0;
		
	/* Check for user specified ISA devices */
	
	for (i=0 ;(i < MAX_ISA_DEVICES) && io[i] && irq[i]; i++){
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk("ISA device specified io=%04X,irq=%d,dma=%d\n",
				io[i], irq[i], dma[i] );
		
		info = mgsl_allocate_device();
		if ( !info ) {
			/* error allocating device instance data */
			if ( debug_level >= DEBUG_LEVEL_ERROR )
				printk( "can't allocate device instance data.\n");
			continue;
		}
		
		/* Copy user configuration info to device instance data */
		info->io_base = (unsigned int)io[i];
		info->irq_level = (unsigned int)irq[i];
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
		info->irq_level = irq_cannonicalize(info->irq_level);
#else		
		if (info->irq_level == 2)
			info->irq_level = 9;
#endif			
		info->dma_level = (unsigned int)dma[i];
		info->bus_type = MGSL_BUS_TYPE_ISA;
		info->io_addr_size = 16;
		info->irq_flags = 0;
		
		/* override default max frame size if arg available */
		if ( num_devices < MAX_TOTAL_DEVICES && 
				maxframe[num_devices] )
			info->max_frame_size = maxframe[num_devices];
				
		/* add new device to device list */
		mgsl_add_device( info );
		
		++num_devices;
	}
	
	
#ifdef CONFIG_PCI
	/* Auto detect PCI adapters */
	
	if ( pcibios_present() ) {
		unsigned char bus;
		unsigned char func;
		unsigned int  shared_mem_base;
		unsigned int  lcr_mem_base;
		unsigned int  io_base;
		unsigned char irq_line;
		
		for(i=0;;i++){
			if ( PCIBIOS_SUCCESSFUL == pcibios_find_device(
				MICROGATE_VENDOR_ID, SYNCLINK_DEVICE_ID, i, &bus, &func) ) {
				
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
				struct pci_dev *pdev = pci_find_slot(bus,func);
				irq_line = pdev->irq;				
#else												
				if (pcibios_read_config_byte(bus,func,
					PCI_INTERRUPT_LINE,&irq_line) ) {
					printk( "%s(%d):USC I/O addr not set.\n",
						__FILE__,__LINE__);
					continue;
				}
#endif

				if (pcibios_read_config_dword(bus,func,
					PCI_BASE_ADDRESS_3,&shared_mem_base) ) {
					printk( "%s(%d):Shared mem addr not set.\n",
						__FILE__,__LINE__);
					continue;
				}
							
				if (pcibios_read_config_dword(bus,func,
					PCI_BASE_ADDRESS_0,&lcr_mem_base) ) {
					printk( "%s(%d):LCR mem addr not set.\n",
						__FILE__,__LINE__);
					continue;
				}
				
				if (pcibios_read_config_dword(bus,func,
					PCI_BASE_ADDRESS_2,&io_base) ) {
					printk( "%s(%d):USC I/O addr not set.\n",
						__FILE__,__LINE__);
					continue;
				}
				
				info = mgsl_allocate_device();
				if ( !info ) {
					/* error allocating device instance data */
					if ( debug_level >= DEBUG_LEVEL_ERROR )
						printk( "can't allocate device instance data.\n");
					continue;
				}
		
				/* Copy user configuration info to device instance data */
		
				info->io_base = io_base & PCI_BASE_ADDRESS_IO_MASK;
				info->irq_level = (unsigned int)irq_line;
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
				info->irq_level = irq_cannonicalize(info->irq_level);
#else		
				if (info->irq_level == 2)
					info->irq_level = 9;
#endif			
				info->phys_memory_base = shared_mem_base & PCI_BASE_ADDRESS_MEM_MASK;
				
				/* Because veremap only works on page boundaries we must map
				 * a larger area than is actually implemented for the LCR
				 * memory range. We map a full page starting at the page boundary.
				 */
				info->phys_lcr_base = lcr_mem_base & PCI_BASE_ADDRESS_MEM_MASK;
				info->lcr_offset    = info->phys_lcr_base & (PAGE_SIZE-1);
				info->phys_lcr_base &= ~(PAGE_SIZE-1);
				
				info->bus_type = MGSL_BUS_TYPE_PCI;
				info->io_addr_size = 8;
				info->irq_flags = SA_SHIRQ;
				info->bus = bus;
				info->function = func;
		
				/* override default max frame size if arg available */
				if ( num_devices < MAX_TOTAL_DEVICES && 
						maxframe[num_devices] )
				info->max_frame_size = maxframe[num_devices];
				
		/* Store the PCI9050 misc control register value because a flaw
		 * in the PCI9050 prevents LCR registers from being read if 
		 * BIOS assigns an LCR base address with bit 7 set.
		 *  
		 * Only the misc control register is accessed for which only 
		 * write access is needed, so set an initial value and change 
		 * bits to the device instance data as we write the value
		 * to the actual misc control register.
		 */
				info->misc_ctrl_value = 0x087e4546;
				
				/* add new device to device list */
				mgsl_add_device( info );
			} else {
				break;
			}
		}
	}
#endif

	/*
	 * Allocate memory to hold the following tty/termios arrays
	 * with an element for each enumerated device.
	 */	
	
	serial_table = (struct tty_struct**)kmalloc(sizeof(struct tty_struct*)*mgsl_device_count, GFP_KERNEL);
	serial_termios = (struct termios**)kmalloc(sizeof(struct termios*)*mgsl_device_count, GFP_KERNEL);
	serial_termios_locked = (struct termios**)kmalloc(sizeof(struct termios*)*mgsl_device_count, GFP_KERNEL);
	
	if (!serial_table || !serial_termios || !serial_termios_locked){
		printk("%s(%d):Can't allocate tty/termios arrays.\n",
			__FILE__,__LINE__);
		return -ENOMEM;
	}
	
	memset(serial_table,0,sizeof(struct tty_struct*)*mgsl_device_count);
	memset(serial_termios,0,sizeof(struct termios*)*mgsl_device_count);
	memset(serial_termios_locked,0,sizeof(struct termios*)*mgsl_device_count);

	return 0;
	
}	/* end of mgsl_enumerate_devices() */

/* mgsl_init()
 * 
 * 	Driver initialization entry point.
 * 	
 * Arguments:	None
 * Return Value:	0 if success, otherwise error code
 */
int __init mgsl_init(void)
{
	struct mgsl_struct *info;

#if LINUX_VERSION_CODE >= VERSION(2,1,0)
	EXPORT_NO_SYMBOLS;
#else
	register_symtab(NULL);
#endif		
	
 	printk("%s version %s\n", driver_name, driver_version);
	
	/* determine how many SyncLink devices are installed */
	mgsl_enumerate_devices();
	if ( !mgsl_device_list ) {
		printk("%s(%d):No SyncLink devices found.\n",__FILE__,__LINE__);
		return -ENODEV;
	}

	/* Initialize the tty_driver structure */
	
	memset(&serial_driver, 0, sizeof(struct tty_driver));
	serial_driver.magic = TTY_DRIVER_MAGIC;
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
	serial_driver.driver_name = "synclink";
#endif	
	serial_driver.name = "ttySL";
	serial_driver.major = ttymajor;
	serial_driver.minor_start = 64;
	serial_driver.num = mgsl_device_count;
	serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
	serial_driver.subtype = SERIAL_TYPE_NORMAL;
	serial_driver.init_termios = tty_std_termios;
	serial_driver.init_termios.c_cflag =
		B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	serial_driver.flags = TTY_DRIVER_REAL_RAW;
	serial_driver.refcount = &serial_refcount;
	serial_driver.table = serial_table;
	serial_driver.termios = serial_termios;
	serial_driver.termios_locked = serial_termios_locked;

	serial_driver.open = mgsl_open;
	serial_driver.close = mgsl_close;
	serial_driver.write = mgsl_write;
	serial_driver.put_char = mgsl_put_char;
	serial_driver.flush_chars = mgsl_flush_chars;
	serial_driver.write_room = mgsl_write_room;
	serial_driver.chars_in_buffer = mgsl_chars_in_buffer;
	serial_driver.flush_buffer = mgsl_flush_buffer;
	serial_driver.ioctl = mgsl_ioctl;
	serial_driver.throttle = mgsl_throttle;
	serial_driver.unthrottle = mgsl_unthrottle;
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
	serial_driver.send_xchar = mgsl_send_xchar;
	serial_driver.break_ctl = mgsl_break;
	serial_driver.wait_until_sent = mgsl_wait_until_sent;
 	serial_driver.read_proc = mgsl_read_proc;
#endif	
	serial_driver.set_termios = mgsl_set_termios;
	serial_driver.stop = mgsl_stop;
	serial_driver.start = mgsl_start;
	serial_driver.hangup = mgsl_hangup;
	
	/*
	 * The callout device is just like normal device except for
	 * major number and the subtype code.
	 */
	callout_driver = serial_driver;
	callout_driver.name = "cuaSL";
	callout_driver.major = cuamajor;
	callout_driver.subtype = SERIAL_TYPE_CALLOUT;
#if LINUX_VERSION_CODE >= VERSION(2,1,0)
	callout_driver.read_proc = 0;
	callout_driver.proc_entry = 0;
#endif	

	if (tty_register_driver(&serial_driver) < 0)
		printk("%s(%d):Couldn't register serial driver\n",
			__FILE__,__LINE__);
			
	if (tty_register_driver(&callout_driver) < 0)
		printk("%s(%d):Couldn't register callout driver\n",
			__FILE__,__LINE__);

 	printk("%s version %s, tty major#%d callout major#%d\n",
		driver_name, driver_version,
		serial_driver.major, callout_driver.major);
		
	/* Propagate these values to all device instances */
	
	info = mgsl_device_list;
	while(info){
		info->callout_termios = callout_driver.init_termios;
		info->normal_termios  = serial_driver.init_termios;
		info = info->next_device;
	}
	
	return 0;
	
}	/* end of mgsl_init() */

#ifdef MODULE
int init_module(void)
{
/* Uncomment this to kernel debug module.
 * mgsl_get_text_ptr() leaves the .text address in eax
 * which can be used with add-symbol-file with gdb.
 */
	if (break_on_load) {
	 	mgsl_get_text_ptr();
  		BREAKPOINT();
	}
	
	return mgsl_init();
}

void cleanup_module(void) 
{
	unsigned long flags;
	int rc;
	struct mgsl_struct *info;

	printk("Unloading %s: version %s\n", driver_name, driver_version);
	save_flags(flags);
	cli();
	if ((rc = tty_unregister_driver(&serial_driver)))
		printk("%s(%d) failed to unregister tty driver err=%d\n",
		       __FILE__,__LINE__,rc);
	if ((rc = tty_unregister_driver(&callout_driver)))
		printk("%s(%d) failed to unregister callout driver err=%d\n",
		       __FILE__,__LINE__,rc);
	restore_flags(flags);

	info = mgsl_device_list;
	while(info) {
		mgsl_release_resources(info);
		info = info->next_device;
	}
	
	if (tmp_buf) {
		free_page((unsigned long) tmp_buf);
		tmp_buf = NULL;
	}
	
	if (serial_table)
		kfree_s(serial_table,sizeof(struct tty_struct*)*mgsl_device_count);
		
	if (serial_termios)
		kfree_s(serial_termios,sizeof(struct termios*)*mgsl_device_count);
		
	if (serial_termios_locked)
		kfree_s(serial_termios_locked,sizeof(struct termios*)*mgsl_device_count);
	
}	/* end of cleanup_module() */

#endif /* MODULE */


/*
 * usc_RTCmd()
 *
 * Issue a USC Receive/Transmit command to the
 * Channel Command/Address Register (CCAR).
 *
 * Notes:
 *
 *    The command is encoded in the most significant 5 bits <15..11>
 *    of the CCAR value. Bits <10..7> of the CCAR must be preserved
 *    and Bits <6..0> must be written as zeros.
 *
 * Arguments:
 *
 *    info   pointer to device information structure
 *    Cmd    command mask (use symbolic macros)
 *
 * Return Value:
 *
 *    None
 */
void usc_RTCmd( struct mgsl_struct *info, u16 Cmd )
{
	/* output command to CCAR in bits <15..11> */
	/* preserve bits <10..7>, bits <6..0> must be zero */

	outw( Cmd + info->loopback_bits, info->io_base + CCAR );

	/* Read to flush write to CCAR */
	if ( info->bus_type == MGSL_BUS_TYPE_PCI )
		inw( info->io_base + CCAR );

}	/* end of usc_RTCmd() */

/*
 * usc_DmaCmd()
 *
 *    Issue a DMA command to the DMA Command/Address Register (DCAR).
 *
 * Arguments:
 *
 *    info   pointer to device information structure
 *    Cmd    DMA command mask (usc_DmaCmd_XX Macros)
 *
 * Return Value:
 *
 *       None
 */
void usc_DmaCmd( struct mgsl_struct *info, u16 Cmd )
{
	/* write command mask to DCAR */
	outw( Cmd + info->mbre_bit, info->io_base );

	/* Read to flush write to DCAR */
	if ( info->bus_type == MGSL_BUS_TYPE_PCI )
		inw( info->io_base );

}	/* end of usc_DmaCmd() */

/*
 * usc_OutDmaReg()
 *
 *    Write a 16-bit value to a USC DMA register
 *
 * Arguments:
 *
 *    info      pointer to device info structure
 *    RegAddr   register address (number) for write
 *    RegValue  16-bit value to write to register
 *
 * Return Value:
 *
 *    None
 *
 */
void usc_OutDmaReg( struct mgsl_struct *info, u16 RegAddr, u16 RegValue )
{
	/* Note: The DCAR is located at the adapter base address */
	/* Note: must preserve state of BIT8 in DCAR */

	outw( RegAddr + info->mbre_bit, info->io_base );
	outw( RegValue, info->io_base );

	/* Read to flush write to DCAR */
	if ( info->bus_type == MGSL_BUS_TYPE_PCI )
		inw( info->io_base );

}	/* end of usc_OutDmaReg() */
 
/*
 * usc_InDmaReg()
 *
 *    Read a 16-bit value from a DMA register
 *
 * Arguments:
 *
 *    info     pointer to device info structure
 *    RegAddr  register address (number) to read from
 *
 * Return Value:
 *
 *    The 16-bit value read from register
 *
 */
u16 usc_InDmaReg( struct mgsl_struct *info, u16 RegAddr )
{
	/* Note: The DCAR is located at the adapter base address */
	/* Note: must preserve state of BIT8 in DCAR */

	outw( RegAddr + info->mbre_bit, info->io_base );
	return inw( info->io_base );

}	/* end of usc_InDmaReg() */

/*
 *
 * usc_OutReg()
 *
 *    Write a 16-bit value to a USC serial channel register 
 *
 * Arguments:
 *
 *    info      pointer to device info structure
 *    RegAddr   register address (number) to write to
 *    RegValue  16-bit value to write to register
 *
 * Return Value:
 *
 *    None
 *
 */
void usc_OutReg( struct mgsl_struct *info, u16 RegAddr, u16 RegValue )
{
	outw( RegAddr + info->loopback_bits, info->io_base + CCAR );
	outw( RegValue, info->io_base + CCAR );

	/* Read to flush write to CCAR */
	if ( info->bus_type == MGSL_BUS_TYPE_PCI )
		inw( info->io_base + CCAR );

}	/* end of usc_OutReg() */

/*
 * usc_InReg()
 *
 *    Reads a 16-bit value from a USC serial channel register
 *
 * Arguments:
 *
 *    info       pointer to device extension
 *    RegAddr    register address (number) to read from
 *
 * Return Value:
 *
 *    16-bit value read from register
 */
u16 usc_InReg( struct mgsl_struct *info, u16 RegAddr )
{
	outw( RegAddr + info->loopback_bits, info->io_base + CCAR );
	return inw( info->io_base + CCAR );

}	/* end of usc_InReg() */

/* usc_set_sdlc_mode()
 *
 *    Set up the adapter for SDLC DMA communications.
 *
 * Arguments:		info    pointer to device instance data
 * Return Value: 	NONE
 */
void usc_set_sdlc_mode( struct mgsl_struct *info )
{
	u16 RegValue;
	int PreSL1660;
	
	/*
	 * determine if the IUSC on the adapter is pre-SL1660. If
	 * not, take advantage of the UnderWait feature of more
	 * modern chips. If an underrun occurs and this bit is set,
	 * the transmitter will idle the programmed idle pattern
	 * until the driver has time to service the underrun. Otherwise,
	 * the dma controller may get the cycles previously requested
	 * and begin transmitting queued tx data.
	 */
	usc_OutReg(info,TMCR,0x1f);
	RegValue=usc_InReg(info,TMDR);
	if ( RegValue == IUSC_PRE_SL1660 )
		PreSL1660 = 1;
	else
		PreSL1660 = 0;
	

 	if ( info->params.flags & HDLC_FLAG_HDLC_LOOPMODE )
 	{
 	   /*
 	   ** Channel Mode Register (CMR)
 	   **
 	   ** <15..14>    10    Tx Sub Modes, Send Flag on Underrun
 	   ** <13>        0     0 = Transmit Disabled (initially)
 	   ** <12>        0     1 = Consecutive Idles share common 0
 	   ** <11..8>     1110  Transmitter Mode = HDLC/SDLC Loop
 	   ** <7..4>      0000  Rx Sub Modes, addr/ctrl field handling
 	   ** <3..0>      0110  Receiver Mode = HDLC/SDLC
 	   **
 	   ** 1000 1110 0000 0110 = 0x8e06
 	   */
 	   RegValue = 0x8e06;
 
 	   /*--------------------------------------------------
 	    * ignore user options for UnderRun Actions and
 	    * preambles
 	    *--------------------------------------------------*/
 	}
 	else
 	{	
		/* Channel mode Register (CMR)
		 *
		 * <15..14>  00    Tx Sub modes, Underrun Action
		 * <13>      0     1 = Send Preamble before opening flag
		 * <12>      0     1 = Consecutive Idles share common 0
		 * <11..8>   0110  Transmitter mode = HDLC/SDLC
		 * <7..4>    0000  Rx Sub modes, addr/ctrl field handling
		 * <3..0>    0110  Receiver mode = HDLC/SDLC
		 *
		 * 0000 0110 0000 0110 = 0x0606
		 */

		RegValue = 0x0606;

		if ( info->params.flags & HDLC_FLAG_UNDERRUN_ABORT15 )
			RegValue |= BIT14;
		else if ( info->params.flags & HDLC_FLAG_UNDERRUN_FLAG )
			RegValue |= BIT15;
		else if ( info->params.flags & HDLC_FLAG_UNDERRUN_CRC )
			RegValue |= BIT15 + BIT14;

		if ( info->params.preamble != HDLC_PREAMBLE_PATTERN_NONE )
			RegValue |= BIT13;
	}

	if ( info->params.flags & HDLC_FLAG_SHARE_ZERO )
		RegValue |= BIT12;

	if ( info->params.addr_filter != 0xff )
	{
		/* set up receive address filtering */
		usc_OutReg( info, RSR, info->params.addr_filter );
		RegValue |= BIT4;
	}

	usc_OutReg( info, CMR, RegValue );
	info->cmr_value = RegValue;

	/* Receiver mode Register (RMR)
	 *
	 * <15..13>  000    encoding
	 * <12..11>  00     FCS = 16bit CRC CCITT (x15 + x12 + x5 + 1)
	 * <10>      1      1 = Set CRC to all 1s (use for SDLC/HDLC)
	 * <9>       0      1 = Include Receive chars in CRC
	 * <8>       1      1 = Use Abort/PE bit as abort indicator
	 * <7..6>    00     Even parity
	 * <5>       0      parity disabled
	 * <4..2>    000    Receive Char Length = 8 bits
	 * <1..0>    00     Disable Receiver
	 *
	 * 0000 0101 0000 0000 = 0x0500
	 */

	RegValue = 0x0500;

	switch ( info->params.encoding ) {
	case HDLC_ENCODING_NRZB:               RegValue |= BIT13; break;
	case HDLC_ENCODING_NRZI_MARK:          RegValue |= BIT14; break;
	case HDLC_ENCODING_NRZI_SPACE:	       RegValue |= BIT14 + BIT13; break;
	case HDLC_ENCODING_BIPHASE_MARK:       RegValue |= BIT15; break;
	case HDLC_ENCODING_BIPHASE_SPACE:      RegValue |= BIT15 + BIT13; break;
	case HDLC_ENCODING_BIPHASE_LEVEL:      RegValue |= BIT15 + BIT14; break;
	case HDLC_ENCODING_DIFF_BIPHASE_LEVEL: RegValue |= BIT15 + BIT14 + BIT13; break;
	}

	if ( info->params.crc_type == HDLC_CRC_16_CCITT )
		RegValue |= BIT9;
	else if ( info->params.crc_type == HDLC_CRC_32_CCITT )
		RegValue |= ( BIT12 | BIT10 | BIT9 );

	usc_OutReg( info, RMR, RegValue );


 
	/* Set the Receive count Limit Register (RCLR) to 0xffff. */
	/* When an opening flag of an SDLC frame is recognized the */
	/* Receive Character count (RCC) is loaded with the value in */
	/* RCLR. The RCC is decremented for each received byte.  The */
	/* value of RCC is stored after the closing flag of the frame */
	/* allowing the frame size to be computed. */

	usc_OutReg( info, RCLR, RCLRVALUE );

	usc_RCmd( info, RCmd_SelectRicrdma_level );

	/* Receive Interrupt Control Register (RICR)
	 *
	 * <15..8>	?	RxFIFO DMA Request Level
	 * <7>		0	Exited Hunt IA (Interrupt Arm)
	 * <6>		0	Idle Received IA
	 * <5>		0	Break/Abort IA
	 * <4>		0	Rx Bound IA
	 * <3>		1	Queued status reflects oldest 2 bytes in FIFO
	 * <2>		0	Abort/PE IA
	 * <1>		1	Rx Overrun IA
	 * <0>		0	Select TC0 value for readback
	 *
	 *	0000 0000 0000 1000 = 0x000a
	 */

	/* Carry over the Exit Hunt and Idle Received bits */
	/* in case they have been armed by usc_ArmEvents.   */

	RegValue = usc_InReg( info, RICR ) & 0xc0;

	if ( info->bus_type == MGSL_BUS_TYPE_PCI )
		usc_OutReg( info, RICR, (u16)(0x030a | RegValue) );
	else
		usc_OutReg( info, RICR, (u16)(0x140a | RegValue) );

	/* Unlatch all Rx status bits and clear Rx status IRQ Pending */

	usc_UnlatchRxstatusBits( info, RXSTATUS_ALL );
	usc_ClearIrqPendingBits( info, RECEIVE_STATUS );

	/* Transmit mode Register (TMR)
	 *	
	 * <15..13>	000	encoding
	 * <12..11>	00	FCS = 16bit CRC CCITT (x15 + x12 + x5 + 1)
	 * <10>		1	1 = Start CRC as all 1s (use for SDLC/HDLC)
	 * <9>		0	1 = Tx CRC Enabled
	 * <8>		0	1 = Append CRC to end of transmit frame
	 * <7..6>	00	Transmit parity Even
	 * <5>		0	Transmit parity Disabled
	 * <4..2>	000	Tx Char Length = 8 bits
	 * <1..0>	00	Disable Transmitter
	 *
	 * 	0000 0100 0000 0000 = 0x0400
	 */

	RegValue = 0x0400;

	switch ( info->params.encoding ) {
	case HDLC_ENCODING_NRZB:               RegValue |= BIT13; break;
	case HDLC_ENCODING_NRZI_MARK:          RegValue |= BIT14; break;
	case HDLC_ENCODING_NRZI_SPACE:         RegValue |= BIT14 + BIT13; break;
	case HDLC_ENCODING_BIPHASE_MARK:       RegValue |= BIT15; break;
	case HDLC_ENCODING_BIPHASE_SPACE:      RegValue |= BIT15 + BIT13; break;
	case HDLC_ENCODING_BIPHASE_LEVEL:      RegValue |= BIT15 + BIT14; break;
	case HDLC_ENCODING_DIFF_BIPHASE_LEVEL: RegValue |= BIT15 + BIT14 + BIT13; break;
	}

	if ( info->params.crc_type == HDLC_CRC_16_CCITT )
		RegValue |= BIT9 + BIT8;
	else if ( info->params.crc_type == HDLC_CRC_32_CCITT )
		RegValue |= ( BIT12 | BIT10 | BIT9 | BIT8);

	usc_OutReg( info, TMR, RegValue );

	usc_set_txidle( info );


	usc_TCmd( info, TCmd_SelectTicrdma_level );

	/* Transmit Interrupt Control Register (TICR)
	 *
	 * <15..8>	?	Transmit FIFO DMA Level
	 * <7>		0	Present IA (Interrupt Arm)
	 * <6>		0	Idle Sent IA
	 * <5>		1	Abort Sent IA
	 * <4>		1	EOF/EOM Sent IA
	 * <3>		0	CRC Sent IA
	 * <2>		1	1 = Wait for SW Trigger to Start Frame
	 * <1>		1	Tx Underrun IA
	 * <0>		0	TC0 constant on read back
	 *
	 *	0000 0000 0011 0110 = 0x0036
	 */

	if ( info->bus_type == MGSL_BUS_TYPE_PCI )
		usc_OutReg( info, TICR, 0x0736 );
	else								
		usc_OutReg( info, TICR, 0x1436 );

	usc_UnlatchTxstatusBits( info, TXSTATUS_ALL );
	usc_ClearIrqPendingBits( info, TRANSMIT_STATUS );

	/*
	** Transmit Command/Status Register (TCSR)
	**
	** <15..12>	0000	TCmd
	** <11> 	0/1	UnderWait
	** <10..08>	000	TxIdle
	** <7>		x	PreSent
	** <6>         	x	IdleSent
	** <5>         	x	AbortSent
	** <4>         	x	EOF/EOM Sent
	** <3>         	x	CRC Sent
	** <2>         	x	All Sent
	** <1>         	x	TxUnder
	** <0>         	x	TxEmpty
	** 
	** 0000 0000 0000 0000 = 0x0000
	*/
	info->tcsr_value = 0;

	if ( !PreSL1660 )
		info->tcsr_value |= TCSR_UNDERWAIT;
		
	usc_OutReg( info, TCSR, info->tcsr_value );

	/* Clock mode Control Register (CMCR)
	 *
	 * <15..14>	00	counter 1 Source = Disabled
	 * <13..12> 	00	counter 0 Source = Disabled
	 * <11..10> 	11	BRG1 Input is TxC Pin
	 * <9..8>	11	BRG0 Input is TxC Pin
	 * <7..6>	01	DPLL Input is BRG1 Output
	 * <5..3>	XXX	TxCLK comes from Port 0
	 * <2..0>   	XXX	RxCLK comes from Port 1
	 *
	 *	0000 1111 0111 0111 = 0x0f77
	 */

	RegValue = 0x0f40;

	if ( info->params.flags & HDLC_FLAG_RXC_DPLL )
		RegValue |= 0x0003;	/* RxCLK from DPLL */
	else if ( info->params.flags & HDLC_FLAG_RXC_BRG )
		RegValue |= 0x0004;	/* RxCLK from BRG0 */
 	else if ( info->params.flags & HDLC_FLAG_RXC_TXCPIN)
 		RegValue |= 0x0006;	/* RxCLK from TXC Input */
	else
		RegValue |= 0x0007;	/* RxCLK from Port1 */

	if ( info->params.flags & HDLC_FLAG_TXC_DPLL )
		RegValue |= 0x0018;	/* TxCLK from DPLL */
	else if ( info->params.flags & HDLC_FLAG_TXC_BRG )
		RegValue |= 0x0020;	/* TxCLK from BRG0 */
 	else if ( info->params.flags & HDLC_FLAG_TXC_RXCPIN)
 		RegValue |= 0x0038;	/* RxCLK from TXC Input */
	else
		RegValue |= 0x0030;	/* TxCLK from Port0 */

	usc_OutReg( info, CMCR, RegValue );


	/* Hardware Configuration Register (HCR)
	 *
	 * <15..14>	00	CTR0 Divisor:00=32,01=16,10=8,11=4
	 * <13>		0	CTR1DSel:0=CTR0Div determines CTR0Div
	 * <12>		0	CVOK:0=report code violation in biphase
	 * <11..10>	00	DPLL Divisor:00=32,01=16,10=8,11=4
	 * <9..8>	XX	DPLL mode:00=disable,01=NRZ,10=Biphase,11=Biphase Level
	 * <7..6>	00	reserved
	 * <5>		0	BRG1 mode:0=continuous,1=single cycle
	 * <4>		X	BRG1 Enable
	 * <3..2>	00	reserved
	 * <1>		0	BRG0 mode:0=continuous,1=single cycle
	 * <0>		0	BRG0 Enable
	 */

	RegValue = 0x0000;

	if ( info->params.flags & (HDLC_FLAG_RXC_DPLL + HDLC_FLAG_TXC_DPLL) ) {
		u32 XtalSpeed;
		u32 DpllDivisor;
		u16 Tc;

		/*  DPLL is enabled. Use BRG1 to provide continuous reference clock  */
		/*  for DPLL. DPLL mode in HCR is dependent on the encoding used. */

		if ( info->bus_type == MGSL_BUS_TYPE_PCI )
			XtalSpeed = 11059200;
		else
			XtalSpeed = 14745600;

		if ( info->params.flags & HDLC_FLAG_DPLL_DIV16 ) {
			DpllDivisor = 16;
			RegValue |= BIT10;
		}
		else if ( info->params.flags & HDLC_FLAG_DPLL_DIV8 ) {
			DpllDivisor = 8;
			RegValue |= BIT11;
		}
		else
			DpllDivisor = 32;

		/*  Tc = (Xtal/Speed) - 1 */
		/*  If twice the remainder of (Xtal/Speed) is greater than Speed */
		/*  then rounding up gives a more precise time constant. Instead */
		/*  of rounding up and then subtracting 1 we just don't subtract */
		/*  the one in this case. */

 		/*--------------------------------------------------
 		 * ejz: for DPLL mode, application should use the
 		 * same clock speed as the partner system, even 
 		 * though clocking is derived from the input RxData.
 		 * In case the user uses a 0 for the clock speed,
 		 * default to 0xffffffff and don't try to divide by
 		 * zero
 		 *--------------------------------------------------*/
 		if ( info->params.clock_speed )
 		{
			Tc = (u16)((XtalSpeed/DpllDivisor)/info->params.clock_speed);
			if ( !((((XtalSpeed/DpllDivisor) % info->params.clock_speed) * 2)
			       / info->params.clock_speed) )
				Tc--;
 		}
 		else
 			Tc = -1;
 				  

		/* Write 16-bit Time Constant for BRG1 */
		usc_OutReg( info, TC1R, Tc );

		RegValue |= BIT4;		/* enable BRG1 */

		switch ( info->params.encoding ) {
		case HDLC_ENCODING_NRZ:
		case HDLC_ENCODING_NRZB:
		case HDLC_ENCODING_NRZI_MARK:
		case HDLC_ENCODING_NRZI_SPACE: RegValue |= BIT8; break;
		case HDLC_ENCODING_BIPHASE_MARK:
		case HDLC_ENCODING_BIPHASE_SPACE: RegValue |= BIT9; break;
		case HDLC_ENCODING_BIPHASE_LEVEL:
		case HDLC_ENCODING_DIFF_BIPHASE_LEVEL: RegValue |= BIT9 + BIT8; break;
		}
	}

	usc_OutReg( info, HCR, RegValue );


	/* Channel Control/status Register (CCSR)
	 *
	 * <15>		X	RCC FIFO Overflow status (RO)
	 * <14>		X	RCC FIFO Not Empty status (RO)
	 * <13>		0	1 = Clear RCC FIFO (WO)
	 * <12>		X	DPLL Sync (RW)
	 * <11>		X	DPLL 2 Missed Clocks status (RO)
	 * <10>		X	DPLL 1 Missed Clock status (RO)
	 * <9..8>	00	DPLL Resync on rising and falling edges (RW)
	 * <7>		X	SDLC Loop On status (RO)
	 * <6>		X	SDLC Loop Send status (RO)
	 * <5>		1	Bypass counters for TxClk and RxClk (RW)
	 * <4..2>   	000	Last Char of SDLC frame has 8 bits (RW)
	 * <1..0>   	00	reserved
	 *
	 *	0000 0000 0010 0000 = 0x0020
	 */

	usc_OutReg( info, CCSR, 0x1020 );


	if ( info->params.flags & HDLC_FLAG_AUTO_CTS ) {
		usc_OutReg( info, SICR,
			    (u16)(usc_InReg(info,SICR) | SICR_CTS_INACTIVE) );
	}
	

	/* enable Master Interrupt Enable bit (MIE) */
	usc_EnableMasterIrqBit( info );

	usc_ClearIrqPendingBits( info, RECEIVE_STATUS + RECEIVE_DATA +
				TRANSMIT_STATUS + TRANSMIT_DATA );

	info->mbre_bit = 0;
	outw( 0, info->io_base ); 			/* clear Master Bus Enable (DCAR) */
	usc_DmaCmd( info, DmaCmd_ResetAllChannels );	/* disable both DMA channels */
	info->mbre_bit = BIT8;
	outw( BIT8, info->io_base );			/* set Master Bus Enable (DCAR) */

	/* Enable DMAEN (Port 7, Bit 14) */
	/* This connects the DMA request signal to the ISA bus */
	/* on the ISA adapter. This has no effect for the PCI adapter */
	usc_OutReg( info, PCR, (u16)((usc_InReg(info, PCR) | BIT15) & ~BIT14) );

	/* DMA Control Register (DCR)
	 *
	 * <15..14>	10	Priority mode = Alternating Tx/Rx
	 *		01	Rx has priority
	 *		00	Tx has priority
	 *
	 * <13>		1	Enable Priority Preempt per DCR<15..14>
	 *			(WARNING DCR<11..10> must be 00 when this is 1)
	 *		0	Choose activate channel per DCR<11..10>
	 *
	 * <12>		0	Little Endian for Array/List
	 * <11..10>	00	Both Channels can use each bus grant
	 * <9..6>	0000	reserved
	 * <5>		0	7 CLK - Minimum Bus Re-request Interval
	 * <4>		0	1 = drive D/C and S/D pins
	 * <3>		1	1 = Add one wait state to all DMA cycles.
	 * <2>		0	1 = Strobe /UAS on every transfer.
	 * <1..0>	11	Addr incrementing only affects LS24 bits
	 *
	 *	0110 0000 0000 1011 = 0x600b
	 */

	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		/* PCI adapter does not need DMA wait state */
		usc_OutDmaReg( info, DCR, 0xa00b );
	}
	else
		usc_OutDmaReg( info, DCR, 0x800b );


	/* Receive DMA mode Register (RDMR)
	 *
	 * <15..14>	11	DMA mode = Linked List Buffer mode
	 * <13>		1	RSBinA/L = store Rx status Block in Arrary/List entry
	 * <12>		1	Clear count of List Entry after fetching
	 * <11..10>	00	Address mode = Increment
	 * <9>		1	Terminate Buffer on RxBound
	 * <8>		0	Bus Width = 16bits
	 * <7..0>	?	status Bits (write as 0s)
	 *
	 * 1111 0010 0000 0000 = 0xf200
	 */

	usc_OutDmaReg( info, RDMR, 0xf200 );


	/* Transmit DMA mode Register (TDMR)
	 *
	 * <15..14>	11	DMA mode = Linked List Buffer mode
	 * <13>		1	TCBinA/L = fetch Tx Control Block from List entry
	 * <12>		1	Clear count of List Entry after fetching
	 * <11..10>	00	Address mode = Increment
	 * <9>		1	Terminate Buffer on end of frame
	 * <8>		0	Bus Width = 16bits
	 * <7..0>	?	status Bits (Read Only so write as 0)
	 *
	 *	1111 0010 0000 0000 = 0xf200
	 */

	usc_OutDmaReg( info, TDMR, 0xf200 );


	/* DMA Interrupt Control Register (DICR)
	 *
	 * <15>		1	DMA Interrupt Enable
	 * <14>		0	1 = Disable IEO from USC
	 * <13>		0	1 = Don't provide vector during IntAck
	 * <12>		1	1 = Include status in Vector
	 * <10..2>	0	reserved, Must be 0s
	 * <1>		0	1 = Rx DMA Interrupt Enabled
	 * <0>		0	1 = Tx DMA Interrupt Enabled
	 *
	 *	1001 0000 0000 0000 = 0x9000
	 */

	usc_OutDmaReg( info, DICR, 0x9000 );

	usc_InDmaReg( info, RDMR );		/* clear pending receive DMA IRQ bits */
	usc_InDmaReg( info, TDMR );		/* clear pending transmit DMA IRQ bits */
	usc_OutDmaReg( info, CDIR, 0x0303 );	/* clear IUS and Pending for Tx and Rx */

	/* Channel Control Register (CCR)
	 *
	 * <15..14>	10	Use 32-bit Tx Control Blocks (TCBs)
	 * <13>		0	Trigger Tx on SW Command Disabled
	 * <12>		0	Flag Preamble Disabled
	 * <11..10>	00	Preamble Length
	 * <9..8>	00	Preamble Pattern
	 * <7..6>	10	Use 32-bit Rx status Blocks (RSBs)
	 * <5>		0	Trigger Rx on SW Command Disabled
	 * <4..0>	0	reserved
	 *
	 *	1000 0000 1000 0000 = 0x8080
	 */

	RegValue = 0x8080;

	switch ( info->params.preamble_length ) {
	case HDLC_PREAMBLE_LENGTH_16BITS: RegValue |= BIT10; break;
	case HDLC_PREAMBLE_LENGTH_32BITS: RegValue |= BIT11; break;
	case HDLC_PREAMBLE_LENGTH_64BITS: RegValue |= BIT11 + BIT10; break;
	}

	switch ( info->params.preamble ) {
	case HDLC_PREAMBLE_PATTERN_FLAGS: RegValue |= BIT8 + BIT12; break;
	case HDLC_PREAMBLE_PATTERN_ONES:  RegValue |= BIT8; break;
	case HDLC_PREAMBLE_PATTERN_10:    RegValue |= BIT9; break;
	case HDLC_PREAMBLE_PATTERN_01:    RegValue |= BIT9 + BIT8; break;
	}

	usc_OutReg( info, CCR, RegValue );


	/*
	 * Burst/Dwell Control Register
	 *
	 * <15..8>	0x20	Maximum number of transfers per bus grant
	 * <7..0>	0x00	Maximum number of clock cycles per bus grant
	 */

	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		/* don't limit bus occupancy on PCI adapter */
		usc_OutDmaReg( info, BDCR, 0x0000 );
	}
	else
		usc_OutDmaReg( info, BDCR, 0x2000 );

	usc_stop_transmitter(info);
	usc_stop_receiver(info);
	
}	/* end of usc_set_sdlc_mode() */

/* usc_enable_loopback()
 *
 * Set the 16C32 for internal loopback mode.
 * The TxCLK and RxCLK signals are generated from the BRG0 and
 * the TxD is looped back to the RxD internally.
 *
 * Arguments:		info	pointer to device instance data
 *			enable	1 = enable loopback, 0 = disable
 * Return Value:	None
 */
void usc_enable_loopback(struct mgsl_struct *info, int enable)
{
	if (enable) {
		/* blank external TXD output */
		usc_OutReg(info,IOCR,usc_InReg(info,IOCR) | (BIT7+BIT6));
	
		/* Clock mode Control Register (CMCR)
		 *
		 * <15..14>	00	counter 1 Disabled
		 * <13..12> 	00	counter 0 Disabled
		 * <11..10> 	11	BRG1 Input is TxC Pin
		 * <9..8>	11	BRG0 Input is TxC Pin
		 * <7..6>	01	DPLL Input is BRG1 Output
		 * <5..3>	100	TxCLK comes from BRG0
		 * <2..0>   	100	RxCLK comes from BRG0
		 *
		 * 0000 1111 0110 0100 = 0x0f64
		 */

		usc_OutReg( info, CMCR, 0x0f64 );

		/* Write 16-bit Time Constant for BRG0 */
		/* use clock speed if available, otherwise use 8 for diagnostics */
		if (info->params.clock_speed) {
			if (info->bus_type == MGSL_BUS_TYPE_PCI)
				usc_OutReg(info, TC0R, (u16)((11059200/info->params.clock_speed)-1));
			else
				usc_OutReg(info, TC0R, (u16)((14745600/info->params.clock_speed)-1));
		} else
			usc_OutReg(info, TC0R, (u16)8);

		/* Hardware Configuration Register (HCR) Clear Bit 1, BRG0
		   mode = Continuous Set Bit 0 to enable BRG0.  */
		usc_OutReg( info, HCR, (u16)((usc_InReg( info, HCR ) & ~BIT1) | BIT0) );

		/* Input/Output Control Reg, <2..0> = 100, Drive RxC pin with BRG0 */
		usc_OutReg(info, IOCR, (u16)((usc_InReg(info, IOCR) & 0xfff8) | 0x0004));

		/* set Internal Data loopback mode */
		info->loopback_bits = 0x300;
		outw( 0x0300, info->io_base + CCAR );
	} else {
		/* enable external TXD output */
		usc_OutReg(info,IOCR,usc_InReg(info,IOCR) & ~(BIT7+BIT6));
	
		/* clear Internal Data loopback mode */
		info->loopback_bits = 0;
		outw( 0,info->io_base + CCAR );
	}
	
}	/* end of usc_enable_loopback() */

/* usc_enable_aux_clock()
 *
 * Enabled the AUX clock output at the specified frequency.
 *
 * Arguments:
 *
 *	info		pointer to device extension
 *	data_rate	data rate of clock in bits per second
 *			A data rate of 0 disables the AUX clock.
 *
 * Return Value:	None
 */
void usc_enable_aux_clock( struct mgsl_struct *info, u32 data_rate )
{
	u32 XtalSpeed;
	u16 Tc;

	if ( data_rate ) {
		if ( info->bus_type == MGSL_BUS_TYPE_PCI )
			XtalSpeed = 11059200;
		else
			XtalSpeed = 14745600;


		/* Tc = (Xtal/Speed) - 1 */
		/* If twice the remainder of (Xtal/Speed) is greater than Speed */
		/* then rounding up gives a more precise time constant. Instead */
		/* of rounding up and then subtracting 1 we just don't subtract */
		/* the one in this case. */


		Tc = (u16)(XtalSpeed/data_rate);
		if ( !(((XtalSpeed % data_rate) * 2) / data_rate) )
			Tc--;

		/* Write 16-bit Time Constant for BRG0 */
		usc_OutReg( info, TC0R, Tc );

		/*
		 * Hardware Configuration Register (HCR)
		 * Clear Bit 1, BRG0 mode = Continuous
		 * Set Bit 0 to enable BRG0.
		 */

		usc_OutReg( info, HCR, (u16)((usc_InReg( info, HCR ) & ~BIT1) | BIT0) );

		/* Input/Output Control Reg, <2..0> = 100, Drive RxC pin with BRG0 */
		usc_OutReg( info, IOCR, (u16)((usc_InReg(info, IOCR) & 0xfff8) | 0x0004) );
	} else {
		/* data rate == 0 so turn off BRG0 */
		usc_OutReg( info, HCR, (u16)(usc_InReg( info, HCR ) & ~BIT0) );
	}

}	/* end of usc_enable_aux_clock() */

/*
 *
 * usc_process_rxoverrun_sync()
 *
 *		This function processes a receive overrun by resetting the
 *		receive DMA buffers and issuing a Purge Rx FIFO command
 *		to allow the receiver to continue receiving.
 *
 * Arguments:
 *
 *	info		pointer to device extension
 *
 * Return Value: None
 */
void usc_process_rxoverrun_sync( struct mgsl_struct *info )
{
	int start_index;
	int end_index;
	int frame_start_index;
	int start_of_frame_found = FALSE;
	int end_of_frame_found = FALSE;
	int reprogram_dma = FALSE;

	DMABUFFERENTRY *buffer_list = info->rx_buffer_list;
	u32 phys_addr;

	usc_DmaCmd( info, DmaCmd_PauseRxChannel );
	usc_RCmd( info, RCmd_EnterHuntmode );
	usc_RTCmd( info, RTCmd_PurgeRxFifo );

	/* CurrentRxBuffer points to the 1st buffer of the next */
	/* possibly available receive frame. */
	
	frame_start_index = start_index = end_index = info->current_rx_buffer;

	/* Search for an unfinished string of buffers. This means */
	/* that a receive frame started (at least one buffer with */
	/* count set to zero) but there is no terminiting buffer */
	/* (status set to non-zero). */

	while( !buffer_list[end_index].count )
	{
		/* Count field has been reset to zero by 16C32. */
		/* This buffer is currently in use. */

		if ( !start_of_frame_found )
		{
			start_of_frame_found = TRUE;
			frame_start_index = end_index;
			end_of_frame_found = FALSE;
		}

		if ( buffer_list[end_index].status )
		{
			/* Status field has been set by 16C32. */
			/* This is the last buffer of a received frame. */

			/* We want to leave the buffers for this frame intact. */
			/* Move on to next possible frame. */

			start_of_frame_found = FALSE;
			end_of_frame_found = TRUE;
		}

  		/* advance to next buffer entry in linked list */
  		end_index++;
  		if ( end_index == info->rx_buffer_count )
  			end_index = 0;

		if ( start_index == end_index )
		{
			/* The entire list has been searched with all Counts == 0 and */
			/* all Status == 0. The receive buffers are */
			/* completely screwed, reset all receive buffers! */
			mgsl_reset_rx_dma_buffers( info );
			frame_start_index = 0;
			start_of_frame_found = FALSE;
			reprogram_dma = TRUE;
			break;
		}
	}

	if ( start_of_frame_found && !end_of_frame_found )
	{
		/* There is an unfinished string of receive DMA buffers */
		/* as a result of the receiver overrun. */

		/* Reset the buffers for the unfinished frame */
		/* and reprogram the receive DMA controller to start */
		/* at the 1st buffer of unfinished frame. */

		start_index = frame_start_index;

		do
		{
			*((unsigned long *)&(info->rx_buffer_list[start_index++].count)) = DMABUFFERSIZE;

  			/* Adjust index for wrap around. */
  			if ( start_index == info->rx_buffer_count )
  				start_index = 0;

		} while( start_index != end_index );

		reprogram_dma = TRUE;
	}

	if ( reprogram_dma )
	{
		usc_UnlatchRxstatusBits(info,RXSTATUS_ALL);
		usc_ClearIrqPendingBits(info, RECEIVE_DATA|RECEIVE_STATUS);
		usc_UnlatchRxstatusBits(info, RECEIVE_DATA|RECEIVE_STATUS);
		
		usc_EnableReceiver(info,DISABLE_UNCONDITIONAL);
		
		/* This empties the receive FIFO and loads the RCC with RCLR */
		usc_OutReg( info, CCSR, (u16)(usc_InReg(info,CCSR) | BIT13) );

		/* program 16C32 with physical address of 1st DMA buffer entry */
		phys_addr = info->rx_buffer_list[frame_start_index].phys_entry;
		usc_OutDmaReg( info, NRARL, (u16)phys_addr );
		usc_OutDmaReg( info, NRARU, (u16)(phys_addr >> 16) );

		usc_UnlatchRxstatusBits( info, RXSTATUS_ALL );
		usc_ClearIrqPendingBits( info, RECEIVE_DATA + RECEIVE_STATUS );
		usc_EnableInterrupts( info, RECEIVE_STATUS );

		/* 1. Arm End of Buffer (EOB) Receive DMA Interrupt (BIT2 of RDIAR) */
		/* 2. Enable Receive DMA Interrupts (BIT1 of DICR) */

		usc_OutDmaReg( info, RDIAR, BIT3 + BIT2 );
		usc_OutDmaReg( info, DICR, (u16)(usc_InDmaReg(info,DICR) | BIT1) );
		usc_DmaCmd( info, DmaCmd_InitRxChannel );
		if ( info->params.flags & HDLC_FLAG_AUTO_DCD )
			usc_EnableReceiver(info,ENABLE_AUTO_DCD);
		else
			usc_EnableReceiver(info,ENABLE_UNCONDITIONAL);
	}
	else
	{
		/* This empties the receive FIFO and loads the RCC with RCLR */
		usc_OutReg( info, CCSR, (u16)(usc_InReg(info,CCSR) | BIT13) );
		usc_RTCmd( info, RTCmd_PurgeRxFifo );
	}

}	/* end of usc_process_rxoverrun_sync() */

/* usc_stop_receiver()
 *
 *	Disable USC receiver
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_stop_receiver( struct mgsl_struct *info )
{
	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):usc_stop_receiver(%s)\n",
			 __FILE__,__LINE__, info->device_name );
			 
	/* Disable receive DMA channel. */
	/* This also disables receive DMA channel interrupts */
	usc_DmaCmd( info, DmaCmd_ResetRxChannel );

	usc_UnlatchRxstatusBits( info, RXSTATUS_ALL );
	usc_ClearIrqPendingBits( info, RECEIVE_DATA + RECEIVE_STATUS );
	usc_DisableInterrupts( info, RECEIVE_DATA + RECEIVE_STATUS );

	usc_EnableReceiver(info,DISABLE_UNCONDITIONAL);

	/* This empties the receive FIFO and loads the RCC with RCLR */
	usc_OutReg( info, CCSR, (u16)(usc_InReg(info,CCSR) | BIT13) );
	usc_RTCmd( info, RTCmd_PurgeRxFifo );

	info->rx_enabled = 0;
	info->rx_overflow = 0;
	
}	/* end of stop_receiver() */

/* usc_start_receiver()
 *
 *	Enable the USC receiver 
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_start_receiver( struct mgsl_struct *info )
{
	u32 phys_addr;
	
	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):usc_start_receiver(%s)\n",
			 __FILE__,__LINE__, info->device_name );

	mgsl_reset_rx_dma_buffers( info );
	usc_stop_receiver( info );

	usc_OutReg( info, CCSR, (u16)(usc_InReg(info,CCSR) | BIT13) );
	usc_RTCmd( info, RTCmd_PurgeRxFifo );

	if ( info->params.mode == MGSL_MODE_HDLC ) {
		/* DMA mode Transfers */
		/* Program the DMA controller. */
		/* Enable the DMA controller end of buffer interrupt. */

		/* program 16C32 with physical address of 1st DMA buffer entry */
		phys_addr = info->rx_buffer_list[0].phys_entry;
		usc_OutDmaReg( info, NRARL, (u16)phys_addr );
		usc_OutDmaReg( info, NRARU, (u16)(phys_addr >> 16) );

		usc_UnlatchRxstatusBits( info, RXSTATUS_ALL );
		usc_ClearIrqPendingBits( info, RECEIVE_DATA + RECEIVE_STATUS );
		usc_EnableInterrupts( info, RECEIVE_STATUS );

		/* 1. Arm End of Buffer (EOB) Receive DMA Interrupt (BIT2 of RDIAR) */
		/* 2. Enable Receive DMA Interrupts (BIT1 of DICR) */

		usc_OutDmaReg( info, RDIAR, BIT3 + BIT2 );
		usc_OutDmaReg( info, DICR, (u16)(usc_InDmaReg(info,DICR) | BIT1) );
		usc_DmaCmd( info, DmaCmd_InitRxChannel );
		if ( info->params.flags & HDLC_FLAG_AUTO_DCD )
			usc_EnableReceiver(info,ENABLE_AUTO_DCD);
		else
			usc_EnableReceiver(info,ENABLE_UNCONDITIONAL);
	} else {
		usc_UnlatchRxstatusBits(info, RXSTATUS_ALL);
		usc_ClearIrqPendingBits(info, RECEIVE_DATA + RECEIVE_STATUS);
		usc_EnableInterrupts(info, RECEIVE_DATA);

		usc_RTCmd( info, RTCmd_PurgeRxFifo );
		usc_RCmd( info, RCmd_EnterHuntmode );

		usc_EnableReceiver(info,ENABLE_UNCONDITIONAL);
	}

	usc_OutReg( info, CCSR, 0x1020 );

	info->rx_enabled = 1;

}	/* end of usc_start_receiver() */

/* usc_start_transmitter()
 *
 *	Enable the USC transmitter and send a transmit frame if
 *	one is loaded in the DMA buffers.
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_start_transmitter( struct mgsl_struct *info )
{
	u32 phys_addr;
	unsigned int FrameSize;

	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):usc_start_transmitter(%s)\n",
			 __FILE__,__LINE__, info->device_name );
			 
	if ( info->xmit_cnt ) {

		/* If auto RTS enabled and RTS is inactive, then assert */
		/* RTS and set a flag indicating that the driver should */
		/* negate RTS when the transmission completes. */

		info->drop_rts_on_tx_done = 0;

		if ( info->params.flags & HDLC_FLAG_AUTO_RTS ) {
			usc_get_serial_signals( info );
			if ( !(info->serial_signals & SerialSignal_RTS) ) {
				info->serial_signals |= SerialSignal_RTS;
				usc_set_serial_signals( info );
				info->drop_rts_on_tx_done = 1;
			}
		}


		if ( info->params.mode == MGSL_MODE_ASYNC ) {
			if ( !info->tx_active ) {
				usc_UnlatchTxstatusBits(info, TXSTATUS_ALL);
				usc_ClearIrqPendingBits(info, TRANSMIT_STATUS + TRANSMIT_DATA);
				usc_EnableInterrupts(info, TRANSMIT_DATA);
				usc_load_txfifo(info);
			}
		} else {
			/* Disable transmit DMA controller while programming. */
			usc_DmaCmd( info, DmaCmd_ResetTxChannel );
			
			/* Transmit DMA buffer is loaded, so program USC */
			/* to send the frame contained in the buffers.	 */


			FrameSize = info->tx_buffer_list[0].rcc;

			/* Program the Transmit Character Length Register (TCLR) */
			/* and clear FIFO (TCC is loaded with TCLR on FIFO clear) */
			usc_OutReg( info, TCLR, (u16)FrameSize );

			usc_RTCmd( info, RTCmd_PurgeTxFifo );

			/* Program the address of the 1st DMA Buffer Entry in linked list */
			phys_addr = info->tx_buffer_list[0].phys_entry;
			usc_OutDmaReg( info, NTARL, (u16)phys_addr );
			usc_OutDmaReg( info, NTARU, (u16)(phys_addr >> 16) );

			usc_UnlatchTxstatusBits( info, TXSTATUS_ALL );
			usc_ClearIrqPendingBits( info, TRANSMIT_STATUS );
			usc_EnableInterrupts( info, TRANSMIT_STATUS );

			/* Initialize Transmit DMA Channel */
			usc_DmaCmd( info, DmaCmd_InitTxChannel );
			
			usc_TCmd( info, TCmd_SendFrame );
			
			info->tx_timer.expires = jiffies + jiffies_from_ms(5000);
			add_timer(&info->tx_timer);	
		}
		info->tx_active = 1;
	}

	if ( !info->tx_enabled ) {
		info->tx_enabled = 1;
		if ( info->params.flags & HDLC_FLAG_AUTO_CTS )
			usc_EnableTransmitter(info,ENABLE_AUTO_CTS);
		else
			usc_EnableTransmitter(info,ENABLE_UNCONDITIONAL);
	}

}	/* end of usc_start_transmitter() */

/* usc_stop_transmitter()
 *
 *	Stops the transmitter and DMA
 *
 * Arguments:		info	pointer to device isntance data
 * Return Value:	None
 */
void usc_stop_transmitter( struct mgsl_struct *info )
{
	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):usc_stop_transmitter(%s)\n",
			 __FILE__,__LINE__, info->device_name );
			 
	del_timer(&info->tx_timer);	
			 
	usc_UnlatchTxstatusBits( info, TXSTATUS_ALL );
	usc_ClearIrqPendingBits( info, TRANSMIT_STATUS + TRANSMIT_DATA );
	usc_DisableInterrupts( info, TRANSMIT_STATUS + TRANSMIT_DATA );

	usc_EnableTransmitter(info,DISABLE_UNCONDITIONAL);
	usc_DmaCmd( info, DmaCmd_ResetTxChannel );
	usc_RTCmd( info, RTCmd_PurgeTxFifo );

	info->tx_enabled = 0;
	info->tx_active  = 0;

}	/* end of usc_stop_transmitter() */

/* usc_load_txfifo()
 *
 *	Fill the transmit FIFO until the FIFO is full or
 *	there is no more data to load.
 *
 * Arguments:		info	pointer to device extension (instance data)
 * Return Value:	None
 */
void usc_load_txfifo( struct mgsl_struct *info )
{
	int Fifocount;
	u8 TwoBytes[2];
	
	if ( !info->xmit_cnt && !info->x_char )
		return; 
		
	/* Select transmit FIFO status readback in TICR */
	usc_TCmd( info, TCmd_SelectTicrTxFifostatus );

	/* load the Transmit FIFO until FIFOs full or all data sent */

	while( (Fifocount = usc_InReg(info, TICR) >> 8) && info->xmit_cnt ) {
		/* there is more space in the transmit FIFO and */
		/* there is more data in transmit buffer */

		if ( (info->xmit_cnt > 1) && (Fifocount > 1) && !info->x_char ) {
 			/* write a 16-bit word from transmit buffer to 16C32 */
				
			TwoBytes[0] = info->xmit_buf[info->xmit_tail++];
			info->xmit_tail = info->xmit_tail & (SERIAL_XMIT_SIZE-1);
			TwoBytes[1] = info->xmit_buf[info->xmit_tail++];
			info->xmit_tail = info->xmit_tail & (SERIAL_XMIT_SIZE-1);
			
			outw( *((u16 *)TwoBytes), info->io_base + DATAREG);
				
			info->xmit_cnt -= 2;
			info->icount.tx += 2;
		} else {
			/* only 1 byte left to transmit or 1 FIFO slot left */
			
			outw( (inw( info->io_base + CCAR) & 0x0780) | (TDR+LSBONLY),
				info->io_base + CCAR );
			
			if (info->x_char) {
				/* transmit pending high priority char */
				outw( info->x_char,info->io_base + CCAR );
				info->x_char = 0;
			} else {
				outw( info->xmit_buf[info->xmit_tail++],info->io_base + CCAR );
				info->xmit_tail = info->xmit_tail & (SERIAL_XMIT_SIZE-1);
				info->xmit_cnt--;
			}
			info->icount.tx++;
		}
	}

}	/* end of usc_load_txfifo() */

/* usc_reset()
 *
 *	Reset the adapter to a known state and prepare it for further use.
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_reset( struct mgsl_struct *info )
{
	if ( info->bus_type == MGSL_BUS_TYPE_PCI ) {
		int i;
		volatile u32 readval;

		/* Set BIT30 of Misc Control Register */
		/* (Local Control Register 0x50) to force reset of USC. */

		u32 *MiscCtrl = (u32 *)(info->lcr_base + 0x50);
		u32 *LCR0BRDR = (u32 *)(info->lcr_base + 0x28);

		info->misc_ctrl_value |= BIT30;
		*MiscCtrl = info->misc_ctrl_value;

		/*
		 * Force at least 170ns delay before clearing 
		 * reset bit. Each read from LCR takes at least 
		 * 30ns so 10 times for 300ns to be safe.
		 */
		for(i=0;i<10;i++)
			readval = *MiscCtrl;

		info->misc_ctrl_value &= ~BIT30;
		*MiscCtrl = info->misc_ctrl_value;

		*LCR0BRDR = BUS_DESCRIPTOR(
			1,		// Write Strobe Hold (0-3)
			2,		// Write Strobe Delay (0-3)
			2,		// Read Strobe Delay  (0-3)
			0,		// NWDD (Write data-data) (0-3)
			4,		// NWAD (Write Addr-data) (0-31)
			0,		// NXDA (Read/Write Data-Addr) (0-3)
			0,		// NRDD (Read Data-Data) (0-3)
			5		// NRAD (Read Addr-Data) (0-31)
			);
	} else {
		/* do HW reset */
		outb( 0,info->io_base + 8 );
	}

	info->mbre_bit = 0;
	info->loopback_bits = 0;
	info->usc_idle_mode = 0;

	/*
	 * Program the Bus Configuration Register (BCR)
	 *
	 * <15>		0	Don't use seperate address
	 * <14..6>	0	reserved
	 * <5..4>	00	IAckmode = Default, don't care
	 * <3>		1	Bus Request Totem Pole output
	 * <2>		1	Use 16 Bit data bus
	 * <1>		0	IRQ Totem Pole output
	 * <0>		0	Don't Shift Right Addr
	 *
	 * 0000 0000 0000 1100 = 0x000c
	 *
	 * By writing to io_base + SDPIN the Wait/Ack pin is
	 * programmed to work as a Wait pin.
	 */
	
	outw( 0x000c,info->io_base + SDPIN );


	outw( 0,info->io_base );
	outw( 0,info->io_base + CCAR );

	/* select little endian byte ordering */
	usc_RTCmd( info, RTCmd_SelectLittleEndian );


	/* Port Control Register (PCR)
	 *
	 * <15..14>	11	Port 7 is Output (~DMAEN, Bit 14 : 0 = Enabled)
	 * <13..12>	11	Port 6 is Output (~INTEN, Bit 12 : 0 = Enabled)
	 * <11..10> 	00	Port 5 is Input (No Connect, Don't Care)
	 * <9..8> 	00	Port 4 is Input (No Connect, Don't Care)
	 * <7..6>	11	Port 3 is Output (~RTS, Bit 6 : 0 = Enabled )
	 * <5..4>	11	Port 2 is Output (~DTR, Bit 4 : 0 = Enabled )
	 * <3..2>	01	Port 1 is Input (Dedicated RxC)
	 * <1..0>	01	Port 0 is Input (Dedicated TxC)
	 *
	 *	1111 0000 1111 0101 = 0xf0f5
	 */

	usc_OutReg( info, PCR, 0xf0f5 );


	/*
	 * Input/Output Control Register
	 *
	 * <15..14>	00	CTS is active low input
	 * <13..12>	00	DCD is active low input
	 * <11..10>	00	TxREQ pin is input (DSR)
	 * <9..8>	00	RxREQ pin is input (RI)
	 * <7..6>	00	TxD is output (Transmit Data)
	 * <5..3>	000	TxC Pin in Input (14.7456MHz Clock)
	 * <2..0>	100	RxC is Output (drive with BRG0)
	 *
	 *	0000 0000 0000 0100 = 0x0004
	 */

	usc_OutReg( info, IOCR, 0x0004 );

}	/* end of usc_reset() */

/* usc_set_async_mode()
 *
 *	Program adapter for asynchronous communications.
 *
 * Arguments:		info		pointer to device instance data
 * Return Value:	None
 */
void usc_set_async_mode( struct mgsl_struct *info )
{
	u16 RegValue;

	/* disable interrupts while programming USC */
	usc_DisableMasterIrqBit( info );

	outw( 0, info->io_base ); 			/* clear Master Bus Enable (DCAR) */
	usc_DmaCmd( info, DmaCmd_ResetAllChannels );	/* disable both DMA channels */

	usc_loopback_frame( info );

	/* Channel mode Register (CMR)
	 *
	 * <15..14>	00	Tx Sub modes, 00 = 1 Stop Bit
	 * <13..12>	00	              00 = 16X Clock
	 * <11..8>	0000	Transmitter mode = Asynchronous
	 * <7..6>	00	reserved?
	 * <5..4>	00	Rx Sub modes, 00 = 16X Clock
	 * <3..0>	0000	Receiver mode = Asynchronous
	 *
	 * 0000 0000 0000 0000 = 0x0
	 */

	RegValue = 0;
	if ( info->params.stop_bits != 1 )
		RegValue |= BIT14;
	usc_OutReg( info, CMR, RegValue );

	
	/* Receiver mode Register (RMR)
	 *
	 * <15..13>	000	encoding = None
	 * <12..08>	00000	reserved (Sync Only)
	 * <7..6>   	00	Even parity
	 * <5>		0	parity disabled
	 * <4..2>	000	Receive Char Length = 8 bits
	 * <1..0>	00	Disable Receiver
	 *
	 * 0000 0000 0000 0000 = 0x0
	 */

	RegValue = 0;

	if ( info->params.data_bits != 8 )
		RegValue |= BIT4+BIT3+BIT2;

	if ( info->params.parity != ASYNC_PARITY_NONE ) {
		RegValue |= BIT5;
		if ( info->params.parity != ASYNC_PARITY_ODD )
			RegValue |= BIT6;
	}

	usc_OutReg( info, RMR, RegValue );


	/* Set IRQ trigger level */

	usc_RCmd( info, RCmd_SelectRicrIntLevel );

	
	/* Receive Interrupt Control Register (RICR)
	 *
	 * <15..8>	?		RxFIFO IRQ Request Level
	 *
	 * Note: For async mode the receive FIFO level must be set
	 * to 0 to aviod the situation where the FIFO contains fewer bytes
	 * than the trigger level and no more data is expected.
	 *
	 * <7>		0		Exited Hunt IA (Interrupt Arm)
	 * <6>		0		Idle Received IA
	 * <5>		0		Break/Abort IA
	 * <4>		0		Rx Bound IA
	 * <3>		0		Queued status reflects oldest byte in FIFO
	 * <2>		0		Abort/PE IA
	 * <1>		0		Rx Overrun IA
	 * <0>		0		Select TC0 value for readback
	 *
	 * 0000 0000 0100 0000 = 0x0000 + (FIFOLEVEL in MSB)
	 */
	
	usc_OutReg( info, RICR, 0x0000 );

	usc_UnlatchRxstatusBits( info, RXSTATUS_ALL );
	usc_ClearIrqPendingBits( info, RECEIVE_STATUS );

	
	/* Transmit mode Register (TMR)
	 *
	 * <15..13>	000	encoding = None
	 * <12..08>	00000	reserved (Sync Only)
	 * <7..6>	00	Transmit parity Even
	 * <5>		0	Transmit parity Disabled
	 * <4..2>	000	Tx Char Length = 8 bits
	 * <1..0>	00	Disable Transmitter
	 *
	 * 0000 0000 0000 0000 = 0x0
	 */

	RegValue = 0;

	if ( info->params.data_bits != 8 )
		RegValue |= BIT4+BIT3+BIT2;

	if ( info->params.parity != ASYNC_PARITY_NONE ) {
		RegValue |= BIT5;
		if ( info->params.parity != ASYNC_PARITY_ODD )
			RegValue |= BIT6;
	}

	usc_OutReg( info, TMR, RegValue );

	usc_set_txidle( info );


	/* Set IRQ trigger level */

	usc_TCmd( info, TCmd_SelectTicrIntLevel );

	
	/* Transmit Interrupt Control Register (TICR)
	 *
	 * <15..8>	?	Transmit FIFO IRQ Level
	 * <7>		0	Present IA (Interrupt Arm)
	 * <6>		1	Idle Sent IA
	 * <5>		0	Abort Sent IA
	 * <4>		0	EOF/EOM Sent IA
	 * <3>		0	CRC Sent IA
	 * <2>		0	1 = Wait for SW Trigger to Start Frame
	 * <1>		0	Tx Underrun IA
	 * <0>		0	TC0 constant on read back
	 *
	 *	0000 0000 0100 0000 = 0x0040
	 */

	usc_OutReg( info, TICR, 0x1f40 );

	usc_UnlatchTxstatusBits( info, TXSTATUS_ALL );
	usc_ClearIrqPendingBits( info, TRANSMIT_STATUS );

	usc_enable_async_clock( info, info->params.data_rate );

	
	/* Channel Control/status Register (CCSR)
	 *
	 * <15>		X	RCC FIFO Overflow status (RO)
	 * <14>		X	RCC FIFO Not Empty status (RO)
	 * <13>		0	1 = Clear RCC FIFO (WO)
	 * <12>		X	DPLL in Sync status (RO)
	 * <11>		X	DPLL 2 Missed Clocks status (RO)
	 * <10>		X	DPLL 1 Missed Clock status (RO)
	 * <9..8>	00	DPLL Resync on rising and falling edges (RW)
	 * <7>		X	SDLC Loop On status (RO)
	 * <6>		X	SDLC Loop Send status (RO)
	 * <5>		1	Bypass counters for TxClk and RxClk (RW)
	 * <4..2>   	000	Last Char of SDLC frame has 8 bits (RW)
	 * <1..0>   	00	reserved
	 *
	 *	0000 0000 0010 0000 = 0x0020
	 */
	
	usc_OutReg( info, CCSR, 0x0020 );

	usc_DisableInterrupts( info, TRANSMIT_STATUS + TRANSMIT_DATA +
			      RECEIVE_DATA + RECEIVE_STATUS );

	usc_ClearIrqPendingBits( info, TRANSMIT_STATUS + TRANSMIT_DATA +
				RECEIVE_DATA + RECEIVE_STATUS );

	usc_EnableMasterIrqBit( info );

	/* Enable INTEN (Port 6, Bit12) */
	/* This connects the IRQ request signal to the ISA bus */
	/* on the ISA adapter. This has no effect for the PCI adapter */
	usc_OutReg( info, PCR, (u16)((usc_InReg(info, PCR) | BIT13) & ~BIT12) );

}	/* end of usc_set_async_mode() */

/* usc_loopback_frame()
 *
 *	Loop back a small (2 byte) dummy SDLC frame.
 *	Interrupts and DMA are NOT used. The purpose of this is to
 *	clear any 'stale' status info left over from running in	async mode.
 *
 *	The 16C32 shows the strange behaviour of marking the 1st
 *	received SDLC frame with a CRC error even when there is no
 *	CRC error. To get around this a small dummy from of 2 bytes
 *	is looped back when switching from async to sync mode.
 *
 * Arguments:		info		pointer to device instance data
 * Return Value:	None
 */
void usc_loopback_frame( struct mgsl_struct *info )
{
	int i;
	
	usc_DisableMasterIrqBit( info );

	usc_set_sdlc_mode( info );
	usc_enable_loopback( info, 1 );

	/* Write 16-bit Time Constant for BRG0 */
	usc_OutReg( info, TC0R, 0 );
	
	/* Channel Control Register (CCR)
	 *
	 * <15..14>	00	Don't use 32-bit Tx Control Blocks (TCBs)
	 * <13>		0	Trigger Tx on SW Command Disabled
	 * <12>		0	Flag Preamble Disabled
	 * <11..10>	00	Preamble Length = 8-Bits
	 * <9..8>	01	Preamble Pattern = flags
	 * <7..6>	10	Don't use 32-bit Rx status Blocks (RSBs)
	 * <5>		0	Trigger Rx on SW Command Disabled
	 * <4..0>	0	reserved
	 *
	 *	0000 0001 0000 0000 = 0x0100
	 */

	usc_OutReg( info, CCR, 0x0100 );

	/* SETUP RECEIVER */
	usc_RTCmd( info, RTCmd_PurgeRxFifo );
	usc_EnableReceiver(info,ENABLE_UNCONDITIONAL);

	/* SETUP TRANSMITTER */
	/* Program the Transmit Character Length Register (TCLR) */
	/* and clear FIFO (TCC is loaded with TCLR on FIFO clear) */
	usc_OutReg( info, TCLR, 2 );
	usc_RTCmd( info, RTCmd_PurgeTxFifo );

	/* unlatch Tx status bits, and start transmit channel. */
	usc_UnlatchTxstatusBits(info,TXSTATUS_ALL);
	outw(0,info->io_base + DATAREG);

	/* ENABLE TRANSMITTER */
	usc_TCmd( info, TCmd_SendFrame );
	usc_EnableTransmitter(info,ENABLE_UNCONDITIONAL);
							
	/* WAIT FOR RECEIVE COMPLETE */
	for (i=0 ; i<1000 ; i++)
		if (usc_InReg( info, RCSR ) & (BIT8 + BIT4 + BIT3 + BIT1))
			break;

	/* clear Internal Data loopback mode */
	usc_enable_loopback(info, 0);

	usc_EnableMasterIrqBit(info);

}	/* end of usc_loopback_frame() */

/* usc_set_sync_mode()	Programs the USC for SDLC communications.
 *
 * Arguments:		info	pointer to adapter info structure
 * Return Value:	None
 */
void usc_set_sync_mode( struct mgsl_struct *info )
{
	usc_loopback_frame( info );
	usc_set_sdlc_mode( info );

	/* Enable INTEN (Port 6, Bit12) */
	/* This connects the IRQ request signal to the ISA bus */
	/* on the ISA adapter. This has no effect for the PCI adapter */
	usc_OutReg(info, PCR, (u16)((usc_InReg(info, PCR) | BIT13) & ~BIT12));

	usc_enable_aux_clock(info, info->params.clock_speed);

	if (info->params.loopback)
		usc_enable_loopback(info,1);

}	/* end of mgsl_set_sync_mode() */

/* usc_set_txidle()	Set the HDLC idle mode for the transmitter.
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_set_txidle( struct mgsl_struct *info )
{
	u16 usc_idle_mode = IDLEMODE_FLAGS;

	/* Map API idle mode to USC register bits */

	switch( info->idle_mode ){
	case HDLC_TXIDLE_FLAGS:			usc_idle_mode = IDLEMODE_FLAGS; break;
	case HDLC_TXIDLE_ALT_ZEROS_ONES:	usc_idle_mode = IDLEMODE_ALT_ONE_ZERO; break;
	case HDLC_TXIDLE_ZEROS:			usc_idle_mode = IDLEMODE_ZERO; break;
	case HDLC_TXIDLE_ONES:			usc_idle_mode = IDLEMODE_ONE; break;
	case HDLC_TXIDLE_ALT_MARK_SPACE:	usc_idle_mode = IDLEMODE_ALT_MARK_SPACE; break;
	case HDLC_TXIDLE_SPACE:			usc_idle_mode = IDLEMODE_SPACE; break;
	case HDLC_TXIDLE_MARK:			usc_idle_mode = IDLEMODE_MARK; break;
	}

	info->usc_idle_mode = usc_idle_mode;
	//usc_OutReg(info, TCSR, usc_idle_mode);
	info->tcsr_value &= ~IDLEMODE_MASK;	/* clear idle mode bits */
	info->tcsr_value += usc_idle_mode;
	usc_OutReg(info, TCSR, info->tcsr_value);

}	/* end of usc_set_txidle() */

/* usc_get_serial_signals()
 *
 *	Query the adapter for the state of the V24 status (input) signals.
 *
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_get_serial_signals( struct mgsl_struct *info )
{
	u16 status;

	/* clear all serial signals except DTR and RTS */
	info->serial_signals &= SerialSignal_DTR + SerialSignal_RTS;

	/* Read the Misc Interrupt status Register (MISR) to get */
	/* the V24 status signals. */

	status = usc_InReg( info, MISR );

	/* set serial signal bits to reflect MISR */

	if ( status & MISCSTATUS_CTS )
		info->serial_signals |= SerialSignal_CTS;

	if ( status & MISCSTATUS_DCD )
		info->serial_signals |= SerialSignal_DCD;

	if ( status & MISCSTATUS_RI )
		info->serial_signals |= SerialSignal_RI;

	if ( status & MISCSTATUS_DSR )
		info->serial_signals |= SerialSignal_DSR;

}	/* end of usc_get_serial_signals() */

/* usc_set_serial_signals()
 *
 *	Set the state of DTR and RTS based on contents of
 *	serial_signals member of device extension.
 *	
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void usc_set_serial_signals( struct mgsl_struct *info )
{
	u16 Control;
	unsigned char V24Out = info->serial_signals;

	/* get the current value of the Port Control Register (PCR) */

	Control = usc_InReg( info, PCR );

	if ( V24Out & SerialSignal_RTS )
		Control &= ~(BIT6);
	else
		Control |= BIT6;

	if ( V24Out & SerialSignal_DTR )
		Control &= ~(BIT4);
	else
		Control |= BIT4;

	usc_OutReg( info, PCR, Control );

}	/* end of usc_set_serial_signals() */

/* usc_enable_async_clock()
 *
 *	Enable the async clock at the specified frequency.
 *
 * Arguments:		info		pointer to device instance data
 *			data_rate	data rate of clock in bps
 *					0 disables the AUX clock.
 * Return Value:	None
 */
void usc_enable_async_clock( struct mgsl_struct *info, u32 data_rate )
{
	if ( data_rate )	{
		/*
		 * Clock mode Control Register (CMCR)
		 * 
		 * <15..14>     00      counter 1 Disabled
		 * <13..12>     00      counter 0 Disabled
		 * <11..10>     11      BRG1 Input is TxC Pin
		 * <9..8>       11      BRG0 Input is TxC Pin
		 * <7..6>       01      DPLL Input is BRG1 Output
		 * <5..3>       100     TxCLK comes from BRG0
		 * <2..0>       100     RxCLK comes from BRG0
		 *
		 * 0000 1111 0110 0100 = 0x0f64
		 */
		
		usc_OutReg( info, CMCR, 0x0f64 );


		/*
		 * Write 16-bit Time Constant for BRG0
		 * Time Constant = (ClkSpeed / data_rate) - 1
		 * ClkSpeed = 921600 (ISA), 691200 (PCI)
		 */

		if ( info->bus_type == MGSL_BUS_TYPE_PCI )
			usc_OutReg( info, TC0R, (u16)((691200/data_rate) - 1) );
		else
			usc_OutReg( info, TC0R, (u16)((921600/data_rate) - 1) );

		
		/*
		 * Hardware Configuration Register (HCR)
		 * Clear Bit 1, BRG0 mode = Continuous
		 * Set Bit 0 to enable BRG0.
		 */

		usc_OutReg( info, HCR,
			    (u16)((usc_InReg( info, HCR ) & ~BIT1) | BIT0) );


		/* Input/Output Control Reg, <2..0> = 100, Drive RxC pin with BRG0 */

		usc_OutReg( info, IOCR,
			    (u16)((usc_InReg(info, IOCR) & 0xfff8) | 0x0004) );
	} else {
		/* data rate == 0 so turn off BRG0 */
		usc_OutReg( info, HCR, (u16)(usc_InReg( info, HCR ) & ~BIT0) );
	}

}	/* end of usc_enable_async_clock() */

/*
 * Buffer Structures:
 *
 * Normal memory access uses virtual addresses that can make discontiguous
 * physical memory pages appear to be contiguous in the virtual address
 * space (the processors memory mapping handles the conversions).
 *
 * DMA transfers require physically contiguous memory. This is because
 * the DMA system controller and DMA bus masters deal with memory using
 * only physical addresses.
 *
 * This causes a problem under Windows NT when large DMA buffers are
 * needed. Fragmentation of the nonpaged pool prevents allocations of
 * physically contiguous buffers larger than the PAGE_SIZE.
 *
 * However the 16C32 supports Bus Master Scatter/Gather DMA which
 * allows DMA transfers to physically discontiguous buffers. Information
 * about each data transfer buffer is contained in a memory structure
 * called a 'buffer entry'. A list of buffer entries is maintained
 * to track and control the use of the data transfer buffers.
 *
 * To support this strategy we will allocate sufficient PAGE_SIZE
 * contiguous memory buffers to allow for the total required buffer
 * space.
 *
 * The 16C32 accesses the list of buffer entries using Bus Master
 * DMA. Control information is read from the buffer entries by the
 * 16C32 to control data transfers. status information is written to
 * the buffer entries by the 16C32 to indicate the status of completed
 * transfers.
 *
 * The CPU writes control information to the buffer entries to control
 * the 16C32 and reads status information from the buffer entries to
 * determine information about received and transmitted frames.
 *
 * Because the CPU and 16C32 (adapter) both need simultaneous access
 * to the buffer entries, the buffer entry memory is allocated with
 * HalAllocateCommonBuffer(). This restricts the size of the buffer
 * entry list to PAGE_SIZE.
 *
 * The actual data buffers on the other hand will only be accessed
 * by the CPU or the adapter but not by both simultaneously. This allows
 * Scatter/Gather packet based DMA procedures for using physically
 * discontiguous pages.
 */

/*
 * mgsl_reset_rx_dma_buffers()
 * 
 * 	Set the count for all receive buffers to DMABUFFERSIZE
 * 	and set the current buffer to the first buffer. This effectively
 * 	makes all buffers free and discards any data in buffers.
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	None
 */
void mgsl_reset_rx_dma_buffers( struct mgsl_struct *info )
{
	unsigned int i;

	for ( i = 0; i < info->rx_buffer_count; i++ ) {
		*((unsigned long *)&(info->rx_buffer_list[i].count)) = DMABUFFERSIZE;
//		info->rx_buffer_list[i].count = DMABUFFERSIZE;
//		info->rx_buffer_list[i].status = 0;
	}

	info->current_rx_buffer = 0;

}	/* end of mgsl_reset_rx_dma_buffers() */

/*
 * mgsl_free_rx_frame_buffers()
 * 
 * 	Free the receive buffers used by a received SDLC
 * 	frame such that the buffers can be reused.
 * 
 * Arguments:
 * 
 * 	info			pointer to device instance data
 * 	StartIndex		index of 1st receive buffer of frame
 * 	EndIndex		index of last receive buffer of frame
 * 
 * Return Value:	None
 */
void mgsl_free_rx_frame_buffers( struct mgsl_struct *info, unsigned int StartIndex, unsigned int EndIndex )
{
	int Done = 0;
	DMABUFFERENTRY *pBufEntry;
	unsigned int Index;

	/* Starting with 1st buffer entry of the frame clear the status */
	/* field and set the count field to DMA Buffer Size. */

	Index = StartIndex;

	while( !Done ) {
		pBufEntry = &(info->rx_buffer_list[Index]);

		if ( Index == EndIndex ) {
			/* This is the last buffer of the frame! */
			Done = 1;
		}

		/* reset current buffer for reuse */
//		pBufEntry->status = 0;
//		pBufEntry->count = DMABUFFERSIZE;
		*((unsigned long *)&(pBufEntry->count)) = DMABUFFERSIZE;

		/* advance to next buffer entry in linked list */
		Index++;
		if ( Index == info->rx_buffer_count )
			Index = 0;
	}

	/* set current buffer to next buffer after last buffer of frame */
	info->current_rx_buffer = Index;

}	/* end of free_rx_frame_buffers() */

/* mgsl_get_rx_frame()
 * 
 * 	This function attempts to return a received SDLC frame from the
 * 	receive DMA buffers. Only frames received without errors are returned.
 *
 * Arguments:	 	info	pointer to device extension
 * Return Value:	1 if frame returned, otherwise 0
 */
int mgsl_get_rx_frame(struct mgsl_struct *info)
{
	unsigned int StartIndex, EndIndex;	/* index of 1st and last buffers of Rx frame */
	unsigned short status;
	DMABUFFERENTRY *pBufEntry;
	unsigned int framesize;
	int ReturnCode = 0;
	unsigned long flags;
	struct tty_struct *tty = info->tty;
	
	/*
	 * current_rx_buffer points to the 1st buffer of the next available
	 * receive frame. To find the last buffer of the frame look for
	 * a non-zero status field in the buffer entries. (The status
	 * field is set by the 16C32 after completing a receive frame.
	 */

	StartIndex = EndIndex = info->current_rx_buffer;

	while( !info->rx_buffer_list[EndIndex].status ) {
		/*
		 * If the count field of the buffer entry is non-zero then
		 * this buffer has not been used. (The 16C32 clears the count
		 * field when it starts using the buffer.) If an unused buffer
		 * is encountered then there are no frames available.
		 */

		if ( info->rx_buffer_list[EndIndex].count )
			goto Cleanup;

		/* advance to next buffer entry in linked list */
		EndIndex++;
		if ( EndIndex == info->rx_buffer_count )
			EndIndex = 0;

		/* if entire list searched then no frame available */
		if ( EndIndex == StartIndex ) {
			/* If this occurs then something bad happened,
			 * all buffers have been 'used' but none mark
			 * the end of a frame. Reset buffers and receiver.
			 */

			if ( info->rx_enabled ){
				spin_lock_irqsave(&info->irq_spinlock,flags);
				usc_start_receiver(info);
				spin_unlock_irqrestore(&info->irq_spinlock,flags);
			}
			goto Cleanup;
		}
	}


	/* check status of receive frame */
	
	status = info->rx_buffer_list[EndIndex].status;

	if ( status & (RXSTATUS_SHORT_FRAME + RXSTATUS_OVERRUN +
			RXSTATUS_CRC_ERROR + RXSTATUS_ABORT) ) {
		if ( status & RXSTATUS_SHORT_FRAME )
			info->icount.rxshort++;
		else if ( status & RXSTATUS_ABORT )
			info->icount.rxabort++;
		else if ( status & RXSTATUS_OVERRUN )
			info->icount.rxover++;
		else 
			info->icount.rxcrc++;
		framesize = 0;
	} else {
		/* receive frame has no errors, get frame size.
		 * The frame size is the starting value of the RCC (which was
		 * set to 0xffff) minus the ending value of the RCC (decremented
		 * once for each receive character) minus 2 for the 16-bit CRC.
		 */

		framesize = RCLRVALUE - info->rx_buffer_list[EndIndex].rcc;

		/* adjust frame size for CRC if any */
		if ( info->params.crc_type == HDLC_CRC_16_CCITT )
			framesize -= 2;
		else if ( info->params.crc_type == HDLC_CRC_32_CCITT )
			framesize -= 4;		
	}

	if ( debug_level >= DEBUG_LEVEL_BH )
		printk("%s(%d):mgsl_get_rx_frame(%s) status=%04X size=%d\n",
			__FILE__,__LINE__,info->device_name,status,framesize);
			
	if ( debug_level >= DEBUG_LEVEL_DATA )
		mgsl_trace_block(info,info->rx_buffer_list[StartIndex].virt_addr,
			MIN(framesize,DMABUFFERSIZE),0);	
		
	if (framesize) {
		if (framesize > info->max_frame_size)
			info->icount.rxlong++;
		else {
#if 1
			/* 
			 * copy contents of dma frame buffer(s) to intermediate
		         * rxbuffer for presentation to line discipline
			 */	 
			int copy_count = framesize;
			int index = StartIndex;
			unsigned char *ptmp = info->intermediate_rxbuffer;

			info->icount.rxok++;
			
			while( copy_count )
			{
				int partial_count;
				if ( copy_count > DMABUFFERSIZE )
					partial_count = DMABUFFERSIZE;
				else
					partial_count = copy_count;
			
				pBufEntry = &(info->rx_buffer_list[index]);
				memcpy( ptmp, pBufEntry->virt_addr, partial_count );
				ptmp += partial_count;
				copy_count -= partial_count;
				
				if ( ++index == info->rx_buffer_count )
					index = 0;
				
			}
			
			/* Call the line discipline receive callback directly. */
			tty->ldisc.receive_buf(tty, info->intermediate_rxbuffer, info->flag_buf, framesize);
#else
			info->icount.rxok++;
			pBufEntry = &(info->rx_buffer_list[StartIndex]);
			/* Call the line discipline receive callback directly. */
			tty->ldisc.receive_buf(tty, pBufEntry->virt_addr, info->flag_buf, framesize);
#endif
		}
	}
	/* Free the buffers used by this frame. */
	mgsl_free_rx_frame_buffers( info, StartIndex, EndIndex );

	ReturnCode = 1;

Cleanup:

	if ( info->rx_enabled && info->rx_overflow ) {
		/* The receiver needs to restarted because of 
		 * a receive overflow (buffer or FIFO). If the 
		 * receive buffers are now empty, then restart receiver.
		 */

		if ( !info->rx_buffer_list[EndIndex].status &&
			info->rx_buffer_list[EndIndex].count ) {
			spin_lock_irqsave(&info->irq_spinlock,flags);
			usc_start_receiver(info);
			spin_unlock_irqrestore(&info->irq_spinlock,flags);
		}
	}

	return ReturnCode;

}	/* end of mgsl_get_rx_frame() */

/* mgsl_load_tx_dma_buffer()
 * 
 * 	Load the transmit DMA buffer with the specified data.
 * 
 * Arguments:
 * 
 * 	info		pointer to device extension
 * 	Buffer		pointer to buffer containing frame to load
 * 	BufferSize	size in bytes of frame in Buffer
 * 
 * Return Value: 	None
 */
void mgsl_load_tx_dma_buffer(struct mgsl_struct *info, const char *Buffer,
	 unsigned int BufferSize)
{
	unsigned short Copycount;
	unsigned int i = 0;
	DMABUFFERENTRY *pBufEntry;
	
	if ( debug_level >= DEBUG_LEVEL_DATA )
		mgsl_trace_block(info,Buffer, MIN(BufferSize,DMABUFFERSIZE), 1);	

	if (info->params.flags & HDLC_FLAG_HDLC_LOOPMODE) {
		/* set CMR:13 to start transmit when
		 * next GoAhead (abort) is received
		 */
	 	info->cmr_value |= BIT13;			  
	}
		
	/* Setup the status and RCC (Frame Size) fields of the 1st */
	/* buffer entry in the transmit DMA buffer list. */

	info->tx_buffer_list[0].status = info->cmr_value & 0xf000;
	info->tx_buffer_list[0].rcc    = BufferSize;
	info->tx_buffer_list[0].count  = BufferSize;

	/* Copy frame data from 1st source buffer to the DMA buffers. */
	/* The frame data may span multiple DMA buffers. */

	while( BufferSize ){
		/* Get a pointer to next DMA buffer entry. */
		pBufEntry = &info->tx_buffer_list[i++];
			
		/* Calculate the number of bytes that can be copied from */
		/* the source buffer to this DMA buffer. */
		if ( BufferSize > DMABUFFERSIZE )
			Copycount = DMABUFFERSIZE;
		else
			Copycount = BufferSize;

		/* Actually copy data from source buffer to DMA buffer. */
		/* Also set the data count for this individual DMA buffer. */
		if ( info->bus_type == MGSL_BUS_TYPE_PCI )
			mgsl_load_pci_memory(pBufEntry->virt_addr, Buffer,Copycount);
		else
			memcpy(pBufEntry->virt_addr, Buffer, Copycount);

		pBufEntry->count = Copycount;

		/* Advance source pointer and reduce remaining data count. */
		Buffer += Copycount;
		BufferSize -= Copycount;
	}

}	/* end of mgsl_load_tx_dma_buffer() */

/*
 * mgsl_register_test()
 * 
 * 	Performs a register test of the 16C32.
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:		TRUE if test passed, otherwise FALSE
 */
BOOLEAN mgsl_register_test( struct mgsl_struct *info )
{
	static unsigned short BitPatterns[] =
		{ 0x0000, 0xffff, 0xaaaa, 0x5555, 0x1234, 0x6969, 0x9696, 0x0f0f };
	static unsigned int Patterncount = sizeof(BitPatterns)/sizeof(unsigned short);
	unsigned int i;
	BOOLEAN rc = TRUE;
	unsigned long flags;

	spin_lock_irqsave(&info->irq_spinlock,flags);
	usc_reset(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

	/* Verify the reset state of some registers. */

	if ( (usc_InReg( info, SICR ) != 0) ||
		  (usc_InReg( info, IVR  ) != 0) ||
		  (usc_InDmaReg( info, DIVR ) != 0) ){
		rc = FALSE;
	}

	if ( rc == TRUE ){
		/* Write bit patterns to various registers but do it out of */
		/* sync, then read back and verify values. */

		for ( i = 0 ; i < Patterncount ; i++ ) {
			usc_OutReg( info, TC0R, BitPatterns[i] );
			usc_OutReg( info, TC1R, BitPatterns[(i+1)%Patterncount] );
			usc_OutReg( info, TCLR, BitPatterns[(i+2)%Patterncount] );
			usc_OutReg( info, RCLR, BitPatterns[(i+3)%Patterncount] );
			usc_OutReg( info, RSR,  BitPatterns[(i+4)%Patterncount] );
			usc_OutDmaReg( info, TBCR, BitPatterns[(i+5)%Patterncount] );

			if ( (usc_InReg( info, TC0R ) != BitPatterns[i]) ||
				  (usc_InReg( info, TC1R ) != BitPatterns[(i+1)%Patterncount]) ||
				  (usc_InReg( info, TCLR ) != BitPatterns[(i+2)%Patterncount]) ||
				  (usc_InReg( info, RCLR ) != BitPatterns[(i+3)%Patterncount]) ||
				  (usc_InReg( info, RSR )  != BitPatterns[(i+4)%Patterncount]) ||
				  (usc_InDmaReg( info, TBCR ) != BitPatterns[(i+5)%Patterncount]) ){
				rc = FALSE;
				break;
			}
		}
	}

	spin_lock_irqsave(&info->irq_spinlock,flags);
	usc_reset(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

	return rc;

}	/* end of mgsl_register_test() */

/* mgsl_irq_test() 	Perform interrupt test of the 16C32.
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	TRUE if test passed, otherwise FALSE
 */
BOOLEAN mgsl_irq_test( struct mgsl_struct *info )
{
	unsigned long EndTime;
	unsigned long flags;

	spin_lock_irqsave(&info->irq_spinlock,flags);
	usc_reset(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

	/*
	 * Setup 16C32 to interrupt on TxC pin (14MHz clock) transition. 
	 * The ISR sets irq_occurred to 1. 
	 */

	info->irq_occurred = FALSE;

	/* Enable INTEN gate for ISA adapter (Port 6, Bit12) */
	/* Enable INTEN (Port 6, Bit12) */
	/* This connects the IRQ request signal to the ISA bus */
	/* on the ISA adapter. This has no effect for the PCI adapter */
	usc_OutReg( info, PCR, (unsigned short)((usc_InReg(info, PCR) | BIT13) & ~BIT12) );

	usc_EnableMasterIrqBit(info);
	usc_EnableInterrupts(info, IO_PIN);
	usc_ClearIrqPendingBits(info, IO_PIN);
	
	usc_UnlatchIostatusBits(info, MISCSTATUS_TXC_LATCHED);
	usc_EnableStatusIrqs(info, SICR_TXC_ACTIVE + SICR_TXC_INACTIVE);

	EndTime=100;
	while( EndTime-- && !info->irq_occurred ) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(jiffies_from_ms(10));
		set_current_state(TASK_RUNNING);
	}
	
	spin_lock_irqsave(&info->irq_spinlock,flags);
	usc_reset(info);
	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	if ( !info->irq_occurred ) 
		return FALSE;
	else
		return TRUE;

}	/* end of mgsl_irq_test() */

/* mgsl_dma_test()
 * 
 * 	Perform a DMA test of the 16C32. A small frame is
 * 	transmitted via DMA from a transmit buffer to a receive buffer
 * 	using single buffer DMA mode.
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	TRUE if test passed, otherwise FALSE
 */
BOOLEAN mgsl_dma_test( struct mgsl_struct *info )
{
	unsigned short FifoLevel;
	unsigned long phys_addr;
	unsigned int FrameSize;
	unsigned int i;
	char *TmpPtr;
	BOOLEAN rc = TRUE;
	volatile unsigned short status;
	volatile unsigned long EndTime;
	unsigned long flags;
	MGSL_PARAMS tmp_params;

	/* save current port options */
	memcpy(&tmp_params,&info->params,sizeof(MGSL_PARAMS));
	/* load default port options */
	memcpy(&info->params,&default_params,sizeof(MGSL_PARAMS));
	
#define TESTFRAMESIZE 40

	spin_lock_irqsave(&info->irq_spinlock,flags);
	
	/* setup 16C32 for SDLC DMA transfer mode */

	usc_reset(info);
	usc_set_sdlc_mode(info);
	usc_enable_loopback(info,1);
	
	/* Reprogram the RDMR so that the 16C32 does NOT clear the count
	 * field of the buffer entry after fetching buffer address. This
	 * way we can detect a DMA failure for a DMA read (which should be
	 * non-destructive to system memory) before we try and write to
	 * memory (where a failure could corrupt system memory).
	 */

	/* Receive DMA mode Register (RDMR)
	 * 
	 * <15..14>	11	DMA mode = Linked List Buffer mode
	 * <13>		1	RSBinA/L = store Rx status Block in List entry
	 * <12>		0	1 = Clear count of List Entry after fetching
	 * <11..10>	00	Address mode = Increment
	 * <9>		1	Terminate Buffer on RxBound
	 * <8>		0	Bus Width = 16bits
	 * <7..0>		?	status Bits (write as 0s)
	 * 
	 * 1110 0010 0000 0000 = 0xe200
	 */

	usc_OutDmaReg( info, RDMR, 0xe200 );
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);


	/* SETUP TRANSMIT AND RECEIVE DMA BUFFERS */

	FrameSize = TESTFRAMESIZE;

	/* setup 1st transmit buffer entry: */
	/* with frame size and transmit control word */

	info->tx_buffer_list[0].count  = FrameSize;
	info->tx_buffer_list[0].rcc    = FrameSize;
	info->tx_buffer_list[0].status = 0x4000;

	/* build a transmit frame in 1st transmit DMA buffer */

	TmpPtr = info->tx_buffer_list[0].virt_addr;
	for (i = 0; i < FrameSize; i++ )
		*TmpPtr++ = i;

	/* setup 1st receive buffer entry: */
	/* clear status, set max receive buffer size */

	info->rx_buffer_list[0].status = 0;
	info->rx_buffer_list[0].count = FrameSize + 4;

	/* zero out the 1st receive buffer */

	memset( info->rx_buffer_list[0].virt_addr, 0, FrameSize + 4 );

	/* Set count field of next buffer entries to prevent */
	/* 16C32 from using buffers after the 1st one. */

	info->tx_buffer_list[1].count = 0;
	info->rx_buffer_list[1].count = 0;
	

	/***************************/
	/* Program 16C32 receiver. */
	/***************************/
	
	spin_lock_irqsave(&info->irq_spinlock,flags);

	/* setup DMA transfers */
	usc_RTCmd( info, RTCmd_PurgeRxFifo );

	/* program 16C32 receiver with physical address of 1st DMA buffer entry */
	phys_addr = info->rx_buffer_list[0].phys_entry;
	usc_OutDmaReg( info, NRARL, (unsigned short)phys_addr );
	usc_OutDmaReg( info, NRARU, (unsigned short)(phys_addr >> 16) );

	/* Clear the Rx DMA status bits (read RDMR) and start channel */
	usc_InDmaReg( info, RDMR );
	usc_DmaCmd( info, DmaCmd_InitRxChannel );

	/* Enable Receiver (RMR <1..0> = 10) */
	usc_OutReg( info, RMR, (unsigned short)((usc_InReg(info, RMR) & 0xfffc) | 0x0002) );
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);


	/*************************************************************/
	/* WAIT FOR RECEIVER TO DMA ALL PARAMETERS FROM BUFFER ENTRY */
	/*************************************************************/

	/* Wait 100ms for interrupt. */
	EndTime = jiffies + jiffies_from_ms(100);

	for(;;) {
		if ( jiffies > EndTime ) {
			rc = FALSE;
			break;
		}

		spin_lock_irqsave(&info->irq_spinlock,flags);
		status = usc_InDmaReg( info, RDMR );
		spin_unlock_irqrestore(&info->irq_spinlock,flags);

		if ( !(status & BIT4) && (status & BIT5) ) {
			/* INITG (BIT 4) is inactive (no entry read in progress) AND */
			/* BUSY  (BIT 5) is active (channel still active). */
			/* This means the buffer entry read has completed. */
			break;
		}
	}


	/******************************/
	/* Program 16C32 transmitter. */
	/******************************/
	
	spin_lock_irqsave(&info->irq_spinlock,flags);

	/* Program the Transmit Character Length Register (TCLR) */
	/* and clear FIFO (TCC is loaded with TCLR on FIFO clear) */

	usc_OutReg( info, TCLR, (unsigned short)info->tx_buffer_list[0].count );
	usc_RTCmd( info, RTCmd_PurgeTxFifo );

	/* Program the address of the 1st DMA Buffer Entry in linked list */

	phys_addr = info->tx_buffer_list[0].phys_entry;
	usc_OutDmaReg( info, NTARL, (unsigned short)phys_addr );
	usc_OutDmaReg( info, NTARU, (unsigned short)(phys_addr >> 16) );

	/* unlatch Tx status bits, and start transmit channel. */

	usc_OutReg( info, TCSR, (unsigned short)(( usc_InReg(info, TCSR) & 0x0f00) | 0xfa) );
	usc_DmaCmd( info, DmaCmd_InitTxChannel );

	/* wait for DMA controller to fill transmit FIFO */

	usc_TCmd( info, TCmd_SelectTicrTxFifostatus );
	
	spin_unlock_irqrestore(&info->irq_spinlock,flags);


	/**********************************/
	/* WAIT FOR TRANSMIT FIFO TO FILL */
	/**********************************/
	
	/* Wait 100ms */
	EndTime = jiffies + jiffies_from_ms(100);

	for(;;) {
		if ( jiffies > EndTime ) {
			rc = FALSE;
			break;
		}

		spin_lock_irqsave(&info->irq_spinlock,flags);
		FifoLevel = usc_InReg(info, TICR) >> 8;
		spin_unlock_irqrestore(&info->irq_spinlock,flags);
			
		if ( FifoLevel < 16 )
			break;
		else
			if ( FrameSize < 32 ) {
				/* This frame is smaller than the entire transmit FIFO */
				/* so wait for the entire frame to be loaded. */
				if ( FifoLevel <= (32 - FrameSize) )
					break;
			}
	}


	if ( rc == TRUE )
	{
		/* Enable 16C32 transmitter. */

		spin_lock_irqsave(&info->irq_spinlock,flags);
		
		/* Transmit mode Register (TMR), <1..0> = 10, Enable Transmitter */
		usc_TCmd( info, TCmd_SendFrame );
		usc_OutReg( info, TMR, (unsigned short)((usc_InReg(info, TMR) & 0xfffc) | 0x0002) );
		
		spin_unlock_irqrestore(&info->irq_spinlock,flags);

						
		/******************************/
		/* WAIT FOR TRANSMIT COMPLETE */
		/******************************/

		/* Wait 100ms */
		EndTime = jiffies + jiffies_from_ms(100);

		/* While timer not expired wait for transmit complete */

		spin_lock_irqsave(&info->irq_spinlock,flags);
		status = usc_InReg( info, TCSR );
		spin_unlock_irqrestore(&info->irq_spinlock,flags);

		while ( !(status & (BIT6+BIT5+BIT4+BIT2+BIT1)) ) {
			if ( jiffies > EndTime ) {
				rc = FALSE;
				break;
			}

			spin_lock_irqsave(&info->irq_spinlock,flags);
			status = usc_InReg( info, TCSR );
			spin_unlock_irqrestore(&info->irq_spinlock,flags);
		}
	}


	if ( rc == TRUE ){
		/* CHECK FOR TRANSMIT ERRORS */
		if ( status & (BIT5 + BIT1) ) 
			rc = FALSE;
	}

	if ( rc == TRUE ) {
		/* WAIT FOR RECEIVE COMPLETE */

		/* Wait 100ms */
		EndTime = jiffies + jiffies_from_ms(100);

		/* Wait for 16C32 to write receive status to buffer entry. */
		status=info->rx_buffer_list[0].status;
		while ( status == 0 ) {
			if ( jiffies > EndTime ) {
			printk(KERN_ERR"mark 4\n");
				rc = FALSE;
				break;
			}
			status=info->rx_buffer_list[0].status;
		}
	}


	if ( rc == TRUE ) {
		/* CHECK FOR RECEIVE ERRORS */
		status = info->rx_buffer_list[0].status;

		if ( status & (BIT8 + BIT3 + BIT1) ) {
			/* receive error has occured */
			rc = FALSE;
		} else {
			if ( memcmp( info->tx_buffer_list[0].virt_addr ,
				info->rx_buffer_list[0].virt_addr, FrameSize ) ){
				rc = FALSE;
			}
		}
	}

	usc_reset( info );

	/* restore current port options */
	memcpy(&info->params,&tmp_params,sizeof(MGSL_PARAMS));
	
	return rc;

}	/* end of mgsl_dma_test() */

/* mgsl_adapter_test()
 * 
 * 	Perform the register, IRQ, and DMA tests for the 16C32.
 * 	
 * Arguments:		info	pointer to device instance data
 * Return Value:	0 if success, otherwise -ENODEV
 */
int mgsl_adapter_test( struct mgsl_struct *info )
{
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):Testing device %s\n",
			__FILE__,__LINE__,info->device_name );
			
	if ( !mgsl_register_test( info ) ) {
		info->init_error = DiagStatus_AddressFailure;
		printk( "%s(%d):Register test failure for device %s Addr=%04X\n",
			__FILE__,__LINE__,info->device_name, (unsigned short)(info->io_base) );
		return -ENODEV;
	}

	if ( !mgsl_irq_test( info ) ) {
		info->init_error = DiagStatus_IrqFailure;
		printk( "%s(%d):Interrupt test failure for device %s IRQ=%d\n",
			__FILE__,__LINE__,info->device_name, (unsigned short)(info->irq_level) );
		return -ENODEV;
	}

	if ( !mgsl_dma_test( info ) ) {
		info->init_error = DiagStatus_DmaFailure;
		printk( "%s(%d):DMA test failure for device %s DMA=%d\n",
			__FILE__,__LINE__,info->device_name, (unsigned short)(info->dma_level) );
		return -ENODEV;
	}

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):device %s passed diagnostics\n",
			__FILE__,__LINE__,info->device_name );
			
	return 0;

}	/* end of mgsl_adapter_test() */

/* mgsl_memory_test()
 * 
 * 	Test the shared memory on a PCI adapter.
 * 
 * Arguments:		info	pointer to device instance data
 * Return Value:	TRUE if test passed, otherwise FALSE
 */
BOOLEAN mgsl_memory_test( struct mgsl_struct *info )
{
	static unsigned long BitPatterns[] = { 0x0, 0x55555555, 0xaaaaaaaa,
											0x66666666, 0x99999999, 0xffffffff, 0x12345678 };
	unsigned long Patterncount = sizeof(BitPatterns)/sizeof(unsigned long);
	unsigned long i;
	unsigned long TestLimit = SHARED_MEM_ADDRESS_SIZE/sizeof(unsigned long);
	unsigned long * TestAddr;

	if ( info->bus_type != MGSL_BUS_TYPE_PCI )
		return TRUE;

	TestAddr = (unsigned long *)info->memory_base;

	/* Test data lines with test pattern at one location. */

	for ( i = 0 ; i < Patterncount ; i++ ) {
		*TestAddr = BitPatterns[i];
		if ( *TestAddr != BitPatterns[i] )
			return FALSE;
	}

	/* Test address lines with incrementing pattern over */
	/* entire address range. */

	for ( i = 0 ; i < TestLimit ; i++ ) {
		*TestAddr = i * 4;
		TestAddr++;
	}

	TestAddr = (unsigned long *)info->memory_base;

	for ( i = 0 ; i < TestLimit ; i++ ) {
		if ( *TestAddr != i * 4 )
			return FALSE;
		TestAddr++;
	}

	memset( info->memory_base, 0, SHARED_MEM_ADDRESS_SIZE );

	return TRUE;

}	/* End Of mgsl_memory_test() */


#pragma optimize( "", off )
/* mgsl_load_pci_memory()
 * 
 * 	Load a large block of data into the PCI shared memory.
 * 	Use this instead of memcpy() or memmove() to move data
 * 	into the PCI shared memory.
 * 
 * Notes:
 * 
 * 	This function prevents the PCI9050 interface chip from hogging
 * 	the adapter local bus, which can starve the 16C32 by preventing
 * 	16C32 bus master cycles.
 * 
 * 	The PCI9050 documentation says that the 9050 will always release
 * 	control of the local bus after completing the current read
 * 	or write operation.
 * 
 * 	It appears that as long as the PCI9050 write FIFO is full, the
 * 	PCI9050 treats all of the writes as a single burst transaction
 * 	and will not release the bus. This causes DMA latency problems
 * 	at high speeds when copying large data blocks to the shared
 * 	memory.
 * 
 * 	This function in effect, breaks the a large shared memory write
 * 	into multiple transations by interleaving a shared memory read
 * 	which will flush the write FIFO and 'complete' the write
 * 	transation. This allows any pending DMA request to gain control
 * 	of the local bus in a timely fasion.
 * 
 * Arguments:
 * 
 * 	TargetPtr	pointer to target address in PCI shared memory
 * 	SourcePtr	pointer to source buffer for data
 * 	count		count in bytes of data to copy
 *
 * Return Value:	None
 */
void mgsl_load_pci_memory( char* TargetPtr, const char* SourcePtr, 
	unsigned short count )
{
	/*******************************************************/
	/* A load interval of 16 allows for 4 32-bit writes at */
	/* 60ns each for a maximum latency of 240ns on the		 */
	/* local bus.														 */
	/*******************************************************/

#define PCI_LOAD_INTERVAL 64

	unsigned short Intervalcount = count / PCI_LOAD_INTERVAL;
	unsigned short Index;
	unsigned long Dummy;

	for ( Index = 0 ; Index < Intervalcount ; Index++ )
	{
		memcpy(TargetPtr, SourcePtr, PCI_LOAD_INTERVAL);
		Dummy = *((unsigned long *)TargetPtr);
		TargetPtr += PCI_LOAD_INTERVAL;
		SourcePtr += PCI_LOAD_INTERVAL;
	}

	memcpy( TargetPtr, SourcePtr, count % PCI_LOAD_INTERVAL );

}	/* End Of mgsl_load_pci_memory() */
#pragma optimize( "", on )

void mgsl_trace_block(struct mgsl_struct *info,const char* data, int count, int xmit)
{
	int i;
	int linecount;
	if (xmit)
		printk("%s tx data:\n",info->device_name);
	else
		printk("%s rx data:\n",info->device_name);
		
	while(count) {
		if (count > 16)
			linecount = 16;
		else
			linecount = count;
			
		for(i=0;i<linecount;i++)
			printk("%02X ",(unsigned char)data[i]);
		for(;i<17;i++)
			printk("   ");
		for(i=0;i<linecount;i++) {
			if (data[i]>=040 && data[i]<=0176)
				printk("%c",data[i]);
			else
				printk(".");
		}
		printk("\n");
		
		data  += linecount;
		count -= linecount;
	}
}	/* end of mgsl_trace_block() */

/* mgsl_tx_timeout()
 * 
 * 	called when HDLC frame times out
 * 	update stats and do tx completion processing
 * 	
 * Arguments:	context		pointer to device instance data
 * Return Value:	None
 */
void mgsl_tx_timeout(unsigned long context)
{
	struct mgsl_struct *info = (struct mgsl_struct*)context;
	unsigned long flags;
	
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):mgsl_tx_timeout(%s)\n",
			__FILE__,__LINE__,info->device_name);
	if(info->tx_active && info->params.mode == MGSL_MODE_HDLC) {
		info->icount.txtimeout++;
	}
	spin_lock_irqsave(&info->irq_spinlock,flags);
	info->tx_active = 0;
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;

	if ( info->params.flags & HDLC_FLAG_HDLC_LOOPMODE )
		usc_loopmode_cancel_transmit( info );

	spin_unlock_irqrestore(&info->irq_spinlock,flags);
	
	mgsl_bh_transmit_data(info,0);
	
}	/* end of mgsl_tx_timeout() */

/* signal that there are no more frames to send, so that
 * line is 'released' by echoing RxD to TxD when current
 * transmission is complete (or immediately if no tx in progress).
 */
static int mgsl_loopmode_send_done( struct mgsl_struct * info )
{
	unsigned long flags;
	
	spin_lock_irqsave(&info->irq_spinlock,flags);
	if (info->params.flags & HDLC_FLAG_HDLC_LOOPMODE) {
		if (info->tx_active)
			info->loopmode_send_done_requested = TRUE;
		else
			usc_loopmode_send_done(info);
	}
	spin_unlock_irqrestore(&info->irq_spinlock,flags);

	return 0;
}

/* release the line by echoing RxD to TxD
 * upon completion of a transmit frame
 */
void usc_loopmode_send_done( struct mgsl_struct * info )
{
 	info->loopmode_send_done_requested = FALSE;
 	/* clear CMR:13 to 0 to start echoing RxData to TxData */
 	info->cmr_value &= ~BIT13;			  
 	usc_OutReg(info, CMR, info->cmr_value);
}

/* abort a transmit in progress while in HDLC LoopMode
 */
void usc_loopmode_cancel_transmit( struct mgsl_struct * info )
{
 	/* reset tx dma channel and purge TxFifo */
 	usc_RTCmd( info, RTCmd_PurgeTxFifo );
 	usc_DmaCmd( info, DmaCmd_ResetTxChannel );
  	usc_loopmode_send_done( info );
}

/* for HDLC/SDLC LoopMode, setting CMR:13 after the transmitter is enabled
 * is an Insert Into Loop action. Upon receipt of a GoAhead sequence (RxAbort)
 * we must clear CMR:13 to begin repeating TxData to RxData
 */
void usc_loopmode_insert_request( struct mgsl_struct * info )
{
 	info->loopmode_insert_requested = TRUE;
 
 	/* enable RxAbort irq. On next RxAbort, clear CMR:13 to
 	 * begin repeating TxData on RxData (complete insertion)
	 */
 	usc_OutReg( info, RICR, 
		(usc_InReg( info, RICR ) | RXSTATUS_ABORT_RECEIVED ) );
		
	/* set CMR:13 to insert into loop on next GoAhead (RxAbort) */
	info->cmr_value |= BIT13;
 	usc_OutReg(info, CMR, info->cmr_value);
}

/* return 1 if station is inserted into the loop, otherwise 0
 */
int usc_loopmode_active( struct mgsl_struct * info)
{
 	return usc_InReg( info, CCSR ) & BIT7 ? 1 : 0 ;
}

/* return 1 if USC is in loop send mode, otherwise 0
 */
int usc_loopmode_send_active( struct mgsl_struct * info )
{
	return usc_InReg( info, CCSR ) & BIT6 ? 1 : 0 ;
}			  

