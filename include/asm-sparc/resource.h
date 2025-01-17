/* $Id: resource.h,v 1.9 1999/08/14 03:52:05 anton Exp $
 * resource.h: Resource definitions.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 */

#ifndef _SPARC_RESOURCE_H
#define _SPARC_RESOURCE_H

/*
 * Resource limits
 */

#define RLIMIT_CPU	0		/* CPU time in ms */
#define RLIMIT_FSIZE	1		/* Maximum filesize */
#define RLIMIT_DATA	2		/* max data size */
#define RLIMIT_STACK	3		/* max stack size */
#define RLIMIT_CORE	4		/* max core file size */
#define RLIMIT_RSS	5		/* max resident set size */
#define RLIMIT_NOFILE	6		/* max number of open files */
#define RLIMIT_NPROC	7		/* max number of processes */
#define RLIMIT_MEMLOCK  8               /* max locked-in-memory address space */
#define RLIMIT_AS       9               /* address space limit */

#define RLIM_NLIMITS	10

#ifdef __KERNEL__
#define INIT_RLIMITS				\
{						\
    {LONG_MAX, LONG_MAX}, {LONG_MAX, LONG_MAX},	\
    {LONG_MAX, LONG_MAX}, {_STK_LIM, LONG_MAX},	\
    {       0, LONG_MAX}, {LONG_MAX, LONG_MAX},	\
    {INR_OPEN, INR_OPEN}, {0, 0},		\
    {LONG_MAX, LONG_MAX}, {LONG_MAX, LONG_MAX}  \
}

#endif /* __KERNEL__ */

#endif /* !(_SPARC_RESOURCE_H) */
