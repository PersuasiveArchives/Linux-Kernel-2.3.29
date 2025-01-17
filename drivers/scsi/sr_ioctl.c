#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/errno.h>
#include <linux/string.h>

#include <linux/blk.h>
#include <linux/blkpg.h>
#include "scsi.h"
#include "hosts.h"
#include <scsi/scsi_ioctl.h>

#include <linux/cdrom.h>
#include "sr.h"

#if 0
# define DEBUG
#endif

/* The sr_is_xa() seems to trigger firmware bugs with some drives :-(
 * It is off by default and can be turned on with this module parameter */
static int xa_test = 0;

extern void get_sectorsize(int);

#define IOCTL_RETRIES 3
/* The CDROM is fairly slow, so we need a little extra time */
/* In fact, it is very slow if it has to spin up first */
#define IOCTL_TIMEOUT 30*HZ

static void sr_ioctl_done(Scsi_Cmnd * SCpnt)
{
    struct request * req;
    
    req = &SCpnt->request;
    req->rq_status = RQ_SCSI_DONE; /* Busy, but indicate request done */

    if (SCpnt->buffer && req->buffer && SCpnt->buffer != req->buffer) {
	memcpy(req->buffer, SCpnt->buffer, SCpnt->bufflen);
	scsi_free(SCpnt->buffer, (SCpnt->bufflen + 511) & ~511);
	SCpnt->buffer = req->buffer;
    } 
    
    if (req->sem != NULL) {
	up(req->sem);
    }
}

/* We do our own retries because we want to know what the specific
   error code is.  Normally the UNIT_ATTENTION code will automatically
   clear after one error */

int sr_do_ioctl(int target, unsigned char * sr_cmd, void * buffer, unsigned buflength, int quiet)
{
    Scsi_Cmnd * SCpnt;
    Scsi_Device * SDev;
    int result, err = 0, retries = 0;
    unsigned long flags;
    char * bounce_buffer;

    spin_lock_irqsave(&io_request_lock, flags);
    SDev  = scsi_CDs[target].device;
    SCpnt = scsi_allocate_device(NULL, scsi_CDs[target].device, 1);
    spin_unlock_irqrestore(&io_request_lock, flags);

    /* use ISA DMA buffer if necessary */
    SCpnt->request.buffer=buffer;
    if (buffer && SCpnt->host->unchecked_isa_dma &&
       (virt_to_phys(buffer) + buflength - 1 > ISA_DMA_THRESHOLD)) {
	bounce_buffer = (char *)scsi_malloc((buflength + 511) & ~511);
	if (bounce_buffer == NULL) {
		printk("SCSI DMA pool exhausted.");
		return -ENOMEM;
	}
	memcpy(bounce_buffer, (char *)buffer, buflength);
	buffer = bounce_buffer;
    }

retry:
    if( !scsi_block_when_processing_errors(SDev) )
        return -ENODEV;

    scsi_wait_cmd(SCpnt, (void *)sr_cmd, (void *)buffer, buflength,
		  sr_ioctl_done, IOCTL_TIMEOUT, IOCTL_RETRIES);

    result = SCpnt->result;
    
    /* Minimal error checking.  Ignore cases we know about, and report the rest. */
    if(driver_byte(result) != 0) {
	switch(SCpnt->sense_buffer[2] & 0xf) {
	case UNIT_ATTENTION:
	    scsi_CDs[target].device->changed = 1;
	    if (!quiet)
	    	printk(KERN_INFO "sr%d: disc change detected.\n", target);
	    if (retries++ < 10)
		goto retry;
	    err = -ENOMEDIUM;
	    break;
	case NOT_READY: /* This happens if there is no disc in drive */
            if (SCpnt->sense_buffer[12] == 0x04 &&
                SCpnt->sense_buffer[13] == 0x01) {
                /* sense: Logical unit is in process of becoming ready */
                if (!quiet)
                    printk(KERN_INFO "sr%d: CDROM not ready yet.\n", target);
		if (retries++ < 10) {
		    /* sleep 2 sec and try again */
		    /*
		     * The spinlock is silly - we should really lock more of this
		     * function, but the minimal locking required to not lock up
		     * is around this - scsi_sleep() assumes we hold the spinlock.
		     */
		    spin_lock_irqsave(&io_request_lock, flags);
		    scsi_sleep(2*HZ);
		    spin_unlock_irqrestore(&io_request_lock, flags);
                    goto retry;
		} else {
		    /* 20 secs are enough? */
		    err = -ENOMEDIUM;
		    break;
		}
            }
	    if (!quiet)
            	printk(KERN_INFO "sr%d: CDROM not ready.  Make sure there is a disc in the drive.\n",target);
#ifdef DEBUG
            print_sense("sr", SCpnt);
#endif
            err = -ENOMEDIUM;
	    break;
	case ILLEGAL_REQUEST:
            if (!quiet)
		printk(KERN_ERR "sr%d: CDROM (ioctl) reports ILLEGAL "
		       "REQUEST.\n", target);
            if (SCpnt->sense_buffer[12] == 0x20 &&
                SCpnt->sense_buffer[13] == 0x00) {
                /* sense: Invalid command operation code */
                err = -EDRIVE_CANT_DO_THIS;
            } else {
                err = -EINVAL;
            }
#ifdef DEBUG
	    print_command(sr_cmd);
            print_sense("sr", SCpnt);
#endif
	    break;
	default:
	    printk(KERN_ERR "sr%d: CDROM (ioctl) error, command: ", target);
	    print_command(sr_cmd);
	    print_sense("sr", SCpnt);
            err = -EIO;
	}
    }
   
    spin_lock_irqsave(&io_request_lock, flags);
    result = SCpnt->result;
    /* Wake up a process waiting for device*/
    wake_up(&SCpnt->device->device_wait);
    scsi_release_command(SCpnt);
    SCpnt = NULL;
    spin_unlock_irqrestore(&io_request_lock, flags);
    return err;
}

/* ---------------------------------------------------------------------- */
/* interface to cdrom.c                                                   */

static int test_unit_ready(int minor)
{
	u_char  sr_cmd[10];

        sr_cmd[0] = GPCMD_TEST_UNIT_READY;
        sr_cmd[1] = ((scsi_CDs[minor].device -> lun) << 5);
        sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = sr_cmd[5] = 0;
        return sr_do_ioctl(minor, sr_cmd, NULL, 255, 1);
}

int sr_tray_move(struct cdrom_device_info *cdi, int pos)
{
        u_char  sr_cmd[10];

        sr_cmd[0] = GPCMD_START_STOP_UNIT;
        sr_cmd[1] = ((scsi_CDs[MINOR(cdi->dev)].device -> lun) << 5);
        sr_cmd[2] = sr_cmd[3] = sr_cmd[5] = 0;
        sr_cmd[4] = (pos == 0) ? 0x03 /* close */ : 0x02 /* eject */;
	
        return sr_do_ioctl(MINOR(cdi->dev), sr_cmd, NULL, 255, 0);
}

int sr_lock_door(struct cdrom_device_info *cdi, int lock)
{
        return scsi_ioctl (scsi_CDs[MINOR(cdi->dev)].device,
                           lock ? SCSI_IOCTL_DOORLOCK : SCSI_IOCTL_DOORUNLOCK,
                           0);
}

int sr_drive_status(struct cdrom_device_info *cdi, int slot)
{
        if (CDSL_CURRENT != slot) {
                /* we have no changer support */
                return -EINVAL;
        }

        if (0 == test_unit_ready(MINOR(cdi->dev)))
            return CDS_DISC_OK;

        return CDS_TRAY_OPEN;
}

int sr_disk_status(struct cdrom_device_info *cdi)
{
	struct cdrom_tochdr    toc_h;
	struct cdrom_tocentry  toc_e;
        int                    i,rc,have_datatracks = 0;

        /* look for data tracks */
        if (0 != (rc = sr_audio_ioctl(cdi, CDROMREADTOCHDR, &toc_h)))
                return (rc == -ENOMEDIUM) ? CDS_NO_DISC : CDS_NO_INFO;

        for (i = toc_h.cdth_trk0; i <= toc_h.cdth_trk1; i++) {
                toc_e.cdte_track  = i;
                toc_e.cdte_format = CDROM_LBA;
                if (sr_audio_ioctl(cdi, CDROMREADTOCENTRY, &toc_e))
                        return CDS_NO_INFO;
                if (toc_e.cdte_ctrl & CDROM_DATA_TRACK) {
                        have_datatracks = 1;
                        break;
                }
        }
        if (!have_datatracks)
            return CDS_AUDIO;

        if (scsi_CDs[MINOR(cdi->dev)].xa_flag)
            return CDS_XA_2_1;
        else
            return CDS_DATA_1;
}

int sr_get_last_session(struct cdrom_device_info *cdi,
                        struct cdrom_multisession* ms_info)
{
        ms_info->addr.lba=scsi_CDs[MINOR(cdi->dev)].ms_offset;
        ms_info->xa_flag=scsi_CDs[MINOR(cdi->dev)].xa_flag ||
            (scsi_CDs[MINOR(cdi->dev)].ms_offset > 0);

	return 0;
}

int sr_get_mcn(struct cdrom_device_info *cdi,struct cdrom_mcn *mcn)
{
        u_char  sr_cmd[10];
	char buffer[32];
        int result;
        	
	sr_cmd[0] = GPCMD_READ_SUBCHANNEL;
	sr_cmd[1] = ((scsi_CDs[MINOR(cdi->dev)].device->lun) << 5);
	sr_cmd[2] = 0x40;    /* I do want the subchannel info */
	sr_cmd[3] = 0x02;    /* Give me medium catalog number info */
	sr_cmd[4] = sr_cmd[5] = 0;
	sr_cmd[6] = 0;
	sr_cmd[7] = 0;
	sr_cmd[8] = 24;
	sr_cmd[9] = 0;

	result = sr_do_ioctl(MINOR(cdi->dev), sr_cmd, buffer, 24, 0);
	
	memcpy (mcn->medium_catalog_number, buffer + 9, 13);
        mcn->medium_catalog_number[13] = 0;

	return result;
}

int sr_reset(struct cdrom_device_info *cdi)
{
	invalidate_buffers(cdi->dev);
	return 0;        
}

int sr_select_speed(struct cdrom_device_info *cdi, int speed)
{
        u_char  sr_cmd[12];

        if (speed == 0)
            speed = 0xffff; /* set to max */
        else
            speed *= 177;   /* Nx to kbyte/s */
        
	memset(sr_cmd,0,12);
	sr_cmd[0] = GPCMD_SET_SPEED; /* SET CD SPEED */
	sr_cmd[1] = (scsi_CDs[MINOR(cdi->dev)].device->lun) << 5;
	sr_cmd[2] = (speed >> 8) & 0xff; /* MSB for speed (in kbytes/sec) */
	sr_cmd[3] =  speed       & 0xff; /* LSB */

        if (sr_do_ioctl(MINOR(cdi->dev), sr_cmd, NULL, 0, 0))
            return -EIO;
	return 0;
}

/* ----------------------------------------------------------------------- */
/* this is called by the generic cdrom driver. arg is a _kernel_ pointer,  */
/* becauce the generic cdrom driver does the user access stuff for us.     */
/* only cdromreadtochdr and cdromreadtocentry are left - for use with the  */
/* sr_disk_status interface for the generic cdrom driver.                  */

int sr_audio_ioctl(struct cdrom_device_info *cdi, unsigned int cmd, void* arg)
{
    u_char  sr_cmd[10];    
    int result, target  = MINOR(cdi->dev);
    unsigned char buffer[32];
    
    switch (cmd) 
    {
    case CDROMREADTOCHDR:
    {
	struct cdrom_tochdr* tochdr = (struct cdrom_tochdr*)arg;
	
	sr_cmd[0] = GPCMD_READ_TOC_PMA_ATIP;
	sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5);
	sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = sr_cmd[5] = 0;
	sr_cmd[6] = 0;
	sr_cmd[7] = 0;              /* MSB of length (12) */
	sr_cmd[8] = 12;             /* LSB of length */
	sr_cmd[9] = 0;
	
	result = sr_do_ioctl(target, sr_cmd, buffer, 12, 1);
	
	tochdr->cdth_trk0 = buffer[2];
	tochdr->cdth_trk1 = buffer[3];
	
        break;
    }
	
    case CDROMREADTOCENTRY:
    {
	struct cdrom_tocentry* tocentry = (struct cdrom_tocentry*)arg;
	
	sr_cmd[0] = GPCMD_READ_TOC_PMA_ATIP;
	sr_cmd[1] = ((scsi_CDs[target].device->lun) << 5) |
          (tocentry->cdte_format == CDROM_MSF ? 0x02 : 0);
	sr_cmd[2] = sr_cmd[3] = sr_cmd[4] = sr_cmd[5] = 0;
	sr_cmd[6] = tocentry->cdte_track;
	sr_cmd[7] = 0;             /* MSB of length (12)  */
	sr_cmd[8] = 12;            /* LSB of length */
	sr_cmd[9] = 0;
	
	result = sr_do_ioctl (target, sr_cmd, buffer, 12, 0);
	
        tocentry->cdte_ctrl = buffer[5] & 0xf;	
        tocentry->cdte_adr = buffer[5] >> 4;
        tocentry->cdte_datamode = (tocentry->cdte_ctrl & 0x04) ? 1 : 0;
	if (tocentry->cdte_format == CDROM_MSF) {
	    tocentry->cdte_addr.msf.minute = buffer[9];
	    tocentry->cdte_addr.msf.second = buffer[10];
	    tocentry->cdte_addr.msf.frame = buffer[11];
	} else
	    tocentry->cdte_addr.lba = (((((buffer[8] << 8) + buffer[9]) << 8)
                                       + buffer[10]) << 8) + buffer[11];
	
        break;
    }

    default:
        return -EINVAL;
    }

#if 0
    if (result)
        printk("DEBUG: sr_audio: result for ioctl %x: %x\n",cmd,result);
#endif
    
    return result;
}

/* -----------------------------------------------------------------------
 * a function to read all sorts of funny cdrom sectors using the READ_CD
 * scsi-3 mmc command
 *
 * lba:     linear block address
 * format:  0 = data (anything)
 *          1 = audio
 *          2 = data (mode 1)
 *          3 = data (mode 2)
 *          4 = data (mode 2 form1)
 *          5 = data (mode 2 form2)
 * blksize: 2048 | 2336 | 2340 | 2352
 */

int
sr_read_cd(int minor, unsigned char *dest, int lba, int format, int blksize)
{
    unsigned char  cmd[12];

#ifdef DEBUG
    printk("sr%d: sr_read_cd lba=%d format=%d blksize=%d\n",
           minor,lba,format,blksize);
#endif

    memset(cmd,0,12);
    cmd[0] = GPCMD_READ_CD; /* READ_CD */
    cmd[1] = (scsi_CDs[minor].device->lun << 5) | ((format & 7) << 2);
    cmd[2] = (unsigned char)(lba >> 24) & 0xff;
    cmd[3] = (unsigned char)(lba >> 16) & 0xff;
    cmd[4] = (unsigned char)(lba >>  8) & 0xff;
    cmd[5] = (unsigned char) lba        & 0xff;
    cmd[8] = 1;
    switch (blksize) {
    case 2336: cmd[9] = 0x58; break;
    case 2340: cmd[9] = 0x78; break;
    case 2352: cmd[9] = 0xf8; break;
    default:   cmd[9] = 0x10; break;
    }
    return sr_do_ioctl(minor, cmd, dest, blksize, 0);
}

/*
 * read sectors with blocksizes other than 2048
 */

int
sr_read_sector(int minor, int lba, int blksize, unsigned char *dest)
{
    unsigned char   cmd[12];    /* the scsi-command */
    int             rc;

    /* we try the READ CD command first... */
    if (scsi_CDs[minor].readcd_known) {
        rc = sr_read_cd(minor, dest, lba, 0, blksize);
        if (-EDRIVE_CANT_DO_THIS != rc)
            return rc;
        scsi_CDs[minor].readcd_known = 0;
        printk("CDROM does'nt support READ CD (0xbe) command\n");
        /* fall & retry the other way */
    }

    /* ... if this fails, we switch the blocksize using MODE SELECT */
    if (blksize != scsi_CDs[minor].sector_size)
        if (0 != (rc = sr_set_blocklength(minor, blksize)))
            return rc;

#ifdef DEBUG
    printk("sr%d: sr_read_sector lba=%d blksize=%d\n",minor,lba,blksize);
#endif
    
    memset(cmd,0,12);
    cmd[0] = GPCMD_READ_10;
    cmd[1] = (scsi_CDs[minor].device->lun << 5);
    cmd[2] = (unsigned char)(lba >> 24) & 0xff;
    cmd[3] = (unsigned char)(lba >> 16) & 0xff;
    cmd[4] = (unsigned char)(lba >>  8) & 0xff;
    cmd[5] = (unsigned char) lba        & 0xff;
    cmd[8] = 1;
    rc = sr_do_ioctl(minor, cmd, dest, blksize, 0);
    
    return rc;
}

/*
 * read a sector in raw mode to check the sector format
 * ret: 1 == mode2 (XA), 0 == mode1, <0 == error 
 */

int
sr_is_xa(int minor)
{
    unsigned char *raw_sector;
    int is_xa;
    unsigned long flags;
    
    if (!xa_test)
        return 0;
   
    spin_lock_irqsave(&io_request_lock, flags);
    raw_sector = (unsigned char *) scsi_malloc(2048+512);
    spin_unlock_irqrestore(&io_request_lock, flags);
    if (!raw_sector) return -ENOMEM;
    if (0 == sr_read_sector(minor,scsi_CDs[minor].ms_offset+16,
                            CD_FRAMESIZE_RAW1,raw_sector)) {
        is_xa = (raw_sector[3] == 0x02) ? 1 : 0;
    } else {
        /* read a raw sector failed for some reason. */
        is_xa = -1;
    }
    spin_lock_irqsave(&io_request_lock, flags);
    scsi_free(raw_sector, 2048+512);
    spin_unlock_irqrestore(&io_request_lock, flags);
#ifdef DEBUG
    printk("sr%d: sr_is_xa: %d\n",minor,is_xa);
#endif
    return is_xa;
}

int sr_dev_ioctl(struct cdrom_device_info *cdi,
                 unsigned int cmd, unsigned long arg)
{
    int target;
    
    target = MINOR(cdi->dev);
    
    switch (cmd) {
    case BLKROSET:
    case BLKROGET:
    case BLKRASET:
    case BLKRAGET:
    case BLKFLSBUF:
    case BLKSSZGET:
	return blk_ioctl(cdi->dev, cmd, arg);

    default:
	return scsi_ioctl(scsi_CDs[target].device,cmd,(void *) arg);
    }
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 4
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -4
 * c-argdecl-indent: 4
 * c-label-offset: -4
 * c-continued-statement-offset: 4
 * c-continued-brace-offset: 0
 * indent-tabs-mode: nil
 * tab-width: 8
 * End:
 */
