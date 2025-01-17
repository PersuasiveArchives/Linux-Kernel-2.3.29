#ifndef NDEBUG
#define NDEBUG (NDEBUG_RESTART_SELECT | NDEBUG_ABORT)
#endif
/* 
 * NCR 5380 generic driver routines.  These should make it *trivial*
 *      to implement 5380 SCSI drivers under Linux with a non-trantor
 *      architecture.
 *
 *      Note that these routines also work with NR53c400 family chips.
 *
 * Copyright 1993, Drew Eckhardt
 *      Visionary Computing 
 *      (Unix and Linux consulting and custom programming)
 *      drew@colorado.edu
 *      +1 (303) 666-5836
 *
 * DISTRIBUTION RELEASE 6. 
 *
 * For more information, please consult 
 *
 * NCR 5380 Family
 * SCSI Protocol Controller
 * Databook
 *
 * NCR Microelectronics
 * 1635 Aeroplaza Drive
 * Colorado Springs, CO 80916
 * 1+ (719) 578-3400
 * 1+ (800) 334-5454
 */

/*
 * $Log: NCR5380.c,v $

 * Revision 1.10 1998/9/2	Alan Cox
 *				(alan@redhat.com)
 * Fixed up the timer lockups reported so far. Things still suck. Looking 
 * forward to 2.3 and per device request queues. Then it'll be possible to
 * SMP thread this beast and improve life no end.
 
 * Revision 1.9  1997/7/27	Ronald van Cuijlenborg
 *				(ronald.van.cuijlenborg@tip.nl or nutty@dds.nl)
 * (hopefully) fixed and enhanced USLEEP
 * added support for DTC3181E card (for Mustek scanner)
 *

 * Revision 1.8			Ingmar Baumgart
 *				(ingmar@gonzo.schwaben.de)
 * added support for NCR53C400a card
 *

 * Revision 1.7  1996/3/2       Ray Van Tassle (rayvt@comm.mot.com)
 * added proc_info
 * added support needed for DTC 3180/3280
 * fixed a couple of bugs
 *

 * Revision 1.5  1994/01/19  09:14:57  drew
 * Fixed udelay() hack that was being used on DATAOUT phases
 * instead of a proper wait for the final handshake.
 *
 * Revision 1.4  1994/01/19  06:44:25  drew
 * *** empty log message ***
 *
 * Revision 1.3  1994/01/19  05:24:40  drew
 * Added support for TCR LAST_BYTE_SENT bit.
 *
 * Revision 1.2  1994/01/15  06:14:11  drew
 * REAL DMA support, bug fixes.
 *
 * Revision 1.1  1994/01/15  06:00:54  drew
 * Initial revision
 *
 */

/*
 * Further development / testing that should be done : 
 * 1.  Cleanup the NCR5380_transfer_dma function and DMA operation complete
 *     code so that everything does the same thing that's done at the 
 *     end of a pseudo-DMA read operation.
 *
 * 2.  Fix REAL_DMA (interrupt driven, polled works fine) -
 *     basically, transfer size needs to be reduced by one 
 *     and the last byte read as is done with PSEUDO_DMA.
 * 
 * 3.  Test USLEEP code 
 *
 * 4.  Test SCSI-II tagged queueing (I have no devices which support 
 *      tagged queueing)
 *
 * 5.  Test linked command handling code after Eric is ready with 
 *      the high level code.
 */

#if (NDEBUG & NDEBUG_LISTS)
#define LIST(x,y) {printk("LINE:%d   Adding %p to %p\n", __LINE__, (void*)(x), (void*)(y)); if ((x)==(y)) udelay(5); }
#define REMOVE(w,x,y,z) {printk("LINE:%d   Removing: %p->%p  %p->%p \n", __LINE__, (void*)(w), (void*)(x), (void*)(y), (void*)(z)); if ((x)==(y)) udelay(5); }
#else
#define LIST(x,y)
#define REMOVE(w,x,y,z)
#endif

#ifndef notyet
#undef LINKED
#undef REAL_DMA
#endif

#ifdef REAL_DMA_POLL
#undef READ_OVERRUNS
#define READ_OVERRUNS
#endif

/*
 * Design
 * Issues :
 *
 * The other Linux SCSI drivers were written when Linux was Intel PC-only,
 * and specifically for each board rather than each chip.  This makes their
 * adaptation to platforms like the Mac (Some of which use NCR5380's)
 * more difficult than it has to be.
 *
 * Also, many of the SCSI drivers were written before the command queuing
 * routines were implemented, meaning their implementations of queued 
 * commands were hacked on rather than designed in from the start.
 *
 * When I designed the Linux SCSI drivers I figured that 
 * while having two different SCSI boards in a system might be useful
 * for debugging things, two of the same type wouldn't be used.
 * Well, I was wrong and a number of users have mailed me about running
 * multiple high-performance SCSI boards in a server.
 *
 * Finally, when I get questions from users, I have no idea what 
 * revision of my driver they are running.
 *
 * This driver attempts to address these problems :
 * This is a generic 5380 driver.  To use it on a different platform, 
 * one simply writes appropriate system specific macros (ie, data
 * transfer - some PC's will use the I/O bus, 68K's must use 
 * memory mapped) and drops this file in their 'C' wrapper.
 *
 * As far as command queueing, two queues are maintained for 
 * each 5380 in the system - commands that haven't been issued yet,
 * and commands that are currently executing.  This means that an 
 * unlimited number of commands may be queued, letting 
 * more commands propagate from the higher driver levels giving higher 
 * throughput.  Note that both I_T_L and I_T_L_Q nexuses are supported, 
 * allowing multiple commands to propagate all the way to a SCSI-II device 
 * while a command is already executing.
 *
 * To solve the multiple-boards-in-the-same-system problem, 
 * there is a separate instance structure for each instance
 * of a 5380 in the system.  So, multiple NCR5380 drivers will
 * be able to coexist with appropriate changes to the high level
 * SCSI code.  
 *
 * A NCR5380_PUBLIC_REVISION macro is provided, with the release
 * number (updated for each public release) printed by the 
 * NCR5380_print_options command, which should be called from the 
 * wrapper detect function, so that I know what release of the driver
 * users are using.
 *
 * Issues specific to the NCR5380 : 
 *
 * When used in a PIO or pseudo-dma mode, the NCR5380 is a braindead 
 * piece of hardware that requires you to sit in a loop polling for 
 * the REQ signal as long as you are connected.  Some devices are 
 * brain dead (ie, many TEXEL CD ROM drives) and won't disconnect 
 * while doing long seek operations.
 * 
 * The workaround for this is to keep track of devices that have
 * disconnected.  If the device hasn't disconnected, for commands that
 * should disconnect, we do something like 
 *
 * while (!REQ is asserted) { sleep for N usecs; poll for M usecs }
 * 
 * Some tweaking of N and M needs to be done.  An algorithm based 
 * on "time to data" would give the best results as long as short time
 * to datas (ie, on the same track) were considered, however these 
 * broken devices are the exception rather than the rule and I'd rather
 * spend my time optimizing for the normal case.
 *
 * Architecture :
 *
 * At the heart of the design is a coroutine, NCR5380_main,
 * which is started when not running by the interrupt handler,
 * timer, and queue command function.  It attempts to establish
 * I_T_L or I_T_L_Q nexuses by removing the commands from the 
 * issue queue and calling NCR5380_select() if a nexus 
 * is not established. 
 *
 * Once a nexus is established, the NCR5380_information_transfer()
 * phase goes through the various phases as instructed by the target.
 * if the target goes into MSG IN and sends a DISCONNECT message,
 * the command structure is placed into the per instance disconnected
 * queue, and NCR5380_main tries to find more work.  If USLEEP
 * was defined, and the target is idle for too long, the system
 * will try to sleep.
 *
 * If a command has disconnected, eventually an interrupt will trigger,
 * calling NCR5380_intr()  which will in turn call NCR5380_reselect
 * to reestablish a nexus.  This will run main if necessary.
 *
 * On command termination, the done function will be called as 
 * appropriate.
 *
 * SCSI pointers are maintained in the SCp field of SCSI command 
 * structures, being initialized after the command is connected
 * in NCR5380_select, and set as appropriate in NCR5380_information_transfer.
 * Note that in violation of the standard, an implicit SAVE POINTERS operation
 * is done, since some BROKEN disks fail to issue an explicit SAVE POINTERS.
 */

/*
 * Using this file :
 * This file a skeleton Linux SCSI driver for the NCR 5380 series
 * of chips.  To use it, you write an architecture specific functions 
 * and macros and include this file in your driver.
 *
 * These macros control options : 
 * AUTOPROBE_IRQ - if defined, the NCR5380_probe_irq() function will be 
 *      defined.
 * 
 * AUTOSENSE - if defined, REQUEST SENSE will be performed automatically
 *      for commands that return with a CHECK CONDITION status. 
 *
 * DIFFERENTIAL - if defined, NCR53c81 chips will use external differential
 *      transceivers. 
 *
 * DONT_USE_INTR - if defined, never use interrupts, even if we probe or
 *      override-configure an IRQ.
 *
 * LIMIT_TRANSFERSIZE - if defined, limit the pseudo-dma transfers to 512
 *      bytes at a time.  Since interrupts are disabled by default during
 *      these transfers, we might need this to give reasonable interrupt
 *      service time if the transfer size gets too large.
 *
 * LINKED - if defined, linked commands are supported.
 *
 * PSEUDO_DMA - if defined, PSEUDO DMA is used during the data transfer phases.
 *
 * REAL_DMA - if defined, REAL DMA is used during the data transfer phases.
 *
 * REAL_DMA_POLL - if defined, REAL DMA is used but the driver doesn't
 *      rely on phase mismatch and EOP interrupts to determine end 
 *      of phase.
 *
 * SCSI2 - if defined, SCSI-2 tagged queuing is used where possible
 *
 * UNSAFE - leave interrupts enabled during pseudo-DMA transfers.  You
 *          only really want to use this if you're having a problem with
 *          dropped characters during high speed communications, and even
 *          then, you're going to be better off twiddling with transfersize
 *          in the high level code.
 *
 * USLEEP - if defined, on devices that aren't disconnecting from the 
 *      bus, we will go to sleep so that the CPU can get real work done 
 *      when we run a command that won't complete immediately.
 *
 * Defaults for these will be provided if USLEEP is defined, although
 * the user may want to adjust these to allocate CPU resources to 
 * the SCSI driver or "real" code.
 * 
 * USLEEP_SLEEP - amount of time, in jiffies, to sleep
 *
 * USLEEP_POLL - amount of time, in jiffies, to poll
 *
 * These macros MUST be defined :
 * NCR5380_local_declare() - declare any local variables needed for your
 *      transfer routines.
 *
 * NCR5380_setup(instance) - initialize any local variables needed from a given
 *      instance of the host adapter for NCR5380_{read,write,pread,pwrite}
 * 
 * NCR5380_read(register)  - read from the specified register
 *
 * NCR5380_write(register, value) - write to the specific register 
 *
 * NCR5380_implementation_fields  - additional fields needed for this 
 *      specific implementation of the NCR5380
 *
 * Either real DMA *or* pseudo DMA may be implemented
 * REAL functions : 
 * NCR5380_REAL_DMA should be defined if real DMA is to be used.
 * Note that the DMA setup functions should return the number of bytes 
 *      that they were able to program the controller for.
 *
 * Also note that generic i386/PC versions of these macros are 
 *      available as NCR5380_i386_dma_write_setup,
 *      NCR5380_i386_dma_read_setup, and NCR5380_i386_dma_residual.
 *
 * NCR5380_dma_write_setup(instance, src, count) - initialize
 * NCR5380_dma_read_setup(instance, dst, count) - initialize
 * NCR5380_dma_residual(instance); - residual count
 *
 * PSEUDO functions :
 * NCR5380_pwrite(instance, src, count)
 * NCR5380_pread(instance, dst, count);
 *
 * If nothing specific to this implementation needs doing (ie, with external
 * hardware), you must also define 
 *  
 * NCR5380_queue_command
 * NCR5380_reset
 * NCR5380_abort
 * NCR5380_proc_info
 *
 * to be the global entry points into the specific driver, ie 
 * #define NCR5380_queue_command t128_queue_command.
 *
 * If this is not done, the routines will be defined as static functions
 * with the NCR5380* names and the user must provide a globally
 * accessible wrapper function.
 *
 * The generic driver is initialized by calling NCR5380_init(instance),
 * after setting the appropriate host specific fields and ID.  If the 
 * driver wishes to autoprobe for an IRQ line, the NCR5380_probe_irq(instance,
 * possible) function may be used.  Before the specific driver initialization
 * code finishes, NCR5380_print_options should be called.
 */

static int do_abort(struct Scsi_Host *host);
static void do_reset(struct Scsi_Host *host);
static struct Scsi_Host *first_instance = NULL;
static Scsi_Host_Template *the_template = NULL;

#ifdef USLEEP
struct timer_list usleep_timer;
#endif

/*
 * Function : void initialize_SCp(Scsi_Cmnd *cmd)
 *
 * Purpose : initialize the saved data pointers for cmd to point to the 
 *      start of the buffer.
 *
 * Inputs : cmd - Scsi_Cmnd structure to have pointers reset.
 */

static __inline__ void initialize_SCp(Scsi_Cmnd * cmd)
{
	/* 
	 * Initialize the Scsi Pointer field so that all of the commands in the 
	 * various queues are valid.
	 */

	if (cmd->use_sg) {
		cmd->SCp.buffer = (struct scatterlist *) cmd->buffer;
		cmd->SCp.buffers_residual = cmd->use_sg - 1;
		cmd->SCp.ptr = (char *) cmd->SCp.buffer->address;
		cmd->SCp.this_residual = cmd->SCp.buffer->length;
	} else {
		cmd->SCp.buffer = NULL;
		cmd->SCp.buffers_residual = 0;
		cmd->SCp.ptr = (char *) cmd->request_buffer;
		cmd->SCp.this_residual = cmd->request_bufflen;
	}
}

#include <linux/delay.h>

#ifdef NDEBUG
static struct {
	unsigned char mask;
	const char *name;
} signals[] = { {

		SR_DBP, "PARITY"
}, {
	SR_RST, "RST"
}, {
	SR_BSY, "BSY"
},
{
	SR_REQ, "REQ"
}, {
	SR_MSG, "MSG"
}, {
	SR_CD, "CD"
}, {
	SR_IO, "IO"
},
{
	SR_SEL, "SEL"
}, {
	0, NULL
}
},

basrs[] = { {
		BASR_ATN, "ATN"
}, {
	BASR_ACK, "ACK"
}, {
	0, NULL
}
},

icrs[] = { {
		ICR_ASSERT_RST, "ASSERT RST"
}, {
	ICR_ASSERT_ACK, "ASSERT ACK"
},
{
	ICR_ASSERT_BSY, "ASSERT BSY"
}, {
	ICR_ASSERT_SEL, "ASSERT SEL"
},
{
	ICR_ASSERT_ATN, "ASSERT ATN"
}, {
	ICR_ASSERT_DATA, "ASSERT DATA"
},
{
	0, NULL
}
},

mrs[] = { {
		MR_BLOCK_DMA_MODE, "MODE BLOCK DMA"
}, {
	MR_TARGET, "MODE TARGET"
},
{
	MR_ENABLE_PAR_CHECK, "MODE PARITY CHECK"
}, {
	MR_ENABLE_PAR_INTR,
	    "MODE PARITY INTR"
}, {
	MR_MONITOR_BSY, "MODE MONITOR BSY"
},
{
	MR_DMA_MODE, "MODE DMA"
}, {
	MR_ARBITRATE, "MODE ARBITRATION"
},
{
	0, NULL
}
};

/*
 * Function : void NCR5380_print(struct Scsi_Host *instance)
 *
 * Purpose : print the SCSI bus signals for debugging purposes
 *
 * Input : instance - which NCR5380
 */

static void NCR5380_print(struct Scsi_Host *instance)
{
	NCR5380_local_declare();
	unsigned long flags;
	unsigned char status, data, basr, mr, icr, i;
	NCR5380_setup(instance);
	save_flags(flags);
	cli();
	data = NCR5380_read(CURRENT_SCSI_DATA_REG);
	status = NCR5380_read(STATUS_REG);
	mr = NCR5380_read(MODE_REG);
	icr = NCR5380_read(INITIATOR_COMMAND_REG);
	basr = NCR5380_read(BUS_AND_STATUS_REG);
	restore_flags(flags);
	printk("STATUS_REG: %02x ", status);
	for (i = 0; signals[i].mask; ++i)
		if (status & signals[i].mask)
			printk(",%s", signals[i].name);
	printk("\nBASR: %02x ", basr);
	for (i = 0; basrs[i].mask; ++i)
		if (basr & basrs[i].mask)
			printk(",%s", basrs[i].name);
	printk("\nICR: %02x ", icr);
	for (i = 0; icrs[i].mask; ++i)
		if (icr & icrs[i].mask)
			printk(",%s", icrs[i].name);
	printk("\nMODE: %02x ", mr);
	for (i = 0; mrs[i].mask; ++i)
		if (mr & mrs[i].mask)
			printk(",%s", mrs[i].name);
	printk("\n");
}

static struct {
	unsigned char value;
	const char *name;
} phases[] = {

	{
		PHASE_DATAOUT, "DATAOUT"
	}, {
		PHASE_DATAIN, "DATAIN"
	}, {
		PHASE_CMDOUT, "CMDOUT"
	},
	{
		PHASE_STATIN, "STATIN"
	}, {
		PHASE_MSGOUT, "MSGOUT"
	}, {
		PHASE_MSGIN, "MSGIN"
	},
	{
		PHASE_UNKNOWN, "UNKNOWN"
	}
};

/* 
 * Function : void NCR5380_print_phase(struct Scsi_Host *instance)
 *
 * Purpose : print the current SCSI phase for debugging purposes
 *
 * Input : instance - which NCR5380
 */

static void NCR5380_print_phase(struct Scsi_Host *instance)
{
	NCR5380_local_declare();
	unsigned char status;
	int i;
	NCR5380_setup(instance);

	status = NCR5380_read(STATUS_REG);
	if (!(status & SR_REQ))
		printk("scsi%d : REQ not asserted, phase unknown.\n",
		       instance->host_no);
	else {
		for (i = 0; (phases[i].value != PHASE_UNKNOWN) &&
		     (phases[i].value != (status & PHASE_MASK)); ++i);
		printk("scsi%d : phase %s\n", instance->host_no, phases[i].name);
	}
}
#endif

/*
 * We need to have our coroutine active given these constraints : 
 * 1.  The mutex flag, main_running, can only be set when the main 
 *     routine can actually process data, otherwise SCSI commands
 *     will never get issued.
 *
 * 2.  NCR5380_main() shouldn't be called before it has exited, because
 *     other drivers have had kernel stack overflows in similar
 *     situations.
 *
 * 3.  We don't want to inline NCR5380_main() because of space concerns,
 *     even though it is only called in two places.
 *
 * So, the solution is to set the mutex in an inline wrapper for the 
 * main coroutine, and have the main coroutine exit with interrupts 
 * disabled after the final search through the queues so that no race 
 * conditions are possible.
 */

static volatile int main_running = 0;

/* 
 * Function : run_main(void)
 * 
 * Purpose : insure that the coroutine is running and will process our 
 *      request.  main_running is checked/set here (in an inline function)
 *      rather than in NCR5380_main itself to reduce the chances of stack
 *      overflow.
 *
 */

static __inline__ void run_main(void)
{
	unsigned long flags;
	save_flags(flags);
	cli();
	if (!main_running) {
		main_running = 1;
		NCR5380_main();
	}
	restore_flags(flags);
}

#ifdef USLEEP

/*
 * These need tweaking, and would probably work best as per-device 
 * flags initialized differently for disk, tape, cd, etc devices.
 * People with broken devices are free to experiment as to what gives
 * the best results for them.
 *
 * USLEEP_SLEEP should be a minimum seek time.
 *
 * USLEEP_POLL should be a maximum rotational latency.
 */
#ifndef USLEEP_SLEEP
/* 20 ms (reasonable hard disk speed) */
#define USLEEP_SLEEP (20*HZ/1000)
#endif
/* 300 RPM (floppy speed) */
#ifndef USLEEP_POLL
#define USLEEP_POLL (200*HZ/1000)
#endif
#ifndef USLEEP_WAITLONG
/* RvC: (reasonable time to wait on select error) */
#define USLEEP_WAITLONG USLEEP_SLEEP
#endif

static struct Scsi_Host *expires_first = NULL;

/* 
 * Function : int should_disconnect (unsigned char cmd)
 *
 * Purpose : decide weather a command would normally disconnect or 
 *      not, since if it won't disconnect we should go to sleep.
 *
 * Input : cmd - opcode of SCSI command
 *
 * Returns : DISCONNECT_LONG if we should disconnect for a really long 
 *      time (ie always, sleep, look for REQ active, sleep), 
 *      DISCONNECT_TIME_TO_DATA if we would only disconnect for a normal
 *      time-to-data delay, DISCONNECT_NONE if this command would return
 *      immediately.
 *
 *      Future sleep algorithms based on time to data can exploit 
 *      something like this so they can differentiate between "normal" 
 *      (ie, read, write, seek) and unusual commands (ie, * format).
 *
 * Note : We don't deal with commands that handle an immediate disconnect,
 *        
 */

static int should_disconnect(unsigned char cmd)
{
	switch (cmd) {
		case READ_6:
		    case WRITE_6:
		    case SEEK_6:
		    case READ_10:
		    case WRITE_10:
		    case SEEK_10:
		    return DISCONNECT_TIME_TO_DATA;
		case FORMAT_UNIT:
		    case SEARCH_HIGH:
		    case SEARCH_LOW:
		    case SEARCH_EQUAL:
		    return DISCONNECT_LONG;
		default:
		    return DISCONNECT_NONE;
	}
}

/*
 * Assumes instance->time_expires has been set in higher level code.
 */

static int NCR5380_set_timer(struct Scsi_Host *instance)
{
	unsigned long flags;
	struct Scsi_Host *tmp, **prev;

	save_flags(flags);
	cli();
	if (((struct NCR5380_hostdata *) (instance->hostdata))->next_timer) {
		restore_flags(flags);
		return -1;
	}
	for (prev = &expires_first, tmp = expires_first; tmp;
		prev = &(((struct NCR5380_hostdata *) tmp->hostdata)->next_timer), 
		tmp = ((struct NCR5380_hostdata *) tmp->hostdata)->next_timer)
		if (((struct NCR5380_hostdata *)instance->hostdata)->time_expires < 
			((struct NCR5380_hostdata *)tmp->hostdata)->time_expires) 
			break;

	((struct NCR5380_hostdata *) instance->hostdata)->next_timer = tmp;
	*prev = instance;
   
	del_timer(&usleep_timer);
	usleep_timer.expires = ((struct NCR5380_hostdata *) expires_first->hostdata)->time_expires;
	add_timer(&usleep_timer);
	restore_flags(flags);
	return 0;
}

/* Doing something about unwanted reentrancy here might be useful */
void NCR5380_timer_fn(unsigned long surplus_to_requirements)
{
	unsigned long flags;
	struct Scsi_Host *instance;
	save_flags(flags);
	cli();
	for (; expires_first &&
		time_before_eq(((struct NCR5380_hostdata *)expires_first->hostdata)->time_expires, jiffies); )
	{
		instance = ((struct NCR5380_hostdata *) expires_first->hostdata)->next_timer;
		((struct NCR5380_hostdata *) expires_first->hostdata)->next_timer = NULL;
		((struct NCR5380_hostdata *) expires_first->hostdata)->time_expires = 0;
		expires_first = instance;
	}

	del_timer(&usleep_timer);
	if (expires_first) 
	{
		usleep_timer.expires = ((struct NCR5380_hostdata *)expires_first->hostdata)->time_expires;
		add_timer(&usleep_timer);
	}
	restore_flags(flags);

	spin_lock_irqsave(&io_request_lock, flags);
	run_main();
	spin_unlock_irqrestore(&io_request_lock, flags);
}
#endif				/* def USLEEP */

static inline void NCR5380_all_init(void)
{
	static int done = 0;
	if (!done) {
#if (NDEBUG & NDEBUG_INIT)
		printk("scsi : NCR5380_all_init()\n");
#endif
		done = 1;
#ifdef USLEEP
		init_timer(&usleep_timer);
		usleep_timer.function = NCR5380_timer_fn;
#endif
	}
}

#ifdef AUTOPROBE_IRQ
/*
 * Function : int NCR5380_probe_irq (struct Scsi_Host *instance, int possible)
 * 
 * Purpose : autoprobe for the IRQ line used by the NCR5380.  
 *
 * Inputs : instance - pointer to this instance of the NCR5380 driver,
 *          possible - bitmask of permissible interrupts.
 *
 * Returns : number of the IRQ selected, IRQ_NONE if no interrupt fired.
 * 
 * XXX no effort is made to deal with spurious interrupts. 
 */


static int probe_irq __initdata = 0;

static void __init probe_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	probe_irq = irq;
}

static int __init NCR5380_probe_irq(struct Scsi_Host *instance, int possible)
{
	NCR5380_local_declare();
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	instance->hostdata;
	unsigned long timeout;
	int trying_irqs, i, mask;
	NCR5380_setup(instance);

	for (trying_irqs = i = 0, mask = 1; i < 16; ++i, mask <<= 1)
		if ((mask & possible) && (request_irq(i, &probe_intr, SA_INTERRUPT, "NCR-probe", NULL)
					  == 0))
			trying_irqs |= mask;

	timeout = jiffies + (250 * HZ / 1000);
	probe_irq = IRQ_NONE;

/*
 * A interrupt is triggered whenever BSY = false, SEL = true
 * and a bit set in the SELECT_ENABLE_REG is asserted on the 
 * SCSI bus.
 *
 * Note that the bus is only driven when the phase control signals
 * (I/O, C/D, and MSG) match those in the TCR, so we must reset that
 * to zero.
 */

	NCR5380_write(TARGET_COMMAND_REG, 0);
	NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
	NCR5380_write(OUTPUT_DATA_REG, hostdata->id_mask);
	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_DATA |
		      ICR_ASSERT_SEL);

	while (probe_irq == IRQ_NONE && time_before(jiffies,timeout))
		barrier();

	NCR5380_write(SELECT_ENABLE_REG, 0);
	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

	for (i = 0, mask = 1; i < 16; ++i, mask <<= 1)
		if (trying_irqs & mask)
			free_irq(i, NULL);

	return probe_irq;
}
#endif				/* AUTOPROBE_IRQ */

/*
 * Function : void NCR58380_print_options (struct Scsi_Host *instance)
 *
 * Purpose : called by probe code indicating the NCR5380 driver
 *           options that were selected.
 *
 * Inputs : instance, pointer to this instance.  Unused.
 */

static void __init NCR5380_print_options(struct Scsi_Host *instance)
{
	printk(" generic options"
#ifdef AUTOPROBE_IRQ
	       " AUTOPROBE_IRQ"
#endif
#ifdef AUTOSENSE
	       " AUTOSENSE"
#endif
#ifdef DIFFERENTIAL
	       " DIFFERENTIAL"
#endif
#ifdef REAL_DMA
	       " REAL DMA"
#endif
#ifdef REAL_DMA_POLL
	       " REAL DMA POLL"
#endif
#ifdef PARITY
	       " PARITY"
#endif
#ifdef PSEUDO_DMA
	       " PSEUDO DMA"
#endif
#ifdef SCSI2
	       " SCSI-2"
#endif
#ifdef UNSAFE
	       " UNSAFE "
#endif
	    );
#ifdef USLEEP
	printk(" USLEEP, USLEEP_POLL=%d USLEEP_SLEEP=%d", USLEEP_POLL, USLEEP_SLEEP);
#endif
	printk(" generic release=%d", NCR5380_PUBLIC_RELEASE);
	if (((struct NCR5380_hostdata *) instance->hostdata)->flags & FLAG_NCR53C400) {
		printk(" ncr53c400 release=%d", NCR53C400_PUBLIC_RELEASE);
	}
}

/*
 * Function : void NCR5380_print_status (struct Scsi_Host *instance)
 *
 * Purpose : print commands in the various queues, called from
 *      NCR5380_abort and NCR5380_debug to aid debugging.
 *
 * Inputs : instance, pointer to this instance.  
 */

static void NCR5380_print_status(struct Scsi_Host *instance)
{
	static char pr_bfr[512];
	char *start;
	int len;

	printk("NCR5380 : coroutine is%s running.\n",
	       main_running ? "" : "n't");

#ifdef NDEBUG
	NCR5380_print(instance);
	NCR5380_print_phase(instance);
#endif

	len = NCR5380_proc_info(pr_bfr, &start, 0, sizeof(pr_bfr),
				instance->host_no, 0);
	pr_bfr[len] = 0;
	printk("\n%s\n", pr_bfr);
}

/******************************************/
/*
 * /proc/scsi/[dtc pas16 t128 generic]/[0-ASC_NUM_BOARD_SUPPORTED]
 *
 * *buffer: I/O buffer
 * **start: if inout == FALSE pointer into buffer where user read should start
 * offset: current offset
 * length: length of buffer
 * hostno: Scsi_Host host_no
 * inout: TRUE - user is writing; FALSE - user is reading
 *
 * Return the number of bytes read from or written
 */

#undef SPRINTF
#define SPRINTF(args...) do { if(pos < buffer + length-80) pos += sprintf(pos, ## args); } while(0)
static
char *lprint_Scsi_Cmnd(Scsi_Cmnd * cmd, char *pos, char *buffer, int length);
static
char *lprint_command(unsigned char *cmd, char *pos, char *buffer, int len);
static
char *lprint_opcode(int opcode, char *pos, char *buffer, int length);

#ifndef NCR5380_proc_info
static
#endif
int NCR5380_proc_info(
			     char *buffer, char **start, off_t offset,
			     int length, int hostno, int inout)
{
	unsigned long flags;
	char *pos = buffer;
	struct Scsi_Host *instance;
	struct NCR5380_hostdata *hostdata;
	Scsi_Cmnd *ptr;

	for (instance = first_instance; instance &&
	     instance->host_no != hostno; instance = instance->next);
	if (!instance)
		return (-ESRCH);
	hostdata = (struct NCR5380_hostdata *) instance->hostdata;

	if (inout) {		/* Has data been written to the file ? */
#ifdef DTC_PUBLIC_RELEASE
		dtc_wmaxi = dtc_maxi = 0;
#endif
#ifdef PAS16_PUBLIC_RELEASE
		pas_wmaxi = pas_maxi = 0;
#endif
		return (-ENOSYS);	/* Currently this is a no-op */
	}
	SPRINTF("NCR5380 core release=%d.   ", NCR5380_PUBLIC_RELEASE);
	if (((struct NCR5380_hostdata *) instance->hostdata)->flags & FLAG_NCR53C400)
		SPRINTF("ncr53c400 release=%d.  ", NCR53C400_PUBLIC_RELEASE);
#ifdef DTC_PUBLIC_RELEASE
	SPRINTF("DTC 3180/3280 release %d", DTC_PUBLIC_RELEASE);
#endif
#ifdef T128_PUBLIC_RELEASE
	SPRINTF("T128 release %d", T128_PUBLIC_RELEASE);
#endif
#ifdef GENERIC_NCR5380_PUBLIC_RELEASE
	SPRINTF("Generic5380 release %d", GENERIC_NCR5380_PUBLIC_RELEASE);
#endif
#ifdef PAS16_PUBLIC_RELEASE
	SPRINTF("PAS16 release=%d", PAS16_PUBLIC_RELEASE);
#endif

	SPRINTF("\nBase Addr: 0x%05lX    ", (long) instance->base);
	SPRINTF("io_port: %04x      ", (int) instance->io_port);
	if (instance->irq == IRQ_NONE)
		SPRINTF("IRQ: None.\n");
	else
		SPRINTF("IRQ: %d.\n", instance->irq);

#ifdef DTC_PUBLIC_RELEASE
	SPRINTF("Highwater I/O busy_spin_counts -- write: %d  read: %d\n",
		dtc_wmaxi, dtc_maxi);
#endif
#ifdef PAS16_PUBLIC_RELEASE
	SPRINTF("Highwater I/O busy_spin_counts -- write: %d  read: %d\n",
		pas_wmaxi, pas_maxi);
#endif
	save_flags(flags);
	cli();
	SPRINTF("NCR5380 : coroutine is%s running.\n", main_running ? "" : "n't");
	if (!hostdata->connected)
		SPRINTF("scsi%d: no currently connected command\n", instance->host_no);
	else
		pos = lprint_Scsi_Cmnd((Scsi_Cmnd *) hostdata->connected,
				       pos, buffer, length);
	SPRINTF("scsi%d: issue_queue\n", instance->host_no);
	for (ptr = (Scsi_Cmnd *) hostdata->issue_queue; ptr;
	     ptr = (Scsi_Cmnd *) ptr->host_scribble)
		pos = lprint_Scsi_Cmnd(ptr, pos, buffer, length);

	SPRINTF("scsi%d: disconnected_queue\n", instance->host_no);
	for (ptr = (Scsi_Cmnd *) hostdata->disconnected_queue; ptr;
	     ptr = (Scsi_Cmnd *) ptr->host_scribble)
		pos = lprint_Scsi_Cmnd(ptr, pos, buffer, length);

	restore_flags(flags);
	*start = buffer;
	if (pos - buffer < offset)
		return 0;
	else if (pos - buffer - offset < length)
		return pos - buffer - offset;
	return length;
}

static
char *lprint_Scsi_Cmnd(Scsi_Cmnd * cmd, char *pos, char *buffer, int length)
{
	SPRINTF("scsi%d : destination target %d, lun %d\n",
		cmd->host->host_no, cmd->target, cmd->lun);
	SPRINTF("        command = ");
	pos = lprint_command(cmd->cmnd, pos, buffer, length);
	return (pos);
}

static
char *lprint_command(unsigned char *command,
		     char *pos, char *buffer, int length)
{
	int i, s;
	pos = lprint_opcode(command[0], pos, buffer, length);
	for (i = 1, s = COMMAND_SIZE(command[0]); i < s; ++i)
		SPRINTF("%02x ", command[i]);
	SPRINTF("\n");
	return (pos);
}

static
char *lprint_opcode(int opcode, char *pos, char *buffer, int length)
{
	SPRINTF("%2d (0x%02x)", opcode, opcode);
	return (pos);
}


/* 
 * Function : void NCR5380_init (struct Scsi_Host *instance, flags)
 *
 * Purpose : initializes *instance and corresponding 5380 chip,
 *      with flags OR'd into the initial flags value.
 *
 * Inputs : instance - instantiation of the 5380 driver.  
 *
 * Notes : I assume that the host, hostno, and id bits have been
 *      set correctly.  I don't care about the irq and other fields. 
 * 
 */

static void __init NCR5380_init(struct Scsi_Host *instance, int flags)
{
	NCR5380_local_declare();
	int i, pass;
	unsigned long timeout;
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	instance->hostdata;

	/* 
	 * On NCR53C400 boards, NCR5380 registers are mapped 8 past 
	 * the base address.
	 */

#ifdef NCR53C400
	if (flags & FLAG_NCR53C400)
		instance->NCR5380_instance_name += NCR53C400_address_adjust;
#endif

	NCR5380_setup(instance);

	NCR5380_all_init();

	hostdata->aborted = 0;
	hostdata->id_mask = 1 << instance->this_id;
	for (i = hostdata->id_mask; i <= 0x80; i <<= 1)
		if (i > hostdata->id_mask)
			hostdata->id_higher_mask |= i;
	for (i = 0; i < 8; ++i)
		hostdata->busy[i] = 0;
#ifdef REAL_DMA
	hostdata->dmalen = 0;
#endif
	hostdata->targets_present = 0;
	hostdata->connected = NULL;
	hostdata->issue_queue = NULL;
	hostdata->disconnected_queue = NULL;
#ifdef NCR5380_STATS
	for (i = 0; i < 8; ++i) {
		hostdata->time_read[i] = 0;
		hostdata->time_write[i] = 0;
		hostdata->bytes_read[i] = 0;
		hostdata->bytes_write[i] = 0;
	}
	hostdata->timebase = 0;
	hostdata->pendingw = 0;
	hostdata->pendingr = 0;
#endif

	/* The CHECK code seems to break the 53C400. Will check it later maybe */
	if (flags & FLAG_NCR53C400)
		hostdata->flags = FLAG_HAS_LAST_BYTE_SENT | flags;
	else
		hostdata->flags = FLAG_CHECK_LAST_BYTE_SENT | flags;

	if (!the_template) {
		the_template = instance->hostt;
		first_instance = instance;
	}
#ifdef USLEEP
	hostdata->time_expires = 0;
	hostdata->next_timer = NULL;
#endif

#ifndef AUTOSENSE
	if ((instance->cmd_per_lun > 1) || instance->can_queue > 1)
		)
		    printk("scsi%d : WARNING : support for multiple outstanding commands enabled\n"
			   "         without AUTOSENSE option, contingent allegiance conditions may\n"
		"         be incorrectly cleared.\n", instance->host_no);
#endif				/* def AUTOSENSE */

	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
	NCR5380_write(MODE_REG, MR_BASE);
	NCR5380_write(TARGET_COMMAND_REG, 0);
	NCR5380_write(SELECT_ENABLE_REG, 0);

#ifdef NCR53C400
	if (hostdata->flags & FLAG_NCR53C400) {
		NCR5380_write(C400_CONTROL_STATUS_REG, CSR_BASE);
	}
#endif

	/* 
	 * Detect and correct bus wedge problems.
	 *
	 * If the system crashed, it may have crashed in a state 
	 * where a SCSI command was still executing, and the 
	 * SCSI bus is not in a BUS FREE STATE.
	 *
	 * If this is the case, we'll try to abort the currently
	 * established nexus which we know nothing about, and that
	 * failing, do a hard reset of the SCSI bus 
	 */

	for (pass = 1; (NCR5380_read(STATUS_REG) & SR_BSY) &&
	     pass <= 6; ++pass) {
		switch (pass) {
		case 1:
		case 3:
		case 5:
			printk("scsi%d: SCSI bus busy, waiting up to five seconds\n",
			       instance->host_no);
			timeout = jiffies + 5 * HZ;
			while (time_before(jiffies,timeout) && (NCR5380_read(STATUS_REG) & SR_BSY));
			break;
		case 2:
			printk("scsi%d: bus busy, attempting abort\n",
			       instance->host_no);
			do_abort(instance);
			break;
		case 4:
			printk("scsi%d: bus busy, attempting reset\n",
			       instance->host_no);
			do_reset(instance);
			break;
		case 6:
			printk("scsi%d: bus locked solid or invalid override\n",
			       instance->host_no);
		}
	}
}

/* 
 * Function : int NCR5380_queue_command (Scsi_Cmnd *cmd, 
 *      void (*done)(Scsi_Cmnd *)) 
 *
 * Purpose :  enqueues a SCSI command
 *
 * Inputs : cmd - SCSI command, done - function called on completion, with
 *      a pointer to the command descriptor.
 * 
 * Returns : 0
 *
 * Side effects : 
 *      cmd is added to the per instance issue_queue, with minor 
 *      twiddling done to the host specific fields of cmd.  If the 
 *      main coroutine is not running, it is restarted.
 *
 */

/* Only make static if a wrapper function is used */
#ifndef NCR5380_queue_command
static
#endif
int NCR5380_queue_command(Scsi_Cmnd * cmd, void (*done) (Scsi_Cmnd *)) {
	struct Scsi_Host *instance = cmd->host;
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	 instance->hostdata;
	Scsi_Cmnd *tmp;

#if (NDEBUG & NDEBUG_NO_WRITE)
	switch (cmd->cmnd[0]) {
		case WRITE_6:
		    case WRITE_10:
		    printk("scsi%d : WRITE attempted with NO_WRITE debugging flag set\n",
			   instance->host_no);
		cmd->result = (DID_ERROR << 16);
		done(cmd);
		return 0;
	}
#endif				/* (NDEBUG & NDEBUG_NO_WRITE) */

#ifdef NCR5380_STATS
#if 0
	if (!hostdata->connected && !hostdata->issue_queue &&
	    !hostdata->disconnected_queue) {
		hostdata->timebase = jiffies;
	}
#endif
#ifdef NCR5380_STAT_LIMIT
	if (cmd->request_bufflen > NCR5380_STAT_LIMIT)
#endif
		switch (cmd->cmnd[0]) {
			case WRITE:
			    case WRITE_6:
			    case WRITE_10:
			    hostdata->time_write[cmd->target] -= (jiffies - hostdata->timebase);
			hostdata->bytes_write[cmd->target] += cmd->request_bufflen;
			hostdata->pendingw++;
			break;
			case READ:
			    case READ_6:
			    case READ_10:
			    hostdata->time_read[cmd->target] -= (jiffies - hostdata->timebase);
			hostdata->bytes_read[cmd->target] += cmd->request_bufflen;
			hostdata->pendingr++;
			break;
		}
#endif

	/* 
	 * We use the host_scribble field as a pointer to the next command  
	 * in a queue 
	 */

	cmd->host_scribble = NULL;
	cmd->scsi_done = done;

	cmd->result = 0;


	/* 
	 * Insert the cmd into the issue queue. Note that REQUEST SENSE 
	 * commands are added to the head of the queue since any command will
	 * clear the contingent allegiance condition that exists and the 
	 * sense data is only guaranteed to be valid while the condition exists.
	 */

	cli();
	if (!(hostdata->issue_queue) || (cmd->cmnd[0] == REQUEST_SENSE)) {
		LIST(cmd, hostdata->issue_queue);
		cmd->host_scribble = (unsigned char *) hostdata->issue_queue;
		hostdata->issue_queue = cmd;
	} else {
		for (tmp = (Scsi_Cmnd *) hostdata->issue_queue; tmp->host_scribble;
		     tmp = (Scsi_Cmnd *) tmp->host_scribble);
		LIST(cmd, tmp);
		tmp->host_scribble = (unsigned char *) cmd;
	}
#if (NDEBUG & NDEBUG_QUEUES)
	printk("scsi%d : command added to %s of queue\n", instance->host_no,
	       (cmd->cmnd[0] == REQUEST_SENSE) ? "head" : "tail");
#endif

/* Run the coroutine if it isn't already running. */
	run_main();
	return 0;
}

/*
 * Function : NCR5380_main (void) 
 *
 * Purpose : NCR5380_main is a coroutine that runs as long as more work can 
 *      be done on the NCR5380 host adapters in a system.  Both 
 *      NCR5380_queue_command() and NCR5380_intr() will try to start it 
 *      in case it is not running.
 * 
 * NOTE : NCR5380_main exits with interrupts *disabled*, the caller should 
 *  reenable them.  This prevents reentrancy and kernel stack overflow.
 */

static void NCR5380_main(void) {
	Scsi_Cmnd *tmp, *prev;
	struct Scsi_Host *instance;
	struct NCR5380_hostdata *hostdata;
	int done;
	unsigned long flags;

	/*
	 * We run (with interrupts disabled) until we're sure that none of 
	 * the host adapters have anything that can be done, at which point 
	 * we set main_running to 0 and exit.
	 *
	 * Interrupts are enabled before doing various other internal 
	 * instructions, after we've decided that we need to run through
	 * the loop again.
	 *
	 * this should prevent any race conditions.
	 */

	spin_unlock_irq(&io_request_lock);
	
	save_flags(flags);
	
	do {
		cli();		/* Freeze request queues */
		done = 1;
		for (instance = first_instance; instance &&
		     instance->hostt == the_template; instance = instance->next) {
			 hostdata = (struct NCR5380_hostdata *) instance->hostdata;
			 cli();
#ifdef USLEEP
			if (!hostdata->connected && !hostdata->selecting) {
#else
			if (!hostdata->connected) {
#endif			
#if (NDEBUG & NDEBUG_MAIN)
				printk("scsi%d : not connected\n", instance->host_no);
#endif
				/*
				 * Search through the issue_queue for a command destined
				 * for a target that's not busy.
				 */
#if (NDEBUG & NDEBUG_LISTS)
				for (tmp = (Scsi_Cmnd *) hostdata->issue_queue, prev = NULL; tmp && (tmp != prev); prev = tmp, tmp = (Scsi_Cmnd *) tmp->host_scribble);
				/*printk("%p  ", tmp); */
				if ((tmp == prev) && tmp)
					printk(" LOOP\n");	/* else printk("\n"); */
#endif
				for (tmp = (Scsi_Cmnd *) hostdata->issue_queue,
				     prev = NULL; tmp; prev = tmp, tmp = (Scsi_Cmnd *)
				     tmp->host_scribble) {

#if (NDEBUG & NDEBUG_LISTS)
					if (prev != tmp)
						printk("MAIN tmp=%p   target=%d   busy=%d lun=%d\n", tmp, tmp->target, hostdata->busy[tmp->target], tmp->lun);
#endif
					/*  When we find one, remove it from the issue queue. */
					if (!(hostdata->busy[tmp->target] & (1 << tmp->lun))) {
						if (prev) {
							REMOVE(prev, prev->host_scribble, tmp, tmp->host_scribble);
							prev->host_scribble = tmp->host_scribble;
						} else {
							REMOVE(-1, hostdata->issue_queue, tmp, tmp->host_scribble);
							hostdata->issue_queue = (Scsi_Cmnd *) tmp->host_scribble;
						}
						tmp->host_scribble = NULL;

						/* reenable interrupts after finding one */
						restore_flags(flags);

						/* 
						 * Attempt to establish an I_T_L nexus here. 
						 * On success, instance->hostdata->connected is set.
						 * On failure, we must add the command back to the
						 *   issue queue so we can keep trying. 
						 */
#if (NDEBUG & (NDEBUG_MAIN | NDEBUG_QUEUES))
						printk("scsi%d : main() : command for target %d lun %d removed from issue_queue\n",
						       instance->host_no, tmp->target, tmp->lun);
#endif

						/*
						 * A successful selection is defined as one that 
						 * leaves us with the command connected and 
						 * in hostdata->connected, OR has terminated the
						 * command.
						 *
						 * With successful commands, we fall through
						 * and see if we can do an information transfer,
						 * with failures we will restart.
						 */
#ifdef USLEEP
						hostdata->selecting = 0; /* RvC: have to preset this
							to indicate a new command is being performed */
#endif

						if (!NCR5380_select(instance, tmp,
						/* 
						 * REQUEST SENSE commands are issued without tagged
						 * queueing, even on SCSI-II devices because the 
						 * contingent allegiance condition exists for the 
						 * entire unit.
						 */
								    (tmp->cmnd[0] == REQUEST_SENSE) ? TAG_NONE :
							     TAG_NEXT)) {
							break;
						} else {
							cli();
							LIST(tmp, hostdata->issue_queue);
							tmp->host_scribble = (unsigned char *)
							    hostdata->issue_queue;
							hostdata->issue_queue = tmp;
							done = 0;
							restore_flags(flags);
#if (NDEBUG & (NDEBUG_MAIN | NDEBUG_QUEUES))
							printk("scsi%d : main(): select() failed, returned to issue_queue\n",
							       instance->host_no);
#endif
						}
					}	/* if target/lun is not busy */
				}	/* for */
			}	/* if (!hostdata->connected) */
#ifdef USLEEP
			if (hostdata->selecting) 
			{
				tmp = (Scsi_Cmnd *)hostdata->selecting;
				if (!NCR5380_select(instance, tmp, 
					(tmp->cmnd[0] == REQUEST_SENSE) ? TAG_NONE : TAG_NEXT)) 
				{
					/* Ok ?? */
				}
				else
				{
					/* RvC: device failed, so we wait a long time
					this is needed for Mustek scanners, that
					do not respond to commands immediately
					after a scan */
					printk(KERN_DEBUG "scsi%d: device %d did not respond in time\n",
						instance->host_no, tmp->target);
					cli();
					LIST(tmp, hostdata->issue_queue);
					tmp->host_scribble = (unsigned char *) hostdata->issue_queue;
					hostdata->issue_queue = tmp;
					restore_flags(flags);

					hostdata->time_expires = jiffies + USLEEP_WAITLONG;
					NCR5380_set_timer (instance);
				}
			} /* if hostdata->selecting */
#endif
			if (hostdata->connected
#ifdef REAL_DMA
			    && !hostdata->dmalen
#endif
#ifdef USLEEP
			    && (!hostdata->time_expires || time_before_eq(hostdata->time_expires, jiffies))
#endif
			    ) {
				restore_flags(flags);
#if (NDEBUG & NDEBUG_MAIN)
				printk("scsi%d : main() : performing information transfer\n",
				       instance->host_no);
#endif
				NCR5380_information_transfer(instance);
#if (NDEBUG & NDEBUG_MAIN)
				printk("scsi%d : main() : done set false\n", instance->host_no);
#endif
				done = 0;
			} else
				break;
		}		/* for instance */
	} while (!done);
	spin_lock_irq(&io_request_lock);
  /* 	cli();*/
	main_running = 0;
}

#ifndef DONT_USE_INTR
#include <linux/blk.h>
#include <linux/spinlock.h>

/*
 * Function : void NCR5380_intr (int irq)
 * 
 * Purpose : handle interrupts, reestablishing I_T_L or I_T_L_Q nexuses
 *      from the disconnected queue, and restarting NCR5380_main() 
 *      as required.
 *
 * Inputs : int irq, irq that caused this interrupt.
 *
 */

static void NCR5380_intr(int irq, void *dev_id, struct pt_regs *regs) {
	NCR5380_local_declare();
	struct Scsi_Host *instance;
	int done;
	unsigned char basr;
	unsigned long flags;

	 save_flags(flags);
	 cli();
#if (NDEBUG & NDEBUG_INTR)
	 printk("scsi : NCR5380 irq %d triggered\n", irq);
#endif
	do {
		done = 1;
		for (instance = first_instance; instance && (instance->hostt ==
				the_template); instance = instance->next)
			if (instance->irq == irq) {

				/* Look for pending interrupts */
				NCR5380_setup(instance);
				basr = NCR5380_read(BUS_AND_STATUS_REG);
				/* XXX dispatch to appropriate routine if found and done=0 */
				if (basr & BASR_IRQ) {
#if (NDEBUG & NDEBUG_INTR)
					NCR5380_print(instance);
#endif
					if ((NCR5380_read(STATUS_REG) & (SR_SEL | SR_IO)) ==
					    (SR_SEL | SR_IO)) {
						done = 0;
						restore_flags(flags);
#if (NDEBUG & NDEBUG_INTR)
						printk("scsi%d : SEL interrupt\n", instance->host_no);
#endif
						NCR5380_reselect(instance);
						(void) NCR5380_read(RESET_PARITY_INTERRUPT_REG);
					} else if (basr & BASR_PARITY_ERROR) {
#if (NDEBUG & NDEBUG_INTR)
						printk("scsi%d : PARITY interrupt\n", instance->host_no);
#endif
						(void) NCR5380_read(RESET_PARITY_INTERRUPT_REG);
					} else if ((NCR5380_read(STATUS_REG) & SR_RST) == SR_RST) {
#if (NDEBUG & NDEBUG_INTR)
						printk("scsi%d : RESET interrupt\n", instance->host_no);
#endif
						(void) NCR5380_read(RESET_PARITY_INTERRUPT_REG);
					} else {
/*  
 * XXX the rest of the interrupt conditions should *only* occur during a 
 * DMA transfer, which I haven't gotten around to fixing yet.
 */

#if defined(REAL_DMA)
						/*
						 * We should only get PHASE MISMATCH and EOP interrupts
						 * if we have DMA enabled, so do a sanity check based on
						 * the current setting of the MODE register.
						 */

						if ((NCR5380_read(MODE_REG) & MR_DMA) && ((basr &
						BASR_END_DMA_TRANSFER) ||
											  !(basr & BASR_PHASE_MATCH))) {
							int transfered;

							if (!hostdata->connected)
								panic("scsi%d : received end of DMA interrupt with no connected cmd\n",
								      instance->hostno);

							transfered = (hostdata->dmalen - NCR5380_dma_residual(instance));
							hostdata->connected->SCp.this_residual -= transferred;
							hostdata->connected->SCp.ptr += transferred;
							hostdata->dmalen = 0;

							(void) NCR5380_read(RESET_PARITY_INTERRUPT_REG);
#if NCR_TIMEOUT
							{
								unsigned long timeout = jiffies + NCR_TIMEOUT;

								spin_unlock_irq(&io_request_lock);
								while (NCR5380_read(BUS_AND_STATUS_REG) & BASR_ACK
								       && time_before(jiffies, timeout));
								spin_lock_irq(&io_request_lock);
								
								if (time_after_eq(jiffies, timeout) )
									printk("scsi%d: timeout at NCR5380.c:%d\n",
									       host->host_no, __LINE__);
							}
#else				/* NCR_TIMEOUT */
							while (NCR5380_read(BUS_AND_STATUS_REG) & BASR_ACK);
#endif

							NCR5380_write(MODE_REG, MR_BASE);
							NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
						}
#else
#if (NDEBUG & NDEBUG_INTR)
						printk("scsi : unknown interrupt, BASR 0x%X, MR 0x%X, SR 0x%x\n", basr, NCR5380_read(MODE_REG), NCR5380_read(STATUS_REG));
#endif
						(void) NCR5380_read(RESET_PARITY_INTERRUPT_REG);
#endif
					}
				}	/* if BASR_IRQ */
				if (!done)
					run_main();
			}	/* if (instance->irq == irq) */
	} while (!done);
}


static void do_NCR5380_intr(int irq, void *dev_id, struct pt_regs *regs) {
	unsigned long flags;

	 spin_lock_irqsave(&io_request_lock, flags);
	 NCR5380_intr(irq, dev_id, regs);
	 spin_unlock_irqrestore(&io_request_lock, flags);
}

#endif

#ifdef NCR5380_STATS
static void collect_stats(struct NCR5380_hostdata *hostdata, Scsi_Cmnd * cmd) {
#ifdef NCR5380_STAT_LIMIT
	if (cmd->request_bufflen > NCR5380_STAT_LIMIT)
#endif
		switch (cmd->cmnd[0]) {
			case WRITE:
			    case WRITE_6:
			    case WRITE_10:
			    hostdata->time_write[cmd->target] += (jiffies - hostdata->timebase);
			/*hostdata->bytes_write[cmd->target] += cmd->request_bufflen; */
			hostdata->pendingw--;
			break;
			case READ:
			case READ_6:
			case READ_10:
			hostdata->time_read[cmd->target] += (jiffies - hostdata->timebase);
			/*hostdata->bytes_read[cmd->target] += cmd->request_bufflen; */
			hostdata->pendingr--;
			break;
		}
}
#endif
/* 
 * Function : int NCR5380_select (struct Scsi_Host *instance, Scsi_Cmnd *cmd, 
 *      int tag);
 *
 * Purpose : establishes I_T_L or I_T_L_Q nexus for new or existing command,
 *      including ARBITRATION, SELECTION, and initial message out for 
 *      IDENTIFY and queue messages. 
 *
 * Inputs : instance - instantiation of the 5380 driver on which this 
 *      target lives, cmd - SCSI command to execute, tag - set to TAG_NEXT for 
 *      new tag, TAG_NONE for untagged queueing, otherwise set to the tag for 
 *      the command that is presently connected.
 * 
 * Returns : -1 if selection could not execute for some reason,
 *      0 if selection succeeded or failed because the target 
 *      did not respond.
 *
 * Side effects : 
 *      If bus busy, arbitration failed, etc, NCR5380_select() will exit 
 *              with registers as they should have been on entry - ie
 *              SELECT_ENABLE will be set appropriately, the NCR5380
 *              will cease to drive any SCSI bus signals.
 *
 *      If successful : I_T_L or I_T_L_Q nexus will be established, 
 *              instance->connected will be set to cmd.  
 *              SELECT interrupt will be disabled.
 *
 *      If failed (no target) : cmd->scsi_done() will be called, and the 
 *              cmd->result host byte set to DID_BAD_TARGET.
 */
static int NCR5380_select(struct Scsi_Host *instance, Scsi_Cmnd * cmd, int tag) {
	NCR5380_local_declare();
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *) instance->hostdata;
	unsigned char tmp[3], phase;
	unsigned char *data;
	int len;
	unsigned long timeout;
	unsigned long flags;
#ifdef USLEEP
	unsigned char value;
#endif

	NCR5380_setup(instance);

#ifdef USLEEP

	if (hostdata->selecting) 
	{
		goto part2;	/* RvC: sorry prof. Dijkstra, but it keeps the
				   rest of the code nearly the same */
	}
#endif

	 hostdata->restart_select = 0;
#if defined (NDEBUG) && (NDEBUG & NDEBUG_ARBITRATION)
	 NCR5380_print(instance);
	 printk("scsi%d : starting arbitration, id = %d\n", instance->host_no,
		instance->this_id);
#endif
	 save_flags(flags);
	 cli();

	/* 
	 * Set the phase bits to 0, otherwise the NCR5380 won't drive the 
	 * data bus during SELECTION.
	 */

	 NCR5380_write(TARGET_COMMAND_REG, 0);


	/* 
	 * Start arbitration.
	 */

	 NCR5380_write(OUTPUT_DATA_REG, hostdata->id_mask);
	 NCR5380_write(MODE_REG, MR_ARBITRATE);

	 restore_flags(flags);

	/* Wait for arbitration logic to complete */
#if NCR_TIMEOUT
	{
		unsigned long timeout = jiffies + 2 * NCR_TIMEOUT;

		spin_unlock_irq(&io_request_lock);

		while (!(NCR5380_read(INITIATOR_COMMAND_REG) & ICR_ARBITRATION_PROGRESS)
		       && time_before(jiffies,timeout));

		spin_lock_irq(&io_request_lock);
		       
		if (time_after_eq(jiffies,timeout)) {
			printk("scsi: arbitration timeout at %d\n", __LINE__);
			NCR5380_write(MODE_REG, MR_BASE);
			NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
			return -1;
		}
	}
#else				/* NCR_TIMEOUT */
	while (!(NCR5380_read(INITIATOR_COMMAND_REG) & ICR_ARBITRATION_PROGRESS));
#endif

#if (NDEBUG & NDEBUG_ARBITRATION)
	 printk("scsi%d : arbitration complete\n", instance->host_no);
/* Avoid GCC 2.4.5 asm needs to many reloads error */
	 __asm__("nop");
#endif

	/* 
	 * The arbitration delay is 2.2us, but this is a minimum and there is 
	 * no maximum so we can safely sleep for ceil(2.2) usecs to accommodate
	 * the integral nature of udelay().
	 *
	 */

	 udelay(3);

	/* Check for lost arbitration */
	if ((NCR5380_read(INITIATOR_COMMAND_REG) & ICR_ARBITRATION_LOST) ||
	     (NCR5380_read(CURRENT_SCSI_DATA_REG) & hostdata->id_higher_mask) ||
	  (NCR5380_read(INITIATOR_COMMAND_REG) & ICR_ARBITRATION_LOST)) {
		NCR5380_write(MODE_REG, MR_BASE);
#if (NDEBUG & NDEBUG_ARBITRATION)
		printk("scsi%d : lost arbitration, deasserting MR_ARBITRATE\n",
		       instance->host_no);
#endif
		return -1;
	}
	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_SEL);

	if (!(hostdata->flags & FLAG_DTC3181E) &&
		/* RvC: DTC3181E has some trouble with this
		 *	so we simply removed it. Seems to work with
		 *	only Mustek scanner attached
		 */
		(NCR5380_read(INITIATOR_COMMAND_REG) & ICR_ARBITRATION_LOST)) 
	{
		NCR5380_write(MODE_REG, MR_BASE);
		NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
#if (NDEBUG & NDEBUG_ARBITRATION)
		printk("scsi%d : lost arbitration, deasserting ICR_ASSERT_SEL\n",
		       instance->host_no);
#endif
		return -1;
	}
	/* 
	 * Again, bus clear + bus settle time is 1.2us, however, this is 
	 * a minimum so we'll udelay ceil(1.2)
	 */

	udelay(2);

#if (NDEBUG & NDEBUG_ARBITRATION)
	printk("scsi%d : won arbitration\n", instance->host_no);
#endif


	/* 
	 * Now that we have won arbitration, start Selection process, asserting 
	 * the host and target ID's on the SCSI bus.
	 */

	NCR5380_write(OUTPUT_DATA_REG, (hostdata->id_mask | (1 << cmd->target)));

	/* 
	 * Raise ATN while SEL is true before BSY goes false from arbitration,
	 * since this is the only way to guarantee that we'll get a MESSAGE OUT
	 * phase immediately after selection.
	 */

	NCR5380_write(INITIATOR_COMMAND_REG, (ICR_BASE | ICR_ASSERT_BSY |
		     ICR_ASSERT_DATA | ICR_ASSERT_ATN | ICR_ASSERT_SEL));
	NCR5380_write(MODE_REG, MR_BASE);

	/* 
	 * Reselect interrupts must be turned off prior to the dropping of BSY,
	 * otherwise we will trigger an interrupt.
	 */
	NCR5380_write(SELECT_ENABLE_REG, 0);

	/*
	 * The initiator shall then wait at least two deskew delays and release 
	 * the BSY signal.
	 */
	udelay(1);		/* wingel -- wait two bus deskew delay >2*45ns */

	/* Reset BSY */
	NCR5380_write(INITIATOR_COMMAND_REG, (ICR_BASE | ICR_ASSERT_DATA |
				       ICR_ASSERT_ATN | ICR_ASSERT_SEL));

	/* 
	 * Something weird happens when we cease to drive BSY - looks
	 * like the board/chip is letting us do another read before the 
	 * appropriate propagation delay has expired, and we're confusing
	 * a BSY signal from ourselves as the target's response to SELECTION.
	 *
	 * A small delay (the 'C++' frontend breaks the pipeline with an
	 * unnecessary jump, making it work on my 386-33/Trantor T128, the
	 * tighter 'C' code breaks and requires this) solves the problem - 
	 * the 1 us delay is arbitrary, and only used because this delay will 
	 * be the same on other platforms and since it works here, it should 
	 * work there.
	 *
	 * wingel suggests that this could be due to failing to wait
	 * one deskew delay.
	 */

	udelay(1);

#if (NDEBUG & NDEBUG_SELECTION)
	printk("scsi%d : selecting target %d\n", instance->host_no, cmd->target);
#endif

	/* 
	 * The SCSI specification calls for a 250 ms timeout for the actual 
	 * selection.
	 */

	timeout = jiffies + (250 * HZ / 1000);

	/* 
	 * XXX very interesting - we're seeing a bounce where the BSY we 
	 * asserted is being reflected / still asserted (propagation delay?)
	 * and it's detecting as true.  Sigh.
	 */

#ifdef USLEEP
	hostdata->select_time = 0; /* we count the clock ticks at which we polled */
	hostdata->selecting = cmd;

part2:
    	/* RvC: here we enter after a sleeping period, or immediately after
		execution of part 1
		we poll only once ech clock tick */
	value = NCR5380_read(STATUS_REG) & (SR_BSY | SR_IO);

	if (!value && (hostdata->select_time < 25)) 
	{
		/* RvC: we still must wait for a device response */
		hostdata->select_time++; /* after 25 ticks the device has failed */
		hostdata->time_expires = jiffies + 1;
		NCR5380_set_timer(instance);
		return 0;	/* RvC: we return here with hostdata->selecting set,
				   to go to sleep */
	}

	hostdata->selecting = 0; /* clear this pointer, because we passed the
				waiting period */
#else
	spin_unlock_irq(&io_request_lock);
	while (time_before(jiffies, timeout) && !(NCR5380_read(STATUS_REG) &
					(SR_BSY | SR_IO)));
	spin_lock_irq(&io_request_lock);
#endif
	if ((NCR5380_read(STATUS_REG) & (SR_SEL | SR_IO)) ==
	    (SR_SEL | SR_IO)) {
		NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
		NCR5380_reselect(instance);
		printk("scsi%d : reselection after won arbitration?\n",
		       instance->host_no);
		NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
		return -1;
	}
	/* 
	 * No less than two deskew delays after the initiator detects the 
	 * BSY signal is true, it shall release the SEL signal and may 
	 * change the DATA BUS.                                     -wingel
	 */

	udelay(1);

	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ATN);

	if (!(NCR5380_read(STATUS_REG) & SR_BSY)) {
		NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
		if (hostdata->targets_present & (1 << cmd->target)) {
			printk("scsi%d : weirdness\n", instance->host_no);
			if (hostdata->restart_select)
				printk("\trestart select\n");
#if (NDEBUG & NDEBUG_SELECTION)
			NCR5380_print(instance);
#endif
			NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
			return -1;
		}
		cmd->result = DID_BAD_TARGET << 16;
#ifdef NCR5380_STATS
		collect_stats(hostdata, cmd);
#endif
		cmd->scsi_done(cmd);
		NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
#if (NDEBUG & NDEBUG_SELECTION)
		printk("scsi%d : target did not respond within 250ms\n",
		       instance->host_no);
#endif
		NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
		return 0;
	}
	hostdata->targets_present |= (1 << cmd->target);

	/*
	 * Since we followed the SCSI spec, and raised ATN while SEL 
	 * was true but before BSY was false during selection, the information
	 * transfer phase should be a MESSAGE OUT phase so that we can send the
	 * IDENTIFY message.
	 * 
	 * If SCSI-II tagged queuing is enabled, we also send a SIMPLE_QUEUE_TAG
	 * message (2 bytes) with a tag ID that we increment with every command
	 * until it wraps back to 0.
	 *
	 * XXX - it turns out that there are some broken SCSI-II devices,
	 *       which claim to support tagged queuing but fail when more than
	 *       some number of commands are issued at once.
	 */

	/* Wait for start of REQ/ACK handshake */
#ifdef NCR_TIMEOUT
	{
		unsigned long timeout = jiffies + NCR_TIMEOUT;

		spin_unlock_irq(&io_request_lock);
		while (!(NCR5380_read(STATUS_REG) & SR_REQ) && time_before(jiffies, timeout));
		spin_lock_irq(&io_request_lock);
		
		if (time_after_eq(jiffies, timeout)) {
			printk("scsi%d: timeout at NCR5380.c:%d\n", instance->host_no, __LINE__);
			NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
			return -1;
		}
	}
#else				/* NCR_TIMEOUT */
	while (!(NCR5380_read(STATUS_REG) & SR_REQ));
#endif				/* def NCR_TIMEOUT */

#if (NDEBUG & NDEBUG_SELECTION)
	printk("scsi%d : target %d selected, going into MESSAGE OUT phase.\n",
	       instance->host_no, cmd->target);
#endif
	tmp[0] = IDENTIFY(((instance->irq == IRQ_NONE) ? 0 : 1), cmd->lun);
#ifdef SCSI2
	if (cmd->device->tagged_queue && (tag != TAG_NONE)) {
		tmp[1] = SIMPLE_QUEUE_TAG;
		if (tag == TAG_NEXT) {
			/* 0 is TAG_NONE, used to imply no tag for this command */
			if (cmd->device->current_tag == 0)
				cmd->device->current_tag = 1;

			cmd->tag = cmd->device->current_tag;
			cmd->device->current_tag++;
		} else
			cmd->tag = (unsigned char) tag;

		tmp[2] = cmd->tag;
		hostdata->last_message = SIMPLE_QUEUE_TAG;
		len = 3;
	} else
#endif				/* def SCSI2 */
	{
		len = 1;
		cmd->tag = 0;
	}

	/* Send message(s) */
	data = tmp;
	phase = PHASE_MSGOUT;
	NCR5380_transfer_pio(instance, &phase, &len, &data);
#if (NDEBUG & NDEBUG_SELECTION)
	printk("scsi%d : nexus established.\n", instance->host_no);
#endif
	/* XXX need to handle errors here */
	hostdata->connected = cmd;
#ifdef SCSI2
	if (!cmd->device->tagged_queue)
#endif
		hostdata->busy[cmd->target] |= (1 << cmd->lun);

	initialize_SCp(cmd);


	return 0;
}

/* 
 * Function : int NCR5380_transfer_pio (struct Scsi_Host *instance, 
 *      unsigned char *phase, int *count, unsigned char **data)
 *
 * Purpose : transfers data in given phase using polled I/O
 *
 * Inputs : instance - instance of driver, *phase - pointer to 
 *      what phase is expected, *count - pointer to number of 
 *      bytes to transfer, **data - pointer to data pointer.
 * 
 * Returns : -1 when different phase is entered without transferring
 *      maximum number of bytes, 0 if all bytes or transfered or exit
 *      is in same phase.
 *
 *      Also, *phase, *count, *data are modified in place.
 *
 * XXX Note : handling for bus free may be useful.
 */

/*
 * Note : this code is not as quick as it could be, however it 
 * IS 100% reliable, and for the actual data transfer where speed
 * counts, we will always do a pseudo DMA or DMA transfer.
 */

static int NCR5380_transfer_pio(struct Scsi_Host *instance,
		unsigned char *phase, int *count, unsigned char **data) {
	NCR5380_local_declare();
	register unsigned char p = *phase, tmp;
	register int c = *count;
	register unsigned char *d = *data;
#ifdef USLEEP
	/*
	 *	RvC: some administrative data to process polling time
	 */
	int break_allowed = 0;
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *) instance->hostdata;
#endif
	NCR5380_setup(instance);

#if (NDEBUG & NDEBUG_PIO)
	if (!(p & SR_IO))
		 printk("scsi%d : pio write %d bytes\n", instance->host_no, c);
	else
		 printk("scsi%d : pio read %d bytes\n", instance->host_no, c);
#endif

	/* 
	 * The NCR5380 chip will only drive the SCSI bus when the 
	 * phase specified in the appropriate bits of the TARGET COMMAND
	 * REGISTER match the STATUS REGISTER
	 */

	 NCR5380_write(TARGET_COMMAND_REG, PHASE_SR_TO_TCR(p));

#ifdef USLEEP
	/* RvC: don't know if this is necessary, but other SCSI I/O is short
	 *	so breaks are not necessary there
	 */
	if ((p == PHASE_DATAIN) || (p == PHASE_DATAOUT)) 
	{
		break_allowed = 1;
	}
#endif


	do {
		/* 
		 * Wait for assertion of REQ, after which the phase bits will be 
		 * valid 
		 */

#ifdef USLEEP
		/* RvC: we simply poll once, after that we stop temporarily
		 *	and let the device buffer fill up
		 *	if breaking is not allowed, we keep polling as long as needed
		 */

		while ( !((tmp = NCR5380_read(STATUS_REG)) & SR_REQ) &&
			!break_allowed );
		if (!(tmp & SR_REQ)) 
		{
			/* timeout condition */
			hostdata->time_expires = jiffies + USLEEP_SLEEP;
			NCR5380_set_timer (instance);
			break;
		}
#else
		while ( !((tmp = NCR5380_read(STATUS_REG)) & SR_REQ) ); 
#endif

#if (NDEBUG & NDEBUG_HANDSHAKE)
		printk("scsi%d : REQ detected\n", instance->host_no);
#endif

		/* Check for phase mismatch */
		if ((tmp & PHASE_MASK) != p) {
#if (NDEBUG & NDEBUG_PIO)
			printk("scsi%d : phase mismatch\n", instance->host_no);
			NCR5380_print_phase(instance);
#endif
			break;
		}
		/* Do actual transfer from SCSI bus to / from memory */ if (!(p & SR_IO))
			 NCR5380_write(OUTPUT_DATA_REG, *d);
		else
			*d = NCR5380_read(CURRENT_SCSI_DATA_REG);

		++d;

		/* 
		 * The SCSI standard suggests that in MSGOUT phase, the initiator
		 * should drop ATN on the last byte of the message phase
		 * after REQ has been asserted for the handshake but before
		 * the initiator raises ACK.
		 */

		if (!(p & SR_IO)) {
			if (!((p & SR_MSG) && c > 1)) {
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
					      ICR_ASSERT_DATA);
#if (NDEBUG & NDEBUG_PIO)
				NCR5380_print(instance);
#endif
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
				       ICR_ASSERT_DATA | ICR_ASSERT_ACK);
			} else {
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
				       ICR_ASSERT_DATA | ICR_ASSERT_ATN);
#if (NDEBUG & NDEBUG_PIO)
				NCR5380_print(instance);
#endif
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
					      ICR_ASSERT_DATA | ICR_ASSERT_ATN | ICR_ASSERT_ACK);
			}
		} else {
#if (NDEBUG & NDEBUG_PIO)
			NCR5380_print(instance);
#endif
			NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ACK);
		}

		while (NCR5380_read(STATUS_REG) & SR_REQ);

#if (NDEBUG & NDEBUG_HANDSHAKE)
		printk("scsi%d : req false, handshake complete\n", instance->host_no);
#endif

/*
 * We have several special cases to consider during REQ/ACK handshaking : 
 * 1.  We were in MSGOUT phase, and we are on the last byte of the 
 *      message.  ATN must be dropped as ACK is dropped.
 *
 * 2.  We are in a MSGIN phase, and we are on the last byte of the  
 *      message.  We must exit with ACK asserted, so that the calling
 *      code may raise ATN before dropping ACK to reject the message.
 *
 * 3.  ACK and ATN are clear and the target may proceed as normal.
 */
		if (!(p == PHASE_MSGIN && c == 1)) {
			if (p == PHASE_MSGOUT && c > 1)
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ATN);
			else
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
		}
	} while (--c);

#if (NDEBUG & NDEBUG_PIO)
	printk("scsi%d : residual %d\n", instance->host_no, c);
#endif

	*count = c;
	*data = d;
	tmp = NCR5380_read(STATUS_REG);
	if (tmp & SR_REQ)
		*phase = tmp & PHASE_MASK;
	else
		*phase = PHASE_UNKNOWN;

	if (!c || (*phase == p))
		return 0;
	else
		return -1;
}

static void do_reset(struct Scsi_Host *host) {
	unsigned long flags;
	 NCR5380_local_declare();
	 NCR5380_setup(host);

	 save_flags(flags);
	 cli();
	 NCR5380_write(TARGET_COMMAND_REG,
		 PHASE_SR_TO_TCR(NCR5380_read(STATUS_REG) & PHASE_MASK));
	 NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_RST);
	 udelay(25);
	 NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
	 restore_flags(flags);
}				/*

				 * Function : do_abort (Scsi_Host *host)
				 * 
				 * Purpose : abort the currently established nexus.  Should only be 
				 *      called from a routine which can drop into a 
				 * 
				 * Returns : 0 on success, -1 on failure.
				 */ static int do_abort(struct Scsi_Host *host) {
	NCR5380_local_declare();
	unsigned char tmp, *msgptr, phase;
	int len;
	 NCR5380_setup(host);


	/* Request message out phase */
	 NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ATN);

	/* 
	 * Wait for the target to indicate a valid phase by asserting 
	 * REQ.  Once this happens, we'll have either a MSGOUT phase 
	 * and can immediately send the ABORT message, or we'll have some 
	 * other phase and will have to source/sink data.
	 * 
	 * We really don't care what value was on the bus or what value
	 * the target sees, so we just handshake.
	 */

	while (!(tmp = NCR5380_read(STATUS_REG)) & SR_REQ);

	 NCR5380_write(TARGET_COMMAND_REG, PHASE_SR_TO_TCR(tmp));

	if ((tmp & PHASE_MASK) != PHASE_MSGOUT) {
		NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ATN |
			      ICR_ASSERT_ACK);
		while (NCR5380_read(STATUS_REG) & SR_REQ);
		NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ATN);
	}
	tmp = ABORT;
	msgptr = &tmp;
	len = 1;
	phase = PHASE_MSGOUT;
	NCR5380_transfer_pio(host, &phase, &len, &msgptr);

	/*
	 * If we got here, and the command completed successfully,
	 * we're about to go into bus free state.
	 */

	return len ? -1 : 0;
}

#if defined(REAL_DMA) || defined(PSEUDO_DMA) || defined (REAL_DMA_POLL)
/* 
 * Function : int NCR5380_transfer_dma (struct Scsi_Host *instance, 
 *      unsigned char *phase, int *count, unsigned char **data)
 *
 * Purpose : transfers data in given phase using either real
 *      or pseudo DMA.
 *
 * Inputs : instance - instance of driver, *phase - pointer to 
 *      what phase is expected, *count - pointer to number of 
 *      bytes to transfer, **data - pointer to data pointer.
 * 
 * Returns : -1 when different phase is entered without transferring
 *      maximum number of bytes, 0 if all bytes or transfered or exit
 *      is in same phase.
 *
 *      Also, *phase, *count, *data are modified in place.
 *
 */


static int NCR5380_transfer_dma(struct Scsi_Host *instance,
		unsigned char *phase, int *count, unsigned char **data) {
	NCR5380_local_declare();
	register int c = *count;
	register unsigned char p = *phase;
	register unsigned char *d = *data;
	unsigned char tmp;
	unsigned long flags;
	int foo;
#if defined(REAL_DMA_POLL)
	int cnt, toPIO;
	unsigned char saved_data = 0, overrun = 0, residue;
#endif

	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	 instance->hostdata;

	 NCR5380_setup(instance);

	if ((tmp = (NCR5380_read(STATUS_REG) & PHASE_MASK)) != p) {
		*phase = tmp;
		return -1;
	}
#if defined(REAL_DMA) || defined(REAL_DMA_POLL)
#ifdef READ_OVERRUNS if (p & SR_IO) { c -= 2;
}
#endif
#if (NDEBUG & NDEBUG_DMA)
printk("scsi%d : initializing DMA channel %d for %s, %d bytes %s %0x\n",
       instance->host_no, instance->dma_channel, (p & SR_IO) ? "reading" :
       "writing", c, (p & SR_IO) ? "to" : "from", (unsigned) d);
#endif
hostdata->dma_len = (p & SR_IO) ?
NCR5380_dma_read_setup(instance, d, c) :
NCR5380_dma_write_setup(instance, d, c);
#endif

NCR5380_write(TARGET_COMMAND_REG, PHASE_SR_TO_TCR(p));

#ifdef REAL_DMA
NCR5380_write(MODE_REG, MR_BASE | MR_DMA_MODE | MR_ENABLE_EOP_INTR | MR_MONITOR_BSY);
#elif defined(REAL_DMA_POLL)
NCR5380_write(MODE_REG, MR_BASE | MR_DMA_MODE);
#else
	/*
	 * Note : on my sample board, watch-dog timeouts occurred when interrupts
	 * were not disabled for the duration of a single DMA transfer, from 
	 * before the setting of DMA mode to after transfer of the last byte.
	 */

#if defined(PSEUDO_DMA) && !defined(UNSAFE)
save_flags(flags);
cli();
#endif
	/* KLL May need eop and parity in 53c400 */
if (hostdata->flags & FLAG_NCR53C400)
	NCR5380_write(MODE_REG, MR_BASE | MR_DMA_MODE | MR_ENABLE_PAR_CHECK
		  | MR_ENABLE_PAR_INTR | MR_ENABLE_EOP_INTR | MR_DMA_MODE
		      | MR_MONITOR_BSY);
else
	NCR5380_write(MODE_REG, MR_BASE | MR_DMA_MODE);
#endif				/* def REAL_DMA */

#if (NDEBUG & NDEBUG_DMA) & 0
printk("scsi%d : mode reg = 0x%X\n", instance->host_no, NCR5380_read(MODE_REG));
#endif

/* 
 * FOO stuff. For some UNAPPARENT reason, I'm getting 
 * watchdog timers fired on bootup for NO APPARENT REASON, meaning it's
 * probably a timing problem.
 *
 * Since this is the only place I have back-to-back writes, perhaps this 
 * is the problem?
 */

if (p & SR_IO)
{
#ifndef FOO
udelay(1);
#endif
NCR5380_write(START_DMA_INITIATOR_RECEIVE_REG, 0);
} else {
#ifndef FOO
	udelay(1);
#endif
	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_DATA);
#ifndef FOO
	udelay(1);
#endif
	NCR5380_write(START_DMA_SEND_REG, 0);
#ifndef FOO
	udelay(1);
#endif
}

#if defined(REAL_DMA_POLL)
do {
	tmp = NCR5380_read(BUS_AND_STATUS_REG);
} while ((tmp & BASR_PHASE_MATCH) && !(tmp & (BASR_BUSY_ERROR |

					      BASR_END_DMA_TRANSFER)));

/*
   At this point, either we've completed DMA, or we have a phase mismatch,
   or we've unexpectedly lost BUSY (which is a real error).

   For write DMAs, we want to wait until the last byte has been
   transferred out over the bus before we turn off DMA mode.  Alas, there
   seems to be no terribly good way of doing this on a 5380 under all
   conditions.  For non-scatter-gather operations, we can wait until REQ
   and ACK both go false, or until a phase mismatch occurs.  Gather-writes
   are nastier, since the device will be expecting more data than we
   are prepared to send it, and REQ will remain asserted.  On a 53C8[01] we
   could test LAST BIT SENT to assure transfer (I imagine this is precisely
   why this signal was added to the newer chips) but on the older 538[01]
   this signal does not exist.  The workaround for this lack is a watchdog;
   we bail out of the wait-loop after a modest amount of wait-time if
   the usual exit conditions are not met.  Not a terribly clean or
   correct solution :-%

   Reads are equally tricky due to a nasty characteristic of the NCR5380.
   If the chip is in DMA mode for an READ, it will respond to a target's
   REQ by latching the SCSI data into the INPUT DATA register and asserting
   ACK, even if it has _already_ been notified by the DMA controller that
   the current DMA transfer has completed!  If the NCR5380 is then taken
   out of DMA mode, this already-acknowledged byte is lost.

   This is not a problem for "one DMA transfer per command" reads, because
   the situation will never arise... either all of the data is DMA'ed
   properly, or the target switches to MESSAGE IN phase to signal a
   disconnection (either operation bringing the DMA to a clean halt).
   However, in order to handle scatter-reads, we must work around the
   problem.  The chosen fix is to DMA N-2 bytes, then check for the
   condition before taking the NCR5380 out of DMA mode.  One or two extra
   bytes are transferred via PIO as necessary to fill out the original
   request.
 */

if (p & SR_IO) {
#ifdef READ_OVERRUNS
	udelay(10);
	if (((NCR5380_read(BUS_AND_STATUS_REG) & (BASR_PHASE_MATCH | BASR_ACK)) ==
	     (BASR_PHASE_MATCH | BASR_ACK))) {
		saved_data = NCR5380_read(INPUT_DATA_REGISTER);
		overrun = 1;
	}
#endif
} else {
	int limit = 100;
	while (((tmp = NCR5380_read(BUS_AND_STATUS_REG)) & BASR_ACK) ||
	       (NCR5380_read(STATUS_REG) & SR_REQ)) {
		if (!(tmp & BASR_PHASE_MATCH))
			break;
		if (--limit < 0)
			break;
	}
}


#if (NDEBUG & NDEBUG_DMA)
printk("scsi%d : polled DMA transfer complete, basr 0x%X, sr 0x%X\n",
       instance->host_no, tmp, NCR5380_read(STATUS_REG));
#endif

NCR5380_write(MODE_REG, MR_BASE);
NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

residue = NCR5380_dma_residual(instance);
c -= residue;
*count -= c;
*data += c;
*phase = NCR5380_read(STATUS_REG) & PHASE_MASK;

#ifdef READ_OVERRUNS
if (*phase == p && (p & SR_IO) && residue == 0)
{
if (overrun) {
#if (NDEBUG & NDEBUG_DMA)
	printk("Got an input overrun, using saved byte\n");
#endif
	**data = saved_data;
	*data += 1;
	*count -= 1;
	cnt = toPIO = 1;
} else {
	printk("No overrun??\n");
	cnt = toPIO = 2;
}
#if (NDEBUG & NDEBUG_DMA)
printk("Doing %d-byte PIO to 0x%X\n", cnt, *data);
#endif
NCR5380_transfer_pio(instance, phase, &cnt, data);
*count -= toPIO - cnt;
}
#endif

#if (NDEBUG & NDEBUG_DMA)
printk("Return with data ptr = 0x%X, count %d, last 0x%X, next 0x%X\n",
       *data, *count, *(*data + *count - 1), *(*data + *count));
#endif
return 0;

#elif defined(REAL_DMA)
return 0;
#else				/* defined(REAL_DMA_POLL) */
if (p & SR_IO) {
#ifdef DMA_WORKS_RIGHT
	foo = NCR5380_pread(instance, d, c);
#else
	int diff = 1;
	if (hostdata->flags & FLAG_NCR53C400) {
		diff = 0;
	}
	if (!(foo = NCR5380_pread(instance, d, c - diff))) {
		/*
		 * We can't disable DMA mode after successfully transferring 
		 * what we plan to be the last byte, since that would open up
		 * a race condition where if the target asserted REQ before 
		 * we got the DMA mode reset, the NCR5380 would have latched
		 * an additional byte into the INPUT DATA register and we'd
		 * have dropped it.
		 * 
		 * The workaround was to transfer one fewer bytes than we 
		 * intended to with the pseudo-DMA read function, wait for 
		 * the chip to latch the last byte, read it, and then disable
		 * pseudo-DMA mode.
		 * 
		 * After REQ is asserted, the NCR5380 asserts DRQ and ACK.
		 * REQ is deasserted when ACK is asserted, and not reasserted
		 * until ACK goes false.  Since the NCR5380 won't lower ACK
		 * until DACK is asserted, which won't happen unless we twiddle
		 * the DMA port or we take the NCR5380 out of DMA mode, we 
		 * can guarantee that we won't handshake another extra 
		 * byte.
		 */

		if (!(hostdata->flags & FLAG_NCR53C400)) {
			while (!(NCR5380_read(BUS_AND_STATUS_REG) & BASR_DRQ));
			/* Wait for clean handshake */
			while (NCR5380_read(STATUS_REG) & SR_REQ);
			d[c - 1] = NCR5380_read(INPUT_DATA_REG);
		}
	}
#endif
} else {
#ifdef DMA_WORKS_RIGHT
	foo = NCR5380_pwrite(instance, d, c);
#else
	int timeout;
#if (NDEBUG & NDEBUG_C400_PWRITE)
	printk("About to pwrite %d bytes\n", c);
#endif
	if (!(foo = NCR5380_pwrite(instance, d, c))) {
		/*
		 * Wait for the last byte to be sent.  If REQ is being asserted for 
		 * the byte we're interested, we'll ACK it and it will go false.  
		 */
		if (!(hostdata->flags & FLAG_HAS_LAST_BYTE_SENT)) {
			timeout = 20000;
#if 1
#if 1
			while (!(NCR5380_read(BUS_AND_STATUS_REG) &
			 BASR_DRQ) && (NCR5380_read(BUS_AND_STATUS_REG) &
				       BASR_PHASE_MATCH));
#else
			if (NCR5380_read(STATUS_REG) & SR_REQ) {
				for (; timeout &&
				     !(NCR5380_read(BUS_AND_STATUS_REG) & BASR_ACK);
				     --timeout);
				for (; timeout && (NCR5380_read(STATUS_REG) & SR_REQ);
				     --timeout);
			}
#endif


#if (NDEBUG & NDEBUG_LAST_BYTE_SENT)
			if (!timeout)
				printk("scsi%d : timed out on last byte\n",
				       instance->host_no);
#endif


			if (hostdata->flags & FLAG_CHECK_LAST_BYTE_SENT) {
				hostdata->flags &= ~FLAG_CHECK_LAST_BYTE_SENT;
				if (NCR5380_read(TARGET_COMMAND_REG) & TCR_LAST_BYTE_SENT) {
					hostdata->flags |= FLAG_HAS_LAST_BYTE_SENT;
#if (NDEBUG & NDEBUG_LAST_BYTE_SENT)
					printk("scsi%d : last bit sent works\n",
					       instance->host_no);
#endif
				}
			}
		} else {
#if (NDEBUG & NDEBUG_C400_PWRITE)
			printk("Waiting for LASTBYTE\n");
#endif
			while (!(NCR5380_read(TARGET_COMMAND_REG) & TCR_LAST_BYTE_SENT));
#if (NDEBUG & NDEBUG_C400_PWRITE)
			printk("Got LASTBYTE\n");
#endif
		}
#else
			udelay(5);
#endif
		}
#endif
	}
	NCR5380_write(MODE_REG, MR_BASE);
	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

	if ((!(p & SR_IO)) && (hostdata->flags & FLAG_NCR53C400)) {
#if (NDEBUG & NDEBUG_C400_PWRITE)
		printk("53C400w: Checking for IRQ\n");
#endif
		if (NCR5380_read(BUS_AND_STATUS_REG) & BASR_IRQ) {
#if (NDEBUG & NDEBUG_C400_PWRITE)
			printk("53C400w:    got it, reading reset interrupt reg\n");
#endif
			NCR5380_read(RESET_PARITY_INTERRUPT_REG);
		} else {
			printk("53C400w:    IRQ NOT THERE!\n");
		}
	}
	*data = d + c;
	*count = 0;
	*phase = NCR5380_read(STATUS_REG) & PHASE_MASK;
#if 0
	NCR5380_print_phase(instance);
#endif
#if defined(PSEUDO_DMA) && !defined(UNSAFE)
	restore_flags(flags);
#endif				/* defined(REAL_DMA_POLL) */
	return foo;
#endif				/* def REAL_DMA */
}
#endif				/* defined(REAL_DMA) | defined(PSEUDO_DMA) */

/*
 * Function : NCR5380_information_transfer (struct Scsi_Host *instance)
 *
 * Purpose : run through the various SCSI phases and do as the target 
 *      directs us to.  Operates on the currently connected command, 
 *      instance->connected.
 *
 * Inputs : instance, instance for which we are doing commands
 *
 * Side effects : SCSI things happen, the disconnected queue will be 
 *      modified if a command disconnects, *instance->connected will
 *      change.
 *
 * XXX Note : we need to watch for bus free or a reset condition here 
 *      to recover from an unexpected bus free condition.
 */

static void NCR5380_information_transfer(struct Scsi_Host *instance) {
	NCR5380_local_declare();
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	 instance->hostdata;
	unsigned char msgout = NOP;
	int sink = 0;
	int len;
#if defined(PSEUDO_DMA) || defined(REAL_DMA_POLL)
	int transfersize;
#endif
	unsigned char *data;
	unsigned char phase, tmp, extended_msg[10], old_phase = 0xff;
	Scsi_Cmnd *cmd = (Scsi_Cmnd *) hostdata->connected;
#ifdef USLEEP
	/* RvC: we need to set the end of the polling time */
	unsigned long poll_time = jiffies + USLEEP_POLL;
#endif

	NCR5380_setup(instance);

	while (1) {
		tmp = NCR5380_read(STATUS_REG);
		/* We only have a valid SCSI phase when REQ is asserted */
		if (tmp & SR_REQ) {
			phase = (tmp & PHASE_MASK);
			if (phase != old_phase) {
				old_phase = phase;
#if (NDEBUG & NDEBUG_INFORMATION)
				NCR5380_print_phase(instance);
#endif
			}
			if (sink && (phase != PHASE_MSGOUT)) {
				NCR5380_write(TARGET_COMMAND_REG, PHASE_SR_TO_TCR(tmp));

				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_ATN |
					      ICR_ASSERT_ACK);
				while (NCR5380_read(STATUS_REG) & SR_REQ);
				NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
					      ICR_ASSERT_ATN);
				sink = 0;
				continue;
			}
			switch (phase) {
				case PHASE_DATAIN:
				    case PHASE_DATAOUT:
#if (NDEBUG & NDEBUG_NO_DATAOUT)
				    printk("scsi%d : NDEBUG_NO_DATAOUT set, attempted DATAOUT aborted\n",
					   instance->host_no);
				sink = 1;
				do_abort(instance);
				cmd->result = DID_ERROR << 16;
				cmd->done(cmd);
				return;
#endif
				/* 
				 * If there is no room left in the current buffer in the
				 * scatter-gather list, move onto the next one.
				 */

				if (!cmd->SCp.this_residual && cmd->SCp.buffers_residual) {
					++cmd->SCp.buffer;
					--cmd->SCp.buffers_residual;
					cmd->SCp.this_residual = cmd->SCp.buffer->length;
					cmd->SCp.ptr = cmd->SCp.buffer->address;
#if (NDEBUG & NDEBUG_INFORMATION)
					printk("scsi%d : %d bytes and %d buffers left\n",
					       instance->host_no, cmd->SCp.this_residual,
					       cmd->SCp.buffers_residual);
#endif
				}
				/*
				 * The preferred transfer method is going to be 
				 * PSEUDO-DMA for systems that are strictly PIO,
				 * since we can let the hardware do the handshaking.
				 *
				 * For this to work, we need to know the transfersize
				 * ahead of time, since the pseudo-DMA code will sit
				 * in an unconditional loop.
				 */

#if defined(PSEUDO_DMA) || defined(REAL_DMA_POLL)
				/* KLL
				 * PSEUDO_DMA is defined here. If this is the g_NCR5380
				 * driver then it will always be defined, so the
				 * FLAG_NO_PSEUDO_DMA is used to inhibit PDMA in the base
				 * NCR5380 case.  I think this is a fairly clean solution.
				 * We supplement these 2 if's with the flag.
				 */
#ifdef NCR5380_dma_xfer_len
				if (!cmd->device->borken &&
				!(hostdata->flags & FLAG_NO_PSEUDO_DMA) &&
				    (transfersize = NCR5380_dma_xfer_len(instance, cmd)) != 0) {
#else
				transfersize = cmd->transfersize;

#ifdef LIMIT_TRANSFERSIZE	/* If we have problems with interrupt service */
				if (transfersize > 512)
					transfersize = 512;
#endif				/* LIMIT_TRANSFERSIZE */

				if (!cmd->device->borken && transfersize &&
				!(hostdata->flags & FLAG_NO_PSEUDO_DMA) &&
				    cmd->SCp.this_residual && !(cmd->SCp.this_residual %
							 transfersize)) {
					/* Limit transfers to 32K, for xx400 & xx406
					 * pseudoDMA that transfers in 128 bytes blocks. */
					if (transfersize > 32 * 1024)
						transfersize = 32 * 1024;
#endif
					len = transfersize;
					if (NCR5380_transfer_dma(instance, &phase,
								 &len, (unsigned char **) &cmd->SCp.ptr)) {
						/*
						 * If the watchdog timer fires, all future accesses to this
						 * device will use the polled-IO.
						 */
						printk("scsi%d : switching target %d lun %d to slow handshake\n",
						       instance->host_no, cmd->target, cmd->lun);
						cmd->device->borken = 1;
						NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
							 ICR_ASSERT_ATN);
						sink = 1;
						do_abort(instance);
						cmd->result = DID_ERROR << 16;
						cmd->done(cmd);
						/* XXX - need to source or sink data here, as appropriate */
					} else
						cmd->SCp.this_residual -= transfersize - len;
				} else
#endif				/* defined(PSEUDO_DMA) || defined(REAL_DMA_POLL) */
					NCR5380_transfer_pio(instance, &phase,
							     (int *) &cmd->SCp.this_residual, (unsigned char **)
							  &cmd->SCp.ptr);
				break;
				case PHASE_MSGIN:
				    len = 1;
				data = &tmp;
				NCR5380_transfer_pio(instance, &phase, &len, &data);
				cmd->SCp.Message = tmp;

				switch (tmp) {
					/*
					 * Linking lets us reduce the time required to get the 
					 * next command out to the device, hopefully this will
					 * mean we don't waste another revolution due to the delays
					 * required by ARBITRATION and another SELECTION.
					 *
					 * In the current implementation proposal, low level drivers
					 * merely have to start the next command, pointed to by 
					 * next_link, done() is called as with unlinked commands.
					 */
#ifdef LINKED
					case LINKED_CMD_COMPLETE:
					    case LINKED_FLG_CMD_COMPLETE:
					/* Accept message by clearing ACK */
					    NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

#if (NDEBUG & NDEBUG_LINKED)
					printk("scsi%d : target %d lun %d linked command complete.\n",
					       instance->host_no, cmd->target, cmd->lun);
#endif
					/* 
					 * Sanity check : A linked command should only terminate with
					 * one of these messages if there are more linked commands
					 * available.
					 */

					if (!cmd->next_link) {
						printk("scsi%d : target %d lun %d linked command complete, no next_link\n"
						       instance->host_no, cmd->target, cmd->lun);
						sink = 1;
						do_abort(instance);
						return;
					}
					initialize_SCp(cmd->next_link);
					/* The next command is still part of this process */
					cmd->next_link->tag = cmd->tag;
					cmd->result = cmd->SCp.Status | (cmd->SCp.Message << 8);
#if (NDEBUG & NDEBUG_LINKED)
					printk("scsi%d : target %d lun %d linked request done, calling scsi_done().\n",
					       instance->host_no, cmd->target, cmd->lun);
#endif
#ifdef NCR5380_STATS
					collect_stats(hostdata, cmd);
#endif
					cmd->scsi_done(cmd);
					cmd = hostdata->connected;
					break;
#endif				/* def LINKED */
					case ABORT:
					    case COMMAND_COMPLETE:
					/* Accept message by clearing ACK */
					    sink = 1;
					NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
					hostdata->connected = NULL;
#if (NDEBUG & NDEBUG_QUEUES)
					printk("scsi%d : command for target %d, lun %d completed\n",
					       instance->host_no, cmd->target, cmd->lun);
#endif
					hostdata->busy[cmd->target] &= ~(1 << cmd->lun);

					/* 
					 * I'm not sure what the correct thing to do here is : 
					 * 
					 * If the command that just executed is NOT a request 
					 * sense, the obvious thing to do is to set the result
					 * code to the values of the stored parameters.
					 * 
					 * If it was a REQUEST SENSE command, we need some way 
					 * to differentiate between the failure code of the original
					 * and the failure code of the REQUEST sense - the obvious
					 * case is success, where we fall through and leave the result
					 * code unchanged.
					 * 
					 * The non-obvious place is where the REQUEST SENSE failed 
					 */

					if (cmd->cmnd[0] != REQUEST_SENSE)
						cmd->result = cmd->SCp.Status | (cmd->SCp.Message << 8);
					else if (cmd->SCp.Status != GOOD)
						cmd->result = (cmd->result & 0x00ffff) | (DID_ERROR << 16);

#ifdef AUTOSENSE
					if ((cmd->cmnd[0] != REQUEST_SENSE) &&
					    (cmd->SCp.Status == CHECK_CONDITION)) {
						unsigned long flags;
#if (NDEBUG & NDEBUG_AUTOSENSE)
						printk("scsi%d : performing request sense\n",
						       instance->host_no);
#endif
						cmd->cmnd[0] = REQUEST_SENSE;
						cmd->cmnd[1] &= 0xe0;
						cmd->cmnd[2] = 0;
						cmd->cmnd[3] = 0;
						cmd->cmnd[4] = sizeof(cmd->sense_buffer);
						cmd->cmnd[5] = 0;

						cmd->SCp.buffer = NULL;
						cmd->SCp.buffers_residual = 0;
						cmd->SCp.ptr = (char *) cmd->sense_buffer;
						cmd->SCp.this_residual = sizeof(cmd->sense_buffer);

						save_flags(flags);
						cli();
						LIST(cmd, hostdata->issue_queue);
						cmd->host_scribble = (unsigned char *)
						    hostdata->issue_queue;
						hostdata->issue_queue = (Scsi_Cmnd *) cmd;
						restore_flags(flags);
#if (NDEBUG & NDEBUG_QUEUES)
						printk("scsi%d : REQUEST SENSE added to head of issue queue\n", instance->host_no);
#endif
					} else {
#endif				/* def AUTOSENSE */
#ifdef NCR5380_STATS
						collect_stats(hostdata, cmd);
#endif
						cmd->scsi_done(cmd);
					}

					NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
					/* 
					 * Restore phase bits to 0 so an interrupted selection, 
					 * arbitration can resume.
					 */
					NCR5380_write(TARGET_COMMAND_REG, 0);

					while ((NCR5380_read(STATUS_REG) & SR_BSY) && !hostdata->connected)
						barrier();
					return;
					case MESSAGE_REJECT:
					/* Accept message by clearing ACK */
					    NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
					switch (hostdata->last_message) {
						case HEAD_OF_QUEUE_TAG:
						    case ORDERED_QUEUE_TAG:
						    case SIMPLE_QUEUE_TAG:
						    cmd->device->tagged_queue = 0;
						hostdata->busy[cmd->target] |= (1 << cmd->lun);
						break;
						default:
						    break;
					}
					case DISCONNECT: {
						unsigned long flags;
						/* Accept message by clearing ACK */
						NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
						cmd->device->disconnect = 1;
						save_flags(flags);
						cli();
						LIST(cmd, hostdata->disconnected_queue);
						cmd->host_scribble = (unsigned char *)
						    hostdata->disconnected_queue;
						hostdata->connected = NULL;
						hostdata->disconnected_queue = cmd;
						restore_flags(flags);
#if (NDEBUG & NDEBUG_QUEUES)
						printk("scsi%d : command for target %d lun %d was moved from connected to"
						       "  the disconnected_queue\n", instance->host_no,
						  cmd->target, cmd->lun);
#endif
						/* 
						 * Restore phase bits to 0 so an interrupted selection, 
						 * arbitration can resume.
						 */
						NCR5380_write(TARGET_COMMAND_REG, 0);

						/* Enable reselect interrupts */
						NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
						/* Wait for bus free to avoid nasty timeouts */
						while ((NCR5380_read(STATUS_REG) & SR_BSY) && !hostdata->connected)
							barrier();
#if 0
						NCR5380_print_status(instance);
#endif
						return;
					}
					/* 
					 * The SCSI data pointer is *IMPLICITLY* saved on a disconnect
					 * operation, in violation of the SCSI spec so we can safely 
					 * ignore SAVE/RESTORE pointers calls.
					 *
					 * Unfortunately, some disks violate the SCSI spec and 
					 * don't issue the required SAVE_POINTERS message before
					 * disconnecting, and we have to break spec to remain 
					 * compatible.
					 */
					case SAVE_POINTERS:
					    case RESTORE_POINTERS:
					/* Accept message by clearing ACK */
					    NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
					break;
					case EXTENDED_MESSAGE:
/* 
 * Extended messages are sent in the following format :
 * Byte         
 * 0            EXTENDED_MESSAGE == 1
 * 1            length (includes one byte for code, doesn't 
 *              include first two bytes)
 * 2            code
 * 3..length+1  arguments
 *
 * Start the extended message buffer with the EXTENDED_MESSAGE
 * byte, since print_msg() wants the whole thing.  
 */
					    extended_msg[0] = EXTENDED_MESSAGE;
					/* Accept first byte by clearing ACK */
					NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

#if (NDEBUG & NDEBUG_EXTENDED)
					printk("scsi%d : receiving extended message\n",
					       instance->host_no);
#endif

					len = 2;
					data = extended_msg + 1;
					phase = PHASE_MSGIN;
					NCR5380_transfer_pio(instance, &phase, &len, &data);

#if (NDEBUG & NDEBUG_EXTENDED)
					printk("scsi%d : length=%d, code=0x%02x\n",
					       instance->host_no, (int) extended_msg[1],
					       (int) extended_msg[2]);
#endif

					if (!len && extended_msg[1] <=
					    (sizeof(extended_msg) - 1)) {
						/* Accept third byte by clearing ACK */
						NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
						len = extended_msg[1] - 1;
						data = extended_msg + 3;
						phase = PHASE_MSGIN;

						NCR5380_transfer_pio(instance, &phase, &len, &data);

#if (NDEBUG & NDEBUG_EXTENDED)
						printk("scsi%d : message received, residual %d\n",
						 instance->host_no, len);
#endif

						switch (extended_msg[2]) {
							case EXTENDED_SDTR:
							    case EXTENDED_WDTR:
							    case EXTENDED_MODIFY_DATA_POINTER:
							    case EXTENDED_EXTENDED_IDENTIFY:
							    tmp = 0;
						}
					} else if (len) {
						printk("scsi%d: error receiving extended message\n",
						       instance->host_no);
						tmp = 0;
					} else {
						printk("scsi%d: extended message code %02x length %d is too long\n",
						       instance->host_no, extended_msg[2], extended_msg[1]);
						tmp = 0;
					}
					/* Fall through to reject message */

					/* 
					 * If we get something weird that we aren't expecting, 
					 * reject it.
					 */
					default:
					    if (!tmp) {
						printk("scsi%d: rejecting message ", instance->host_no);
						print_msg(extended_msg);
						printk("\n");
					} else if (tmp != EXTENDED_MESSAGE)
						printk("scsi%d: rejecting unknown message %02x from target %d, lun %d\n",
						       instance->host_no, tmp, cmd->target, cmd->lun);
					else
						printk("scsi%d: rejecting unknown extended message code %02x, length %d from target %d, lun %d\n",
						       instance->host_no, extended_msg[1], extended_msg[0], cmd->target, cmd->lun);

					msgout = MESSAGE_REJECT;
					NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE |
						      ICR_ASSERT_ATN);
					break;
				}	/* switch (tmp) */
				break;
				case PHASE_MSGOUT:
				    len = 1;
				data = &msgout;
				hostdata->last_message = msgout;
				NCR5380_transfer_pio(instance, &phase, &len, &data);
				if (msgout == ABORT) {
					hostdata->busy[cmd->target] &= ~(1 << cmd->lun);
					hostdata->connected = NULL;
					cmd->result = DID_ERROR << 16;
#ifdef NCR5380_STATS
					collect_stats(hostdata, cmd);
#endif
					cmd->scsi_done(cmd);
					NCR5380_write(SELECT_ENABLE_REG, hostdata->id_mask);
					return;
				}
				msgout = NOP;
				break;
				case PHASE_CMDOUT:
				    len = cmd->cmd_len;
				data = cmd->cmnd;
				/* 
				 * XXX for performance reasons, on machines with a 
				 * PSEUDO-DMA architecture we should probably 
				 * use the dma transfer function.  
				 */
				NCR5380_transfer_pio(instance, &phase, &len,
						     &data);
#ifdef USLEEP
				if (!cmd->device->disconnect &&
					should_disconnect(cmd->cmnd[0])) 
				{
					hostdata->time_expires = jiffies + USLEEP_SLEEP;
#if (NDEBUG & NDEBUG_USLEEP)
					printk("scsi%d : issued command, sleeping until %ul\n", instance->host_no,
					       hostdata->time_expires);
#endif
					NCR5380_set_timer(instance);
					return;
				}
#endif				/* def USLEEP */
				break;
				case PHASE_STATIN:
				    len = 1;
				data = &tmp;
				NCR5380_transfer_pio(instance, &phase, &len, &data);
				cmd->SCp.Status = tmp;
				break;
				default:
				    printk("scsi%d : unknown phase\n", instance->host_no);
#ifdef NDEBUG
				NCR5380_print(instance);
#endif
			}	/* switch(phase) */
		}		/* if (tmp * SR_REQ) */
#ifdef USLEEP
		else 
		{
			/* RvC: go to sleep if polling time expired
			 */
			if (!cmd->device->disconnect && time_after_eq(jiffies, poll_time))
			{
				hostdata->time_expires = jiffies + USLEEP_SLEEP;
#if (NDEBUG & NDEBUG_USLEEP)
				printk("scsi%d : poll timed out, sleeping until %ul\n", instance->host_no,
				       hostdata->time_expires);
#endif
				NCR5380_set_timer(instance);
				return;
			}
		}
#endif
	}			/* while (1) */
}

/*
 * Function : void NCR5380_reselect (struct Scsi_Host *instance)
 *
 * Purpose : does reselection, initializing the instance->connected 
 *      field to point to the Scsi_Cmnd for which the I_T_L or I_T_L_Q 
 *      nexus has been reestablished,
 *      
 * Inputs : instance - this instance of the NCR5380.
 *
 */


static void NCR5380_reselect(struct Scsi_Host *instance) {
	NCR5380_local_declare();
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	 instance->hostdata;
	unsigned char target_mask;
	unsigned char lun, phase;
	int len;
#ifdef SCSI2
	unsigned char tag;
#endif
	unsigned char msg[3];
	unsigned char *data;
	Scsi_Cmnd *tmp = NULL, *prev;
	int abort = 0;
	 NCR5380_setup(instance);

	/*
	 * Disable arbitration, etc. since the host adapter obviously
	 * lost, and tell an interrupted NCR5380_select() to restart.
	 */

	 NCR5380_write(MODE_REG, MR_BASE);
	 hostdata->restart_select = 1;

	 target_mask = NCR5380_read(CURRENT_SCSI_DATA_REG) & ~(hostdata->id_mask);

#if (NDEBUG & NDEBUG_RESELECTION)
	 printk("scsi%d : reselect\n", instance->host_no);
#endif

	/* 
	 * At this point, we have detected that our SCSI ID is on the bus,
	 * SEL is true and BSY was false for at least one bus settle delay
	 * (400 ns).
	 *
	 * We must assert BSY ourselves, until the target drops the SEL
	 * signal.
	 */

	 NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE | ICR_ASSERT_BSY);

	while (NCR5380_read(STATUS_REG) & SR_SEL);
	 NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

	/*
	 * Wait for target to go into MSGIN.
	 */

	while (!(NCR5380_read(STATUS_REG) & SR_REQ));

	 len = 1;
	 data = msg;
	 phase = PHASE_MSGIN;
	 NCR5380_transfer_pio(instance, &phase, &len, &data);


	if (!msg[0] & 0x80) {
		printk("scsi%d : expecting IDENTIFY message, got ",
		       instance->host_no);
		print_msg(msg);
		abort = 1;
	} else {
		/* Accept message by clearing ACK */
		NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);
		lun = (msg[0] & 0x07);

		/* 
		 * We need to add code for SCSI-II to track which devices have
		 * I_T_L_Q nexuses established, and which have simple I_T_L
		 * nexuses so we can chose to do additional data transfer.
		 */

#ifdef SCSI2
#error "SCSI-II tagged queueing is not supported yet"
#endif

		/* 
		 * Find the command corresponding to the I_T_L or I_T_L_Q  nexus we 
		 * just reestablished, and remove it from the disconnected queue.
		 */


		for (tmp = (Scsi_Cmnd *) hostdata->disconnected_queue, prev = NULL;
		 tmp; prev = tmp, tmp = (Scsi_Cmnd *) tmp->host_scribble)
			if ((target_mask == (1 << tmp->target)) && (lun == tmp->lun)
#ifdef SCSI2
			    && (tag == tmp->tag)
#endif
			    ) {
				if (prev) {
					REMOVE(prev, prev->host_scribble, tmp, tmp->host_scribble);
					prev->host_scribble = tmp->host_scribble;
				} else {
					REMOVE(-1, hostdata->disconnected_queue, tmp, tmp->host_scribble);
					hostdata->disconnected_queue = (Scsi_Cmnd *) tmp->host_scribble;
				}
				tmp->host_scribble = NULL;
				break;
			}
		if (!tmp) {
#ifdef SCSI2
			printk("scsi%d : warning : target bitmask %02x lun %d tag %d not in disconnect_queue.\n",
			       instance->host_no, target_mask, lun, tag);
#else
			printk("scsi%d : warning : target bitmask %02x lun %d not in disconnect_queue.\n",
			       instance->host_no, target_mask, lun);
#endif
			/* 
			 * Since we have an established nexus that we can't do anything with,
			 * we must abort it.  
			 */
			abort = 1;
		}
	}

	if (abort) {
		do_abort(instance);
	} else {
		hostdata->connected = tmp;
#if (NDEBUG & NDEBUG_RESELECTION)
		printk("scsi%d : nexus established, target = %d, lun = %d, tag = %d\n",
		     instance->host_no, tmp->target, tmp->lun, tmp->tag);
#endif
	}
}

/*
 * Function : void NCR5380_dma_complete (struct Scsi_Host *instance)
 *
 * Purpose : called by interrupt handler when DMA finishes or a phase
 *      mismatch occurs (which would finish the DMA transfer).  
 *
 * Inputs : instance - this instance of the NCR5380.
 *
 * Returns : pointer to the Scsi_Cmnd structure for which the I_T_L
 *      nexus has been reestablished, on failure NULL is returned.
 */

#ifdef REAL_DMA
static void NCR5380_dma_complete(NCR5380_instance * instance) {
	NCR5380_local_declare();
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *
					     instance->hostdata);
	int transferred;
	NCR5380_setup(instance);

	/*
	 * XXX this might not be right.
	 *
	 * Wait for final byte to transfer, ie wait for ACK to go false.
	 *
	 * We should use the Last Byte Sent bit, unfortunately this is 
	 * not available on the 5380/5381 (only the various CMOS chips)
	 */

	while (NCR5380_read(BUS_AND_STATUS_REG) & BASR_ACK);

	NCR5380_write(MODE_REG, MR_BASE);
	NCR5380_write(INITIATOR_COMMAND_REG, ICR_BASE);

	/*
	 * The only places we should see a phase mismatch and have to send
	 * data from the same set of pointers will be the data transfer
	 * phases.  So, residual, requested length are only important here.
	 */

	if (!(hostdata->connected->SCp.phase & SR_CD)) {
		transferred = instance->dmalen - NCR5380_dma_residual();
		hostdata->connected->SCp.this_residual -= transferred;
		hostdata->connected->SCp.ptr += transferred;
	}
}
#endif				/* def REAL_DMA */

/*
 * Function : int NCR5380_abort (Scsi_Cmnd *cmd)
 *
 * Purpose : abort a command
 *
 * Inputs : cmd - the Scsi_Cmnd to abort, code - code to set the 
 *      host byte of the result field to, if zero DID_ABORTED is 
 *      used.
 *
 * Returns : 0 - success, -1 on failure.
 *
 * XXX - there is no way to abort the command that is currently 
 *       connected, you have to wait for it to complete.  If this is 
 *       a problem, we could implement longjmp() / setjmp(), setjmp()
 *       called where the loop started in NCR5380_main().
 */

#ifndef NCR5380_abort
static
#endif
int NCR5380_abort(Scsi_Cmnd * cmd) {
	NCR5380_local_declare();
	unsigned long flags;
	struct Scsi_Host *instance = cmd->host;
	struct NCR5380_hostdata *hostdata = (struct NCR5380_hostdata *)
	 instance->hostdata;
	Scsi_Cmnd *tmp, **prev;

	printk("scsi%d : aborting command\n", instance->host_no);
	print_Scsi_Cmnd(cmd);

	NCR5380_print_status(instance);

	printk("scsi%d : aborting command\n", instance->host_no);
	print_Scsi_Cmnd(cmd);

	NCR5380_print_status(instance);

	save_flags(flags);
	cli();
	NCR5380_setup(instance);

#if (NDEBUG & NDEBUG_ABORT)
	printk("scsi%d : abort called\n", instance->host_no);
	printk("        basr 0x%X, sr 0x%X\n",
	     NCR5380_read(BUS_AND_STATUS_REG), NCR5380_read(STATUS_REG));
#endif

#if 0
/*
 * Case 1 : If the command is the currently executing command, 
 * we'll set the aborted flag and return control so that 
 * information transfer routine can exit cleanly.
 */

	if (hostdata->connected == cmd) {
#if (NDEBUG & NDEBUG_ABORT)
		printk("scsi%d : aborting connected command\n", instance->host_no);
#endif
		hostdata->aborted = 1;
/*
 * We should perform BSY checking, and make sure we haven't slipped
 * into BUS FREE.
 */

		NCR5380_write(INITIATOR_COMMAND_REG, ICR_ASSERT_ATN);
/* 
 * Since we can't change phases until we've completed the current 
 * handshake, we have to source or sink a byte of data if the current
 * phase is not MSGOUT.
 */

/* 
 * Return control to the executing NCR drive so we can clear the
 * aborted flag and get back into our main loop.
 */

		return 0;
	}
#endif

/* 
 * Case 2 : If the command hasn't been issued yet, we simply remove it 
 *          from the issue queue.
 */
#if (NDEBUG & NDEBUG_ABORT)
	/* KLL */
	printk("scsi%d : abort going into loop.\n", instance->host_no);
#endif
	for (prev = (Scsi_Cmnd **) & (hostdata->issue_queue),
	     tmp = (Scsi_Cmnd *) hostdata->issue_queue;
	     tmp; prev = (Scsi_Cmnd **) & (tmp->host_scribble), tmp =
	     (Scsi_Cmnd *) tmp->host_scribble)
		if (cmd == tmp) {
			REMOVE(5, *prev, tmp, tmp->host_scribble);
			(*prev) = (Scsi_Cmnd *) tmp->host_scribble;
			tmp->host_scribble = NULL;
			tmp->result = DID_ABORT << 16;
			restore_flags(flags);
#if (NDEBUG & NDEBUG_ABORT)
			printk("scsi%d : abort removed command from issue queue.\n",
			       instance->host_no);
#endif
			tmp->done(tmp);
			return SCSI_ABORT_SUCCESS;
		}
#if (NDEBUG  & NDEBUG_ABORT)
	/* KLL */
		else if (prev == tmp)
			printk("scsi%d : LOOP\n", instance->host_no);
#endif

/* 
 * Case 3 : If any commands are connected, we're going to fail the abort
 *          and let the high level SCSI driver retry at a later time or 
 *          issue a reset.
 *
 *          Timeouts, and therefore aborted commands, will be highly unlikely
 *          and handling them cleanly in this situation would make the common
 *          case of noresets less efficient, and would pollute our code.  So,
 *          we fail.
 */

	if (hostdata->connected) {
		restore_flags(flags);
#if (NDEBUG & NDEBUG_ABORT)
		printk("scsi%d : abort failed, command connected.\n", instance->host_no);
#endif
		return SCSI_ABORT_NOT_RUNNING;
	}
/*
 * Case 4: If the command is currently disconnected from the bus, and 
 *      there are no connected commands, we reconnect the I_T_L or 
 *      I_T_L_Q nexus associated with it, go into message out, and send 
 *      an abort message.
 *
 * This case is especially ugly. In order to reestablish the nexus, we
 * need to call NCR5380_select().  The easiest way to implement this 
 * function was to abort if the bus was busy, and let the interrupt
 * handler triggered on the SEL for reselect take care of lost arbitrations
 * where necessary, meaning interrupts need to be enabled.
 *
 * When interrupts are enabled, the queues may change - so we 
 * can't remove it from the disconnected queue before selecting it
 * because that could cause a failure in hashing the nexus if that 
 * device reselected.
 * 
 * Since the queues may change, we can't use the pointers from when we
 * first locate it.
 *
 * So, we must first locate the command, and if NCR5380_select()
 * succeeds, then issue the abort, relocate the command and remove
 * it from the disconnected queue.
 */

	for (tmp = (Scsi_Cmnd *) hostdata->disconnected_queue; tmp;
	     tmp = (Scsi_Cmnd *) tmp->host_scribble)
		if (cmd == tmp) {
			restore_flags(flags);
#if (NDEBUG & NDEBUG_ABORT)
			printk("scsi%d : aborting disconnected command.\n", instance->host_no);
#endif

			if (NCR5380_select(instance, cmd, (int) cmd->tag))
				return SCSI_ABORT_BUSY;

#if (NDEBUG & NDEBUG_ABORT)
			printk("scsi%d : nexus reestablished.\n", instance->host_no);
#endif

			do_abort(instance);

			cli();
			for (prev = (Scsi_Cmnd **) & (hostdata->disconnected_queue),
			tmp = (Scsi_Cmnd *) hostdata->disconnected_queue;
			     tmp; prev = (Scsi_Cmnd **) & (tmp->host_scribble), tmp =
			     (Scsi_Cmnd *) tmp->host_scribble)
				if (cmd == tmp) {
					REMOVE(5, *prev, tmp, tmp->host_scribble);
					*prev = (Scsi_Cmnd *) tmp->host_scribble;
					tmp->host_scribble = NULL;
					tmp->result = DID_ABORT << 16;
					restore_flags(flags);
					tmp->done(tmp);
					return SCSI_ABORT_SUCCESS;
				}
		}
/*
 * Case 5 : If we reached this point, the command was not found in any of 
 *          the queues.
 *
 * We probably reached this point because of an unlikely race condition
 * between the command completing successfully and the abortion code,
 * so we won't panic, but we will notify the user in case something really
 * broke.
 */

	restore_flags(flags);
	printk("scsi%d : warning : SCSI command probably completed successfully\n"
	       "         before abortion\n", instance->host_no);
	return SCSI_ABORT_NOT_RUNNING;
}


/* 
 * Function : int NCR5380_reset (Scsi_Cmnd *cmd, unsigned int reset_flags)
 * 
 * Purpose : reset the SCSI bus.
 *
 * Returns : SCSI_RESET_WAKEUP
 *
 */

#ifndef NCR5380_reset
static
#endif
int NCR5380_reset(Scsi_Cmnd * cmd, unsigned int dummy) {
	NCR5380_local_declare();
	NCR5380_setup(cmd->host);

	NCR5380_print_status(cmd->host);
	do_reset(cmd->host);

	return SCSI_RESET_WAKEUP;
}
