/*
 *  acpi.h - ACPI driver interface
 *
 *  Copyright (C) 1999 Andrew Henroid
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LINUX_ACPI_H
#define _LINUX_ACPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/wait.h>

/*
 * Device types
 */
enum
{
	ACPI_SYS_DEV,	/* system device (fan, KB controller, ...) */
	ACPI_PCI_DEV,	/* generic PCI device */
	ACPI_PCI_BUS,	/* PCI bus */
	ACPI_ISA_DEV,	/* generic ISA device */
	ACPI_ISA_BUS,	/* ISA bus */
	ACPI_USB_DEV,	/* generic USB device */
	ACPI_USB_HUB,	/* USB hub device */
	ACPI_USB_CTRL,	/* USB controller */
	ACPI_SCSI_DEV,	/* generic SCSI device */
	ACPI_SCSI_CTRL, /* SCSI controller */
};

typedef int acpi_dev_t;

/*
 * Device addresses
 */
#define ACPI_PCI_ADR(dev) ((dev)->bus->number << 16 | (dev)->devfn)

/*
 * HID (PnP) values
 */
enum
{
	ACPI_UNKNOWN_HID =  0x00000000, /* generic */
	ACPI_KBC_HID =	    0x41d00303, /* keyboard controller */
	ACPI_COM_HID =	    0x41d00500, /* serial port */
	ACPI_FDC_HID =	    0x41d00700, /* floppy controller */
	ACPI_VGA_HID =	    0x41d00900, /* VGA controller */
	ACPI_ISA_HID =	    0x41d00a00, /* ISA bus */
	ACPI_EISA_HID =	    0x41d00a01, /* EISA bus */
	ACPI_PCI_HID =	    0x41d00a03, /* PCI bus */
};

typedef int acpi_hid_t;

/*
 * Device states
 */
enum
{
	ACPI_D0, /* fully-on */
	ACPI_D1, /* partial-on */
	ACPI_D2, /* partial-on */
	ACPI_D3, /* fully-off */
};

typedef int acpi_dstate_t;

/*
 * System sleep states
 */
enum
{
	ACPI_S0, /* working */
	ACPI_S1, /* sleep */
	ACPI_S2, /* sleep */
	ACPI_S3, /* sleep */
	ACPI_S4, /* non-volatile sleep */
	ACPI_S5, /* soft-off */
};

typedef int acpi_sstate_t;

struct acpi_dev;

/*
 * Device state transition function
 */
typedef int (*acpi_transition)(struct acpi_dev *dev, acpi_dstate_t state);

/*
 * Static device information
 */
struct acpi_dev_info
{
	acpi_dev_t	 type;	     /* device type */
	acpi_hid_t	 hid;	     /* PnP identifier */
	acpi_transition	 transition; /* state transition callback */

	/* other information like D-states supported,
	 * D-state latencies, and in-rush current needs
	 * will go here
	 */
};

/*
 * Dynamic device information
 */
struct acpi_dev
{
	struct acpi_dev_info info;     /* static device info */
	unsigned long	     adr;      /* bus address or unique id */
	acpi_dstate_t	     state;    /* current D-state */
	unsigned long	     accessed; /* last access time */
	unsigned long	     idle;     /* last idle time */
	struct list_head     entry;    /* linked list entry */
};

#ifdef CONFIG_ACPI

extern wait_queue_head_t acpi_idle_wait;

/*
 * Register a device with the ACPI subsystem
 */
struct acpi_dev *acpi_register(struct acpi_dev_info *info, unsigned long adr);

/*
 * Unregister a device with ACPI
 */
void acpi_unregister(struct acpi_dev *dev);

/*
 * Update device access time and wake up device, if necessary
 */
extern inline void acpi_access(struct acpi_dev *dev)
{
	extern void acpi_wakeup(struct acpi_dev*);
	if (dev) {
		if (dev->state != ACPI_D0)
			acpi_wakeup(dev);
		dev->accessed = jiffies;
	}
}

/*
 * Identify device as currently being idle
 */
extern inline void acpi_dev_idle(struct acpi_dev *dev)
{
	if (dev) {
		dev->idle = jiffies;
		if (waitqueue_active(&acpi_idle_wait))
			wake_up(&acpi_idle_wait);
	}
}

#else /* CONFIG_ACPI */

extern inline struct acpi_dev*
acpi_register(struct acpi_dev_info *info, unsigned long adr)
{
	return 0;
}

extern inline void acpi_unregister(struct acpi_dev *dev) {}
extern inline void acpi_access(struct acpi_dev *dev) {}
extern inline void acpi_dev_idle(struct acpi_dev *dev) {}

#endif /* CONFIG_ACPI */

extern void (*acpi_idle)(void);
extern void (*acpi_power_off)(void);

#endif /* __KERNEL__ */

/* RSDP location */
#define ACPI_BIOS_ROM_BASE (0x0e0000)
#define ACPI_BIOS_ROM_END  (0x100000)

/* Table signatures */
#define ACPI_RSDP1_SIG 0x20445352 /* 'RSD ' */
#define ACPI_RSDP2_SIG 0x20525450 /* 'PTR ' */
#define ACPI_RSDT_SIG  0x54445352 /* 'RSDT' */
#define ACPI_FACP_SIG  0x50434146 /* 'FACP' */
#define ACPI_DSDT_SIG  0x54445344 /* 'DSDT' */
#define ACPI_FACS_SIG  0x53434146 /* 'FACS' */

/* PM1_STS/EN flags */
#define ACPI_TMR    0x0001
#define ACPI_BM	    0x0010
#define ACPI_GBL    0x0020
#define ACPI_PWRBTN 0x0100
#define ACPI_SLPBTN 0x0200
#define ACPI_RTC    0x0400
#define ACPI_WAK    0x8000

/* PM1_CNT flags */
#define ACPI_SCI_EN   0x0001
#define ACPI_BM_RLD   0x0002
#define ACPI_GBL_RLS  0x0004
#define ACPI_SLP_TYP0 0x0400
#define ACPI_SLP_TYP1 0x0800
#define ACPI_SLP_TYP2 0x1000
#define ACPI_SLP_EN   0x2000

#define ACPI_SLP_TYP_MASK  0x1c00
#define ACPI_SLP_TYP_SHIFT 10

/* PM_TMR masks */
#define ACPI_TMR_MASK	0x00ffffff
#define ACPI_TMR_HZ	3580000 /* 3.58 MHz */

/* strangess to avoid integer overflow */
#define ACPI_uS_TO_TMR_TICKS(val) \
  (((val) * (ACPI_TMR_HZ / 10000)) / 100)
#define ACPI_TMR_TICKS_TO_uS(ticks) \
  (((ticks) * 100) / (ACPI_TMR_HZ / 10000))

/* CPU cycles -> PM timer cycles, looks somewhat heuristic but
   (ticks = 3/11 * CPU_MHz + 2) comes pretty close for my systems
 */
#define ACPI_CPU_TO_TMR_TICKS(cycles) \
  ((cycles) / (3 * (loops_per_sec + 2500) / 500000 / 11 + 2))

/* PM2_CNT flags */
#define ACPI_ARB_DIS 0x01

/* FACP flags */
#define ACPI_WBINVD	  0x00000001
#define ACPI_WBINVD_FLUSH 0x00000002
#define ACPI_PROC_C1	  0x00000004
#define ACPI_P_LVL2_UP	  0x00000008
#define ACPI_PWR_BUTTON	  0x00000010
#define ACPI_SLP_BUTTON	  0x00000020
#define ACPI_FIX_RTC	  0x00000040
#define ACPI_RTC_64	  0x00000080
#define ACPI_TMR_VAL_EXT  0x00000100
#define ACPI_DCK_CAP	  0x00000200

/* FACS flags */
#define ACPI_S4BIOS	  0x00000001

/* processor block offsets */
#define ACPI_P_CNT	  0x00000000
#define ACPI_P_LVL2	  0x00000004
#define ACPI_P_LVL3	  0x00000005

/* C-state latencies (microseconds) */
#define ACPI_MAX_P_LVL2_LAT 100
#define ACPI_MAX_P_LVL3_LAT 1000
#define ACPI_INFINITE_LAT   (~0UL)

struct acpi_rsdp {
	__u32 signature[2];
	__u8 checksum;
	__u8 oem[6];
	__u8 reserved;
	__u32 rsdt;
} __attribute__ ((packed));

struct acpi_table {
	__u32 signature;
	__u32 length;
	__u8 rev;
	__u8 checksum;
	__u8 oem[6];
	__u8 oem_table[8];
	__u32 oem_rev;
	__u32 creator;
	__u32 creator_rev;
} __attribute__ ((packed));

struct acpi_facp {
	struct acpi_table hdr;
	__u32 facs;
	__u32 dsdt;
	__u8 int_model;
	__u8 reserved;
	__u16 sci_int;
	__u32 smi_cmd;
	__u8 acpi_enable;
	__u8 acpi_disable;
	__u8 s4bios_req;
	__u8 reserved2;
	__u32 pm1a_evt;
	__u32 pm1b_evt;
	__u32 pm1a_cnt;
	__u32 pm1b_cnt;
	__u32 pm2_cnt;
	__u32 pm_tmr;
	__u32 gpe0;
	__u32 gpe1;
	__u8 pm1_evt_len;
	__u8 pm1_cnt_len;
	__u8 pm2_cnt_len;
	__u8 pm_tm_len;
	__u8 gpe0_len;
	__u8 gpe1_len;
	__u8 gpe1_base;
	__u8 reserved3;
	__u16 p_lvl2_lat;
	__u16 p_lvl3_lat;
	__u16 flush_size;
	__u16 flush_stride;
	__u8 duty_offset;
	__u8 duty_width;
	__u8 day_alarm;
	__u8 mon_alarm;
	__u8 century;
	__u8 reserved4;
	__u8 reserved5;
	__u8 reserved6;
	__u32 flags;
} __attribute__ ((packed));

struct acpi_facs {
	__u32 signature;
	__u32 length;
	__u32 hw_signature;
	__u32 fw_wake_vector;
	__u32 global_lock;
	__u32 flags;
} __attribute__ ((packed));

/*
 * Sysctl declarations
 */

enum
{
	CTL_ACPI = 10
};

enum
{
	ACPI_FACP = 1,
	ACPI_DSDT,
	ACPI_PM1_ENABLE,
	ACPI_GPE_ENABLE,
	ACPI_GPE_LEVEL,
	ACPI_EVENT,
	ACPI_P_BLK,
	ACPI_P_LVL2_LAT,
	ACPI_P_LVL3_LAT,
	ACPI_S5_SLP_TYP,
};

#define ACPI_SLP_TYP_DISABLED	(~0UL)

/*
 * PIIX4-specific ACPI info (for systems with PIIX4 but no ACPI tables)
 */

#define ACPI_PIIX4_INT_MODEL	0x00
#define ACPI_PIIX4_SCI_INT	0x0009

#define ACPI_PIIX4_SMI_CMD	0x00b2
#define ACPI_PIIX4_ACPI_ENABLE	0xf0
#define ACPI_PIIX4_ACPI_DISABLE 0xf1
#define ACPI_PIIX4_S4BIOS_REQ	0xf2

#define ACPI_PIIX4_PM1_EVT	0x0000
#define ACPI_PIIX4_PM1_CNT	0x0004
#define	  ACPI_PIIX4_S0_MASK	(0x0005 << 10)
#define	  ACPI_PIIX4_S1_MASK	(0x0004 << 10)
#define	  ACPI_PIIX4_S2_MASK	(0x0003 << 10)
#define	  ACPI_PIIX4_S3_MASK	(0x0002 << 10)
#define	  ACPI_PIIX4_S4_MASK	(0x0001 << 10)
#define	  ACPI_PIIX4_S5_MASK	(0x0000 << 10)
#define ACPI_PIIX4_PM_TMR	0x0008
#define ACPI_PIIX4_GPE0		0x000c
#define ACPI_PIIX4_P_BLK	0x0010

#define ACPI_PIIX4_PM1_EVT_LEN	0x04
#define ACPI_PIIX4_PM1_CNT_LEN	0x02
#define ACPI_PIIX4_PM_TM_LEN	0x04
#define ACPI_PIIX4_GPE0_LEN	0x04

#define ACPI_PIIX4_PM2_CNT	0x0022
#define ACPI_PIIX4_PM2_CNT_LEN	0x01

#define ACPI_PIIX4_PMREGMISC	0x80
#define	  ACPI_PIIX4_PMIOSE	0x01

#endif /* _LINUX_ACPI_H */
