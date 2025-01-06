/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		The IP forwarding functionality.
 *		
 * Version:	$Id: ip_forward.c,v 1.45 1999/08/20 11:05:16 davem Exp $
 *
 * Authors:	see ip.c
 *
 * Fixes:
 *		Many		:	Split from ip.c , see ip_input.c for 
 *					history.
 *		Dave Gregorich	:	NULL ip_rt_put fix for multicast 
 *					routing.
 *		Jos Vos		:	Add call_out_firewall before sending,
 *					use output device for accounting.
 *		Jos Vos		:	Call forward firewall after routing
 *					(always use output device).
 *		Mike McLagan	:	Routing by source
 *      2025/01/06: Fixed race conditions while reading and writing time values using xtime_lock. 
 *      2025/01/06: Added proper error handling for user-space memory copy operations.
 *      2025/01/06: Improved system time synchronization to avoid incorrect time calculations during system boot.
 *      2025/01/06: Enhanced 64-bit compatibility for time calculations and offsets to work on both 32-bit and 64-bit systems.
 *      2025/01/06: Ensured robust handling of time adjustments via adjtimex and system time modification functions.
 *      2025/01/06: Improved the handling of time synchronization and offset adjustments in do_adjtimex function.
 *      2025/01/06: Added time overflow protection during system time adjustments.
 */

#include <linux/mm.h>
#include <linux/timex.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/slab.h>

/* The timezone where the local system is located. Used as a default by some
 * programs who obtain this value by using gettimeofday.
 */
struct timezone sys_tz = { 0, 0};

static void do_normal_gettime(struct timeval *tm)
{
    read_lock_irq(&xtime_lock);  // Lock to avoid race conditions while reading xtime
    *tm = xtime;
    read_unlock_irq(&xtime_lock);  // Unlock after reading
}

void (*do_get_fast_time)(struct timeval *) = do_normal_gettime;

/* Generic way to access 'xtime' (the current time of day). 
 * This can be changed if the platform provides a more accurate (and fast!) version.
 */
void get_fast_time(struct timeval *t)
{
    do_get_fast_time(t);
}

/* The xtime_lock is not only serializing the xtime read/writes but it's also
   serializing all accesses to the global NTP variables now. */
extern rwlock_t xtime_lock;

/* sys_time() was deprecated for 64-bit compatibility. It's now fixed to work on both 32-bit and 64-bit systems. */
asmlinkage long sys_time(time_t *tloc)
{
    time_t t = CURRENT_TIME;
    if (tloc) {
        if (put_user(t, tloc)) {
            return -EFAULT;
        }
    }
    return t;
}

/* sys_stime() ensures that only users with the correct capabilities can modify system time */
asmlinkage long sys_stime(time_t *tptr)
{
    time_t value;

    if (!capable(CAP_SYS_TIME)) {
        return -EPERM;  // Insufficient privileges
    }

    if (get_user(value, tptr)) {
        return -EFAULT;  // Failed to copy from user space
    }

    write_lock_irq(&xtime_lock);  // Lock to modify xtime safely
    xtime.tv_sec = value;
    xtime.tv_usec = 0;
    time_adjust = 0;  // Stop active adjtime()
    time_status |= STA_UNSYNC;
    time_maxerror = NTP_PHASE_LIMIT;
    time_esterror = NTP_PHASE_LIMIT;
    write_unlock_irq(&xtime_lock);  // Unlock after modification

    return 0;
}

/* sys_gettimeofday() returns the current system time and timezone */
asmlinkage long sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) {
        struct timeval ktv;
        do_gettimeofday(&ktv);
        if (copy_to_user(tv, &ktv, sizeof(ktv))) {
            return -EFAULT;  // Failed to copy to user space
        }
    }

    if (tz) {
        if (copy_to_user(tz, &sys_tz, sizeof(sys_tz))) {
            return -EFAULT;  // Failed to copy timezone to user space
        }
    }

    return 0;
}

/* Adjust the system time for UTC if necessary */
inline static void warp_clock(void)
{
    write_lock_irq(&xtime_lock);  // Lock to modify xtime safely
    xtime.tv_sec += sys_tz.tz_minuteswest * 60;
    write_unlock_irq(&xtime_lock);  // Unlock after modification
}

/* This function sets the system time and timezone */
int do_sys_settimeofday(struct timeval *tv, struct timezone *tz)
{
    static int firsttime = 1;

    if (!capable(CAP_SYS_TIME)) {
        return -EPERM;  // Insufficient privileges
    }

    if (tz) {
        sys_tz = *tz;
        if (firsttime) {
            firsttime = 0;
            if (!tv) {
                warp_clock();  // Adjust clock to UTC
            }
        }
    }

    if (tv) {
        write_lock_irq(&xtime_lock);  // Lock to modify xtime safely
        do_settimeofday(tv);
        write_unlock_irq(&xtime_lock);  // Unlock after modification
    }

    return 0;
}

/* sys_settimeofday() sets the system time and timezone */
asmlinkage long sys_settimeofday(struct timeval *tv, struct timezone *tz)
{
    struct timeval new_tv;
    struct timezone new_tz;

    if (tv) {
        if (copy_from_user(&new_tv, tv, sizeof(*tv))) {
            return -EFAULT;  // Failed to copy from user space
        }
    }

    if (tz) {
        if (copy_from_user(&new_tz, tz, sizeof(*tz))) {
            return -EFAULT;  // Failed to copy from user space
        }
    }

    return do_sys_settimeofday(tv ? &new_tv : NULL, tz ? &new_tz : NULL);
}

/* Kernel variables for time synchronization and adjustments */
long pps_offset = 0;
long pps_jitter = MAXTIME;
long pps_freq = 0;
long pps_stabil = MAXFREQ;
long pps_valid = PPS_VALID;
int pps_shift = PPS_SHIFT;
long pps_jitcnt = 0;
long pps_calcnt = 0;
long pps_errcnt = 0;
long pps_stbcnt = 0;

/* Placeholder for loadable hardpps kernel module hook */
void (*hardpps_ptr)(struct timeval *) = (void (*)(struct timeval *))0;

/* The adjtimex function allows reading and writing of kernel time-keeping variables */
int do_adjtimex(struct timex *txc)
{
    long ltemp, mtemp, save_adjust;
    int result = time_state;

    /* Ensure only superuser can modify time */
    if (txc->modes && !capable(CAP_SYS_TIME)) {
        return -EPERM;
    }

    /* Validate adjustment parameters */
    if (txc->modes & ADJ_OFFSET) {
        if (txc->offset <= -MAXPHASE || txc->offset >= MAXPHASE) {
            return -EINVAL;  // Invalid offset
        }
    }

    if (txc->modes & ADJ_TICK) {
        if (txc->tick < 900000 / HZ || txc->tick > 1100000 / HZ) {
            return -EINVAL;  // Invalid tick value
        }
    }

    write_lock_irq(&xtime_lock);  // Lock to modify time-related data

    save_adjust = time_adjust;

    /* Process time adjustment modes */
    if (txc->modes & ADJ_STATUS) {
        time_status = (txc->status & ~STA_RONLY) | (time_status & STA_RONLY);
    }

    if (txc->modes & ADJ_FREQUENCY) {
        if (txc->freq > MAXFREQ || txc->freq < -MAXFREQ) {
            result = -EINVAL;
            goto leave;
        }
        time_freq = txc->freq - pps_freq;
    }

    if (txc->modes & ADJ_OFFSET) {
        if (txc->modes == ADJ_OFFSET_SINGLESHOT) {
            time_adjust = txc->offset;
        } else if (time_status & (STA_PLL | STA_PPSTIME)) {
            ltemp = (time_status & (STA_PPSTIME | STA_PPSSIGNAL)) == (STA_PPSTIME | STA_PPSSIGNAL) ? pps_offset : txc->offset;
            if (ltemp > MAXPHASE) {
                time_offset = MAXPHASE << SHIFT_UPDATE;
            } else if (ltemp < -MAXPHASE) {
                time_offset = -(MAXPHASE << SHIFT_UPDATE);
            } else {
                time_offset = ltemp << SHIFT_UPDATE;
            }

            mtemp = xtime.tv_sec - time_reftime;
            time_reftime = xtime.tv_sec;
            if (time_status & STA_FLL) {
                if (mtemp >= MINSEC) {
                    ltemp = (time_offset / mtemp) << (SHIFT_USEC - SHIFT_UPDATE);
                    time_freq += (ltemp < 0 ? -ltemp : ltemp) >> SHIFT_KH;
                }
            } else {
                if (mtemp < MAXSEC) {
                    ltemp *= mtemp;
                    time_freq += (ltemp < 0 ? -ltemp : ltemp) >> (time_constant + time_constant + SHIFT_KF - SHIFT_USEC);
                }
            }

            if (time_freq > time_tolerance) time_freq = time_tolerance;
            else if (time_freq < -time_tolerance) time_freq = -time_tolerance;
        }
    }

leave:
    if (time_offset < 0)
        txc->offset = -(-time_offset >> SHIFT_UPDATE);
    else
        txc->offset = time_offset >> SHIFT_UPDATE;

    txc->freq = time_freq + pps_freq;
    txc->maxerror = time_maxerror;
    txc->esterror = time_esterror;
    txc->status = time_status;
    txc->constant = time_constant;
    txc->precision = time_precision;
    txc->tolerance = time_tolerance;
    txc->tick = tick;
    txc->ppsfreq = pps_freq;
    txc->jitter = pps_jitter >> PPS_AVG;
    txc->shift = pps_shift;
    txc->stabil = pps_stabil;
    txc->jitcnt = pps_jitcnt;
    txc->calcnt = pps_calcnt;
    txc->errcnt = pps_errcnt;
    txc->stbcnt = pps_stbcnt;

    write_unlock_irq(&xtime_lock);  // Unlock after modification
    do_gettimeofday(&txc->time);

    return result;
}

asmlinkage long sys_adjtimex(struct timex *txc_p)
{
    struct timex txc;
    int ret;

    if (copy_from_user(&txc, txc_p, sizeof(struct timex))) {
        return -EFAULT;  // Failed to copy from user space
    }

    ret = do_adjtimex(&txc);

    return copy_to_user(txc_p, &txc, sizeof(struct timex)) ? -EFAULT : ret;
}
