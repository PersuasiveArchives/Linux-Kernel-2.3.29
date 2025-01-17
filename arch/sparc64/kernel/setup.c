/*  $Id: setup.c,v 1.47 1999/08/31 06:54:55 davem Exp $
 *  linux/arch/sparc64/kernel/setup.c
 *
 *  Copyright (C) 1995,1996  David S. Miller (davem@caip.rutgers.edu)
 *  Copyright (C) 1997       Jakub Jelinek (jj@sunsite.mff.cuni.cz)
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
#include <linux/inet.h>
#include <linux/console.h>

#include <asm/segment.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/oplib.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/idprom.h>
#include <asm/head.h>

#ifdef CONFIG_IP_PNP
#include <net/ipconfig.h>
#endif

#undef PROM_DEBUG_CONSOLE

struct screen_info screen_info = {
	0, 0,			/* orig-x, orig-y */
	0,			/* unused */
	0,			/* orig-video-page */
	0,			/* orig-video-mode */
	128,			/* orig-video-cols */
	0, 0, 0,		/* unused, ega_bx, unused */
	54,			/* orig-video-lines */
	0,                      /* orig-video-isVGA */
	16                      /* orig-video-points */
};

/* Typing sync at the prom prompt calls the function pointed to by
 * the sync callback which I set to the following function.
 * This should sync all filesystems and return, for now it just
 * prints out pretty messages and returns.
 */

#if CONFIG_SUN_CONSOLE
void (*prom_palette)(int);
#endif
asmlinkage void sys_sync(void);	/* it's really int */

static void
prom_console_write(struct console *con, const char *s, unsigned n)
{
	prom_printf("%s", s);
}

static struct console prom_console = {
	"prom",
	prom_console_write,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	CON_CONSDEV | CON_ENABLED,
	-1,
	0,
	NULL
};

#define PROM_TRUE	-1
#define PROM_FALSE	0

/* Pretty sick eh? */
int prom_callback(long *args)
{
	struct console *cons, *saved_console = NULL;
	unsigned long flags;
	char *cmd;

	if (!args)
		return -1;
	if (!(cmd = (char *)args[0]))
		return -1;

	save_and_cli(flags);
	cons = console_drivers;
	while (cons) {
		unregister_console(cons);
		cons->flags &= ~(CON_PRINTBUFFER);
		cons->next = saved_console;
		saved_console = cons;
		cons = console_drivers;
	}
	register_console(&prom_console);
	if (!strcmp(cmd, "sync")) {
		prom_printf("PROM `%s' command...\n", cmd);
		show_free_areas();
		if(current->pid != 0) {
			sti();
			sys_sync();
			cli();
		}
		args[2] = 0;
		args[args[1] + 3] = -1;
		prom_printf("Returning to PROM\n");
	} else if (!strcmp(cmd, "va>tte-data")) {
		unsigned long ctx, va;
		unsigned long tte = 0;
		long res = PROM_FALSE;

		ctx = args[3];
		va = args[4];
		if (ctx) {
			/*
			 * Find process owning ctx, lookup mapping.
			 */
			struct task_struct *p;
			struct mm_struct *mm = NULL;
			pgd_t *pgdp;
			pmd_t *pmdp;
			pte_t *ptep;

			for_each_task(p) {
				mm = p->mm;
				if (CTX_HWBITS(mm->context) == ctx)
					break;
			}
			if (!mm ||
			    CTX_HWBITS(mm->context) != ctx)
				goto done;

			pgdp = pgd_offset(mm, va);
			if (pgd_none(*pgdp))
				goto done;
			pmdp = pmd_offset(pgdp, va);
			if (pmd_none(*pmdp))
				goto done;
			ptep = pte_offset(pmdp, va);
			if (!pte_present(*ptep))
				goto done;
			tte = pte_val(*ptep);
			res = PROM_TRUE;
			goto done;
		}

		if ((va >= KERNBASE) && (va < (KERNBASE + (4 * 1024 * 1024)))) {
			/*
			 * Locked down tlb entry 63.
			 */
			tte = spitfire_get_dtlb_data(63);
			res = PROM_TRUE;
			goto done;
		}

		if (va < PGDIR_SIZE) {
			/*
			 * vmalloc or prom_inherited mapping.
			 */
			pgd_t *pgdp;
			pmd_t *pmdp;
			pte_t *ptep;

			pgdp = pgd_offset_k(va);
			if (pgd_none(*pgdp))
				goto done;
			pmdp = pmd_offset(pgdp, va);
			if (pmd_none(*pmdp))
				goto done;
			ptep = pte_offset(pmdp, va);
			if (!pte_present(*ptep))
				goto done;
			tte = pte_val(*ptep);
			res = PROM_TRUE;
			goto done;
		}

		if (va < PAGE_OFFSET) {
			/*
			 * No mappings here.
			 */
			goto done;
		}

		if (va & (1UL << 40)) {
			/*
			 * I/O page.
			 */

			tte = (__pa(va) & _PAGE_PADDR) |
			      _PAGE_VALID | _PAGE_SZ4MB |
			      _PAGE_E | _PAGE_P | _PAGE_W;
			res = PROM_TRUE;
			goto done;
		}

		/*
		 * Normal page.
		 */
		tte = (__pa(va) & _PAGE_PADDR) |
		      _PAGE_VALID | _PAGE_SZ4MB |
		      _PAGE_CP | _PAGE_CV | _PAGE_P | _PAGE_W;
		res = PROM_TRUE;

	done:
		if (res == PROM_TRUE) {
			args[2] = 3;
			args[args[1] + 3] = 0;
			args[args[1] + 4] = res;
			args[args[1] + 5] = tte;
		} else {
			args[2] = 2;
			args[args[1] + 3] = 0;
			args[args[1] + 4] = res;
		}
	} else if (!strcmp(cmd, ".soft1")) {
		unsigned long tte;

		tte = args[3];
		prom_printf("%lx:\"%s%s%s%s%s\" ",
			    (tte & _PAGE_SOFT) >> 7,
			    tte & _PAGE_MODIFIED ? "M" : "-",
			    tte & _PAGE_ACCESSED ? "A" : "-",
			    tte & _PAGE_READ     ? "W" : "-",
			    tte & _PAGE_WRITE    ? "R" : "-",
			    tte & _PAGE_PRESENT  ? "P" : "-");

		args[2] = 2;
		args[args[1] + 3] = 0;
		args[args[1] + 4] = PROM_TRUE;
	} else if (!strcmp(cmd, ".soft2")) {
		unsigned long tte;

		tte = args[3];
		prom_printf("%lx ", (tte & _PAGE_SOFT2) >> 50);

		args[2] = 2;
		args[args[1] + 3] = 0;
		args[args[1] + 4] = PROM_TRUE;
	} else {
		prom_printf("unknown PROM `%s' command...\n", cmd);
	}
	unregister_console(&prom_console);
	while (saved_console) {
		cons = saved_console;
		saved_console = cons->next;
		register_console(cons);
	}
	restore_flags(flags);
	return 0;
}

extern void rs_kgdb_hook(int tty_num); /* sparc/serial.c */

unsigned int boot_flags = 0;
#define BOOTME_DEBUG  0x1
#define BOOTME_SINGLE 0x2
#define BOOTME_KGDB   0x4

#ifdef CONFIG_SUN_CONSOLE
static int console_fb __initdata = 0;
#endif
static unsigned long memory_size = 0;

#ifdef PROM_DEBUG_CONSOLE
static struct console prom_debug_console = {
	"debug",
	prom_console_write,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	CON_PRINTBUFFER,
	-1,
	0,
	NULL
};
#endif

/* XXX Implement this at some point... */
void kernel_enter_debugger(void)
{
}

int obp_system_intr(void)
{
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
		prom_halt();
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
			boot_flags |= BOOTME_KGDB;
			switch (commands[8]) {
#ifdef CONFIG_SUN_SERIAL
			case 'a':
				rs_kgdb_hook(0);
				prom_printf("KGDB: Using serial line /dev/ttya.\n");
				break;
			case 'b':
				rs_kgdb_hook(1);
				prom_printf("KGDB: Using serial line /dev/ttyb.\n");
				break;
#endif
			default:
				printk("KGDB: Unknown tty line.\n");
				boot_flags &= ~BOOTME_KGDB;
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
				 * "mem=XXX[kKmM]" overrides the PROM-reported
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

extern int prom_probe_memory(void);
extern unsigned long start, end;
extern void panic_setup(char *, int *);

extern unsigned short root_flags;
extern unsigned short root_dev;
extern unsigned short ram_flags;
extern unsigned int sparc_ramdisk_image;
extern unsigned int sparc_ramdisk_size;
#define RAMDISK_IMAGE_START_MASK	0x07FF
#define RAMDISK_PROMPT_FLAG		0x8000
#define RAMDISK_LOAD_FLAG		0x4000

extern int root_mountflags;

char saved_command_line[256];
char reboot_command[256];

extern unsigned long phys_base;

static struct pt_regs fake_swapper_regs = { { 0, }, 0, 0, 0, 0 };

extern struct consw sun_serial_con;

void __init setup_arch(char **cmdline_p,
	unsigned long * memory_start_p, unsigned long * memory_end_p)
{
	extern int serial_console;  /* in console.c, of course */
	unsigned long lowest_paddr, end_of_phys_memory = 0;
	int total, i;

	/* Initialize PROM console and command line. */
	*cmdline_p = prom_getbootargs();
	strcpy(saved_command_line, *cmdline_p);

#ifdef PROM_DEBUG_CONSOLE
	register_console(&prom_debug_console);
#endif

	printk("ARCH: SUN4U\n");

#ifdef CONFIG_DUMMY_CONSOLE
	conswitchp = &dummy_con;
#elif defined(CONFIG_PROM_CONSOLE)
	conswitchp = &prom_con;
#endif

	boot_flags_init(*cmdline_p);

	idprom_init();
	total = prom_probe_memory();

	lowest_paddr = 0xffffffffffffffffUL;
	for(i=0; sp_banks[i].num_bytes != 0; i++) {
		if(sp_banks[i].base_addr < lowest_paddr)
			lowest_paddr = sp_banks[i].base_addr;
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
	prom_setcallback(prom_callback);
	prom_feval(": linux-va>tte-data 2 \" va>tte-data\" $callback drop ; "
		   "' linux-va>tte-data to va>tte-data");
	prom_feval(": linux-.soft1 1 \" .soft1\" $callback 2drop ; "
		   "' linux-.soft1 to .soft1");
	prom_feval(": linux-.soft2 1 \" .soft2\" $callback 2drop ; "
		   "' linux-.soft2 to .soft2");

	/* In paging_init() we tip off this value to see if we need
	 * to change init_mm.pgd to point to the real alias mapping.
	 */
	phys_base = lowest_paddr;

	*memory_start_p = PAGE_ALIGN(((unsigned long) &end));
	*memory_end_p = (end_of_phys_memory + PAGE_OFFSET);

#ifdef DAVEM_DEBUGGING
	prom_printf("phys_base[%016lx] memory_start[%016lx] memory_end[%016lx]\n",
		    phys_base, *memory_start_p, *memory_end_p);
#endif

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
		unsigned long start = 0;
		
		if (sparc_ramdisk_image >= (unsigned long)&end - 2 * PAGE_SIZE)
			sparc_ramdisk_image -= KERNBASE;
		initrd_start = sparc_ramdisk_image + phys_base + PAGE_OFFSET;
		initrd_end = initrd_start + sparc_ramdisk_size;
		if (initrd_end > *memory_end_p) {
			printk(KERN_CRIT "initrd extends beyond end of memory "
		                 	 "(0x%016lx > 0x%016lx)\ndisabling initrd\n",
		       			 initrd_end,*memory_end_p);
			initrd_start = 0;
		}
		if (initrd_start)
			start = sparc_ramdisk_image + KERNBASE;
		if (start >= *memory_start_p && start < *memory_start_p + 2 * PAGE_SIZE) {
			initrd_below_start_ok = 1;
			*memory_start_p = PAGE_ALIGN (start + sparc_ramdisk_size);
		}
	}
#endif	

	/* Due to stack alignment restrictions and assumptions... */
	init_mm.mmap->vm_page_prot = PAGE_SHARED;
	init_mm.mmap->vm_start = PAGE_OFFSET;
	init_mm.mmap->vm_end = *memory_end_p;
	init_task.thread.kregs = &fake_swapper_regs;

#ifdef CONFIG_IP_PNP
	if (!ic_set_manually) {
		int chosen = prom_finddevice ("/chosen");
		u32 cl, sv, gw;
		
		cl = prom_getintdefault (chosen, "client-ip", 0);
		sv = prom_getintdefault (chosen, "server-ip", 0);
		gw = prom_getintdefault (chosen, "gateway-ip", 0);
		if (cl && sv) {
			ic_myaddr = cl;
			ic_servaddr = sv;
			if (gw)
				ic_gateway = gw;
#if defined(CONFIG_IP_PNP_BOOTP) || defined(CONFIG_IP_PNP_RARP)
			ic_proto_enabled = 0;
#endif
		}
	}
#endif

#ifdef CONFIG_SUN_SERIAL
	switch (console_fb) {
	case 0: /* Let's get our io devices from prom */
		{
			int idev = prom_query_input_device();
			int odev = prom_query_output_device();
			if (idev == PROMDEV_IKBD && odev == PROMDEV_OSCREEN) {
				serial_console = 0;
			} else if (idev == PROMDEV_ITTYA && odev == PROMDEV_OTTYA) {
				serial_console = 1;
			} else if (idev == PROMDEV_ITTYB && odev == PROMDEV_OTTYB) {
				serial_console = 2;
			} else {
				prom_printf("Inconsistent console: "
					    "input %d, output %d\n",
					    idev, odev);
				prom_halt();
			}
		}
		break;
	case 1: /* Force one of the framebuffers as console */
		serial_console = 0;
		break;
	case 2: /* Force ttya as console */
		serial_console = 1;
		break;
	case 3: /* Force ttyb as console */
		serial_console = 2;
		break;
	}
#else
	serial_console = 0;
#endif
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

extern int smp_info(char *);
extern int smp_bogo(char *);
extern int mmu_info(char *);

int get_cpuinfo(char *buffer)
{
	int cpuid=smp_processor_id();
	int len;

	len = sprintf(buffer, 
	    "cpu\t\t: %s\n"
            "fpu\t\t: %s\n"
            "promlib\t\t: Version 3 Revision %d\n"
            "prom\t\t: %d.%d.%d\n"
            "type\t\t: sun4u\n"
	    "ncpus probed\t: %d\n"
	    "ncpus active\t: %d\n"
#ifndef __SMP__
            "BogoMips\t: %lu.%02lu\n"
#endif
	    ,
            sparc_cpu_type[cpuid],
            sparc_fpu_type[cpuid],
            prom_rev, prom_prev >> 16, (prom_prev >> 8) & 0xff, prom_prev & 0xff,
	    linux_num_cpus, smp_num_cpus
#ifndef __SMP__
            , loops_per_sec/500000, (loops_per_sec/5000) % 100
#endif
	    );
#ifdef __SMP__
	len += smp_bogo(buffer + len);
#endif
	len += mmu_info(buffer + len);
#ifdef __SMP__
	len += smp_info(buffer + len);
#endif
	return len;
}
