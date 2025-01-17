#ifndef __ALPHA_UACCESS_H
#define __ALPHA_UACCESS_H

#include <linux/errno.h>
#include <linux/sched.h>


/*
 * The fs value determines whether argument validity checking should be
 * performed or not.  If get_fs() == USER_DS, checking is performed, with
 * get_fs() == KERNEL_DS, checking is bypassed.
 *
 * Or at least it did once upon a time.  Nowadays it is a mask that
 * defines which bits of the address space are off limits.  This is a
 * wee bit faster than the above.
 *
 * For historical reasons, these macros are grossly misnamed.
 */

#define KERNEL_DS	((mm_segment_t) { 0UL })
#define USER_DS		((mm_segment_t) { -0x40000000000UL })

#define VERIFY_READ	0
#define VERIFY_WRITE	1

#define get_fs()  (current->thread.fs)
#define get_ds()  (KERNEL_DS)
#define set_fs(x) (current->thread.fs = (x))

#define segment_eq(a,b)	((a).seg == (b).seg)


/*
 * Is a address valid? This does a straighforward calculation rather
 * than tests.
 *
 * Address valid if:
 *  - "addr" doesn't have any high-bits set
 *  - AND "size" doesn't have any high-bits set
 *  - AND "addr+size" doesn't have any high-bits set
 *  - OR we are in kernel mode.
 */
#define __access_ok(addr,size,segment) \
	(((segment).seg & (addr | size | (addr+size))) == 0)

#define access_ok(type,addr,size) \
	__access_ok(((unsigned long)(addr)),(size),get_fs())

extern inline int verify_area(int type, const void * addr, unsigned long size)
{
	return access_ok(type,addr,size) ? 0 : -EFAULT;
}

/*
 * These are the main single-value transfer routines.  They automatically
 * use the right size if we just have the right pointer type.
 *
 * As the alpha uses the same address space for kernel and user
 * data, we can just do these as direct assignments.  (Of course, the
 * exception handling means that it's no longer "just"...)
 *
 * Careful to not
 * (a) re-use the arguments for side effects (sizeof/typeof is ok)
 * (b) require any knowledge of processes at this stage
 */
#define put_user(x,ptr) \
  __put_user_check((__typeof__(*(ptr)))(x),(ptr),sizeof(*(ptr)),get_fs())
#define get_user(x,ptr) \
  __get_user_check((x),(ptr),sizeof(*(ptr)),get_fs())

/*
 * The "__xxx" versions do not do address space checking, useful when
 * doing multiple accesses to the same area (the programmer has to do the
 * checks by hand with "access_ok()")
 */
#define __put_user(x,ptr) \
  __put_user_nocheck((__typeof__(*(ptr)))(x),(ptr),sizeof(*(ptr)))
#define __get_user(x,ptr) \
  __get_user_nocheck((x),(ptr),sizeof(*(ptr)))
  
/*
 * The "xxx_ret" versions return constant specified in third argument, if
 * something bad happens. These macros can be optimized for the
 * case of just returning from the function xxx_ret is used.
 */

#define put_user_ret(x,ptr,ret) ({ \
if (put_user(x,ptr)) return ret; })

#define get_user_ret(x,ptr,ret) ({ \
if (get_user(x,ptr)) return ret; })

#define __put_user_ret(x,ptr,ret) ({ \
if (__put_user(x,ptr)) return ret; })

#define __get_user_ret(x,ptr,ret) ({ \
if (__get_user(x,ptr)) return ret; })

/*
 * The "lda %1, 2b-1b(%0)" bits are magic to get the assembler to
 * encode the bits we need for resolving the exception.  See the
 * more extensive comments with fixup_inline_exception below for
 * more information.
 */

extern void __get_user_unknown(void);

#define __get_user_nocheck(x,ptr,size)				\
({								\
	long __gu_err = 0, __gu_val;				\
	switch (size) {						\
	  case 1: __get_user_8(ptr); break;			\
	  case 2: __get_user_16(ptr); break;			\
	  case 4: __get_user_32(ptr); break;			\
	  case 8: __get_user_64(ptr); break;			\
	  default: __get_user_unknown(); break;			\
	}							\
	(x) = (__typeof__(*(ptr))) __gu_val;			\
	__gu_err;						\
})

#define __get_user_check(x,ptr,size,segment)			\
({								\
	long __gu_err = -EFAULT, __gu_val = 0;			\
	const __typeof__(*(ptr)) *__gu_addr = (ptr);		\
	if (__access_ok((long)__gu_addr,size,segment)) {	\
		__gu_err = 0;					\
		switch (size) {					\
		  case 1: __get_user_8(__gu_addr); break;	\
		  case 2: __get_user_16(__gu_addr); break;	\
		  case 4: __get_user_32(__gu_addr); break;	\
		  case 8: __get_user_64(__gu_addr); break;	\
		  default: __get_user_unknown(); break;		\
		}						\
	}							\
	(x) = (__typeof__(*(ptr))) __gu_val;			\
	__gu_err;						\
})

struct __large_struct { unsigned long buf[100]; };
#define __m(x) (*(struct __large_struct *)(x))

#define __get_user_64(addr)				\
	__asm__("1: ldq %0,%2\n"			\
	"2:\n"						\
	".section __ex_table,\"a\"\n"			\
	"	.gprel32 1b\n"				\
	"	lda %0, 2b-1b(%1)\n"			\
	".previous"					\
		: "=r"(__gu_val), "=r"(__gu_err)	\
		: "m"(__m(addr)), "1"(__gu_err))

#define __get_user_32(addr)				\
	__asm__("1: ldl %0,%2\n"			\
	"2:\n"						\
	".section __ex_table,\"a\"\n"			\
	"	.gprel32 1b\n"				\
	"	lda %0, 2b-1b(%1)\n"			\
	".previous"					\
		: "=r"(__gu_val), "=r"(__gu_err)	\
		: "m"(__m(addr)), "1"(__gu_err))

#ifdef __alpha_bwx__
/* Those lucky bastards with ev56 and later CPUs can do byte/word moves.  */

#define __get_user_16(addr)				\
	__asm__("1: ldwu %0,%2\n"			\
	"2:\n"						\
	".section __ex_table,\"a\"\n"			\
	"	.gprel32 1b\n"				\
	"	lda %0, 2b-1b(%1)\n"			\
	".previous"					\
		: "=r"(__gu_val), "=r"(__gu_err)	\
		: "m"(__m(addr)), "1"(__gu_err))

#define __get_user_8(addr)				\
	__asm__("1: ldbu %0,%2\n"			\
	"2:\n"						\
	".section __ex_table,\"a\"\n"			\
	"	.gprel32 1b\n"				\
	"	lda %0, 2b-1b(%1)\n"			\
	".previous"					\
		: "=r"(__gu_val), "=r"(__gu_err)	\
		: "m"(__m(addr)), "1"(__gu_err))
#else
/* Unfortunately, we can't get an unaligned access trap for the sub-word
   load, so we have to do a general unaligned operation.  */

#define __get_user_16(addr)						\
{									\
	long __gu_tmp;							\
	__asm__("1: ldq_u %0,0(%3)\n"					\
	"2:	ldq_u %1,1(%3)\n"					\
	"	extwl %0,%3,%0\n"					\
	"	extwh %1,%3,%1\n"					\
	"	or %0,%1,%0\n"						\
	"3:\n"								\
	".section __ex_table,\"a\"\n"					\
	"	.gprel32 1b\n"						\
	"	lda %0, 3b-1b(%2)\n"					\
	"	.gprel32 2b\n"						\
	"	lda %0, 2b-1b(%2)\n"					\
	".previous"							\
		: "=&r"(__gu_val), "=&r"(__gu_tmp), "=r"(__gu_err)	\
		: "r"(addr), "2"(__gu_err));				\
}

#define __get_user_8(addr)						\
	__asm__("1: ldq_u %0,0(%2)\n"					\
	"	extbl %0,%2,%0\n"					\
	"2:\n"								\
	".section __ex_table,\"a\"\n"					\
	"	.gprel32 1b\n"						\
	"	lda %0, 2b-1b(%1)\n"					\
	".previous"							\
		: "=&r"(__gu_val), "=r"(__gu_err)			\
		: "r"(addr), "1"(__gu_err))
#endif

extern void __put_user_unknown(void);

#define __put_user_nocheck(x,ptr,size)				\
({								\
	long __pu_err = 0;					\
	switch (size) {						\
	  case 1: __put_user_8(x,ptr); break;			\
	  case 2: __put_user_16(x,ptr); break;			\
	  case 4: __put_user_32(x,ptr); break;			\
	  case 8: __put_user_64(x,ptr); break;			\
	  default: __put_user_unknown(); break;			\
	}							\
	__pu_err;						\
})

#define __put_user_check(x,ptr,size,segment)			\
({								\
	long __pu_err = -EFAULT;				\
	__typeof__(*(ptr)) *__pu_addr = (ptr);			\
	if (__access_ok((long)__pu_addr,size,segment)) {	\
		__pu_err = 0;					\
		switch (size) {					\
		  case 1: __put_user_8(x,__pu_addr); break;	\
		  case 2: __put_user_16(x,__pu_addr); break;	\
		  case 4: __put_user_32(x,__pu_addr); break;	\
		  case 8: __put_user_64(x,__pu_addr); break;	\
		  default: __put_user_unknown(); break;		\
		}						\
	}							\
	__pu_err;						\
})

/*
 * The "__put_user_xx()" macros tell gcc they read from memory
 * instead of writing: this is because they do not write to
 * any memory gcc knows about, so there are no aliasing issues
 */
#define __put_user_64(x,addr)					\
__asm__ __volatile__("1: stq %r2,%1\n"				\
	"2:\n"							\
	".section __ex_table,\"a\"\n"				\
	"	.gprel32 1b\n"					\
	"	lda $31,2b-1b(%0)\n"				\
	".previous"						\
		: "=r"(__pu_err)				\
		: "m" (__m(addr)), "rJ" (x), "0"(__pu_err))

#define __put_user_32(x,addr)					\
__asm__ __volatile__("1: stl %r2,%1\n"				\
	"2:\n"							\
	".section __ex_table,\"a\"\n"				\
	"	.gprel32 1b\n"					\
	"	lda $31,2b-1b(%0)\n"				\
	".previous"						\
		: "=r"(__pu_err)				\
		: "m"(__m(addr)), "rJ"(x), "0"(__pu_err))

#ifdef __alpha_bwx__
/* Those lucky bastards with ev56 and later CPUs can do byte/word moves.  */

#define __put_user_16(x,addr)					\
__asm__ __volatile__("1: stw %r2,%1\n"				\
	"2:\n"							\
	".section __ex_table,\"a\"\n"				\
	"	.gprel32 1b\n"					\
	"	lda $31,2b-1b(%0)\n"				\
	".previous"						\
		: "=r"(__pu_err)				\
		: "m"(__m(addr)), "rJ"(x), "0"(__pu_err))

#define __put_user_8(x,addr)					\
__asm__ __volatile__("1: stb %r2,%1\n"				\
	"2:\n"							\
	".section __ex_table,\"a\"\n"				\
	"	.gprel32 1b\n"					\
	"	lda $31,2b-1b(%0)\n"				\
	".previous"						\
		: "=r"(__pu_err)				\
		: "m"(__m(addr)), "rJ"(x), "0"(__pu_err))
#else
/* Unfortunately, we can't get an unaligned access trap for the sub-word
   write, so we have to do a general unaligned operation.  */

#define __put_user_16(x,addr)					\
{								\
	long __pu_tmp1, __pu_tmp2, __pu_tmp3, __pu_tmp4;	\
	__asm__ __volatile__(					\
	"1:	ldq_u %2,1(%5)\n"				\
	"2:	ldq_u %1,0(%5)\n"				\
	"	inswh %6,%5,%4\n"				\
	"	inswl %6,%5,%3\n"				\
	"	mskwh %2,%5,%2\n"				\
	"	mskwl %1,%5,%1\n"				\
	"	or %2,%4,%2\n"					\
	"	or %1,%3,%1\n"					\
	"3:	stq_u %2,1(%5)\n"				\
	"4:	stq_u %1,0(%5)\n"				\
	"5:\n"							\
	".section __ex_table,\"a\"\n"				\
	"	.gprel32 1b\n"					\
	"	lda $31, 5b-1b(%0)\n"				\
	"	.gprel32 2b\n"					\
	"	lda $31, 5b-2b(%0)\n"				\
	"	.gprel32 3b\n"					\
	"	lda $31, 5b-3b(%0)\n"				\
	"	.gprel32 4b\n"					\
	"	lda $31, 5b-4b(%0)\n"				\
	".previous"						\
		: "=r"(__pu_err), "=&r"(__pu_tmp1),		\
		  "=&r"(__pu_tmp2), "=&r"(__pu_tmp3),		\
		  "=&r"(__pu_tmp4)				\
		: "r"(addr), "r"((unsigned long)(x)), "0"(__pu_err)); \
}

#define __put_user_8(x,addr)					\
{								\
	long __pu_tmp1, __pu_tmp2;				\
	__asm__ __volatile__(					\
	"1:	ldq_u %1,0(%4)\n"				\
	"	insbl %3,%4,%2\n"				\
	"	mskbl %1,%4,%1\n"				\
	"	or %1,%2,%1\n"					\
	"2:	stq_u %1,0(%4)\n"				\
	"3:\n"							\
	".section __ex_table,\"a\"\n"				\
	"	.gprel32 1b\n"					\
	"	lda $31, 3b-1b(%0)\n"				\
	"	.gprel32 2b\n"					\
	"	lda $31, 3b-2b(%0)\n"				\
	".previous"						\
		: "=r"(__pu_err),				\
	  	  "=&r"(__pu_tmp1), "=&r"(__pu_tmp2)		\
		: "r"((unsigned long)(x)), "r"(addr), "0"(__pu_err)); \
}
#endif


/*
 * Complex access routines
 */

extern void __copy_user(void);

extern inline long
__copy_tofrom_user_nocheck(void *to, const void *from, long len)
{
	/* This little bit of silliness is to get the GP loaded for
	   a function that ordinarily wouldn't.  Otherwise we could
	   have it done by the macro directly, which can be optimized
	   the linker.  */
	register void * pv __asm__("$27") = __copy_user;

	register void * __cu_to __asm__("$6") = to;
	register const void * __cu_from __asm__("$7") = from;
	register long __cu_len __asm__("$0") = len;

	__asm__ __volatile__(
		"jsr $28,(%3),__copy_user\n\tldgp $29,0($28)"
		: "=r" (__cu_len), "=r" (__cu_from), "=r" (__cu_to), "=r"(pv)
		: "0" (__cu_len), "1" (__cu_from), "2" (__cu_to), "3"(pv)
		: "$1","$2","$3","$4","$5","$28","memory");

	return __cu_len;
}

extern inline long
__copy_tofrom_user(void *to, const void *from, long len, const void *validate)
{
	if (__access_ok((long)validate, len, get_fs())) {
		register void * pv __asm__("$27") = __copy_user;
		register void * __cu_to __asm__("$6") = to;
		register const void * __cu_from __asm__("$7") = from;
		register long __cu_len __asm__("$0") = len;
		__asm__ __volatile__(
			"jsr $28,(%3),__copy_user\n\tldgp $29,0($28)"
			: "=r"(__cu_len), "=r"(__cu_from), "=r"(__cu_to),
			  "=r" (pv)
			: "0" (__cu_len), "1" (__cu_from), "2" (__cu_to), 
			  "3" (pv)
			: "$1","$2","$3","$4","$5","$28","memory");
		len = __cu_len;
	}
	return len;
}

#define __copy_to_user(to,from,n)   __copy_tofrom_user_nocheck((to),(from),(n))
#define __copy_from_user(to,from,n) __copy_tofrom_user_nocheck((to),(from),(n))

extern inline long
copy_to_user(void *to, const void *from, long n)
{
	return __copy_tofrom_user(to, from, n, to);
}

extern inline long
copy_from_user(void *to, const void *from, long n)
{
	return __copy_tofrom_user(to, from, n, from);
}

#define copy_to_user_ret(to,from,n,retval) ({ \
if (copy_to_user(to,from,n)) \
	return retval; \
})

#define copy_from_user_ret(to,from,n,retval) ({ \
if (copy_from_user(to,from,n)) \
	return retval; \
})

extern void __do_clear_user(void);

extern inline long
__clear_user(void *to, long len)
{
	/* This little bit of silliness is to get the GP loaded for
	   a function that ordinarily wouldn't.  Otherwise we could
	   have it done by the macro directly, which can be optimized
	   the linker.  */
	register void * pv __asm__("$27") = __do_clear_user;

	register void * __cl_to __asm__("$6") = to;
	register long __cl_len __asm__("$0") = len;
	__asm__ __volatile__(
		"jsr $28,(%2),__do_clear_user\n\tldgp $29,0($28)"
		: "=r"(__cl_len), "=r"(__cl_to), "=r"(pv)
		: "0"(__cl_len), "1"(__cl_to), "2"(pv)
		: "$1","$2","$3","$4","$5","$28","memory");
	return __cl_len;
}

extern inline long
clear_user(void *to, long len)
{
	if (__access_ok((long)to, len, get_fs())) {
		register void * pv __asm__("$27") = __do_clear_user;
		register void * __cl_to __asm__("$6") = to;
		register long __cl_len __asm__("$0") = len;
		__asm__ __volatile__(
			"jsr $28,(%2),__do_clear_user\n\tldgp $29,0($28)"
			: "=r"(__cl_len), "=r"(__cl_to), "=r"(pv)
			: "0"(__cl_len), "1"(__cl_to), "2"(pv)
			: "$1","$2","$3","$4","$5","$28","memory");
		len = __cl_len;
	}
	return len;
}

/* Returns: -EFAULT if exception before terminator, N if the entire
   buffer filled, else strlen.  */

extern long __strncpy_from_user(char *__to, const char *__from, long __to_len);

extern inline long
strncpy_from_user(char *to, const char *from, long n)
{
	long ret = -EFAULT;
	if (__access_ok((long)from, 0, get_fs()))
		ret = __strncpy_from_user(to, from, n);
	return ret;
}

/* Returns: 0 if bad, string length+1 (memory size) of string if ok */
extern long __strlen_user(const char *);

extern inline long strlen_user(const char *str)
{
	return access_ok(VERIFY_READ,str,0) ? __strlen_user(str) : 0;
}

/* Returns: 0 if exception before NUL or reaching the supplied limit (N),
 * a value greater than N if the limit would be exceeded, else strlen.  */
extern long __strnlen_user(const char *, long);

extern inline long strnlen_user(const char *str, long n)
{
	return access_ok(VERIFY_READ,str,0) ? __strnlen_user(str, n) : 0;
}

/*
 * About the exception table:
 *
 * - insn is a 32-bit offset off of the kernel's or module's gp.
 * - nextinsn is a 16-bit offset off of the faulting instruction
 *   (not off of the *next* instruction as branches are).
 * - errreg is the register in which to place -EFAULT.
 * - valreg is the final target register for the load sequence
 *   and will be zeroed.
 *
 * Either errreg or valreg may be $31, in which case nothing happens.
 *
 * The exception fixup information "just so happens" to be arranged
 * as in a MEM format instruction.  This lets us emit our three
 * values like so:
 *
 *      lda valreg, nextinsn(errreg)
 *
 */

struct exception_table_entry
{
	signed int insn;
	union exception_fixup {
		unsigned unit;
		struct {
			signed int nextinsn : 16;
			unsigned int errreg : 5;
			unsigned int valreg : 5;
		} bits;
	} fixup;
};

/* Returns 0 if exception not found and fixup.unit otherwise.  */
extern unsigned search_exception_table(unsigned long);

/* Returns the new pc */
#define fixup_exception(map_reg, fixup_unit, pc)		\
({								\
	union exception_fixup __fie_fixup;			\
	__fie_fixup.unit = fixup_unit;				\
	if (__fie_fixup.bits.valreg != 31)			\
		map_reg(__fie_fixup.bits.valreg) = 0;		\
	if (__fie_fixup.bits.errreg != 31)			\
		map_reg(__fie_fixup.bits.errreg) = -EFAULT;	\
	(pc) + __fie_fixup.bits.nextinsn;			\
})


#endif /* __ALPHA_UACCESS_H */
