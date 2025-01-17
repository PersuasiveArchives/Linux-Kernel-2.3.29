/*
 * linux/include/asm-arm/arch-cl7500/system.h
 *
 * Copyright (c) 1999 Nexus Electronics Ltd.
 */
#ifndef __ASM_ARCH_SYSTEM_H
#define __ASM_ARCH_SYSTEM_H

#include <asm/iomd.h>

#define arch_reset(mode) {						\
	outb (0, IOMD_ROMCR0);						\
	cli();								\
	__asm__ __volatile__("msr  spsr, r1;"				\
			     "mcr  p15, 0, %0, c1, c0, 0;"		\
			     "movs pc, #0"				\
			 : 						\
			 : "r" (cpu_reset()));				\
	}

/*
 * We can wait for an interrupt...
 */
#define proc_idle()						\
	outb(0, IOMD_SUSMODE)

#endif
