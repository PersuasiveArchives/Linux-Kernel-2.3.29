/*
 * linux/include/asm-arm/proc-fns.h
 *
 * Copyright (C) 1997-1999 Russell King
 */
#ifndef __ASM_PROCFNS_H
#define __ASM_PROCFNS_H

#ifdef __KERNEL__

#include <linux/config.h>

/*
 * Work out if we need multiple CPU support
 */
#undef MULTI_CPU
#undef CPU_NAME

#ifdef CONFIG_CPU_26
# define CPU_INCLUDE_NAME "asm/cpu-multi26.h"
# define MULTI_CPU
#endif

#ifdef CONFIG_CPU_32
# define CPU_INCLUDE_NAME "asm/cpu-multi32.h"
# ifdef CONFIG_CPU_ARM6
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME arm6
#  endif
# endif
# ifdef CONFIG_CPU_ARM7
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME arm7
#  endif
# endif
# ifdef CONFIG_CPU_SA110
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME sa110
#  endif
# endif
#endif

#ifndef MULTI_CPU
#undef CPU_INCLUDE_NAME
#define CPU_INCLUDE_NAME "asm/cpu-single.h"
#endif

#include CPU_INCLUDE_NAME

#endif /* __KERNEL__ */

#if 0
 * The following is to fool mkdep into generating the correct
 * dependencies.  Without this, it cant figure out that this
 * file does indeed depend on the cpu-*.h files.
#include <asm/cpu-single.h>
#include <asm/cpu-multi26.h>
#include <asm/cpu-multi32.h>
 *
#endif

#endif /* __ASM_PROCFNS_H */
