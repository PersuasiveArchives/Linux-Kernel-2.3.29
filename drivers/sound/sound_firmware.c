#include "os.h"
#define __KERNEL_SYSCALLS__
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/unistd.h>
#include <asm/uaccess.h>

static int errno;
static int do_mod_firmware_load(const char *fn, char **fp)
{
    int fd;
    long l;
    char *dp;
    int ret;

    fd = sys_open(fn, O_RDONLY, 0);  // Use sys_open instead of open
    if (fd < 0)
    {
        printk(KERN_INFO "Unable to load '%s'.\n", fn);
        return 0;
    }

    l = sys_lseek(fd, 0L, SEEK_END);  // Use sys_lseek instead of lseek
    if (l <= 0 || l > 131072)
    {
        printk(KERN_INFO "Invalid firmware '%s'\n", fn);
        sys_close(fd);
        return 0;
    }

    sys_lseek(fd, 0L, SEEK_SET);  // Reset file pointer to the start
    dp = vmalloc(l);
    if (dp == NULL)
    {
        printk(KERN_INFO "Out of memory loading '%s'.\n", fn);
        sys_close(fd);
        return 0;
    }

    ret = sys_read(fd, dp, l);  // Use sys_read instead of read
    if (ret != l)
    {
        printk(KERN_INFO "Failed to read '%s'.\n", fn);
        vfree(dp);
        sys_close(fd);
        return 0;
    }

    sys_close(fd);  // Use sys_close instead of close
    *fp = dp;
    return (int) l;
}

int mod_firmware_load(const char *fn, char **fp)
{
    int r;
    mm_segment_t fs;

    fs = get_fs();
    set_fs(get_ds());  // Switch to kernel address space for reading user memory
    r = do_mod_firmware_load(fn, fp);
    set_fs(fs);  // Restore the previous address space

    return r;
}
