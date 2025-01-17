/* $Id: timod.c,v 1.4 1999/09/01 08:07:47 davem Exp $
 * timod.c: timod emulation.
 *
 * Copyright (C) 1998 Patrik Rak (prak3264@ss1000.ms.mff.cuni.cz)
 *
 * Streams & timod emulation based on code
 * Copyright (C) 1995, 1996 Mike Jagdis (jaggy@purplet.demon.co.uk)
 *
 */
 
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/netdevice.h>
#include <linux/poll.h>

#include <asm/uaccess.h>
#include <asm/termios.h>

#include "conv.h"
#include "socksys.h"

extern char *getname32(u32 filename);
#define putname32 putname

extern asmlinkage int sys_ioctl(unsigned int fd, unsigned int cmd, 
	unsigned long arg);
extern asmlinkage int sys32_ioctl(unsigned int fd, unsigned int cmd,
	u32 arg);
asmlinkage int solaris_ioctl(unsigned int fd, unsigned int cmd, u32 arg);

spinlock_t timod_pagelock = SPIN_LOCK_UNLOCKED;
static char * page = NULL ;

#ifndef DEBUG_SOLARIS_KMALLOC

#define mykmalloc kmalloc
#define mykfree kfree

#else

void * mykmalloc(size_t s, int gfp)
{
	static char * page;
	static size_t free = 0;
	void * r;
	s = ((s + 63) & ~63);
	if( s > PAGE_SIZE ) {
		SOLD("too big size, calling real kmalloc");
		return kmalloc(s, gfp);
	}
	if( s > free ) {
		/* we are wasting memory, but we don't care */
		page = (char *)__get_free_page(gfp);
		free = PAGE_SIZE;
	}
	r = page;
	page += s;
	free -= s;
	return r;
}

void mykfree(void *p)
{
}

#endif

#ifndef DEBUG_SOLARIS

#define BUF_SIZE	PAGE_SIZE
#define PUT_MAGIC(a,m)
#define SCHECK_MAGIC(a,m)
#define BUF_OFFSET	0
#define MKCTL_TRAILER	0

#else

#define BUF_SIZE	(PAGE_SIZE-2*sizeof(u64))
#define BUFPAGE_MAGIC	0xBADC0DEDDEADBABEL
#define MKCTL_MAGIC	0xDEADBABEBADC0DEDL
#define PUT_MAGIC(a,m)	do{(*(u64*)(a))=(m);}while(0)
#define SCHECK_MAGIC(a,m)	do{if((*(u64*)(a))!=(m))printk("%s,%u,%s(): magic %08x at %p corrupted!\n",\
				__FILE__,__LINE__,__FUNCTION__,(m),(a));}while(0)
#define BUF_OFFSET	sizeof(u64)
#define MKCTL_TRAILER	sizeof(u64)

#endif

static char *getpage( void )
{
	char *r;
	SOLD("getting page");
	spin_lock(&timod_pagelock);
	if (page) {
		r = page;
		page = NULL;
		spin_unlock(&timod_pagelock);
		SOLD("got cached");
		return r + BUF_OFFSET;
	}
	spin_unlock(&timod_pagelock);
	SOLD("getting new");
	r = (char *)__get_free_page(GFP_KERNEL);
	PUT_MAGIC(r,BUFPAGE_MAGIC);
	PUT_MAGIC(r+PAGE_SIZE-sizeof(u64),BUFPAGE_MAGIC);
	return r + BUF_OFFSET;
}

static void putpage(char *p)
{
	SOLD("putting page");
	p = p - BUF_OFFSET;
	SCHECK_MAGIC(p,BUFPAGE_MAGIC);
	SCHECK_MAGIC(p+PAGE_SIZE-sizeof(u64),BUFPAGE_MAGIC);
	spin_lock(&timod_pagelock);
	if (page) {
		spin_unlock(&timod_pagelock);
		free_page((unsigned long)p);
		SOLD("freed it");
	} else {
		page = p;
		spin_unlock(&timod_pagelock);
		SOLD("cached it");
	}
}

static struct T_primsg *timod_mkctl(int size)
{
	struct T_primsg *it;

	SOLD("creating primsg");
	it = (struct T_primsg *)mykmalloc(size+sizeof(*it)-sizeof(s32)+2*MKCTL_TRAILER, GFP_KERNEL);
	if (it) {
		SOLD("got it");
		it->pri = MSG_HIPRI;
		it->length = size;
		PUT_MAGIC((char*)((u64)(((char *)&it->type)+size+7)&~7),MKCTL_MAGIC);
	}
	return it;
}

static void timod_wake_socket(unsigned int fd)
{
	struct socket *sock;

	SOLD("wakeing socket");
	sock = &current->files->fd[fd]->f_dentry->d_inode->u.socket_i;
	wake_up_interruptible(&sock->wait);
	if (sock->fasync_list && !(sock->flags & SO_WAITDATA))
		kill_fasync(sock->fasync_list, SIGIO, POLL_IN);
	SOLD("done");
}

static void timod_queue(unsigned int fd, struct T_primsg *it)
{
	struct sol_socket_struct *sock;

	SOLD("queuing primsg");
	sock = (struct sol_socket_struct *)current->files->fd[fd]->private_data;
	it->next = sock->pfirst;
	sock->pfirst = it;
	if (!sock->plast)
		sock->plast = it;
	timod_wake_socket(fd);
	SOLD("done");
}

static void timod_queue_end(unsigned int fd, struct T_primsg *it)
{
	struct sol_socket_struct *sock;

	SOLD("queuing primsg at end");
	sock = (struct sol_socket_struct *)current->files->fd[fd]->private_data;
	it->next = NULL;
	if (sock->plast)
		sock->plast->next = it;
	else
		sock->pfirst = it;
	sock->plast = it;
	SOLD("done");
}

static void timod_error(unsigned int fd, int prim, int terr, int uerr)
{
	struct T_primsg *it;
	
	SOLD("making error");
	it = timod_mkctl(sizeof(struct T_error_ack));
	if (it) {
		struct T_error_ack *err = (struct T_error_ack *)&it->type;
		
		SOLD("got it");
		err->PRIM_type = T_ERROR_ACK;
		err->ERROR_prim = prim;
		err->TLI_error = terr;
		err->UNIX_error = uerr; /* FIXME: convert this */
		timod_queue(fd, it);
	}
	SOLD("done");
}

static void timod_ok(unsigned int fd, int prim)
{
	struct T_primsg *it;
	struct T_ok_ack *ok;
	
	SOLD("creating ok ack");
	it = timod_mkctl(sizeof(*ok));
	if (it) {
		SOLD("got it");
		ok = (struct T_ok_ack *)&it->type;
		ok->PRIM_type = T_OK_ACK;
		ok->CORRECT_prim = prim;
		timod_queue(fd, it);
	}
	SOLD("done");
}

static int timod_optmgmt(unsigned int fd, int flag, char *opt_buf, int opt_len, int do_ret)
{
	int error, failed;
	int ret_space, ret_len;
	long args[5];
	char *ret_pos,*ret_buf;
	int (*sys_socketcall)(int, unsigned long *) =
		(int (*)(int, unsigned long *))SYS(socketcall);
	mm_segment_t old_fs = get_fs();

	SOLD("entry");
	SOLDD(("fd %u flg %u buf %p len %u doret %u",fd,flag,opt_buf,opt_len,do_ret));
	if (!do_ret && (!opt_buf || opt_len <= 0))
		return 0;
	SOLD("getting page");
	ret_pos = ret_buf = getpage();
	ret_space = BUF_SIZE;
	ret_len = 0;
	
	error = failed = 0;
	SOLD("looping");
	while(opt_len >= sizeof(struct opthdr)) {
		struct opthdr *opt;
		int orig_opt_len; 
		SOLD("loop start");
		opt = (struct opthdr *)ret_pos; 
		if (ret_space < sizeof(struct opthdr)) {
			failed = TSYSERR;
			break;
		}
		SOLD("getting opthdr");
		if (copy_from_user(opt, opt_buf, sizeof(struct opthdr)) ||
			opt->len > opt_len) {
			failed = TBADOPT;
			break;
		}
		SOLD("got opthdr");
		if (flag == T_NEGOTIATE) {
			char *buf;
			
			SOLD("handling T_NEGOTIATE");
			buf = ret_pos + sizeof(struct opthdr);
			if (ret_space < opt->len + sizeof(struct opthdr) ||
				copy_from_user(buf, opt_buf+sizeof(struct opthdr), opt->len)) {
				failed = TSYSERR;
				break;
			}
			SOLD("got optdata");
			args[0] = fd;
			args[1] = opt->level;
			args[2] = opt->name;
			args[3] = (long)buf;
			args[4] = opt->len;
			SOLD("calling SETSOCKOPT");
			set_fs(KERNEL_DS);
			error = sys_socketcall(SYS_SETSOCKOPT, args);
			set_fs(old_fs);
			if (error) {
				failed = TBADOPT;
				break;
			}
			SOLD("SETSOCKOPT ok");
		}
		orig_opt_len = opt->len;
		opt->len = ret_space - sizeof(struct opthdr);
		if (opt->len < 0) {
			failed = TSYSERR;
			break;
		}
		args[0] = fd;
		args[1] = opt->level;
		args[2] = opt->name;
		args[3] = (long)(ret_pos+sizeof(struct opthdr));
		args[4] = (long)&opt->len;
		SOLD("calling GETSOCKOPT");
		set_fs(KERNEL_DS);
		error = sys_socketcall(SYS_GETSOCKOPT, args);
		set_fs(old_fs);;
		if (error) {
			failed = TBADOPT;
			break;
		}
		SOLD("GETSOCKOPT ok");
		ret_space -= sizeof(struct opthdr) + opt->len;
		ret_len += sizeof(struct opthdr) + opt->len;
		ret_pos += sizeof(struct opthdr) + opt->len;
		opt_len -= sizeof(struct opthdr) + orig_opt_len;
		opt_buf += sizeof(struct opthdr) + orig_opt_len;
		SOLD("loop end");
	}
	SOLD("loop done");
	if (do_ret) {
		SOLD("generating ret msg");
		if (failed)
			timod_error(fd, T_OPTMGMT_REQ, failed, -error);
		else {
			struct T_primsg *it;
			it = timod_mkctl(sizeof(struct T_optmgmt_ack) + ret_len);
			if (it) {
				struct T_optmgmt_ack *ack =
					(struct T_optmgmt_ack *)&it->type;
				SOLD("got primsg");
				ack->PRIM_type = T_OPTMGMT_ACK;
				ack->OPT_length = ret_len;
				ack->OPT_offset = sizeof(struct T_optmgmt_ack);
				ack->MGMT_flags = (failed ? T_FAILURE : flag);
				memcpy(((char*)ack)+sizeof(struct T_optmgmt_ack),
					ret_buf, ret_len);
				timod_queue(fd, it);
			}
		}
	}
	SOLDD(("put_page %p\n", ret_buf));
	putpage(ret_buf);
	SOLD("done");	
	return 0;
}

int timod_putmsg(unsigned int fd, char *ctl_buf, int ctl_len,
			char *data_buf, int data_len, int flags)
{
	int ret, error, terror;
	char *buf;
	struct file *filp;
	struct inode *ino;
	struct sol_socket_struct *sock;
	mm_segment_t old_fs = get_fs();
	long args[6];
	int (*sys_socketcall)(int, unsigned long *) =
		(int (*)(int, unsigned long *))SYS(socketcall);
	int (*sys_sendto)(int, void *, size_t, unsigned, struct sockaddr *, int) =
		(int (*)(int, void *, size_t, unsigned, struct sockaddr *, int))SYS(sendto);
	filp = current->files->fd[fd];
	ino = filp->f_dentry->d_inode;
	sock = (struct sol_socket_struct *)filp->private_data;
	SOLD("entry");
	if (get_user(ret, (int *)A(ctl_buf)))
		return -EFAULT;
	switch (ret) {
	case T_BIND_REQ:
	{
		struct T_bind_req req;
		
		SOLDD(("bind %016lx(%016lx)\n", sock, filp));
		SOLD("T_BIND_REQ");
		if (sock->state != TS_UNBND) {
			timod_error(fd, T_BIND_REQ, TOUTSTATE, 0);
			return 0;
		}
		SOLD("state ok");
		if (copy_from_user(&req, ctl_buf, sizeof(req))) {
			timod_error(fd, T_BIND_REQ, TSYSERR, EFAULT);
			return 0;
		}
		SOLD("got ctl req");
		if (req.ADDR_offset && req.ADDR_length) {
			if (req.ADDR_length > BUF_SIZE) {
				timod_error(fd, T_BIND_REQ, TSYSERR, EFAULT);
				return 0;
			}
			SOLD("req size ok");
			buf = getpage();
			if (copy_from_user(buf, ctl_buf + req.ADDR_offset, req.ADDR_length)) {
				timod_error(fd, T_BIND_REQ, TSYSERR, EFAULT);
				putpage(buf);
				return 0;
			}
			SOLD("got ctl data");
			args[0] = fd;
			args[1] = (long)buf;
			args[2] = req.ADDR_length;
			SOLD("calling BIND");
			set_fs(KERNEL_DS);
			error = sys_socketcall(SYS_BIND, args);
			set_fs(old_fs);
			putpage(buf);
			SOLD("BIND returned");
		} else 
			error = 0;
		if (!error) {
			struct T_primsg *it;
			if (req.CONIND_number) {
	  			args[0] = fd;
  				args[1] = req.CONIND_number;
  				SOLD("calling LISTEN");
  				set_fs(KERNEL_DS);
	  			error = sys_socketcall(SYS_LISTEN, args);
  				set_fs(old_fs);
  				SOLD("LISTEN done");
  			}
			it = timod_mkctl(sizeof(struct T_bind_ack)+sizeof(struct sockaddr));
			if (it) {
				struct T_bind_ack *ack;

				ack = (struct T_bind_ack *)&it->type;
				ack->PRIM_type = T_BIND_ACK;
				ack->ADDR_offset = sizeof(*ack);
				ack->ADDR_length = sizeof(struct sockaddr);
				ack->CONIND_number = req.CONIND_number;
				args[0] = fd;
				args[1] = (long)(ack+sizeof(*ack));
				args[2] = (long)&ack->ADDR_length;
				set_fs(KERNEL_DS);
				sys_socketcall(SYS_GETSOCKNAME,args);
				set_fs(old_fs);
				sock->state = TS_IDLE;
				timod_ok(fd, T_BIND_REQ);
				timod_queue_end(fd, it);
				SOLD("BIND done");
				return 0;
			}
		}
		SOLD("some error");
		switch (error) {
			case -EINVAL:
				terror = TOUTSTATE;
				error = 0;
				break;
			case -EACCES:
				terror = TACCES;
				error = 0;
				break;
			case -EADDRNOTAVAIL:
			case -EADDRINUSE:
				terror = TNOADDR;
				error = 0;
				break;
			default:
				terror = TSYSERR;
				break;
		}
		timod_error(fd, T_BIND_REQ, terror, -error);
		SOLD("BIND done");
		return 0;
	}
	case T_CONN_REQ:
	{
		struct T_conn_req req;
		unsigned short oldflags;
		struct T_primsg *it;
		SOLD("T_CONN_REQ");
		if (sock->state != TS_UNBND && sock->state != TS_IDLE) {
			timod_error(fd, T_CONN_REQ, TOUTSTATE, 0);
			return 0;
		}
		SOLD("state ok");
		if (copy_from_user(&req, ctl_buf, sizeof(req))) {
			timod_error(fd, T_CONN_REQ, TSYSERR, EFAULT);
			return 0;
		}
		SOLD("got ctl req");
		if (ctl_len > BUF_SIZE) {
			timod_error(fd, T_CONN_REQ, TSYSERR, EFAULT);
			return 0;
		}
		SOLD("req size ok");
		buf = getpage();
		if (copy_from_user(buf, ctl_buf, ctl_len)) {
			timod_error(fd, T_CONN_REQ, TSYSERR, EFAULT);
			putpage(buf);
			return 0;
		}
#ifdef DEBUG_SOLARIS		
		{
			char * ptr = buf;
			int len = ctl_len;
			printk("returned data (%d bytes): ",len);
			while( len-- ) {
				if (!(len & 7))
					printk(" ");
				printk("%02x",(unsigned char)*ptr++);
			}
			printk("\n");
		}
#endif
		SOLD("got ctl data");
		args[0] = fd;
		args[1] = (long)buf+req.DEST_offset;
		args[2] = req.DEST_length;
		oldflags = filp->f_flags;
		filp->f_flags &= ~O_NONBLOCK;
		SOLD("calling CONNECT");
		set_fs(KERNEL_DS);
		error = sys_socketcall(SYS_CONNECT, args);
		set_fs(old_fs);
		filp->f_flags = oldflags;
		SOLD("CONNECT done");
		if (!error) {
			struct T_conn_con *con;
			SOLD("no error");
			it = timod_mkctl(ctl_len);
			if (!it) {
				putpage(buf);
				return -ENOMEM;
			}
			con = (struct T_conn_con *)&it->type;
#ifdef DEBUG_SOLARIS			
			{
				char * ptr = buf;
				int len = ctl_len;
				printk("returned data (%d bytes): ",len);
				while( len-- ) {
					if (!(len & 7))
						printk(" ");
					printk("%02x",(unsigned char)*ptr++);
				}
				printk("\n");
			}
#endif
			memcpy(con, buf, ctl_len);
			SOLD("copied ctl_buf");
			con->PRIM_type = T_CONN_CON;
			sock->state = TS_DATA_XFER;
		} else {
			struct T_discon_ind *dis;
			SOLD("some error");
			it = timod_mkctl(sizeof(*dis));
			if (!it) {
				putpage(buf);
				return -ENOMEM;
			}
			SOLD("got primsg");
			dis = (struct T_discon_ind *)&it->type;
			dis->PRIM_type = T_DISCON_IND;
			dis->DISCON_reason = -error;	/* FIXME: convert this as in iABI_errors() */
			dis->SEQ_number = 0;
		}
		putpage(buf);
		timod_ok(fd, T_CONN_REQ);
		it->pri = 0;
		timod_queue_end(fd, it);
		SOLD("CONNECT done");
		return 0;
	}
	case T_OPTMGMT_REQ:
	{
		struct T_optmgmt_req req;
		SOLD("OPTMGMT_REQ");
		if (copy_from_user(&req, ctl_buf, sizeof(req)))
			return -EFAULT;
		SOLD("got req");
		return timod_optmgmt(fd, req.MGMT_flags,
				req.OPT_offset > 0 ? ctl_buf + req.OPT_offset : NULL,
				req.OPT_length, 1);
	}
	case T_UNITDATA_REQ:
	{
		struct T_unitdata_req req;
		
		int err;
		SOLD("T_UNITDATA_REQ");
		if (sock->state != TS_IDLE && sock->state != TS_DATA_XFER) {
			timod_error(fd, T_CONN_REQ, TOUTSTATE, 0);
			return 0;
		}
		SOLD("state ok");
		if (copy_from_user(&req, ctl_buf, sizeof(req))) {
			timod_error(fd, T_CONN_REQ, TSYSERR, EFAULT);
			return 0;
		}
		SOLD("got ctl req");
#ifdef DEBUG_SOLARIS		
		{
			char * ptr = ctl_buf+req.DEST_offset;
			int len = req.DEST_length;
			printk("socket address (%d bytes): ",len);
			while( len-- ) {
				char c;
				if (get_user(c,ptr))
					printk("??");
				else
					printk("%02x",(unsigned char)c);
				ptr++;
			}
			printk("\n");
		}
#endif		
		err = sys_sendto(fd, data_buf, data_len, 0, req.DEST_length > 0 ? (struct sockaddr*)(ctl_buf+req.DEST_offset) : NULL, req.DEST_length);
		if (err == data_len)
			return 0;
		if(err >= 0) {
			printk("timod: sendto failed to send all the data\n");
			return 0;
		}
		timod_error(fd, T_CONN_REQ, TSYSERR, -err);
		return 0;
	}
	default:
		printk("timod_putmsg: unsuported command %u.\n", ret);
		break;
	}
	return -EINVAL;
}

/* copied directly from fs/select.c */

static void free_wait(poll_table * p)
{
	struct poll_table_entry * entry = p->entry + p->nr;

	SOLD("entry");
	while (p->nr > 0) {
		p->nr--;
		entry--;
		remove_wait_queue(entry->wait_address,&entry->wait);
	}
	SOLD("done");
}


int timod_getmsg(unsigned int fd, char *ctl_buf, int ctl_maxlen, s32 *ctl_len,
			char *data_buf, int data_maxlen, s32 *data_len, int *flags_p)
{
	int error;
	int oldflags;
	struct file *filp;
	struct inode *ino;
	struct sol_socket_struct *sock;
	struct T_unitdata_ind udi;
	mm_segment_t old_fs = get_fs();
	long args[6];
	char *tmpbuf;
	int tmplen;
	int (*sys_socketcall)(int, unsigned long *) =
		(int (*)(int, unsigned long *))SYS(socketcall);
	int (*sys_recvfrom)(int, void *, size_t, unsigned, struct sockaddr *, int *);
	
	SOLD("entry");
	SOLDD(("%u %p %d %p %p %d %p %d\n", fd, ctl_buf, ctl_maxlen, ctl_len, data_buf, data_maxlen, data_len, *flags_p));
	filp = current->files->fd[fd];
	ino = filp->f_dentry->d_inode;
	sock = (struct sol_socket_struct *)filp->private_data;
	SOLDD(("%p %p\n", sock->pfirst, sock->pfirst ? sock->pfirst->next : NULL));
	if ( ctl_maxlen > 0 && !sock->pfirst && ino->u.socket_i.type == SOCK_STREAM
		&& sock->state == TS_IDLE) {
		SOLD("calling LISTEN");
		args[0] = fd;
		args[1] = -1;
		set_fs(KERNEL_DS);
		sys_socketcall(SYS_LISTEN, args);
		set_fs(old_fs);
		SOLD("LISTEN done");
	}
	if (!(filp->f_flags & O_NONBLOCK)) {
		poll_table wait_table, *wait;
		struct poll_table_entry *entry;
		SOLD("getting poll_table");
		entry = (struct poll_table_entry *)__get_free_page(GFP_KERNEL);
		if (!entry)
			return -ENOMEM;
		SOLD("got one");
		wait_table.nr = 0;
		wait_table.entry = entry;
		wait = &wait_table;
		for(;;) {
			SOLD("loop");
			set_current_state(TASK_INTERRUPTIBLE);
			/* ! ( l<0 || ( l>=0 && ( ! pfirst || (flags == HIPRI && pri != HIPRI) ) ) ) */ 
			/* ( ! l<0 && ! ( l>=0 && ( ! pfirst || (flags == HIPRI && pri != HIPRI) ) ) ) */ 
			/* ( l>=0 && ( ! l>=0 || ! ( ! pfirst || (flags == HIPRI && pri != HIPRI) ) ) ) */ 
			/* ( l>=0 && ( l<0 || ( pfirst && ! (flags == HIPRI && pri != HIPRI) ) ) ) */ 
			/* ( l>=0 && ( l<0 || ( pfirst && (flags != HIPRI || pri == HIPRI) ) ) ) */ 
			/* ( l>=0 && ( pfirst && (flags != HIPRI || pri == HIPRI) ) ) */ 
			if (ctl_maxlen >= 0 && sock->pfirst && (*flags_p != MSG_HIPRI || sock->pfirst->pri == MSG_HIPRI))
				break;
			SOLD("cond 1 passed");
			if (
			#if 1
				*flags_p != MSG_HIPRI &&
			#endif
				((filp->f_op->poll(filp, wait) & POLLIN) ||
				(filp->f_op->poll(filp, NULL) & POLLIN) ||
				signal_pending(current))
			) {
				break;
			}
			if( *flags_p == MSG_HIPRI ) {
				SOLD("avoiding lockup");
				break ;
			}
			SOLD("scheduling");
			schedule();
		}
		SOLD("loop done");
		current->state = TASK_RUNNING;
		free_wait(&wait_table);
		free_page((unsigned long)entry);
		if (signal_pending(current)) {
			SOLD("signal pending");
			return -EINTR;
		}
	}
	if (ctl_maxlen >= 0 && sock->pfirst) {
		struct T_primsg *it = sock->pfirst;
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
		int l = min(ctl_maxlen, it->length);
		SCHECK_MAGIC((char*)((u64)(((char *)&it->type)+sock->offset+it->length+7)&~7),MKCTL_MAGIC);
		SOLD("purting ctl data");
		if(copy_to_user(ctl_buf,
			(char*)&it->type + sock->offset, l))
			return -EFAULT;
		SOLD("pur it");
		if(put_user(l, ctl_len))
			return -EFAULT;
		SOLD("set ctl_len");
		*flags_p = it->pri;
		it->length -= l;
		if (it->length) {
			SOLD("more ctl");
			sock->offset += l;
			return MORECTL;
		} else {
			SOLD("removing message");
			sock->pfirst = it->next;
			if (!sock->pfirst)
				sock->plast = NULL;
			SOLDD(("getmsg kfree %016lx->%016lx\n", it, sock->pfirst));
			mykfree(it);
			sock->offset = 0;
			SOLD("ctl done");
			return 0;
		}
	}
	*flags_p = 0;
	if (ctl_maxlen >= 0) {
		SOLD("ACCEPT perhaps?");
		if (ino->u.socket_i.type == SOCK_STREAM && sock->state == TS_IDLE) {
			struct T_conn_ind ind;
			char *buf = getpage();
			int len = BUF_SIZE;

			SOLD("trying ACCEPT");
			if (put_user(ctl_maxlen - sizeof(ind), ctl_len))
				return -EFAULT;
			args[0] = fd;
			args[1] = (long)buf;
			args[2] = (long)&len;
			oldflags = filp->f_flags;
			filp->f_flags |= O_NONBLOCK;
			SOLD("calling ACCEPT");
			set_fs(KERNEL_DS);
			error = sys_socketcall(SYS_ACCEPT, args);
			set_fs(old_fs);
			filp->f_flags = oldflags;
			if (error < 0) {
				SOLD("some error");
				putpage(buf);
				return error;
			}
			if (error) {
				SOLD("connect");
				putpage(buf);
				if (sizeof(ind) > ctl_maxlen) {
					SOLD("generating CONN_IND");
					ind.PRIM_type = T_CONN_IND;
					ind.SRC_length = len;
					ind.SRC_offset = sizeof(ind);
					ind.OPT_length = ind.OPT_offset = 0;
					ind.SEQ_number = error;
					if(copy_to_user(ctl_buf, &ind, sizeof(ind))||
					   put_user(sizeof(ind)+ind.SRC_length,ctl_len))
						return -EFAULT;
					SOLD("CONN_IND created");
				}
				if (data_maxlen >= 0)
					put_user(0, data_len);
				SOLD("CONN_IND done");
				return 0;
			}
			if (len>ctl_maxlen) {
				SOLD("data don't fit");
				putpage(buf);
				return -EFAULT;		/* XXX - is this ok ? */
			}
			if(copy_to_user(ctl_buf,buf,len) || put_user(len,ctl_len)){
				SOLD("can't copy data");
				putpage(buf);
				return -EFAULT;
			}
			SOLD("ACCEPT done");
			putpage(buf);
		}
	}
	SOLD("checking data req");
	if (data_maxlen <= 0) {
		if (data_maxlen == 0)
			put_user(0, data_len);
		if (ctl_maxlen >= 0)
			put_user(0, ctl_len);
		return -EAGAIN;
	}
	SOLD("wants data");
	if (ctl_maxlen > sizeof(udi) && sock->state == TS_IDLE) {
		SOLD("udi fits");
		tmpbuf = ctl_buf + sizeof(udi);
		tmplen = ctl_maxlen - sizeof(udi);
	} else {
		SOLD("udi does not fit");
		tmpbuf = NULL;
		tmplen = 0;
	}
	if (put_user(tmplen, ctl_len))
		return -EFAULT;
	SOLD("set ctl_len");
	oldflags = filp->f_flags;
	filp->f_flags |= O_NONBLOCK;
	SOLD("calling recvfrom");
	sys_recvfrom = (int (*)(int, void *, size_t, unsigned, struct sockaddr *, int *))SYS(recvfrom);
	error = sys_recvfrom(fd, data_buf, min(0,data_maxlen), 0, (struct sockaddr*)tmpbuf, ctl_len);
	filp->f_flags = oldflags;
	if (error < 0)
		return error;
	SOLD("error >= 0" ) ;
	if (error && ctl_maxlen > sizeof(udi) && sock->state == TS_IDLE) {
		SOLD("generating udi");
		udi.PRIM_type = T_UNITDATA_IND;
		get_user(udi.SRC_length, ctl_len);
		udi.SRC_offset = sizeof(udi);
		udi.OPT_length = udi.OPT_offset = 0;
		copy_to_user(ctl_buf, &udi, sizeof(udi));
		put_user(sizeof(udi)+udi.SRC_length, ctl_len);
		SOLD("udi done");
	} else
		put_user(0, ctl_len);
	put_user(error, data_len);
	SOLD("done");
	return 0;
}

asmlinkage int solaris_getmsg(unsigned int fd, u32 arg1, u32 arg2, u32 arg3)
{
	struct file *filp;
	struct inode *ino;
	struct strbuf *ctlptr, *datptr;
	struct strbuf ctl, dat;
	int *flgptr;
	int flags;
	int error = -EBADF;

	SOLD("entry");
	lock_kernel();
	if(fd >= NR_OPEN) goto out;

	filp = current->files->fd[fd];
	if(!filp) goto out;

	ino = filp->f_dentry->d_inode;
	if (!ino) goto out;

	if (!ino->i_sock)
		goto out;

	ctlptr = (struct strbuf *)A(arg1);
	datptr = (struct strbuf *)A(arg2);
	flgptr = (int *)A(arg3);

	error = -EFAULT;

	if (ctlptr) {
		if (copy_from_user(&ctl,ctlptr,sizeof(struct strbuf)) || 
		    put_user(-1,&ctlptr->len))
			goto out;
	} else
		ctl.maxlen = -1;

	if (datptr) {
		if (copy_from_user(&dat,datptr,sizeof(struct strbuf)) || 
		    put_user(-1,&datptr->len))
			goto out;
	} else
		dat.maxlen = -1;

	if (get_user(flags,flgptr))
		goto out;

	switch (flags) {
	case 0:
	case MSG_HIPRI:
	case MSG_ANY:
	case MSG_BAND:
		break;
	default:
		error = -EINVAL;
		goto out;
	}

	error = timod_getmsg(fd,(char*)A(ctl.buf),ctl.maxlen,&ctlptr->len,
				(char*)A(dat.buf),dat.maxlen,&datptr->len,&flags);

	if (!error && put_user(flags,flgptr))
		error = -EFAULT;
out:
	unlock_kernel();
	SOLD("done");
	return error;
}

asmlinkage int solaris_putmsg(unsigned int fd, u32 arg1, u32 arg2, u32 arg3)
{
	struct file *filp;
	struct inode *ino;
	struct strbuf *ctlptr, *datptr;
	struct strbuf ctl, dat;
	int flags = (int) arg3;
	int error = -EBADF;

	SOLD("entry");
	lock_kernel();
	if(fd >= NR_OPEN) goto out;

	filp = current->files->fd[fd];
	if(!filp) goto out;

	ino = filp->f_dentry->d_inode;
	if (!ino) goto out;

	if (!ino->i_sock &&
		(MAJOR(ino->i_rdev) != 30 || MINOR(ino->i_rdev) != 1))
		goto out;

	ctlptr = (struct strbuf *)A(arg1);
	datptr = (struct strbuf *)A(arg2);

	error = -EFAULT;

	if (ctlptr) {
		if (copy_from_user(&ctl,ctlptr,sizeof(ctl)))
			goto out;
		if (ctl.len < 0 && flags) {
			error = -EINVAL;
			goto out;
		}
	} else {
		ctl.len = 0;
		ctl.buf = 0;
	}

	if (datptr) {
		if (copy_from_user(&dat,datptr,sizeof(dat)))
			goto out;
	} else {
		dat.len = 0;
		dat.buf = 0;
	}

	error = timod_putmsg(fd,(char*)A(ctl.buf),ctl.len,
				(char*)A(dat.buf),dat.len,flags);
out:
	unlock_kernel();
	SOLD("done");
	return error;
}
