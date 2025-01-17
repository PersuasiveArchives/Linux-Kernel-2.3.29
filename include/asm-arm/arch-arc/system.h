/*
 * linux/include/asm-arm/arch-arc/system.h
 *
 * Copyright (c) 1996-1999 Russell King and Dave Gilbert
 */
#include <linux/config.h>

#ifdef CONFIG_ARCH_ARC

#define cliIF()				\
	do {				\
	  unsigned long temp;		\
	  __asm__ __volatile__(		\
"	mov	%0, pc\n"		\
"	orr %0, %0, #0x0c000000\n"	\
"	teqp	%0, #0\n"		\
	  : "=r" (temp)	\
    : );	\
  } while(0)

#endif

#define arch_do_idle() do { } while (0)

extern __inline__ void arch_reset(char mode)
{
	extern void ecard_reset(int card);

	/*
	 * Reset all expansion cards.
	 */
	ecard_reset(-1);

	/*
	 * copy branch instruction to reset location and call it
	 */
	*(unsigned long *)0 = *(unsigned long *)0x03800000;
	((void(*)(void))0)();
}
