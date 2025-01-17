/*  $Id: setup.c,v 1.111 1999/09/10 10:40:24 davem Exp $
 *  linux/arch/sparc/kernel/setup.c
 *
 *  Copyright (C) 1995  David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <asm/smp.h>
#include <linux/user.h>
#include <linux/a.out.h>
#include <linux/tty.h>
#include <linux/delay.h>
#include <linux/config.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/blk.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/console.h>
#include <linux/spinlock.h>

#include <asm/segment.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/kgdb.h>
#include <asm/processor.h>
#include <asm/oplib.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/traps.h>
#include <asm/vaddrs.h>
#include <asm/kdebug.h>
#include <asm/mbus.h>
#include <asm/idprom.h>
#include <asm/softirq.h>
#include <asm/hardirq.h>
#include <asm/machines.h>

struct screen_info screen_info = {
	0, 0,			/* orig-x, orig-y */
	0,			/* unused */
	0,			/* orig-video-page */
	0,			/* orig-video-mode */
	128,			/* orig-video-cols */
	0,0,0,			/* ega_ax, ega_bx, ega_cx */
	54,			/* orig-video-lines */
	0,                      /* orig-video-isVGA */
	16                      /* orig-video-points */
};

unsigned int phys_bytes_of_ram, end_of_phys_memory;

/* Typing sync at the prom prompt calls the function pointed to by
 * romvec->pv_synchook which I set to the following function.
 * This should sync all filesystems and return, for now it just
 * prints out pretty messages and returns.
 */

extern unsigned long trapbase;
extern int serial_console;
extern void breakpoint(void);
#if CONFIG_SUN_CONSOLE
void (*prom_palette)(int);
#endif
asmlinkage void sys_sync(void);	/* it's really int */

/* Pretty sick eh? */
void prom_sync_me(void)
{
	unsigned long prom_tbr, flags;

#ifdef __SMP__
	global_irq_holder = NO_PROC_ID;
	*((unsigned char *)&global_irq_lock) = 0;
	*((unsigned char *)&global_bh_lock) = 0;
#endif
	__save_and_cli(flags);
	__asm__ __volatile__("rd %%tbr, %0\n\t" : "=r" (prom_tbr));
	__asm__ __volatile__("wr %0, 0x0, %%tbr\n\t"
			     "nop\n\t"
			     "nop\n\t"
			     "nop\n\t" : : "r" (&trapbase));

#ifdef CONFIG_SUN_CONSOLE
	if (prom_palette)
		prom_palette(1);
#endif
	prom_printf("PROM SYNC COMMAND...\n");
	show_free_areas();
	if(current->pid != 0) {
		__sti();
		sys_sync();
		__cli();
	}
	prom_printf("Returning to prom\n");

	__asm__ __volatile__("wr %0, 0x0, %%tbr\n\t"
			     "nop\n\t"
			     "nop\n\t"
			     "nop\n\t" : : "r" (prom_tbr));
	__restore_flags(flags);

	return;
}

extern void rs_kgdb_hook(int tty_num); /* sparc/serial.c */

unsigned int boot_flags;
#define BOOTME_DEBUG  0x1
#define BOOTME_SINGLE 0x2
#define BOOTME_KGDBA  0x4
#define BOOTME_KGDBB  0x8
#define BOOTME_KGDB   0xc

#ifdef CONFIG_SUN_CONSOLE
static int console_fb = 0;
#endif
static unsigned long memory_size __initdata = 0;

void kernel_enter_debugger(void)
{
	if (boot_flags & BOOTME_KGDB) {
		printk("KGDB: Entered\n");
		breakpoint();
	}
}

int obp_system_intr(void)
{
	if (boot_flags & BOOTME_KGDB) {
		printk("KGDB: system interrupted\n");
		breakpoint();
		return 1;
	}
	if (boot_flags & BOOTME_DEBUG) {
		printk("OBP: system interrupted\n");
		prom_halt();
		return 1;
	}
	return 0;
}

/* 
 * Process kernel command line switches that are specific to the
 * SPARC or that require special low-level processing.
 */
static void __init process_switch(char c)
{
	switch (c) {
	case 'd':
		boot_flags |= BOOTME_DEBUG;
		break;
	case 's':
		boot_flags |= BOOTME_SINGLE;
		break;
	case 'h':
		prom_printf("boot_flags_init: Halt!\n");
		halt();
		break;
	default:
		printk("Unknown boot switch (-%c)\n", c);
		break;
	}
}

static void __init boot_flags_init(char *commands)
{
	while (*commands) {
		/* Move to the start of the next "argument". */
		while (*commands && *commands == ' ')
			commands++;

		/* Process any command switches, otherwise skip it. */
		if (*commands == '\0')
			break;
		else if (*commands == '-') {
			commands++;
			while (*commands && *commands != ' ')
				process_switch(*commands++);
		} else if (strlen(commands) >= 9
			   && !strncmp(commands, "kgdb=tty", 8)) {
			switch (commands[8]) {
#ifdef CONFIG_SUN_SERIAL
			case 'a':
				boot_flags |= BOOTME_KGDBA;
				prom_printf("KGDB: Using serial line /dev/ttya.\n");
				break;
			case 'b':
				boot_flags |= BOOTME_KGDBB;
				prom_printf("KGDB: Using serial line /dev/ttyb.\n");
				break;
#endif
#ifdef CONFIG_AP1000
			case 'c':
				printk("KGDB: AP1000+ debugging\n");
				break;
#endif
			default:
				printk("KGDB: Unknown tty line.\n");
				break;
			}
			commands += 9;
		} else {
#if CONFIG_SUN_CONSOLE
			if (!strncmp(commands, "console=", 8)) {
				commands += 8;
				if (!strncmp (commands, "ttya", 4)) {
					console_fb = 2;
					prom_printf ("Using /dev/ttya as console.\n");
				} else if (!strncmp (commands, "ttyb", 4)) {
					console_fb = 3;
					prom_printf ("Using /dev/ttyb as console.\n");
#if defined(CONFIG_PROM_CONSOLE)
				} else if (!strncmp (commands, "prom", 4)) {
					char *p;
					
					for (p = commands - 8; *p && *p != ' '; p++)
						*p = ' ';
					conswitchp = &prom_con;
					console_fb = 1;
#endif
				} else {
					console_fb = 1;
				}
			} else
#endif
			if (!strncmp(commands, "mem=", 4)) {
				/*
				 * "mem=XXX[kKmM] overrides the PROM-reported
				 * memory size.
				 */
				memory_size = simple_strtoul(commands + 4,
							     &commands, 0);
				if (*commands == 'K' || *commands == 'k') {
					memory_size <<= 10;
					commands++;
				} else if (*commands=='M' || *commands=='m') {
					memory_size <<= 20;
					commands++;
				}
			}
			while (*commands && *commands != ' ')
				commands++;
		}
	}
}

/* This routine will in the future do all the nasty prom stuff
 * to probe for the mmu type and its parameters, etc. This will
 * also be where SMP things happen plus the Sparc specific memory
 * physical memory probe as on the alpha.
 */

extern int prom_probe_memory(void);
extern void sun4c_probe_vac(void);
extern char cputypval;
extern unsigned long start, end;
extern void panic_setup(char *, int *);
extern void srmmu_end_memory(unsigned long, unsigned long *);
extern unsigned long sun_serial_setup(unsigned long);

extern unsigned short root_flags;
extern unsigned short root_dev;
extern unsigned short ram_flags;
extern unsigned sparc_ramdisk_image;
extern unsigned sparc_ramdisk_size;
#define RAMDISK_IMAGE_START_MASK	0x07FF
#define RAMDISK_PROMPT_FLAG		0x8000
#define RAMDISK_LOAD_FLAG		0x4000

extern int root_mountflags;

char saved_command_line[256];
char reboot_command[256];
enum sparc_cpu sparc_cpu_model;

struct tt_entry *sparc_ttable;

struct pt_regs fake_swapper_regs = { 0, 0, 0, 0, { 0, } };

static void prom_cons_write(struct console *con, const char *str, unsigned count)
{
	while (count--)
		prom_printf("%c", *str++);
}

static struct console prom_console = {
	"PROM", prom_cons_write, 0, 0, 0, 0, 0, CON_PRINTBUFFER, 0, 0, 0
};

void __init setup_arch(char **cmdline_p,
	unsigned long * memory_start_p, unsigned long * memory_end_p)
{
	int total, i, packed;

	sparc_ttable = (struct tt_entry *) &start;

	/* Initialize PROM console and command line. */
	*cmdline_p = prom_getbootargs();
	strcpy(saved_command_line, *cmdline_p);

	/* Set sparc_cpu_model */
	sparc_cpu_model = sun_unknown;
	if(!strcmp(&cputypval,"sun4 ")) { sparc_cpu_model=sun4; }
	if(!strcmp(&cputypval,"sun4c")) { sparc_cpu_model=sun4c; }
	if(!strcmp(&cputypval,"sun4m")) { sparc_cpu_model=sun4m; }
	if(!strcmp(&cputypval,"sun4s")) { sparc_cpu_model=sun4m; }  /* CP-1200 with PROM 2.30 -E */
	if(!strcmp(&cputypval,"sun4d")) { sparc_cpu_model=sun4d; }
	if(!strcmp(&cputypval,"sun4e")) { sparc_cpu_model=sun4e; }
	if(!strcmp(&cputypval,"sun4u")) { sparc_cpu_model=sun4u; }

#ifdef CONFIG_SUN4
	if (sparc_cpu_model != sun4) {
		prom_printf("This kernel is for Sun4 architecture only.\n");
		prom_halt();
	}
#endif
#if CONFIG_AP1000
	sparc_cpu_model=ap1000;
	strcpy(&cputypval, "ap+");
#endif
	printk("ARCH: ");
	packed = 0;
	switch(sparc_cpu_model) {
	case sun4:
		printk("SUN4\n");
		packed = 0;
		break;
	case sun4c:
		printk("SUN4C\n");
		packed = 0;
		break;
	case sun4m:
		printk("SUN4M\n");
		packed = 1;
		break;
	case sun4d:
		printk("SUN4D\n");
		packed = 1;
		break;
	case sun4e:
		printk("SUN4E\n");
		packed = 0;
		break;
	case sun4u:
		printk("SUN4U\n");
		break;
	case ap1000:
		register_console(&prom_console);
		printk("AP1000\n");
		packed = 1;
		break;
	default:
		printk("UNKNOWN!\n");
		break;
	};

#ifdef CONFIG_DUMMY_CONSOLE
	conswitchp = &dummy_con;
#elif defined(CONFIG_PROM_CONSOLE)
	conswitchp = &prom_con;
#endif
	boot_flags_init(*cmdline_p);

	idprom_init();
	if (ARCH_SUN4C_SUN4)
		sun4c_probe_vac();
	load_mmu();
	total = prom_probe_memory();
	*memory_start_p = PAGE_ALIGN(((unsigned long) &end));

	if(!packed) {
		for(i=0; sp_banks[i].num_bytes != 0; i++) {
			end_of_phys_memory = sp_banks[i].base_addr +
					     sp_banks[i].num_bytes;
			if (memory_size) {
				if (end_of_phys_memory > memory_size) {
					sp_banks[i].num_bytes -=
					    (end_of_phys_memory - memory_size);
					end_of_phys_memory = memory_size;
					sp_banks[++i].base_addr = 0xdeadbeef;
					sp_banks[i].num_bytes = 0;
				}
			}
		}
		*memory_end_p = (end_of_phys_memory + KERNBASE);
	} else
		srmmu_end_memory(memory_size, memory_end_p);

	if (!root_flags)
		root_mountflags &= ~MS_RDONLY;
	ROOT_DEV = to_kdev_t(root_dev);
#ifdef CONFIG_BLK_DEV_RAM
	rd_image_start = ram_flags & RAMDISK_IMAGE_START_MASK;
	rd_prompt = ((ram_flags & RAMDISK_PROMPT_FLAG) != 0);
	rd_doload = ((ram_flags & RAMDISK_LOAD_FLAG) != 0);	
#endif
#ifdef CONFIG_BLK_DEV_INITRD
	if (sparc_ramdisk_image) {
		initrd_start = sparc_ramdisk_image;
		if (initrd_start < KERNBASE) initrd_start += KERNBASE;
		initrd_end = initrd_start + sparc_ramdisk_size;
		if (initrd_end > *memory_end_p) {
			printk(KERN_CRIT "initrd extends beyond end of memory "
		                 	 "(0x%08lx > 0x%08lx)\ndisabling initrd\n",
		       			 initrd_end,*memory_end_p);
			initrd_start = 0;
		}
		if (initrd_start >= *memory_start_p && initrd_start < *memory_start_p + 2 * PAGE_SIZE) {
			initrd_below_start_ok = 1;
			*memory_start_p = PAGE_ALIGN (initrd_end);
		} else if (initrd_start && sparc_ramdisk_image < KERNBASE) {
			switch (sparc_cpu_model) {
			case sun4m:
			case sun4d:
				initrd_start -= KERNBASE;
				initrd_end -= KERNBASE;
				break;
			default:
				break;
			}
		}
	}
#endif	
	prom_setsync(prom_sync_me);

#ifdef CONFIG_SUN_SERIAL
	*memory_start_p = sun_serial_setup(*memory_start_p); /* set this up ASAP */
#endif
	{
#if !CONFIG_SUN_SERIAL
		serial_console = 0;
#else
		switch (console_fb) {
		case 0: /* Let get our io devices from prom */
			{
				int idev = prom_query_input_device();
				int odev = prom_query_output_device();
				if (idev == PROMDEV_IKBD && odev == PROMDEV_OSCREEN) {
					serial_console = 0;
				} else if (idev == PROMDEV_ITTYA && odev == PROMDEV_OTTYA) {
					serial_console = 1;
				} else if (idev == PROMDEV_ITTYB && odev == PROMDEV_OTTYB) {
					serial_console = 2;
				} else if (idev == PROMDEV_I_UNK && odev == PROMDEV_OTTYA) {
					prom_printf("MrCoffee ttya\n");
					serial_console = 1;
				} else if (idev == PROMDEV_I_UNK && odev == PROMDEV_OSCREEN) {
					serial_console = 0;
					prom_printf("MrCoffee keyboard\n");
				} else {
					prom_printf("Inconsistent or unknown console\n");
					prom_printf("You cannot mix serial and non serial input/output devices\n");
					prom_halt();
				}
			}
			break;
		case 1: serial_console = 0; break; /* Force one of the framebuffers as console */
		case 2: serial_console = 1; break; /* Force ttya as console */
		case 3: serial_console = 2; break; /* Force ttyb as console */
		}
#endif
	}

	if ((boot_flags & BOOTME_KGDBA)) {
		rs_kgdb_hook(0);
	}
	if ((boot_flags & BOOTME_KGDBB)) {
		rs_kgdb_hook(1);
	}

	if((boot_flags&BOOTME_DEBUG) && (linux_dbvec!=0) && 
	   ((*(short *)linux_dbvec) != -1)) {
		printk("Booted under KADB. Syncing trap table.\n");
		(*(linux_dbvec->teach_debugger))();
	}
	if((boot_flags & BOOTME_KGDB)) {
		set_debug_traps();
		prom_printf ("Breakpoint!\n");
		breakpoint();
	}


	/* Due to stack alignment restrictions and assumptions... */
	init_mm.mmap->vm_page_prot = PAGE_SHARED;
	init_mm.mmap->vm_start = KERNBASE;
	init_mm.mmap->vm_end = *memory_end_p;
	init_mm.context = (unsigned long) NO_CONTEXT;
	init_task.thread.kregs = &fake_swapper_regs;

	if (serial_console)
		conswitchp = NULL;
}

asmlinkage int sys_ioperm(unsigned long from, unsigned long num, int on)
{
	return -EIO;
}

/* BUFFER is PAGE_SIZE bytes long. */

extern char *sparc_cpu_type[];
extern char *sparc_fpu_type[];

int get_cpuinfo(char *buffer)
{
	int cpuid=hard_smp_processor_id();
	int len;

	len = sprintf(buffer, "cpu\t\t: %s\n"
            "fpu\t\t: %s\n"
            "promlib\t\t: Version %d Revision %d\n"
            "prom\t\t: %d.%d\n"
            "type\t\t: %s\n"
	    "ncpus probed\t: %d\n"
	    "ncpus active\t: %d\n"
#ifndef __SMP__
            "BogoMips\t: %lu.%02lu\n"
#endif
	    ,
	    sparc_cpu_type[cpuid] ? : "undetermined",
	    sparc_fpu_type[cpuid] ? : "undetermined",
            romvec->pv_romvers, prom_rev, romvec->pv_printrev >> 16, (short)romvec->pv_printrev,
            &cputypval,
	    linux_num_cpus, smp_num_cpus
#ifndef __SMP__
	    , loops_per_sec/500000, (loops_per_sec/5000) % 100
#endif
	    );
#ifdef __SMP__
	len += smp_bogo_info(buffer + len);
#endif
	len += mmu_info(buffer + len);
#ifdef __SMP__
	len += smp_info(buffer + len);
#endif
	return len;
}
