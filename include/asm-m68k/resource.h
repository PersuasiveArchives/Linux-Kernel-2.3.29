#ifndef _M68K_RESOURCE_H
#define _M68K_RESOURCE_H

/*
 * Resource limits
 */

#define RLIMIT_CPU	0		/* CPU time in ms */
#define RLIMIT_FSIZE	1		/* Maximum filesize */
#define RLIMIT_DATA	2		/* max data size */
#define RLIMIT_STACK	3		/* max stack size */
#define RLIMIT_CORE	4		/* max core file size */
#define RLIMIT_RSS	5		/* max resident set size */
#define RLIMIT_NPROC	6		/* max number of processes */
#define RLIMIT_NOFILE	7		/* max number of open files */
#define RLIMIT_MEMLOCK	8		/* max locked-in-memory address space*/
#define RLIMIT_AS	9		/* address space limit */

#define RLIM_NLIMITS	10

#ifdef __KERNEL__

#define INIT_RLIMITS	\
{                       \
  {LONG_MAX, LONG_MAX}, \
  {LONG_MAX, LONG_MAX}, \
  {LONG_MAX, LONG_MAX}, \
  {_STK_LIM, LONG_MAX}, \
  {       0, LONG_MAX}, \
  {LONG_MAX, LONG_MAX}, \
  {0, 0},		\
  {INR_OPEN, INR_OPEN}, \
  {LONG_MAX, LONG_MAX}, \
  {LONG_MAX, LONG_MAX}  \
}

#endif /* __KERNEL__ */

#endif /* _M68K_RESOURCE_H */
