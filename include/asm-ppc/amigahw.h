#ifndef __ASMPPC_AMIGAHW_H
#define __ASMPPC_AMIGAHW_H

#include <linux/config.h>
#include <asm-m68k/amigahw.h>

#undef CHIP_PHYSADDR
#ifdef CONFIG_APUS_FAST_EXCEPT
#define CHIP_PHYSADDR      (0x000000)
#else
#define CHIP_PHYSADDR      (0x004000)
#endif


#endif /* __ASMPPC_AMIGAHW_H */
