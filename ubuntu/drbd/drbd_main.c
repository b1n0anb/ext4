/*
-*- Linux-c -*-
   drbd.c
   Kernel module for 2.6.x Kernels

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/version.h>

#include <asm/uaccess.h>
#include <asm/types.h>
#include <net/sock.h>
#include <linux/ctype.h>
#include <linux/smp_lock.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/drbd_config.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/byteorder/swabb.h>

#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/vmalloc.h>

#include <linux/drbd.h>
#include <linux/drbd_limits.h>
#include "drbd_int.h"
#include "drbd_req.h" /* only for _req_mod in tl_release and tl_clear */

struct after_state_chg_work {
	struct drbd_work w;
	union drbd_state_t os;
	union drbd_state_t ns;
	enum chg_state_flags flags;
	struct completion *done;
};

int drbdd_init(struct Drbd_thread *);
int drbd_worker(struct Drbd_thread *);
int drbd_asender(struct Drbd_thread *);

int drbd_init(void);
#ifdef BD_OPS_USE_FMODE
static int drbd_open(struct block_device *bdev, fmode_t mode);
static int drbd_release(struct gendisk *gd, fmode_t mode);
#else
static int drbd_open(struct inode *inode, struct file *file);
static int drbd_release(struct inode *inode, struct file *file);
#endif
STATIC int w_after_state_ch(struct drbd_conf *mdev, struct drbd_work *w, int unused);
STATIC void after_state_ch(struct drbd_conf *mdev, union drbd_state_t os,
			   union drbd_state_t ns, enum chg_state_flags flags);
STATIC int w_md_sync(struct drbd_conf *mdev, struct drbd_work *w, int unused);
STATIC void md_sync_timer_fn(unsigned long data);

MODULE_AUTHOR("Philipp Reisner <phil@linbit.com>, "
	      "Lars Ellenberg <lars@linbit.com>");
MODULE_DESCRIPTION("drbd - Distributed Replicated Block Device v" REL_VERSION);
MODULE_LICENSE("GPL");
MODULE_PARM_DESC(minor_count, "Maximum number of drbd devices (1-255)");
MODULE_ALIAS_BLOCKDEV_MAJOR(DRBD_MAJOR);

#include <linux/moduleparam.h>
/* allow_open_on_secondary */
MODULE_PARM_DESC(allow_oos, "DONT USE!");
/* thanks to these macros, if compiled into the kernel (not-module),
 * this becomes the boot parameter drbd.minor_count */
module_param(minor_count, uint, 0444);
module_param(allow_oos, bool, 0);
module_param(cn_idx, uint, 0444);

#ifdef DRBD_ENABLE_FAULTS
int enable_faults;
int fault_rate;
static int fault_count;
int fault_devs;
/* bitmap of enabled faults */
module_param(enable_faults, int, 0664);
/* fault rate % value - applies to all enabled faults */
module_param(fault_rate, int, 0664);
/* count of faults inserted */
module_param(fault_count, int, 0664);
/* bitmap of devices to insert faults on */
module_param(fault_devs, int, 0644);
#endif

/* module parameter, defined */
unsigned int minor_count = 32;
int allow_oos;
unsigned int cn_idx = CN_IDX_DRBD;

#ifdef ENABLE_DYNAMIC_TRACE
int trace_type;		/* Bitmap of trace types to enable */
int trace_level;	/* Current trace level */
int trace_devs;		/* Bitmap of devices to trace */
int proc_details;       /* Detail level in proc drbd*/

module_param(trace_level, int, 0644);
module_param(trace_type, int, 0644);
module_param(trace_devs, int, 0644);
module_param(proc_details, int, 0644);
#endif

/* Module parameter for setting the user mode helper program
 * to run. Default is /sbin/drbdadm */
char usermode_helper[80] = "/sbin/drbdadm";

module_param_string(usermode_helper, usermode_helper, sizeof(usermode_helper), 0644);

/* in 2.6.x, our device mapping and config info contains our virtual gendisks
 * as member "struct gendisk *vdisk;"
 */
struct drbd_conf **minor_table;

struct kmem_cache *drbd_request_cache;
struct kmem_cache *drbd_ee_cache;
mempool_t *drbd_request_mempool;
mempool_t *drbd_ee_mempool;

/* I do not use a standard mempool, because:
   1) I want to hand out the preallocated objects first.
   2) I want to be able to interrupt sleeping allocation with a signal.
   Note: This is a single linked list, the next pointer is the private
	 member of struct page.
 */
struct page *drbd_pp_pool;
spinlock_t   drbd_pp_lock;
int          drbd_pp_vacant;
wait_queue_head_t drbd_pp_wait;

STATIC struct block_device_operations drbd_ops = {
	.owner =   THIS_MODULE,
	.open =    drbd_open,
	.release = drbd_release,
};

#define ARRY_SIZE(A) (sizeof(A)/sizeof(A[0]))

#ifdef __CHECKER__
/* When checking with sparse, and this is an inline function, sparse will
   give tons of false positives. When this is a real functions sparse works.
 */
int _inc_local_if_state(struct drbd_conf *mdev, enum drbd_disk_state mins)
{
	int io_allowed;

	atomic_inc(&mdev->local_cnt);
	io_allowed = (mdev->state.disk >= mins);
	if (!io_allowed) {
		if (atomic_dec_and_test(&mdev->local_cnt))
			wake_up(&mdev->misc_wait);
	}
	return io_allowed;
}

#endif

/************************* The transfer log start */
STATIC int tl_init(struct drbd_conf *mdev)
{
	struct drbd_barrier *b;

	b = kmalloc(sizeof(struct drbd_barrier), GFP_KERNEL);
	if (!b)
		return 0;
	INIT_LIST_HEAD(&b->requests);
	INIT_LIST_HEAD(&b->w.list);
	b->next = NULL;
	b->br_number = 4711;
	b->n_req = 0;
	b->w.cb = NULL; /* if this is != NULL, we need to dec_ap_pending in tl_clear */

	mdev->oldest_barrier = b;
	mdev->newest_barrier = b;
	INIT_LIST_HEAD(&mdev->out_of_sequence_requests);

	mdev->tl_hash = NULL;
	mdev->tl_hash_s = 0;

	return 1;
}

STATIC void tl_cleanup(struct drbd_conf *mdev)
{
	D_ASSERT(mdev->oldest_barrier == mdev->newest_barrier);
	D_ASSERT(list_empty(&mdev->out_of_sequence_requests));
	kfree(mdev->oldest_barrier);
	kfree(mdev->unused_spare_barrier);
	kfree(mdev->tl_hash);
	mdev->tl_hash_s = 0;
}

/**
 * _tl_add_barrier: Adds a barrier to the TL.
 */
void _tl_add_barrier(struct drbd_conf *mdev, struct drbd_barrier *new)
{
	struct drbd_barrier *newest_before;

	INIT_LIST_HEAD(&new->requests);
	INIT_LIST_HEAD(&new->w.list);
	new->w.cb = NULL; /* if this is != NULL, we need to dec_ap_pending in tl_clear */
	new->next = NULL;
	new->n_req = 0;

	newest_before = mdev->newest_barrier;
	/* never send a barrier number == 0, because that is special-cased
	 * when using TCQ for our write ordering code */
	new->br_number = (newest_before->br_number+1) ?: 1;
	if (mdev->newest_barrier != new) {
		mdev->newest_barrier->next = new;
		mdev->newest_barrier = new;
	}
}

/* when we receive a barrier ack */
void tl_release(struct drbd_conf *mdev, unsigned int barrier_nr,
		       unsigned int set_size)
{
	struct drbd_barrier *b, *nob; /* next old barrier */
	struct list_head *le, *tle;
	struct drbd_request *r;

	spin_lock_irq(&mdev->req_lock);

	b = mdev->oldest_barrier;

	/* first some paranoia code */
	if (b == NULL) {
		ERR("BAD! BarrierAck #%u received, but no epoch in tl!?\n",
			barrier_nr);
		goto bail;
	}
	if (b->br_number != barrier_nr) {
		ERR("BAD! BarrierAck #%u received, expected #%u!\n",
			barrier_nr, b->br_number);
		goto bail;
	}
	if (b->n_req != set_size) {
		ERR("BAD! BarrierAck #%u received with n_req=%u, expected n_req=%u!\n",
			barrier_nr, set_size, b->n_req);
		goto bail;
	}

	/* Clean up list of requests processed during current epoch */
	list_for_each_safe(le, tle, &b->requests) {
		r = list_entry(le, struct drbd_request, tl_requests);
		_req_mod(r, barrier_acked, 0);
	}
	/* There could be requests on the list waiting for completion
	   of the write to the local disk. To avoid corruptions of
	   slab's data structures we have to remove the lists head.

	   Also there could have been a barrier ack out of sequence, overtaking
	   the write acks - which would be a but and violating write ordering.
	   To not deadlock in case we lose connection while such requests are
	   still pending, we need some way to find them for the
	   _req_mode(connection_lost_while_pending).

	   These have been list_move'd to the out_of_sequence_requests list in
	   _req_mod(, barrier_acked,) above.
	   */
	list_del_init(&b->requests);

	nob = b->next;
	if (test_and_clear_bit(CREATE_BARRIER, &mdev->flags)) {
		_tl_add_barrier(mdev, b);
		if (nob)
			mdev->oldest_barrier = nob;
		/* if nob == NULL b was the only barrier, and becomes the new
		   barrer. Threfore mdev->oldest_barrier points already to b */
	} else {
		D_ASSERT(nob != NULL);
		mdev->oldest_barrier = nob;
		kfree(b);
	}

	spin_unlock_irq(&mdev->req_lock);
	dec_ap_pending(mdev);

	return;

bail:
	spin_unlock_irq(&mdev->req_lock);
	drbd_force_state(mdev, NS(conn, ProtocolError));
}


/* called by drbd_disconnect (exiting receiver thread)
 * or from some after_state_ch */
void tl_clear(struct drbd_conf *mdev)
{
	struct drbd_barrier *b, *tmp;
	struct list_head *le, *tle;
	struct drbd_request *r;
	int new_initial_bnr = net_random();

	spin_lock_irq(&mdev->req_lock);

	b = mdev->oldest_barrier;
	while (b) {
		list_for_each_safe(le, tle, &b->requests) {
			r = list_entry(le, struct drbd_request, tl_requests);
			_req_mod(r, connection_lost_while_pending, 0);
		}
		tmp = b->next;

		/* there could still be requests on that ring list,
		 * in case local io is still pending */
		list_del(&b->requests);

		/* dec_ap_pending corresponding to queue_barrier.
		 * the newest barrier may not have been queued yet,
		 * in which case w.cb is still NULL. */
		if (b->w.cb != NULL)
			dec_ap_pending(mdev);

		if (b == mdev->newest_barrier) {
			/* recycle, but reinit! */
			D_ASSERT(tmp == NULL);
			INIT_LIST_HEAD(&b->requests);
			INIT_LIST_HEAD(&b->w.list);
			b->w.cb = NULL;
			b->br_number = new_initial_bnr;
			b->n_req = 0;

			mdev->oldest_barrier = b;
			break;
		}
		kfree(b);
		b = tmp;
	}

	/* we expect this list to be empty. */
	D_ASSERT(list_empty(&mdev->out_of_sequence_requests));

	/* but just in case, clean it up anyways! */
	list_for_each_safe(le, tle, &mdev->out_of_sequence_requests) {
		r = list_entry(le, struct drbd_request, tl_requests);
		_req_mod(r, connection_lost_while_pending, 0);
	}

	/* ensure bit indicating barrier is required is clear */
	clear_bit(CREATE_BARRIER, &mdev->flags);

	spin_unlock_irq(&mdev->req_lock);
}

/**
 * drbd_io_error: Handles the on_io_error setting, should be called in the
 * unlikely(!drbd_bio_uptodate(e->bio)) case from kernel thread context.
 * See also drbd_chk_io_error
 *
 * NOTE: we set ourselves FAILED here if on_io_error is Detach or Panic OR
 *	 if the forcedetach flag is set. This flag is set when failures
 *	 occur writing the meta data portion of the disk as they are
 *	 not recoverable. We also try to write the "need full sync bit" here
 *	 anyways.  This is to make sure that you get a resynchronisation of
 *	 the full device the next time you connect.
 */
int drbd_io_error(struct drbd_conf *mdev, int forcedetach)
{
	enum io_error_handler eh;
	unsigned long flags;
	int send;
	int ok = 1;

	eh = PassOn;
	if (inc_local_if_state(mdev, Failed)) {
		eh = mdev->bc->dc.on_io_error;
		dec_local(mdev);
	}

	if (!forcedetach && eh == PassOn)
		return 1;

	spin_lock_irqsave(&mdev->req_lock, flags);
	send = (mdev->state.disk == Failed);
	if (send)
		_drbd_set_state(_NS(mdev, disk, Diskless), ChgStateHard, NULL);
	spin_unlock_irqrestore(&mdev->req_lock, flags);

	if (!send)
		return ok;

	if (mdev->state.conn >= Connected) {
		ok = drbd_send_state(mdev);
		if (ok)
			drbd_WARN("Notified peer that my disk is broken.\n");
		else
			ERR("Sending state in drbd_io_error() failed\n");
	}

	/* Make sure we try to flush meta-data to disk - we come
	 * in here because of a local disk error so it might fail
	 * but we still need to try -- both because the error might
	 * be in the data portion of the disk and because we need
	 * to ensure the md-sync-timer is stopped if running. */
	drbd_md_sync(mdev);

	/* Releasing the backing device is done in after_state_ch() */

	if (eh == CallIOEHelper)
		drbd_khelper(mdev, "local-io-error");

	return ok;
}

#if DRBD_DEBUG_STATE_CHANGES
static void trace_st(struct drbd_conf *mdev, const unsigned long long seq,
		const char *func, unsigned int line,
		const char *name, union drbd_state_t s);
#endif

/**
 * cl_wide_st_chg:
 * Returns TRUE if this state change should be preformed as a cluster wide
 * transaction. Of course it returns 0 as soon as the connection is lost.
 */
STATIC int cl_wide_st_chg(struct drbd_conf *mdev,
			  union drbd_state_t os, union drbd_state_t ns)
{
	return (os.conn >= Connected && ns.conn >= Connected &&
		 ((os.role != Primary && ns.role == Primary) ||
		  (os.conn != StartingSyncT && ns.conn == StartingSyncT) ||
		  (os.conn != StartingSyncS && ns.conn == StartingSyncS) ||
		  (os.disk != Diskless && ns.disk == Diskless))) ||
		(os.conn >= Connected && ns.conn == Disconnecting) ||
		(os.conn == Connected && ns.conn == VerifyS);
}

int drbd_change_state(struct drbd_conf *mdev, enum chg_state_flags f,
		      union drbd_state_t mask, union drbd_state_t val)
{
#if DRBD_DEBUG_STATE_CHANGES
	static unsigned long long sseq = 0xf0000000LLU;
	unsigned long seq;
	unsigned int line = val.line;
	const char *func = val.func;
#endif

	unsigned long flags;
	union drbd_state_t os, ns;
	int rv;

	spin_lock_irqsave(&mdev->req_lock, flags);
	os = mdev->state;
	ns.i = (os.i & ~mask.i) | val.i;
#if DRBD_DEBUG_STATE_CHANGES
	seq = ++sseq;
	trace_st(mdev, seq, func, line, "!os", os);
	trace_st(mdev, seq, func, line, "!ns", ns);
	ns.func = NULL;
#endif
	rv = _drbd_set_state(mdev, ns, f, NULL);
	ns = mdev->state;
#if DRBD_DEBUG_STATE_CHANGES
	trace_st(mdev, seq, func, line, "=ns", ns);
#endif
	spin_unlock_irqrestore(&mdev->req_lock, flags);

	return rv;
}

void drbd_force_state(struct drbd_conf *mdev,
	union drbd_state_t mask, union drbd_state_t val)
{
	drbd_change_state(mdev, ChgStateHard, mask, val);
}

int is_valid_state(struct drbd_conf *mdev, union drbd_state_t ns);
int is_valid_state_transition(struct drbd_conf *,
	union drbd_state_t, union drbd_state_t);
int drbd_send_state_req(struct drbd_conf *,
	union drbd_state_t, union drbd_state_t);

STATIC enum set_st_err _req_st_cond(struct drbd_conf *mdev,
				    union drbd_state_t mask, union drbd_state_t val)
{
	union drbd_state_t os, ns;
	unsigned long flags;
	int rv;

	if (test_and_clear_bit(CL_ST_CHG_SUCCESS, &mdev->flags))
		return SS_CW_Success;

	if (test_and_clear_bit(CL_ST_CHG_FAIL, &mdev->flags))
		return SS_CW_FailedByPeer;

	rv = 0;
	spin_lock_irqsave(&mdev->req_lock, flags);
	os = mdev->state;
	ns.i = (os.i & ~mask.i) | val.i;
	if (!cl_wide_st_chg(mdev, os, ns))
		rv = SS_CW_NoNeed;
	if (!rv) {
		rv = is_valid_state(mdev, ns);
		if (rv == SS_Success) {
			rv = is_valid_state_transition(mdev, ns, os);
			if (rv == SS_Success)
				rv = 0; /* cont waiting, otherwise fail. */
		}
	}
	spin_unlock_irqrestore(&mdev->req_lock, flags);

	return rv;
}

/**
 * _drbd_request_state:
 * This function is the most gracefull way to change state. For some state
 * transition this function even does a cluster wide transaction.
 * It has a cousin named drbd_request_state(), which is always verbose.
 */
STATIC int drbd_req_state(struct drbd_conf *mdev,
			  union drbd_state_t mask, union drbd_state_t val,
			  enum chg_state_flags f)
{
#if DRBD_DEBUG_STATE_CHANGES
	static unsigned long long sseq = 0;
	unsigned long seq;
	unsigned int line = val.line;
	const char *func = val.func;
#endif

	struct completion done;
	unsigned long flags;
	union drbd_state_t os, ns;
	int rv;

	init_completion(&done);

	if (f & ChgSerialize)
		mutex_lock(&mdev->state_mutex);

	spin_lock_irqsave(&mdev->req_lock, flags);
	os = mdev->state;
	ns.i = (os.i & ~mask.i) | val.i;

#if DRBD_DEBUG_STATE_CHANGES
	seq = ++sseq;
	trace_st(mdev, seq, func, line, "?os", os);
	trace_st(mdev, seq, func, line, "?ns", ns);
	ns.func = NULL;
#endif

	if (cl_wide_st_chg(mdev, os, ns)) {
		rv = is_valid_state(mdev, ns);
		if (rv == SS_Success)
			rv = is_valid_state_transition(mdev, ns, os);
		spin_unlock_irqrestore(&mdev->req_lock, flags);

		if (rv < SS_Success) {
			if (f & ChgStateVerbose)
				print_st_err(mdev, os, ns, rv);
			goto abort;
		}

		drbd_state_lock(mdev);
		if (!drbd_send_state_req(mdev, mask, val)) {
			drbd_state_unlock(mdev);
			rv = SS_CW_FailedByPeer;
			if (f & ChgStateVerbose)
				print_st_err(mdev, os, ns, rv);
			goto abort;
		}

		wait_event(mdev->state_wait,
			(rv = _req_st_cond(mdev, mask, val)));

		if (rv < SS_Success) {
			/* nearly dead code. */
			drbd_state_unlock(mdev);
			if (f & ChgStateVerbose)
				print_st_err(mdev, os, ns, rv);
			goto abort;
		}
		spin_lock_irqsave(&mdev->req_lock, flags);
		os = mdev->state;
		ns.i = (os.i & ~mask.i) | val.i;
		rv = _drbd_set_state(mdev, ns, f, &done);
		drbd_state_unlock(mdev);
	} else {
		rv = _drbd_set_state(mdev, ns, f, &done);
	}

	spin_unlock_irqrestore(&mdev->req_lock, flags);

	if (f & ChgWaitComplete && rv == SS_Success) {
		D_ASSERT(current != mdev->worker.task);
		wait_for_completion(&done);
	}

abort:
#if DRBD_DEBUG_STATE_CHANGES
	trace_st(mdev, seq, func, line, ":os", os);
	trace_st(mdev, seq, func, line, ":ns", ns);
#endif

	if (f & ChgSerialize)
		mutex_unlock(&mdev->state_mutex);

	return rv;
}

/**
 * _drbd_request_state:
 * This function is the most gracefull way to change state. For some state
 * transition this function even does a cluster wide transaction.
 * It has a cousin named drbd_request_state(), which is always verbose.
 */
int _drbd_request_state(struct drbd_conf *mdev,	union drbd_state_t mask,
			union drbd_state_t val,	enum chg_state_flags f)
{
	int rv;

	wait_event(mdev->state_wait,
		   (rv = drbd_req_state(mdev, mask, val, f)) != SS_InTransientState);

	return rv;
}

#if DRBD_DEBUG_STATE_CHANGES
static void trace_st(struct drbd_conf *mdev, const unsigned long long seq,
		const char *func, unsigned int line,
		const char *name, union drbd_state_t s)
{

	const struct task_struct *c = current;
	const char *context =
		c == mdev->worker.task ? "worker" :
		c == mdev->receiver.task ? "receiver" :
		c == mdev->asender.task ? "asender" : "other";

	DBG(" %8llx [%s] %s:%u %s = { cs:%s ro:%s/%s ds:%s/%s %c%c%c%c }\n",
	    seq, context, func, line,
	    name,
	    conns_to_name(s.conn),
	    roles_to_name(s.role),
	    roles_to_name(s.peer),
	    disks_to_name(s.disk),
	    disks_to_name(s.pdsk),
	    s.susp ? 's' : 'r',
	    s.aftr_isp ? 'a' : '-',
	    s.peer_isp ? 'p' : '-',
	    s.user_isp ? 'u' : '-'
	    );
}
#else
#define trace_st(...) do { } while (0)
#endif

STATIC void print_st(struct drbd_conf *mdev, char *name, union drbd_state_t ns)
{
	ERR(" %s = { cs:%s ro:%s/%s ds:%s/%s %c%c%c%c }\n",
	    name,
	    conns_to_name(ns.conn),
	    roles_to_name(ns.role),
	    roles_to_name(ns.peer),
	    disks_to_name(ns.disk),
	    disks_to_name(ns.pdsk),
	    ns.susp ? 's' : 'r',
	    ns.aftr_isp ? 'a' : '-',
	    ns.peer_isp ? 'p' : '-',
	    ns.user_isp ? 'u' : '-'
	    );
}

void print_st_err(struct drbd_conf *mdev,
	union drbd_state_t os, union drbd_state_t ns, int err)
{
	if (err == SS_InTransientState)
		return;
	ERR("State change failed: %s\n", set_st_err_name(err));
	print_st(mdev, " state", os);
	print_st(mdev, "wanted", ns);
}


#define peers_to_name roles_to_name
#define pdsks_to_name disks_to_name

#define susps_to_name(A)     ((A) ? "1" : "0")
#define aftr_isps_to_name(A) ((A) ? "1" : "0")
#define peer_isps_to_name(A) ((A) ? "1" : "0")
#define user_isps_to_name(A) ((A) ? "1" : "0")

#define PSC(A) \
	({ if (ns.A != os.A) { \
		pbp += sprintf(pbp, #A "( %s -> %s ) ", \
			      A##s_to_name(os.A), \
			      A##s_to_name(ns.A)); \
	} })

int is_valid_state(struct drbd_conf *mdev, union drbd_state_t ns)
{
	/* See drbd_state_sw_errors in drbd_strings.c */

	enum fencing_policy fp;
	int rv = SS_Success;

	fp = DontCare;
	if (inc_local(mdev)) {
		fp = mdev->bc->dc.fencing;
		dec_local(mdev);
	}

	if (inc_net(mdev)) {
		if (!mdev->net_conf->two_primaries &&
		    ns.role == Primary && ns.peer == Primary)
			rv = SS_TwoPrimaries;
		dec_net(mdev);
	}

	if (rv <= 0)
		/* already found a reason to abort */;
	else if (ns.role == Secondary && mdev->open_cnt)
		rv = SS_DeviceInUse;

	else if (ns.role == Primary && ns.conn < Connected && ns.disk < UpToDate)
		rv = SS_NoUpToDateDisk;

	else if (fp >= Resource &&
		 ns.role == Primary && ns.conn < Connected && ns.pdsk >= DUnknown)
		rv = SS_PrimaryNOP;

	else if (ns.role == Primary && ns.disk <= Inconsistent && ns.pdsk <= Inconsistent)
		rv = SS_NoUpToDateDisk;

	else if (ns.conn > Connected && ns.disk < UpToDate && ns.pdsk < UpToDate)
		rv = SS_BothInconsistent;

	else if (ns.conn > Connected && (ns.disk == Diskless || ns.pdsk == Diskless))
		rv = SS_SyncingDiskless;

	else if ((ns.conn == Connected ||
		  ns.conn == WFBitMapS ||
		  ns.conn == SyncSource ||
		  ns.conn == PausedSyncS) &&
		  ns.disk == Outdated)
		rv = SS_ConnectedOutdates;

	else if( (ns.conn == VerifyS ||
		  ns.conn == VerifyT) &&
                  (mdev->sync_conf.verify_alg[0] == 0)) rv=SS_NoVerifyAlg;

	else if( (ns.conn == VerifyS ||
		  ns.conn == VerifyT) &&
		  mdev->agreed_pro_version < 88) rv = SS_NotSupported;

	return rv;
}

int is_valid_state_transition(struct drbd_conf *mdev,
	union drbd_state_t ns, union drbd_state_t os)
{
	int rv = SS_Success;

	if ((ns.conn == StartingSyncT || ns.conn == StartingSyncS) &&
	    os.conn > Connected)
		rv = SS_ResyncRunning;

	if (ns.conn == Disconnecting && os.conn == StandAlone)
		rv = SS_AlreadyStandAlone;

	if (ns.disk > Attaching && os.disk == Diskless)
		rv = SS_IsDiskLess;

	if (ns.conn == WFConnection && os.conn < Unconnected)
		rv = SS_NoNetConfig;

	if (ns.disk == Outdated && os.disk < Outdated && os.disk != Attaching)
		rv = SS_LowerThanOutdated;

	if (ns.conn == Disconnecting && os.conn == Unconnected)
		rv = SS_InTransientState;

	if (ns.conn == os.conn && ns.conn == WFReportParams)
		rv = SS_InTransientState;

	if ((ns.conn == VerifyS || ns.conn == VerifyT) && os.conn < Connected)
		rv=SS_NeedConnection;

	if ((ns.conn == VerifyS || ns.conn == VerifyT) &&
	    ns.conn != os.conn && os.conn > Connected)
		rv = SS_ResyncRunning;

	if ((ns.conn == StartingSyncS || ns.conn == StartingSyncT) &&
	    os.conn < Connected)
		rv = SS_NeedConnection;

	return rv;
}

int _drbd_set_state(struct drbd_conf *mdev,
		    union drbd_state_t ns, enum chg_state_flags flags,
		    struct completion *done)
{
#if DRBD_DEBUG_STATE_CHANGES
	static unsigned long long sseq = 0xff000000LLU;
	unsigned long long seq = 0;
#endif
	union drbd_state_t os;
	int rv = SS_Success;
	int warn_sync_abort = 0;
	enum fencing_policy fp;
	struct after_state_chg_work *ascw;

	MUST_HOLD(&mdev->req_lock);

	os = mdev->state;

#if DRBD_DEBUG_STATE_CHANGES
	if (ns.func) {
		seq = ++sseq;
		trace_st(mdev, seq, ns.func, ns.line, "==os", os);
		trace_st(mdev, seq, ns.func, ns.line, "==ns", ns);
	}
#endif

	fp = DontCare;
	if (inc_local(mdev)) {
		fp = mdev->bc->dc.fencing;
		dec_local(mdev);
	}

	/* Early state sanitising. */

	/* Dissalow the invalidate command to connect  */
	if ((ns.conn == StartingSyncS || ns.conn == StartingSyncT) &&
		os.conn < Connected) {
		ns.conn = os.conn;
		ns.pdsk = os.pdsk;
	}

	/* Dissalow Network errors to configure a device's network part */
	if ((ns.conn >= Timeout && ns.conn <= TearDown) &&
	    os.conn <= Disconnecting)
		ns.conn = os.conn;

	/* After a network error (+TearDown) only Unconnected or Disconnecting can follow */
	if (os.conn >= Timeout && os.conn <= TearDown &&
	    ns.conn != Unconnected && ns.conn != Disconnecting)
		ns.conn = os.conn;

	/* After Disconnecting only StandAlone may follow */
	if (os.conn == Disconnecting && ns.conn != StandAlone)
		ns.conn = os.conn;

	if (ns.conn < Connected) {
		ns.peer_isp = 0;
		ns.peer = Unknown;
		if (ns.pdsk > DUnknown || ns.pdsk < Inconsistent)
			ns.pdsk = DUnknown;
	}

	if (ns.conn <= Disconnecting && ns.disk == Diskless)
		ns.pdsk = DUnknown;

	if (os.conn > Connected && ns.conn > Connected &&
            (ns.disk <= Failed || ns.pdsk <= Failed)) {
		warn_sync_abort = 1;
		ns.conn = Connected;
	}

	if (ns.conn >= Connected &&
	    ((ns.disk == Consistent || ns.disk == Outdated) ||
	     (ns.disk == Negotiating && ns.conn == WFBitMapT))) {
		switch (ns.conn) {
		case WFBitMapT:
		case PausedSyncT:
			ns.disk = Outdated;
			break;
		case Connected:
		case WFBitMapS:
		case SyncSource:
		case PausedSyncS:
			ns.disk = UpToDate;
			break;
		case SyncTarget:
			ns.disk = Inconsistent;
			drbd_WARN("Implicit set disk state Inconsistent!\n");
			break;
		}
		if (os.disk == Outdated && ns.disk == UpToDate)
			drbd_WARN("Implicit set disk from Outdate to UpToDate\n");
	}

	if (ns.conn >= Connected &&
	    (ns.pdsk == Consistent || ns.pdsk == Outdated)) {
		switch (ns.conn) {
		case Connected:
		case WFBitMapT:
		case PausedSyncT:
		case SyncTarget:
			ns.pdsk = UpToDate;
			break;
		case WFBitMapS:
		case PausedSyncS:
			ns.pdsk = Outdated;
			break;
		case SyncSource:
			ns.pdsk = Inconsistent;
			drbd_WARN("Implicit set pdsk Inconsistent!\n");
			break;
		}
		if (os.pdsk == Outdated && ns.pdsk == UpToDate)
			drbd_WARN("Implicit set pdsk from Outdate to UpToDate\n");
	}

	/* Connection breaks down before we finished "Negotiating" */
	if (ns.conn < Connected && ns.disk == Negotiating &&
	    inc_local_if_state(mdev, Negotiating)) {
		if (mdev->ed_uuid == mdev->bc->md.uuid[Current]) {
			ns.disk = mdev->new_state_tmp.disk;
			ns.pdsk = mdev->new_state_tmp.pdsk;
		} else {
			ALERT("Connection lost while negotiating, no data!\n");
			ns.disk = Diskless;
			ns.pdsk = DUnknown;
		}
		dec_local(mdev);
	}

	if (fp == Stonith &&
	    (ns.role == Primary &&
	     ns.conn < Connected &&
	     ns.pdsk > Outdated))
			ns.susp = 1;

	if (ns.aftr_isp || ns.peer_isp || ns.user_isp) {
		if (ns.conn == SyncSource)
			ns.conn = PausedSyncS;
		if (ns.conn == SyncTarget)
			ns.conn = PausedSyncT;
	} else {
		if (ns.conn == PausedSyncS)
			ns.conn = SyncSource;
		if (ns.conn == PausedSyncT)
			ns.conn = SyncTarget;
	}

#if DRBD_DEBUG_STATE_CHANGES
	if (ns.func)
		trace_st(mdev, seq, ns.func, ns.line, "==ns", ns);
#endif

	if (ns.i == os.i)
		return SS_NothingToDo;

	if (!(flags & ChgStateHard)) {
		/*  pre-state-change checks ; only look at ns  */
		/* See drbd_state_sw_errors in drbd_strings.c */

		rv = is_valid_state(mdev, ns);
		if (rv < SS_Success) {
			/* If the old state was illegal as well, then let
			   this happen...*/

			if (is_valid_state(mdev, os) == rv) {
				ERR("Considering state change from bad state. "
				    "Error would be: '%s'\n",
				    set_st_err_name(rv));
				print_st(mdev, "old", os);
				print_st(mdev, "new", ns);
				rv = is_valid_state_transition(mdev, ns, os);
			}
		} else
			rv = is_valid_state_transition(mdev, ns, os);
	}

	if (rv < SS_Success) {
		if (flags & ChgStateVerbose)
			print_st_err(mdev, os, ns, rv);
		return rv;
	}

	if (warn_sync_abort)
		drbd_WARN("Resync aborted.\n");

#if DUMP_MD >= 2
	{
	char *pbp, pb[300];
	pbp = pb;
	*pbp = 0;
	PSC(role);
	PSC(peer);
	PSC(conn);
	PSC(disk);
	PSC(pdsk);
	PSC(susp);
	PSC(aftr_isp);
	PSC(peer_isp);
	PSC(user_isp);
	INFO("%s\n", pb);
	}
#endif

#if DRBD_DEBUG_STATE_CHANGES
	if (ns.func)
		trace_st(mdev, seq, ns.func, ns.line, ":=ns", ns);
#endif

	mdev->state.i = ns.i;
	wake_up(&mdev->misc_wait);
	wake_up(&mdev->state_wait);

	/**   post-state-change actions   **/
	if (os.conn >= SyncSource   && ns.conn <= Connected) {
		set_bit(STOP_SYNC_TIMER, &mdev->flags);
		mod_timer(&mdev->resync_timer, jiffies);
	}

	if ((os.conn == PausedSyncT || os.conn == PausedSyncS) &&
	    (ns.conn == SyncTarget  || ns.conn == SyncSource)) {
		INFO("Syncer continues.\n");
		mdev->rs_paused += (long)jiffies-(long)mdev->rs_mark_time;
		if (ns.conn == SyncTarget) {
			if (!test_and_clear_bit(STOP_SYNC_TIMER, &mdev->flags))
				mod_timer(&mdev->resync_timer, jiffies);
			/* This if (!test_bit) is only needed for the case
			   that a device that has ceased to used its timer,
			   i.e. it is already in drbd_resync_finished() gets
			   paused and resumed. */
		}
	}

	if ((os.conn == SyncTarget  || os.conn == SyncSource) &&
	    (ns.conn == PausedSyncT || ns.conn == PausedSyncS)) {
		INFO("Resync suspended\n");
		mdev->rs_mark_time = jiffies;
		if (ns.conn == PausedSyncT)
			set_bit(STOP_SYNC_TIMER, &mdev->flags);
	}

	if (os.conn == Connected &&
	    (ns.conn == VerifyS || ns.conn == VerifyT )) {
		mdev->ov_position = 0;
		mdev->ov_left  =
		mdev->rs_total =
		mdev->rs_mark_left = drbd_bm_bits(mdev);
		mdev->rs_start     =
		mdev->rs_mark_time = jiffies;
		mdev->ov_last_oos_size = 0;
		mdev->ov_last_oos_start = 0;

		if (ns.conn == VerifyS)
			mod_timer(&mdev->resync_timer,jiffies);
	}

	if (inc_local(mdev)) {
		u32 mdf = mdev->bc->md.flags & ~(MDF_Consistent|MDF_PrimaryInd|
						 MDF_ConnectedInd|MDF_WasUpToDate|
						 MDF_PeerOutDated);

		if (test_bit(CRASHED_PRIMARY, &mdev->flags) ||
		    mdev->state.role == Primary ||
		    (mdev->state.pdsk < Inconsistent && mdev->state.peer == Primary))
			mdf |= MDF_PrimaryInd;
		if (mdev->state.conn > WFReportParams)
			mdf |= MDF_ConnectedInd;
		if (mdev->state.disk > Inconsistent)
			mdf |= MDF_Consistent;
		if (mdev->state.disk > Outdated)
			mdf |= MDF_WasUpToDate;
		if (mdev->state.pdsk <= Outdated && mdev->state.pdsk >= Inconsistent)
			mdf |= MDF_PeerOutDated;
		if (mdf != mdev->bc->md.flags) {
			mdev->bc->md.flags = mdf;
			drbd_md_mark_dirty(mdev);
		}
		if (os.disk < Consistent && ns.disk >= Consistent)
			drbd_set_ed_uuid(mdev, mdev->bc->md.uuid[Current]);
		dec_local(mdev);
	}

	/* Peer was forced UpToDate & Primary, consider to resync */
	if (os.disk == Inconsistent && os.pdsk == Inconsistent &&
	    os.peer == Secondary && ns.peer == Primary)
		set_bit(CONSIDER_RESYNC, &mdev->flags);

	/* Receiver should clean up itself */
	if (os.conn != Disconnecting && ns.conn == Disconnecting)
		drbd_thread_stop_nowait(&mdev->receiver);

	/* Now the receiver finished cleaning up itself, it should die */
	if (os.conn != StandAlone && ns.conn == StandAlone)
		drbd_thread_stop_nowait(&mdev->receiver);

	/* Upon network failure, we need to restart the receiver. */
	if (os.conn > TearDown &&
	    ns.conn <= TearDown && ns.conn >= Timeout)
		drbd_thread_restart_nowait(&mdev->receiver);

	ascw = kmalloc(sizeof(*ascw), GFP_ATOMIC);
	if (ascw) {
		ascw->os = os;
		ascw->ns = ns;
		ascw->flags = flags;
		ascw->w.cb = w_after_state_ch;
		ascw->done = done;
		drbd_queue_work(&mdev->data.work, &ascw->w);
	} else {
		drbd_WARN("Could not kmalloc an ascw\n");
	}

	return rv;
}

STATIC int w_after_state_ch(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct after_state_chg_work *ascw;

	ascw = (struct after_state_chg_work *) w;
	after_state_ch(mdev, ascw->os, ascw->ns, ascw->flags);
	if (ascw->flags & ChgWaitComplete) {
		D_ASSERT(ascw->done != NULL);
		complete(ascw->done);
	}
	kfree(ascw);

	return 1;
}

static void abw_start_sync(struct drbd_conf *mdev, int rv)
{
	if (rv) {
		ERR("Writing the bitmap failed not starting resync.\n");
		_drbd_request_state(mdev, NS(conn, Connected), ChgStateVerbose);
		return;
	}

	switch (mdev->state.conn) {
	case StartingSyncT:
		_drbd_request_state(mdev, NS(conn, WFSyncUUID), ChgStateVerbose);
		break;
	case StartingSyncS:
		drbd_start_resync(mdev, SyncSource);
		break;
	}
}

STATIC void after_state_ch(struct drbd_conf *mdev, union drbd_state_t os,
			   union drbd_state_t ns, enum chg_state_flags flags)
{
	enum fencing_policy fp;

	if (os.conn != Connected && ns.conn == Connected) {
		clear_bit(CRASHED_PRIMARY, &mdev->flags);
		if (mdev->p_uuid)
			mdev->p_uuid[UUID_FLAGS] &= ~((u64)2);
	}

	fp = DontCare;
	if (inc_local(mdev)) {
		fp = mdev->bc->dc.fencing;
		dec_local(mdev);
	}

	/* Inform userspace about the change... */
	drbd_bcast_state(mdev, ns);

	if (!(os.role == Primary && os.disk < UpToDate && os.pdsk < UpToDate) &&
	    (ns.role == Primary && ns.disk < UpToDate && ns.pdsk < UpToDate))
		drbd_khelper(mdev, "pri-on-incon-degr");

	/* Here we have the actions that are performed after a
	   state change. This function might sleep */

	if (fp == Stonith && ns.susp) {
		/* case1: The outdate peer handler is successfull:
		 * case2: The connection was established again: */
		if ((os.pdsk > Outdated  && ns.pdsk <= Outdated) ||
		    (os.conn < Connected && ns.conn >= Connected)) {
			tl_clear(mdev);
			spin_lock_irq(&mdev->req_lock);
			_drbd_set_state(_NS(mdev, susp, 0), ChgStateVerbose, NULL);
			spin_unlock_irq(&mdev->req_lock);
		}
	}
	/* Do not change the order of the if above and the two below... */
	if (os.pdsk == Diskless && ns.pdsk > Diskless) {      /* attach on the peer */
		drbd_send_uuids(mdev);
		drbd_send_state(mdev);
	}
	if (os.conn != WFBitMapS && ns.conn == WFBitMapS)
		drbd_queue_bitmap_io(mdev, &drbd_send_bitmap, NULL, "send_bitmap (WFBitMapS)");

	/* Lost contact to peer's copy of the data */
	if ((os.pdsk >= Inconsistent &&
	     os.pdsk != DUnknown &&
	     os.pdsk != Outdated)
	&&  (ns.pdsk < Inconsistent ||
	     ns.pdsk == DUnknown ||
	     ns.pdsk == Outdated)) {
		/* FIXME race with drbd_sync_handshake accessing this! */
		kfree(mdev->p_uuid);
		mdev->p_uuid = NULL;
		if (inc_local(mdev)) {
			if (ns.role == Primary && mdev->bc->md.uuid[Bitmap] == 0 &&
			    ns.disk >= UpToDate)
				drbd_uuid_new_current(mdev);
			if (ns.peer == Primary) {
				/* Note: The condition ns.peer == Primary implies
				   that we are connected. Otherwise it would
				   be ns.peer == Unknown. */
				/* A FullSync is required after a
				   primary detached from its disk! */
				_drbd_uuid_new_current(mdev);
				drbd_send_uuids(mdev);
			}
			dec_local(mdev);
		}
	}

	if (ns.pdsk < Inconsistent && inc_local(mdev)) {
		if (ns.peer == Primary && mdev->bc->md.uuid[Bitmap] == 0) {
			/* Diskless Peer becomes primary */
			if (os.peer == Secondary)
				drbd_uuid_new_current(mdev);

			/* Got connected to diskless, primary peer */
			if (os.peer == Unknown)
				_drbd_uuid_new_current(mdev);
		}

		/* Diskless Peer becomes secondary */
		if (os.peer == Primary && ns.peer == Secondary)
			drbd_al_to_on_disk_bm(mdev);
		dec_local(mdev);
	}

	/* Last part of the attaching process ... */
	if (ns.conn >= Connected &&
	    os.disk == Attaching && ns.disk == Negotiating) {
		kfree(mdev->p_uuid); /* We expect to receive up-to-date UUIDs soon. */
		mdev->p_uuid = NULL; /* ...to not use the old ones in the mean time */
		drbd_send_sizes(mdev);  /* to start sync... */
		drbd_send_uuids(mdev);
		drbd_send_state(mdev);
	}

	/* We want to pause/continue resync, tell peer. */
	if (ns.conn >= Connected &&
	     ((os.aftr_isp != ns.aftr_isp) ||
	      (os.user_isp != ns.user_isp)))
		drbd_send_state(mdev);

	/* In case one of the isp bits got set, suspend other devices. */
	if ((!os.aftr_isp && !os.peer_isp && !os.user_isp) &&
	    (ns.aftr_isp || ns.peer_isp || ns.user_isp))
		suspend_other_sg(mdev);

	/* Make sure the peer gets informed about eventual state
	   changes (ISP bits) while we were in WFReportParams. */
	if (os.conn == WFReportParams && ns.conn >= Connected)
		drbd_send_state(mdev);

	/* We are in the progress to start a full sync... */
	if ((os.conn != StartingSyncT && ns.conn == StartingSyncT) ||
	    (os.conn != StartingSyncS && ns.conn == StartingSyncS))
		drbd_queue_bitmap_io(mdev, &drbd_bmio_set_n_write, &abw_start_sync, "set_n_write from StartingSync");

	/* We are invalidating our self... */
	if (os.conn < Connected && ns.conn < Connected &&
	    os.disk > Inconsistent && ns.disk == Inconsistent)
		drbd_queue_bitmap_io(mdev, &drbd_bmio_set_n_write, NULL, "set_n_write from invalidate");

	if (os.disk > Diskless && ns.disk == Diskless) {
		/* since inc_local() only works as long as disk>=Inconsistent,
		   and it is Diskless here, local_cnt can only go down, it can
		   not increase... It will reach zero */
		wait_event(mdev->misc_wait, !atomic_read(&mdev->local_cnt));

		lc_free(mdev->resync);
		mdev->resync = NULL;
		lc_free(mdev->act_log);
		mdev->act_log = NULL;
		__no_warn(local, drbd_free_bc(mdev->bc););
		wmb(); /* see begin of drbd_nl_disk_conf() */
		__no_warn(local, mdev->bc = NULL;);
	}

	/* Disks got bigger while they were detached */
	if (ns.disk > Negotiating && ns.pdsk > Negotiating &&
	    test_and_clear_bit(RESYNC_AFTER_NEG, &mdev->flags)) {
		if (ns.conn == Connected)
			resync_after_online_grow(mdev);
	}

	/* A resync finished or aborted, wake paused devices... */
	if ((os.conn > Connected && ns.conn <= Connected) ||
	    (os.peer_isp && !ns.peer_isp) ||
	    (os.user_isp && !ns.user_isp))
		resume_next_sg(mdev);

	/* Upon network connection, we need to start the received */
	if (os.conn == StandAlone && ns.conn == Unconnected)
		drbd_thread_start(&mdev->receiver);

	/* Terminate worker thread if we are unconfigured - it will be
	   restarted as needed... */
	if (ns.disk == Diskless && ns.conn == StandAlone && ns.role == Secondary)
		drbd_thread_stop_nowait(&mdev->worker);

	drbd_md_sync(mdev);
}


STATIC int drbd_thread_setup(void *arg)
{
	struct Drbd_thread *thi = (struct Drbd_thread *) arg;
	struct drbd_conf *mdev = thi->mdev;
	long timeout;
	int retval;
	const char *me =
		thi == &mdev->receiver ? "receiver" :
		thi == &mdev->asender  ? "asender"  :
		thi == &mdev->worker   ? "worker"   : "NONSENSE";

	daemonize("drbd_thread");
	D_ASSERT(get_t_state(thi) == Running);
	D_ASSERT(thi->task == NULL);
	spin_lock(&thi->t_lock);
	thi->task = current;
	smp_mb();
	spin_unlock(&thi->t_lock);

	/* stolen from kthread; FIXME we need to convert to kthread api!
	 * wait for wakeup */
	__set_current_state(TASK_UNINTERRUPTIBLE);
	complete(&thi->startstop); /* notify: thi->task is set. */
	timeout = schedule_timeout(10*HZ);
	D_ASSERT(timeout != 0);

restart:
	retval = thi->function(thi);

	spin_lock(&thi->t_lock);

	/* if the receiver has been "Exiting", the last thing it did
	 * was set the conn state to "StandAlone",
	 * if now a re-connect request comes in, conn state goes Unconnected,
	 * and receiver thread will be "started".
	 * drbd_thread_start needs to set "Restarting" in that case.
	 * t_state check and assignement needs to be within the same spinlock,
	 * so either thread_start sees Exiting, and can remap to Restarting,
	 * or thread_start see None, and can proceed as normal.
	 */

	if (thi->t_state == Restarting) {
		INFO("Restarting %s thread\n", me);
		thi->t_state = Running;
		spin_unlock(&thi->t_lock);
		goto restart;
	}

	thi->task = NULL;
	thi->t_state = None;
	smp_mb();

	/* THINK maybe two different completions? */
	complete(&thi->startstop); /* notify: thi->task unset. */
	INFO("Terminating %s thread\n", me);
	spin_unlock(&thi->t_lock);

	/* Release mod reference taken when thread was started */
	module_put(THIS_MODULE);
	return retval;
}

STATIC void drbd_thread_init(struct drbd_conf *mdev, struct Drbd_thread *thi,
		      int (*func) (struct Drbd_thread *))
{
	spin_lock_init(&thi->t_lock);
	thi->task    = NULL;
	thi->t_state = None;
	thi->function = func;
	thi->mdev = mdev;
}

int drbd_thread_start(struct Drbd_thread *thi)
{
	int pid;
	struct drbd_conf *mdev = thi->mdev;
	const char *me =
		thi == &mdev->receiver ? "receiver" :
		thi == &mdev->asender  ? "asender"  :
		thi == &mdev->worker   ? "worker"   : "NONSENSE";

	spin_lock(&thi->t_lock);

	switch (thi->t_state) {
	case None:
		INFO("Starting %s thread (from %s [%d])\n",
				me, current->comm, current->pid);

		/* Get ref on module for thread - this is released when thread exits */
		if (!try_module_get(THIS_MODULE)) {
			ERR("Failed to get module reference in drbd_thread_start\n");
			spin_unlock(&thi->t_lock);
			return FALSE;
		}

		init_completion(&thi->startstop);
		D_ASSERT(thi->task == NULL);
		thi->reset_cpu_mask = 1;
		thi->t_state = Running;
		spin_unlock(&thi->t_lock);
		flush_signals(current); /* otherw. may get -ERESTARTNOINTR */

		/* FIXME rewrite to use kthread interface */
		pid = kernel_thread(drbd_thread_setup, (void *) thi, CLONE_FS);
		if (pid < 0) {
			ERR("Couldn't start thread (%d)\n", pid);

			module_put(THIS_MODULE);
			return FALSE;
		}
		/* waits until thi->task is set */
		wait_for_completion(&thi->startstop);
		if (thi->t_state != Running)
			ERR("ASSERT FAILED: %s t_state == %d expected %d.\n",
					me, thi->t_state, Running);
		if (thi->task)
			wake_up_process(thi->task);
		else
			ERR("ASSERT FAILED thi->task is NULL where it should be set!?\n");
		break;
	case Exiting:
		thi->t_state = Restarting;
		INFO("Restarting %s thread (from %s [%d])\n",
				me, current->comm, current->pid);
	case Running:
	case Restarting:
	default:
		spin_unlock(&thi->t_lock);
		break;
	}

	return TRUE;
}


void _drbd_thread_stop(struct Drbd_thread *thi, int restart, int wait)
{
	struct drbd_conf *mdev = thi->mdev;
	enum Drbd_thread_state ns = restart ? Restarting : Exiting;
	const char *me =
		thi == &mdev->receiver ? "receiver" :
		thi == &mdev->asender  ? "asender"  :
		thi == &mdev->worker   ? "worker"   : "NONSENSE";

	spin_lock(&thi->t_lock);

	/* INFO("drbd_thread_stop: %s [%d]: %s %d -> %d; %d\n",
	     current->comm, current->pid,
	     thi->task ? thi->task->comm : "NULL", thi->t_state, ns, wait); */

	if (thi->t_state == None) {
		spin_unlock(&thi->t_lock);
		if (restart)
			drbd_thread_start(thi);
		return;
	}

	if (thi->t_state != ns) {
		if (thi->task == NULL) {
			spin_unlock(&thi->t_lock);
			return;
		}

		thi->t_state = ns;
		smp_mb();
		init_completion(&thi->startstop);
		if (thi->task != current) {
			force_sig(DRBD_SIGKILL, thi->task);
		} else
			D_ASSERT(!wait);
	}
	spin_unlock(&thi->t_lock);

	if (wait) {
		D_ASSERT(thi->task != current);
		wait_for_completion(&thi->startstop);
		spin_lock(&thi->t_lock);
		D_ASSERT(thi->task == NULL);
		if (thi->t_state != None)
			ERR("ASSERT FAILED: %s t_state == %d expected %d.\n",
					me, thi->t_state, None);
		spin_unlock(&thi->t_lock);
	}
}

#ifdef CONFIG_SMP
/**
 * drbd_calc_cpu_mask: Generates CPU masks, sprad over all CPUs.
 * Forces all threads of a device onto the same CPU. This is benificial for
 * DRBD's performance. May be overwritten by user's configuration.
 */
cpumask_t drbd_calc_cpu_mask(struct drbd_conf *mdev)
{
	int sv, cpu;
	cpumask_t av_cpu_m;

	if (cpus_weight(mdev->cpu_mask))
		return mdev->cpu_mask;

	av_cpu_m = cpu_online_map;
	sv = mdev_to_minor(mdev) % cpus_weight(av_cpu_m);

	for_each_cpu_mask(cpu, av_cpu_m) {
		if (sv-- == 0)
			return cpumask_of_cpu(cpu);
	}

	/* some kernel versions "forget" to add the (cpumask_t) typecast
	 * to that macro, which results in "parse error before '{'" ;-> */
	return (cpumask_t) CPU_MASK_ALL; /* Never reached. */
}

/* modifies the cpu mask of the _current_ thread,
 * call in the "main loop" of _all_ threads.
 * no need for any mutex, current won't die prematurely.
 */
void drbd_thread_current_set_cpu(struct drbd_conf *mdev)
{
	struct task_struct *p = current;
	struct Drbd_thread *thi =
		p == mdev->asender.task  ? &mdev->asender  :
		p == mdev->receiver.task ? &mdev->receiver :
		p == mdev->worker.task   ? &mdev->worker   :
		NULL;
	ERR_IF(thi == NULL)
		return;
	if (!thi->reset_cpu_mask)
		return;
	thi->reset_cpu_mask = 0;
	/* preempt_disable();
	   Thas was a kernel that warned about a call to smp_processor_id() while preemt
	   was not disabled. It seems that this was fixed in manline. */
	set_cpus_allowed(p, mdev->cpu_mask);
	/* preempt_enable(); */
}
#endif

/* the appropriate socket mutex must be held already */
int _drbd_send_cmd(struct drbd_conf *mdev, struct socket *sock,
			  enum Drbd_Packet_Cmd cmd, struct Drbd_Header *h,
			  size_t size, unsigned msg_flags)
{
	int sent, ok;

	ERR_IF(!h) return FALSE;
	ERR_IF(!size) return FALSE;

	h->magic   = BE_DRBD_MAGIC;
	h->command = cpu_to_be16(cmd);
	h->length  = cpu_to_be16(size-sizeof(struct Drbd_Header));

	dump_packet(mdev, sock, 0, (void *)h, __FILE__, __LINE__);
	sent = drbd_send(mdev, sock, h, size, msg_flags);

	ok = (sent == size);
	if (!ok)
		ERR("short sent %s size=%d sent=%d\n",
		    cmdname(cmd), (int)size, sent);
	return ok;
}

/* don't pass the socket. we may only look at it
 * when we hold the appropriate socket mutex.
 */
int drbd_send_cmd(struct drbd_conf *mdev, int use_data_socket,
		  enum Drbd_Packet_Cmd cmd, struct Drbd_Header *h, size_t size)
{
	int ok = 0;
	struct socket *sock;

	if (use_data_socket) {
		down(&mdev->data.mutex);
		sock = mdev->data.socket;
	} else {
		down(&mdev->meta.mutex);
		sock = mdev->meta.socket;
	}

	/* drbd_disconnect() could have called drbd_free_sock()
	 * while we were waiting in down()... */
	if (likely(sock != NULL))
		ok = _drbd_send_cmd(mdev, sock, cmd, h, size, 0);

	if (use_data_socket)
		up(&mdev->data.mutex);
	else
		up(&mdev->meta.mutex);
	return ok;
}

int drbd_send_cmd2(struct drbd_conf *mdev, enum Drbd_Packet_Cmd cmd, char *data,
		   size_t size)
{
	struct Drbd_Header h;
	int ok;

	h.magic   = BE_DRBD_MAGIC;
	h.command = cpu_to_be16(cmd);
	h.length  = cpu_to_be16(size);

	if (!drbd_get_data_sock(mdev))
		return 0;

	dump_packet(mdev, mdev->data.socket, 0, (void *)&h, __FILE__, __LINE__);

	ok = (sizeof(h) ==
		drbd_send(mdev, mdev->data.socket, &h, sizeof(h), 0));
	ok = ok && (size ==
		drbd_send(mdev, mdev->data.socket, data, size, 0));

	drbd_put_data_sock(mdev);

	return ok;
}

int drbd_send_sync_param(struct drbd_conf *mdev, struct syncer_conf *sc)
{
	struct Drbd_SyncParam89_Packet *p;
	struct socket *sock;
	int size, rv;
	const int apv = mdev->agreed_pro_version;

	size = apv <= 87 ? sizeof(struct Drbd_SyncParam_Packet)
	     : apv == 88 ? sizeof(struct Drbd_SyncParam_Packet)
	                   + strlen(mdev->sync_conf.verify_alg) + 1
	     : /* 89 */    sizeof(struct Drbd_SyncParam89_Packet);

	/* used from admin command context and receiver/worker context.
	 * to avoid kmalloc, grab the socket right here,
	 * then use the pre-allocated sbuf there */
	down(&mdev->data.mutex);
	sock = mdev->data.socket;

	if (likely(sock != NULL)) {
		enum Drbd_Packet_Cmd cmd = apv >= 89 ? SyncParam89 : SyncParam;

		p = &mdev->data.sbuf.SyncParam89;

		/* initialize verify_alg and csums_alg */
		memset(p->verify_alg, 0, 2 * SHARED_SECRET_MAX);

		p->rate = cpu_to_be32(sc->rate);

		if (apv >= 88)
			strcpy(p->verify_alg, mdev->sync_conf.verify_alg);
		if (apv >= 89)
			strcpy(p->csums_alg, mdev->sync_conf.csums_alg);

		rv = _drbd_send_cmd(mdev, sock, cmd, &p->head, size, 0);
	} else
		rv = 0; /* not ok */

	up(&mdev->data.mutex);

	return rv;
}

int drbd_send_protocol(struct drbd_conf *mdev)
{
	struct Drbd_Protocol_Packet *p;
	int size,rv;

	size = sizeof(struct Drbd_Protocol_Packet);

	if (mdev->agreed_pro_version >= 87)
		size += strlen(mdev->net_conf->integrity_alg) + 1;

	if ((p = kmalloc(size, GFP_KERNEL)) == NULL)
		return 0;

	p->protocol      = cpu_to_be32(mdev->net_conf->wire_protocol);
	p->after_sb_0p   = cpu_to_be32(mdev->net_conf->after_sb_0p);
	p->after_sb_1p   = cpu_to_be32(mdev->net_conf->after_sb_1p);
	p->after_sb_2p   = cpu_to_be32(mdev->net_conf->after_sb_2p);
	p->want_lose     = cpu_to_be32(mdev->net_conf->want_lose);
	p->two_primaries = cpu_to_be32(mdev->net_conf->two_primaries);

	if (mdev->agreed_pro_version >= 87)
		strcpy(p->integrity_alg, mdev->net_conf->integrity_alg);

	rv = drbd_send_cmd(mdev, USE_DATA_SOCKET, ReportProtocol,
			   (struct Drbd_Header *)p, size);
	kfree(p);
	return rv;
}

int drbd_send_uuids(struct drbd_conf *mdev)
{
	struct Drbd_GenCnt_Packet p;
	int i;

	u64 uuid_flags = 0;

	if (!inc_local_if_state(mdev, Negotiating))
		return 1;

	/* FIXME howto handle diskless ? */
	for (i = Current; i < UUID_SIZE; i++)
		p.uuid[i] = mdev->bc ? cpu_to_be64(mdev->bc->md.uuid[i]) : 0;

	mdev->comm_bm_set = drbd_bm_total_weight(mdev);
	p.uuid[UUID_SIZE] = cpu_to_be64(mdev->comm_bm_set);
	uuid_flags |= mdev->net_conf->want_lose ? 1 : 0;
	uuid_flags |= test_bit(CRASHED_PRIMARY, &mdev->flags) ? 2 : 0;
	uuid_flags |= mdev->new_state_tmp.disk == Inconsistent ? 4 : 0;
	p.uuid[UUID_FLAGS] = cpu_to_be64(uuid_flags);

	dec_local(mdev);

	return drbd_send_cmd(mdev, USE_DATA_SOCKET, ReportUUIDs,
			     (struct Drbd_Header *)&p, sizeof(p));
}

int drbd_send_sync_uuid(struct drbd_conf *mdev, u64 val)
{
	struct Drbd_SyncUUID_Packet p;

	p.uuid = cpu_to_be64(val);

	return drbd_send_cmd(mdev, USE_DATA_SOCKET, ReportSyncUUID,
			     (struct Drbd_Header *)&p, sizeof(p));
}

int drbd_send_sizes(struct drbd_conf *mdev)
{
	struct Drbd_Sizes_Packet p;
	sector_t d_size, u_size;
	int q_order_type;
	int ok;

	if (inc_local_if_state(mdev, Negotiating)) {
		D_ASSERT(mdev->bc->backing_bdev);
		d_size = drbd_get_max_capacity(mdev->bc);
		u_size = mdev->bc->dc.disk_size;
		q_order_type = drbd_queue_order_type(mdev);
		p.queue_order_type = cpu_to_be32(drbd_queue_order_type(mdev));
		dec_local(mdev);
	} else {
		d_size = 0;
		u_size = 0;
		q_order_type = QUEUE_ORDERED_NONE;
	}

	p.d_size = cpu_to_be64(d_size);
	p.u_size = cpu_to_be64(u_size);
	p.c_size = cpu_to_be64(drbd_get_capacity(mdev->this_bdev));
	p.max_segment_size = cpu_to_be32(mdev->rq_queue->max_segment_size);
	p.queue_order_type = cpu_to_be32(q_order_type);

	ok = drbd_send_cmd(mdev, USE_DATA_SOCKET, ReportSizes,
			   (struct Drbd_Header *)&p, sizeof(p));
	return ok;
}

/**
 * drbd_send_state:
 * Informs the peer about our state. Only call it when
 * mdev->state.conn >= Connected (I.e. you may not call it while in
 * WFReportParams. Though there is one valid and necessary exception,
 * drbd_connect() calls drbd_send_state() while in it WFReportParams.
 */
int drbd_send_state(struct drbd_conf *mdev)
{
	struct socket *sock;
	struct Drbd_State_Packet p;
	int ok = 0;

	/* Grab state lock so we wont send state if we're in the middle
	 * of a cluster wide state change on another thread */
	drbd_state_lock(mdev);

	down(&mdev->data.mutex);

	p.state = cpu_to_be32(mdev->state.i); /* Within the send mutex */
	sock = mdev->data.socket;

	if (likely(sock != NULL)) {
		ok = _drbd_send_cmd(mdev, sock, ReportState,
				    (struct Drbd_Header *)&p, sizeof(p), 0);
	}

	up(&mdev->data.mutex);

	drbd_state_unlock(mdev);
	return ok;
}

int drbd_send_state_req(struct drbd_conf *mdev,
	union drbd_state_t mask, union drbd_state_t val)
{
	struct Drbd_Req_State_Packet p;

	p.mask    = cpu_to_be32(mask.i);
	p.val     = cpu_to_be32(val.i);

	return drbd_send_cmd(mdev, USE_DATA_SOCKET, StateChgRequest,
			     (struct Drbd_Header *)&p, sizeof(p));
}

int drbd_send_sr_reply(struct drbd_conf *mdev, int retcode)
{
	struct Drbd_RqS_Reply_Packet p;

	p.retcode    = cpu_to_be32(retcode);

	return drbd_send_cmd(mdev, USE_META_SOCKET, StateChgReply,
			     (struct Drbd_Header *)&p, sizeof(p));
}


/* See the comment at receive_bitmap() */
int _drbd_send_bitmap(struct drbd_conf *mdev)
{
	int want;
	int ok = TRUE;
	int bm_i = 0;
	size_t bm_words, num_words;
	unsigned long *buffer;
	struct Drbd_Header *p;

	ERR_IF(!mdev->bitmap) return FALSE;

	/* maybe we should use some per thread scratch page,
	 * and allocate that during initial device creation? */
	p = (struct Drbd_Header *) __get_free_page(GFP_NOIO);
	if (!p) {
		ERR("failed to allocate one page buffer in %s\n", __func__);
		return FALSE;
	}
	bm_words = drbd_bm_words(mdev);
	buffer = (unsigned long *)p->payload;

	if (inc_local(mdev)) {
		if (drbd_md_test_flag(mdev->bc, MDF_FullSync)) {
			INFO("Writing the whole bitmap, MDF_FullSync was set.\n");
			drbd_bm_set_all(mdev);
			if (drbd_bm_write(mdev)) {
				/* write_bm did fail! Leave full sync flag set in Meta Data
				 * but otherwise process as per normal - need to tell other
				 * side that a full resync is required! */
				ERR("Failed to write bitmap to disk!\n");
			} else {
				drbd_md_clear_flag(mdev, MDF_FullSync);
				drbd_md_sync(mdev);
			}
		}
		dec_local(mdev);
	}

	/*
	 * maybe TODO use some simple compression scheme, nowadays there are
	 * some such algorithms in the kernel anyways.
	 */
	do {
		num_words = min_t(size_t, BM_PACKET_WORDS, bm_words - bm_i);
		want = num_words * sizeof(long);
		if (want)
			drbd_bm_get_lel(mdev, bm_i, num_words, buffer);
		ok = _drbd_send_cmd(mdev, mdev->data.socket, ReportBitMap,
				   p, sizeof(*p) + want, 0);
		bm_i += num_words;
	} while (ok && want);

	free_page((unsigned long) p);
	return ok;
}

int drbd_send_bitmap(struct drbd_conf *mdev)
{
	int err;

	if (!drbd_get_data_sock(mdev))
		return -1;
	err = !_drbd_send_bitmap(mdev);
	drbd_put_data_sock(mdev);
	return err;
}

int drbd_send_b_ack(struct drbd_conf *mdev, u32 barrier_nr, u32 set_size)
{
	int ok;
	struct Drbd_BarrierAck_Packet p;

	p.barrier  = barrier_nr;
	p.set_size = cpu_to_be32(set_size);

	if (mdev->state.conn < Connected)
		return FALSE;
	ok = drbd_send_cmd(mdev, USE_META_SOCKET, BarrierAck,
			(struct Drbd_Header *)&p, sizeof(p));
	return ok;
}

/**
 * _drbd_send_ack:
 * This helper function expects the sector and block_id parameter already
 * in big endian!
 */
STATIC int _drbd_send_ack(struct drbd_conf *mdev, enum Drbd_Packet_Cmd cmd,
			  u64 sector,
			  u32 blksize,
			  u64 block_id)
{
	int ok;
	struct Drbd_BlockAck_Packet p;

	p.sector   = sector;
	p.block_id = block_id;
	p.blksize  = blksize;
	p.seq_num  = cpu_to_be32(atomic_add_return(1, &mdev->packet_seq));

	if (!mdev->meta.socket || mdev->state.conn < Connected)
		return FALSE;
	ok = drbd_send_cmd(mdev, USE_META_SOCKET, cmd,
				(struct Drbd_Header *)&p, sizeof(p));
	return ok;
}

int drbd_send_ack_dp(struct drbd_conf *mdev, enum Drbd_Packet_Cmd cmd,
		     struct Drbd_Data_Packet *dp)
{
	const int header_size = sizeof(struct Drbd_Data_Packet)
			      - sizeof(struct Drbd_Header);
	int data_size  = ((struct Drbd_Header *)dp)->length - header_size;

	return _drbd_send_ack(mdev, cmd, dp->sector, cpu_to_be32(data_size),
			      dp->block_id);
}

int drbd_send_ack_rp(struct drbd_conf *mdev, enum Drbd_Packet_Cmd cmd,
		     struct Drbd_BlockRequest_Packet *rp)
{
	return _drbd_send_ack(mdev, cmd, rp->sector, rp->blksize, rp->block_id);
}

int drbd_send_ack(struct drbd_conf *mdev,
	enum Drbd_Packet_Cmd cmd, struct Tl_epoch_entry *e)
{
	return _drbd_send_ack(mdev, cmd,
			      cpu_to_be64(e->sector),
			      cpu_to_be32(e->size),
			      e->block_id);
}

/* This function misuses the block_id field to signal if the blocks
 * are is sync or not. */
int drbd_send_ack_ex(struct drbd_conf *mdev, enum Drbd_Packet_Cmd cmd,
		     sector_t sector, int blksize, u64 block_id)
{
	return _drbd_send_ack(mdev, cmd,
			      cpu_to_be64(sector),
			      cpu_to_be32(blksize),
			      cpu_to_be64(block_id));
}

int drbd_send_drequest(struct drbd_conf *mdev, int cmd,
		       sector_t sector, int size, u64 block_id)
{
	int ok;
	struct Drbd_BlockRequest_Packet p;

	p.sector   = cpu_to_be64(sector);
	p.block_id = block_id;
	p.blksize  = cpu_to_be32(size);

	/* FIXME BIO_RW_SYNC ? */

	ok = drbd_send_cmd(mdev, USE_DATA_SOCKET, cmd,
				(struct Drbd_Header *)&p, sizeof(p));
	return ok;
}

int drbd_send_drequest_csum(struct drbd_conf *mdev,
			    sector_t sector,int size,
			    void *digest, int digest_size,
			    enum Drbd_Packet_Cmd cmd)
{
	int ok;
	struct Drbd_BlockRequest_Packet p;

	p.sector   = cpu_to_be64(sector);
	p.block_id = BE_DRBD_MAGIC + 0xbeef;
	p.blksize  = cpu_to_be32(size);

	p.head.magic   = BE_DRBD_MAGIC;
	p.head.command = cpu_to_be16(cmd);
	p.head.length  = cpu_to_be16(sizeof(p) - sizeof(struct Drbd_Header) + digest_size);

	down(&mdev->data.mutex);

	ok = (sizeof(p) == drbd_send(mdev, mdev->data.socket, &p, sizeof(p), 0));
	ok = ok && (digest_size == drbd_send(mdev, mdev->data.socket, digest, digest_size, 0));

	up(&mdev->data.mutex);

	return ok;
}

int drbd_send_ov_request(struct drbd_conf *mdev,sector_t sector,int size)
{
	int ok;
	struct Drbd_BlockRequest_Packet p;

	p.sector   = cpu_to_be64(sector);
	p.block_id = BE_DRBD_MAGIC + 0xbabe;
	p.blksize  = cpu_to_be32(size);

	ok = drbd_send_cmd(mdev,USE_DATA_SOCKET, OVRequest,
			   (struct Drbd_Header*)&p,sizeof(p));
	return ok;
}

/* called on sndtimeo
 * returns FALSE if we should retry,
 * TRUE if we think connection is dead
 */
STATIC int we_should_drop_the_connection(struct drbd_conf *mdev, struct socket *sock)
{
	int drop_it;
	/* long elapsed = (long)(jiffies - mdev->last_received); */
	/* DUMPLU(elapsed); // elapsed ignored for now. */

	drop_it =   mdev->meta.socket == sock
		|| !mdev->asender.task
		|| get_t_state(&mdev->asender) != Running
		|| mdev->state.conn < Connected;

	if (drop_it)
		return TRUE;

	drop_it = !--mdev->ko_count;
	if (!drop_it) {
		ERR("[%s/%d] sock_sendmsg time expired, ko = %u\n",
		       current->comm, current->pid, mdev->ko_count);
		request_ping(mdev);
	}

	return drop_it; /* && (mdev->state == Primary) */;
}

/* The idea of sendpage seems to be to put some kind of reference
 * to the page into the skb, and to hand it over to the NIC. In
 * this process get_page() gets called.
 *
 * As soon as the page was really sent over the network put_page()
 * gets called by some part of the network layer. [ NIC driver? ]
 *
 * [ get_page() / put_page() increment/decrement the count. If count
 *   reaches 0 the page will be freed. ]
 *
 * This works nicely with pages from FSs.
 * But this means that in protocol A we might signal IO completion too early!
 *
 * In order not to corrupt data during a resync we must make sure
 * that we do not reuse our own buffer pages (EEs) to early, therefore
 * we have the net_ee list.
 *
 * XFS seems to have problems, still, it submits pages with page_count == 0!
 * As a workaround, we disable sendpage on pages
 * with page_count == 0 or PageSlab.
 */
STATIC int _drbd_no_send_page(struct drbd_conf *mdev, struct page *page,
		   int offset, size_t size)
{
       int ret;
       ret = drbd_send(mdev, mdev->data.socket, kmap(page) + offset, size, 0);
       kunmap(page);
       return ret;
}

int _drbd_send_page(struct drbd_conf *mdev, struct page *page,
		    int offset, size_t size)
{
	mm_segment_t oldfs = get_fs();
	int sent, ok;
	int len = size;

#ifdef SHOW_SENDPAGE_USAGE
	unsigned long now = jiffies;
	static unsigned long total;
	static unsigned long fallback;
	static unsigned long last_rep;

	/* report statistics every hour,
	 * if we had at least one fallback.
	 */
	++total;
	if (fallback && time_before(last_rep+3600*HZ, now)) {
		last_rep = now;
		printk(KERN_INFO "drbd: sendpage() omitted: %lu/%lu\n",
			fallback, total);
	}
#endif

	/* PARANOIA. if this ever triggers,
	 * something in the layers above us is really kaputt.
	 *one roundtrip later:
	 * doh. it triggered. so XFS _IS_ really kaputt ...
	 * oh well...
	 */
	if ((page_count(page) < 1) || PageSlab(page)) {
		/* e.g. XFS meta- & log-data is in slab pages, which have a
		 * page_count of 0 and/or have PageSlab() set...
		 */
#ifdef SHOW_SENDPAGE_USAGE
		++fallback;
#endif
		sent = _drbd_no_send_page(mdev, page, offset, size);
		if (likely(sent > 0))
			len -= sent;
		goto out;
	}

	set_fs(KERNEL_DS);
	do {
		sent = mdev->data.socket->ops->sendpage(mdev->data.socket, page,
							offset, len,
							MSG_NOSIGNAL);
		if (sent == -EAGAIN) {
			if (we_should_drop_the_connection(mdev,
							  mdev->data.socket))
				break;
			else
				continue;
		}
		if (sent <= 0) {
			drbd_WARN("%s: size=%d len=%d sent=%d\n",
			     __func__, (int)size, len, sent);
			break;
		}
		len    -= sent;
		offset += sent;
		/* FIXME test "last_received" ... */
	} while (len > 0 /* THINK && mdev->cstate >= Connected*/);
	set_fs(oldfs);

out:
	ok = (len == 0);
	if (likely(ok))
		mdev->send_cnt += size>>9;
	return ok;
}

static inline int _drbd_send_bio(struct drbd_conf *mdev, struct bio *bio)
{
	struct bio_vec *bvec;
	int i;
	__bio_for_each_segment(bvec, bio, i, 0) {
		if (!_drbd_no_send_page(mdev, bvec->bv_page,
				     bvec->bv_offset, bvec->bv_len))
			return 0;
	}
	return 1;
}

static inline int _drbd_send_zc_bio(struct drbd_conf *mdev, struct bio *bio)
{
	struct bio_vec *bvec;
	int i;
	__bio_for_each_segment(bvec, bio, i, 0) {
		if (!_drbd_send_page(mdev, bvec->bv_page,
				     bvec->bv_offset, bvec->bv_len))
			return 0;
	}

	return 1;
}

/* Used to send write requests
 * Primary -> Peer	(Data)
 */
int drbd_send_dblock(struct drbd_conf *mdev, struct drbd_request *req)
{
	int ok = 1;
	struct Drbd_Data_Packet p;
	unsigned int dp_flags = 0;
	void *dgb;
	int dgs;

	if (!drbd_get_data_sock(mdev))
		return 0;

	dgs = (mdev->agreed_pro_version >= 87 && mdev->integrity_w_tfm) ?
		crypto_hash_digestsize(mdev->integrity_w_tfm) : 0;

	p.head.magic   = BE_DRBD_MAGIC;
	p.head.command = cpu_to_be16(Data);
	p.head.length  =
		cpu_to_be16(sizeof(p) - sizeof(struct Drbd_Header) + dgs + req->size);

	p.sector   = cpu_to_be64(req->sector);
	p.block_id = (unsigned long)req;
	p.seq_num  = cpu_to_be32(req->seq_num =
				 atomic_add_return(1, &mdev->packet_seq));
	dp_flags = 0;

	/* NOTE: no need to check if barriers supported here as we would
	 *       not pass the test in make_request_common in that case
	 */
	if (bio_barrier(req->master_bio))
		dp_flags |= DP_HARDBARRIER;
	if (bio_sync(req->master_bio))
		dp_flags |= DP_RW_SYNC;
	if (mdev->state.conn >= SyncSource &&
	    mdev->state.conn <= PausedSyncT)
		dp_flags |= DP_MAY_SET_IN_SYNC;

	p.dp_flags = cpu_to_be32(dp_flags);
	dump_packet(mdev, mdev->data.socket, 0, (void *)&p, __FILE__, __LINE__);
	set_bit(UNPLUG_REMOTE, &mdev->flags);
	ok = (sizeof(p) ==
		drbd_send(mdev, mdev->data.socket, &p, sizeof(p), MSG_MORE));
	if (ok && dgs) {
		dgb = mdev->int_dig_out;
		drbd_csum(mdev, mdev->integrity_w_tfm, req->master_bio, dgb);
		ok = drbd_send(mdev, mdev->data.socket, dgb, dgs, MSG_MORE);
	}
	if (ok) {
		if (mdev->net_conf->wire_protocol == DRBD_PROT_A)
			ok = _drbd_send_bio(mdev, req->master_bio);
		else
			ok = _drbd_send_zc_bio(mdev, req->master_bio);
	}

	drbd_put_data_sock(mdev);
	return ok;
}

/* answer packet, used to send data back for read requests:
 *  Peer       -> (diskless) Primary   (DataReply)
 *  SyncSource -> SyncTarget         (RSDataReply)
 */
int drbd_send_block(struct drbd_conf *mdev, enum Drbd_Packet_Cmd cmd,
		    struct Tl_epoch_entry *e)
{
	int ok;
	struct Drbd_Data_Packet p;
	void *dgb;
	int dgs;

	dgs = (mdev->agreed_pro_version >= 87 && mdev->integrity_w_tfm) ?
		crypto_hash_digestsize(mdev->integrity_w_tfm) : 0;

	p.head.magic   = BE_DRBD_MAGIC;
	p.head.command = cpu_to_be16(cmd);
	p.head.length  =
		cpu_to_be16(sizeof(p) - sizeof(struct Drbd_Header) + dgs + e->size);

	p.sector   = cpu_to_be64(e->sector);
	p.block_id = e->block_id;
	/* p.seq_num  = 0;    No sequence numbers here.. */

	/* Only called by our kernel thread.
	 * This one may be interupted by DRBD_SIG and/or DRBD_SIGKILL
	 * in response to admin command or module unload.
	 */
	if (!drbd_get_data_sock(mdev))
		return 0;

	dump_packet(mdev, mdev->data.socket, 0, (void *)&p, __FILE__, __LINE__);
	ok = sizeof(p) == drbd_send(mdev, mdev->data.socket, &p,
					sizeof(p), MSG_MORE);
	if (ok && dgs) {
		dgb = mdev->int_dig_out;
		drbd_csum(mdev, mdev->integrity_w_tfm, e->private_bio, dgb);
		ok = drbd_send(mdev, mdev->data.socket, dgb, dgs, MSG_MORE);
	}
	if (ok)
		ok = _drbd_send_zc_bio(mdev, e->private_bio);

	drbd_put_data_sock(mdev);
	return ok;
}

/*
  drbd_send distinguishes two cases:

  Packets sent via the data socket "sock"
  and packets sent via the meta data socket "msock"

		    sock                      msock
  -----------------+-------------------------+------------------------------
  timeout           conf.timeout / 2          conf.timeout / 2
  timeout action    send a ping via msock     Abort communication
					      and close all sockets
*/

/*
 * you must have down()ed the appropriate [m]sock_mutex elsewhere!
 */
int drbd_send(struct drbd_conf *mdev, struct socket *sock,
	      void *buf, size_t size, unsigned msg_flags)
{
#if !HAVE_KERNEL_SENDMSG
	mm_segment_t oldfs;
	struct iovec iov;
#else
	struct kvec iov;
#endif
	struct msghdr msg;
	int rv, sent = 0;

	if (!sock)
		return -1000;

	/* THINK  if (signal_pending) return ... ? */

	iov.iov_base = buf;
	iov.iov_len  = size;

	msg.msg_name       = NULL;
	msg.msg_namelen    = 0;
#if !HAVE_KERNEL_SENDMSG
	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
#endif
	msg.msg_control    = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags      = msg_flags | MSG_NOSIGNAL;

#if !HAVE_KERNEL_SENDMSG
	oldfs = get_fs();
	set_fs(KERNEL_DS);
#endif

	if (sock == mdev->data.socket)
		mdev->ko_count = mdev->net_conf->ko_count;
	do {
		/* STRANGE
		 * tcp_sendmsg does _not_ use its size parameter at all ?
		 *
		 * -EAGAIN on timeout, -EINTR on signal.
		 */
/* THINK
 * do we need to block DRBD_SIG if sock == &meta.socket ??
 * otherwise wake_asender() might interrupt some send_*Ack !
 */
#if !HAVE_KERNEL_SENDMSG
		rv = sock_sendmsg(sock, &msg, iov.iov_len);
#else
		rv = kernel_sendmsg(sock, &msg, &iov, 1, size);
#endif
		if (rv == -EAGAIN) {
			if (we_should_drop_the_connection(mdev, sock))
				break;
			else
				continue;
		}
		D_ASSERT(rv != 0);
		if (rv == -EINTR) {
#if 0
			/* FIXME this happens all the time.
			 * we don't care for now!
			 * eventually this should be sorted out be the proper
			 * use of the SIGNAL_ASENDER bit... */
			if (DRBD_ratelimit(5*HZ, 5)) {
				DBG("Got a signal in drbd_send(,%c,)!\n",
				    sock == mdev->meta.socket ? 'm' : 's');
				/* dump_stack(); */
			}
#endif
			flush_signals(current);
			rv = 0;
		}
		if (rv < 0)
			break;
		sent += rv;
		iov.iov_base += rv;
		iov.iov_len  -= rv;
	} while (sent < size);

#if !HAVE_KERNEL_SENDMSG
	set_fs(oldfs);
#endif

	if (rv <= 0) {
		if (rv != -EAGAIN) {
			ERR("%s_sendmsg returned %d\n",
			    sock == mdev->meta.socket ? "msock" : "sock",
			    rv);
			drbd_force_state(mdev, NS(conn, BrokenPipe));
		} else
			drbd_force_state(mdev, NS(conn, Timeout));
	}

	return sent;
}

#ifdef BD_OPS_USE_FMODE
static int drbd_open(struct block_device *bdev, fmode_t mode)
#else
static int drbd_open(struct inode *inode, struct file *file)
#endif
{
#ifdef BD_OPS_USE_FMODE
	struct drbd_conf *mdev = bdev->bd_disk->private_data;
#else
	int mode = file->f_mode;
	struct drbd_conf *mdev = inode->i_bdev->bd_disk->private_data;
#endif
	unsigned long flags;
	int rv = 0;

	spin_lock_irqsave(&mdev->req_lock, flags);
	/* to have a stable mdev->state.role
	 * and no race with updating open_cnt */

	if (mdev->state.role != Primary) {
		if (mode & FMODE_WRITE)
			rv = -EROFS;
		else if (!allow_oos)
			rv = -EMEDIUMTYPE;
	}

	if (!rv)
		mdev->open_cnt++;
	spin_unlock_irqrestore(&mdev->req_lock, flags);

	return rv;
}

#ifdef BD_OPS_USE_FMODE
static int drbd_release(struct gendisk *gd, fmode_t mode)
{
	struct drbd_conf *mdev = gd->private_data;
	mdev->open_cnt--;
	return 0;
}
#else
static int drbd_release(struct inode *inode, struct file *file)
{
	struct drbd_conf *mdev = inode->i_bdev->bd_disk->private_data;
	mdev->open_cnt--;
	return 0;
}
#endif

STATIC void drbd_unplug_fn(struct request_queue *q)
{
	struct drbd_conf *mdev = q->queuedata;

	MTRACE(TraceTypeUnplug, TraceLvlSummary,
	       INFO("got unplugged ap_bio_count=%d\n",
		    atomic_read(&mdev->ap_bio_cnt));
	       );

	/* unplug FIRST */
	spin_lock_irq(q->queue_lock);
	blk_remove_plug(q);
	spin_unlock_irq(q->queue_lock);

	/* only if connected */
	spin_lock_irq(&mdev->req_lock);
	if (mdev->state.pdsk >= Inconsistent && mdev->state.conn >= Connected) {
		D_ASSERT(mdev->state.role == Primary);
		if (test_and_clear_bit(UNPLUG_REMOTE, &mdev->flags)) {
			/* add to the data.work queue,
			 * unless already queued.
			 * XXX this might be a good addition to drbd_queue_work
			 * anyways, to detect "double queuing" ... */
			if (list_empty(&mdev->unplug_work.list))
				drbd_queue_work(&mdev->data.work,
						&mdev->unplug_work);
		}
	}
	spin_unlock_irq(&mdev->req_lock);

	if (mdev->state.disk >= Inconsistent)
		drbd_kick_lo(mdev);
}

STATIC void drbd_set_defaults(struct drbd_conf *mdev)
{
	mdev->sync_conf.after      = DRBD_AFTER_DEF;
	mdev->sync_conf.rate       = DRBD_RATE_DEF;
	mdev->sync_conf.al_extents = DRBD_AL_EXTENTS_DEF;
	mdev->state = (union drbd_state_t) {
		{ .role = Secondary,
		  .peer = Unknown,
		  .conn = StandAlone,
		  .disk = Diskless,
		  .pdsk = DUnknown,
		  .susp = 0
		} };
}

int w_bitmap_io(struct drbd_conf *mdev, struct drbd_work *w, int unused);

void drbd_init_set_defaults(struct drbd_conf *mdev)
{
	/* the memset(,0,) did most of this.
	 * note: only assignments, no allocation in here */

#ifdef PARANOIA
	SET_MDEV_MAGIC(mdev);
#endif

	drbd_set_defaults(mdev);

	/* for now, we do NOT yet support it,
	 * even though we start some framework
	 * to eventually support barriers */
	set_bit(NO_BARRIER_SUPP, &mdev->flags);

	atomic_set(&mdev->ap_bio_cnt, 0);
	atomic_set(&mdev->ap_pending_cnt, 0);
	atomic_set(&mdev->rs_pending_cnt, 0);
	atomic_set(&mdev->unacked_cnt, 0);
	atomic_set(&mdev->local_cnt, 0);
	atomic_set(&mdev->net_cnt, 0);
	atomic_set(&mdev->packet_seq, 0);
	atomic_set(&mdev->pp_in_use, 0);

	init_MUTEX(&mdev->md_io_mutex);
	init_MUTEX(&mdev->data.mutex);
	init_MUTEX(&mdev->meta.mutex);
	sema_init(&mdev->data.work.s, 0);
	sema_init(&mdev->meta.work.s, 0);
	mutex_init(&mdev->state_mutex);

	spin_lock_init(&mdev->data.work.q_lock);
	spin_lock_init(&mdev->meta.work.q_lock);

	spin_lock_init(&mdev->al_lock);
	spin_lock_init(&mdev->req_lock);
	spin_lock_init(&mdev->peer_seq_lock);
	spin_lock_init(&mdev->epoch_lock);

	INIT_LIST_HEAD(&mdev->active_ee);
	INIT_LIST_HEAD(&mdev->sync_ee);
	INIT_LIST_HEAD(&mdev->done_ee);
	INIT_LIST_HEAD(&mdev->read_ee);
	INIT_LIST_HEAD(&mdev->net_ee);
	INIT_LIST_HEAD(&mdev->resync_reads);
	INIT_LIST_HEAD(&mdev->data.work.q);
	INIT_LIST_HEAD(&mdev->meta.work.q);
	INIT_LIST_HEAD(&mdev->resync_work.list);
	INIT_LIST_HEAD(&mdev->unplug_work.list);
	INIT_LIST_HEAD(&mdev->md_sync_work.list);
	INIT_LIST_HEAD(&mdev->bm_io_work.w.list);
	mdev->resync_work.cb  = w_resync_inactive;
	mdev->unplug_work.cb  = w_send_write_hint;
	mdev->md_sync_work.cb = w_md_sync;
	mdev->bm_io_work.w.cb = w_bitmap_io;
	init_timer(&mdev->resync_timer);
	init_timer(&mdev->md_sync_timer);
	mdev->resync_timer.function = resync_timer_fn;
	mdev->resync_timer.data = (unsigned long) mdev;
	mdev->md_sync_timer.function = md_sync_timer_fn;
	mdev->md_sync_timer.data = (unsigned long) mdev;

	init_waitqueue_head(&mdev->misc_wait);
	init_waitqueue_head(&mdev->state_wait);
	init_waitqueue_head(&mdev->ee_wait);
	init_waitqueue_head(&mdev->al_wait);
	init_waitqueue_head(&mdev->seq_wait);

	drbd_thread_init(mdev, &mdev->receiver, drbdd_init);
	drbd_thread_init(mdev, &mdev->worker, drbd_worker);
	drbd_thread_init(mdev, &mdev->asender, drbd_asender);

	mdev->agreed_pro_version = PRO_VERSION_MAX;
	mdev->write_ordering = WO_bio_barrier;
#ifdef __arch_um__
	INFO("mdev = 0x%p\n", mdev);
#endif
	mdev->resync_wenr = LC_FREE;
}

void drbd_mdev_cleanup(struct drbd_conf *mdev)
{
	/* I'd like to cleanup completely, and memset(,0,) it.
	 * but I'd have to reinit it.
	 * FIXME: do the right thing...
	 */

	/* list of things that may still
	 * hold data of the previous config

	 * act_log        ** re-initialized in set_disk
	 * on_io_error

	 * al_tr_cycle    ** re-initialized in ... FIXME??
	 * al_tr_number
	 * al_tr_pos

	 * backing_bdev   ** re-initialized in drbd_free_ll_dev
	 * lo_file
	 * md_bdev
	 * md_file
	 * md_index

	 * ko_count       ** re-initialized in set_net

	 * last_received  ** currently ignored

	 * mbds_id        ** re-initialized in ... FIXME??

	 * resync         ** re-initialized in ... FIXME??

	*** no re-init necessary (?) ***
	 * md_io_page
	 * this_bdev

	 * vdisk             ?

	 * rq_queue       ** FIXME ASSERT ??
	 * newest_barrier
	 * oldest_barrier
	 */

	if (mdev->receiver.t_state != None)
		ERR("ASSERT FAILED: receiver t_state == %d expected 0.\n",
				mdev->receiver.t_state);

	crypto_free_hash(mdev->csums_tfm);
	mdev->csums_tfm = NULL;

	crypto_free_hash(mdev->verify_tfm);
	mdev->verify_tfm = NULL;

	crypto_free_hash(mdev->integrity_w_tfm);
	mdev->integrity_w_tfm = NULL;

	crypto_free_hash(mdev->integrity_r_tfm);
	mdev->integrity_r_tfm = NULL;
	/* no need to lock it, I'm the only thread alive */
	if (atomic_read(&mdev->current_epoch->epoch_size) !=  0)
		ERR("epoch_size:%d\n", atomic_read(&mdev->current_epoch->epoch_size));
	mdev->al_writ_cnt  =
	mdev->bm_writ_cnt  =
	mdev->read_cnt     =
	mdev->recv_cnt     =
	mdev->send_cnt     =
	mdev->writ_cnt     =
	mdev->p_size       =
	mdev->rs_start     =
	mdev->rs_total     =
	mdev->rs_failed    =
	mdev->rs_mark_left =
	mdev->rs_mark_time = 0;
	D_ASSERT(mdev->net_conf == NULL);
	drbd_set_my_capacity(mdev, 0);
	drbd_bm_resize(mdev, 0);
	drbd_bm_cleanup(mdev);

	/* just in case */
	drbd_free_resources(mdev);

	/*
	 * currently we drbd_init_ee only on module load, so
	 * we may do drbd_release_ee only on module unload!
	 */
	D_ASSERT(list_empty(&mdev->active_ee));
	D_ASSERT(list_empty(&mdev->sync_ee));
	D_ASSERT(list_empty(&mdev->done_ee));
	D_ASSERT(list_empty(&mdev->read_ee));
	D_ASSERT(list_empty(&mdev->net_ee));
	D_ASSERT(list_empty(&mdev->resync_reads));
	D_ASSERT(list_empty(&mdev->data.work.q));
	D_ASSERT(list_empty(&mdev->meta.work.q));
	D_ASSERT(list_empty(&mdev->resync_work.list));
	D_ASSERT(list_empty(&mdev->unplug_work.list));

}


STATIC void drbd_destroy_mempools(void)
{
	struct page *page;

	while (drbd_pp_pool) {
		page = drbd_pp_pool;
		drbd_pp_pool = (struct page *)page_private(page);
		__free_page(page);
		drbd_pp_vacant--;
	}

	/* D_ASSERT(atomic_read(&drbd_pp_vacant)==0); */

	if (drbd_ee_mempool)
		mempool_destroy(drbd_ee_mempool);
	if (drbd_request_mempool)
		mempool_destroy(drbd_request_mempool);
	if (drbd_ee_cache)
		kmem_cache_destroy(drbd_ee_cache);
	if (drbd_request_cache)
		kmem_cache_destroy(drbd_request_cache);

	drbd_ee_mempool      = NULL;
	drbd_request_mempool = NULL;
	drbd_ee_cache        = NULL;
	drbd_request_cache   = NULL;

	return;
}

STATIC int drbd_create_mempools(void)
{
	struct page *page;
	const int number = (DRBD_MAX_SEGMENT_SIZE/PAGE_SIZE) * minor_count;
	int i;

	/* prepare our caches and mempools */
	drbd_request_mempool = NULL;
	drbd_ee_cache        = NULL;
	drbd_request_cache   = NULL;
	drbd_pp_pool         = NULL;

	/* caches */
	drbd_request_cache = kmem_cache_create(
		"drbd_req_cache", sizeof(struct drbd_request), 0, 0, NULL);
	if (drbd_request_cache == NULL)
		goto Enomem;

	drbd_ee_cache = kmem_cache_create(
		"drbd_ee_cache", sizeof(struct Tl_epoch_entry), 0, 0, NULL);
	if (drbd_ee_cache == NULL)
		goto Enomem;

	/* mempools */
	drbd_request_mempool = mempool_create(number,
		mempool_alloc_slab, mempool_free_slab, drbd_request_cache);
	if (drbd_request_mempool == NULL)
		goto Enomem;

	drbd_ee_mempool = mempool_create(number,
		mempool_alloc_slab, mempool_free_slab, drbd_ee_cache);
	if (drbd_request_mempool == NULL)
		goto Enomem;

	/* drbd's page pool */
	spin_lock_init(&drbd_pp_lock);

	for (i = 0; i < number; i++) {
		page = alloc_page(GFP_HIGHUSER);
		if (!page)
			goto Enomem;
		set_page_private(page, (unsigned long)drbd_pp_pool);
		drbd_pp_pool = page;
	}
	drbd_pp_vacant = number;

	return 0;

Enomem:
	drbd_destroy_mempools(); /* in case we allocated some */
	return -ENOMEM;
}

STATIC int drbd_notify_sys(struct notifier_block *this, unsigned long code,
	void *unused)
{
	/* just so we have it.  you never know what interessting things we
	 * might want to do here some day...
	 */

	return NOTIFY_DONE;
}

STATIC struct notifier_block drbd_notifier = {
	.notifier_call = drbd_notify_sys,
};


STATIC void drbd_cleanup(void)
{
	int i, rr;

	unregister_reboot_notifier(&drbd_notifier);

	drbd_nl_cleanup();

	if (minor_table) {
		if (drbd_proc)
			remove_proc_entry("drbd", NULL);
		i = minor_count;
		while (i--) {
			struct drbd_conf *mdev = minor_to_mdev(i);
			struct gendisk  **disk = &mdev->vdisk;
			struct request_queue **q = &mdev->rq_queue;

			if (!mdev)
				continue;
			drbd_free_resources(mdev);

			if (*disk) {
				del_gendisk(*disk);
				put_disk(*disk);
				*disk = NULL;
			}
			if (*q)
				blk_cleanup_queue(*q);
			*q = NULL;

			D_ASSERT(mdev->open_cnt == 0);
			if (mdev->this_bdev)
				bdput(mdev->this_bdev);

			tl_cleanup(mdev);
			if (mdev->bitmap)
				drbd_bm_cleanup(mdev);
			if (mdev->resync)
				lc_free(mdev->resync);

			rr = drbd_release_ee(mdev, &mdev->active_ee);
			if (rr)
				ERR("%d EEs in active list found!\n", rr);

			rr = drbd_release_ee(mdev, &mdev->sync_ee);
			if (rr)
				ERR("%d EEs in sync list found!\n", rr);

			rr = drbd_release_ee(mdev, &mdev->read_ee);
			if (rr)
				ERR("%d EEs in read list found!\n", rr);

			rr = drbd_release_ee(mdev, &mdev->done_ee);
			if (rr)
				ERR("%d EEs in done list found!\n", rr);

			rr = drbd_release_ee(mdev, &mdev->net_ee);
			if (rr)
				ERR("%d EEs in net list found!\n", rr);

			ERR_IF (!list_empty(&mdev->data.work.q)) {
				struct list_head *lp;
				list_for_each(lp, &mdev->data.work.q) {
					DUMPP(lp);
				}
			};

			if (mdev->md_io_page)
				__free_page(mdev->md_io_page);

			if (mdev->md_io_tmpp)
				__free_page(mdev->md_io_tmpp);

			if (mdev->act_log)
				lc_free(mdev->act_log);

			kfree(mdev->ee_hash);
			mdev->ee_hash_s = 0;
			mdev->ee_hash = NULL;

			kfree(mdev->tl_hash);
			mdev->tl_hash_s = 0;
			mdev->tl_hash = NULL;

			kfree(mdev->app_reads_hash);
			mdev->app_reads_hash = NULL;

			kfree(mdev->p_uuid);
			mdev->p_uuid = NULL;

			kfree(mdev->int_dig_out);
			kfree(mdev->int_dig_in);
			kfree(mdev->int_dig_vv);

			kfree(mdev->current_epoch);
		}
		drbd_destroy_mempools();
	}

	kfree(minor_table);

	drbd_unregister_blkdev(DRBD_MAJOR, "drbd");

	printk(KERN_INFO "drbd: module cleanup done.\n");
}

struct drbd_conf *drbd_new_device(int minor)
{
	struct drbd_conf *mdev = NULL;
	struct gendisk *disk;
	struct request_queue *q;

	mdev = kzalloc(sizeof(struct drbd_conf), GFP_KERNEL);
	if (!mdev)
		goto Enomem;

	mdev->minor = minor;

	drbd_init_set_defaults(mdev);

	q = blk_alloc_queue(GFP_KERNEL);
	if (!q)
		goto Enomem;
	mdev->rq_queue = q;
	q->queuedata   = mdev;
	q->max_segment_size = DRBD_MAX_SEGMENT_SIZE;

	disk = alloc_disk(1);
	if (!disk)
		goto Enomem;
	mdev->vdisk = disk;

	set_disk_ro(disk, TRUE);

	disk->queue = q;
	disk->major = DRBD_MAJOR;
	disk->first_minor = minor;
	disk->fops = &drbd_ops;
	sprintf(disk->disk_name, "drbd%d", minor);
	disk->private_data = mdev;
	add_disk(disk);

	mdev->this_bdev = bdget(MKDEV(DRBD_MAJOR, minor));
	/* we have no partitions. we contain only ourselves. */
	mdev->this_bdev->bd_contains = mdev->this_bdev;

	blk_queue_make_request(q, drbd_make_request_26);
	blk_queue_bounce_limit(q, BLK_BOUNCE_ANY);
	blk_queue_merge_bvec(q, drbd_merge_bvec);
	q->queue_lock = &mdev->req_lock; /* needed since we use */
		/* plugging on a queue, that actually has no requests! */
	q->unplug_fn = drbd_unplug_fn;

	mdev->md_io_page = alloc_page(GFP_KERNEL);
	if (!mdev->md_io_page)
		goto Enomem;

	if (drbd_bm_init(mdev))
		goto Enomem;
	/* no need to lock access, we are still initializing the module. */
	if (!tl_init(mdev))
		goto Enomem;

	mdev->app_reads_hash = kzalloc(APP_R_HSIZE*sizeof(void *), GFP_KERNEL);
	if (!mdev->app_reads_hash)
		goto Enomem;

	mdev->current_epoch = kzalloc(sizeof(struct drbd_epoch), GFP_KERNEL);
	INIT_LIST_HEAD(&mdev->current_epoch->list);
	mdev->epochs = 1;

	return mdev;

 Enomem:
	if (mdev) {
		kfree(mdev->app_reads_hash);
		if (mdev->md_io_page)
			__free_page(mdev->md_io_page);
		kfree(mdev->current_epoch);
		kfree(mdev);
	}
	return NULL;
}

int __init drbd_init(void)
{
	int err;

#ifdef __arch_um__
	printk(KERN_INFO "drbd_module = 0x%p core = 0x%p\n",
	       THIS_MODULE, THIS_MODULE->module_core);
#endif

	if (sizeof(struct Drbd_HandShake_Packet) != 80) {
		printk(KERN_ERR
		       "drbd: never change the size or layout "
		       "of the HandShake packet.\n");
		return -EINVAL;
	}

	if (1 > minor_count || minor_count > 255) {
		printk(KERN_ERR
			"drbd: invalid minor_count (%d)\n", minor_count);
#ifdef MODULE
		return -EINVAL;
#else
		minor_count = 8;
#endif
	}

	err = drbd_nl_init();
	if (err)
		return err;

	err = register_blkdev(DRBD_MAJOR, "drbd");
	if (err) {
		printk(KERN_ERR
		       "drbd: unable to register block device major %d\n",
		       DRBD_MAJOR);
		return err;
	}

	register_reboot_notifier(&drbd_notifier);

	/*
	 * allocate all necessary structs
	 */
	err = -ENOMEM;

	init_waitqueue_head(&drbd_pp_wait);

	drbd_proc = NULL; /* play safe for drbd_cleanup */
	minor_table = kzalloc(sizeof(struct drbd_conf *)*minor_count,
				GFP_KERNEL);
	if (!minor_table)
		goto Enomem;

	err = drbd_create_mempools();
	if (err)
		goto Enomem;

#if CONFIG_PROC_FS
	/*
	 * register with procfs
	 */
	drbd_proc = create_proc_entry("drbd",  S_IFREG | S_IRUGO , NULL);

	if (!drbd_proc)	{
		printk(KERN_ERR "drbd: unable to register proc file\n");
		goto Enomem;
	}

	drbd_proc->proc_fops = &drbd_proc_fops;
	drbd_proc->owner = THIS_MODULE;
#else
# error "Currently drbd depends on the proc file system (CONFIG_PROC_FS)"
#endif

	printk(KERN_INFO "drbd: initialised. "
	       "Version: " REL_VERSION " (api:%d/proto:%d-%d)\n",
	       API_VERSION, PRO_VERSION_MIN, PRO_VERSION_MAX);
	printk(KERN_INFO "drbd: %s\n", drbd_buildtag());
	printk(KERN_INFO "drbd: registered as block device major %d\n",
		DRBD_MAJOR);
	printk(KERN_INFO "drbd: minor_table @ 0x%p\n", minor_table);

	return 0; /* Success! */

Enomem:
	drbd_cleanup();
	if (err == -ENOMEM)
		/* currently always the case */
		printk(KERN_ERR "drbd: ran out of memory\n");
	else
		printk(KERN_ERR "drbd: initialization failure\n");
	return err;
}

void drbd_free_bc(struct drbd_backing_dev *bc)
{
	if (bc == NULL)
		return;

	bd_release(bc->backing_bdev);
	bd_release(bc->md_bdev);

	fput(bc->lo_file);
	fput(bc->md_file);

	kfree(bc);
}

void drbd_free_sock(struct drbd_conf *mdev)
{
	if (mdev->data.socket) {
		sock_release(mdev->data.socket);
		mdev->data.socket = NULL;
	}
	if (mdev->meta.socket) {
		sock_release(mdev->meta.socket);
		mdev->meta.socket = NULL;
	}
}


void drbd_free_resources(struct drbd_conf *mdev)
{
	crypto_free_hash(mdev->cram_hmac_tfm);
	mdev->cram_hmac_tfm = NULL;
	crypto_free_hash(mdev->integrity_w_tfm);
	mdev->integrity_w_tfm=NULL;
	crypto_free_hash(mdev->integrity_r_tfm);
	mdev->integrity_r_tfm=NULL;
	drbd_free_sock(mdev);
	__no_warn(local,
		  drbd_free_bc(mdev->bc);
		  mdev->bc = NULL;);
}

/*********************************/
/* meta data management */

struct meta_data_on_disk {
	u64 la_size;           /* last agreed size. */
	u64 uuid[UUID_SIZE];   /* UUIDs. */
	u64 device_uuid;
	u64 reserved_u64_1;
	u32 flags;             /* MDF */
	u32 magic;
	u32 md_size_sect;
	u32 al_offset;         /* offset to this block */
	u32 al_nr_extents;     /* important for restoring the AL */
	      /* `-- act_log->nr_elements <-- sync_conf.al_extents */
	u32 bm_offset;         /* offset to the bitmap, from here */
	u32 bm_bytes_per_bit;  /* BM_BLOCK_SIZE */
	u32 reserved_u32[4];

} __attribute((packed));

/**
 * drbd_md_sync:
 * Writes the meta data super block if the MD_DIRTY flag bit is set.
 */
void drbd_md_sync(struct drbd_conf *mdev)
{
	struct meta_data_on_disk *buffer;
	sector_t sector;
	int i;

	if (!test_and_clear_bit(MD_DIRTY, &mdev->flags))
		return;
	del_timer(&mdev->md_sync_timer);

	/* We use here Failed and not Attaching because we try to write
	 * metadata even if we detach due to a disk failure! */
	if (!inc_local_if_state(mdev, Failed))
		return;

	MTRACE(TraceTypeMDIO, TraceLvlSummary,
	       INFO("Writing meta data super block now.\n");
	       );

	down(&mdev->md_io_mutex);
	buffer = (struct meta_data_on_disk *)page_address(mdev->md_io_page);
	memset(buffer, 0, 512);

	buffer->la_size = cpu_to_be64(drbd_get_capacity(mdev->this_bdev));
	for (i = Current; i < UUID_SIZE; i++)
		buffer->uuid[i] = cpu_to_be64(mdev->bc->md.uuid[i]);
	buffer->flags = cpu_to_be32(mdev->bc->md.flags);
	buffer->magic = cpu_to_be32(DRBD_MD_MAGIC);

	buffer->md_size_sect  = cpu_to_be32(mdev->bc->md.md_size_sect);
	buffer->al_offset     = cpu_to_be32(mdev->bc->md.al_offset);
	buffer->al_nr_extents = cpu_to_be32(mdev->act_log->nr_elements);
	buffer->bm_bytes_per_bit = cpu_to_be32(BM_BLOCK_SIZE);
	buffer->device_uuid = cpu_to_be64(mdev->bc->md.device_uuid);

	buffer->bm_offset = cpu_to_be32(mdev->bc->md.bm_offset);

	D_ASSERT(drbd_md_ss__(mdev, mdev->bc) == mdev->bc->md.md_offset);
	sector = mdev->bc->md.md_offset;

	if (drbd_md_sync_page_io(mdev, mdev->bc, sector, WRITE)) {
		clear_bit(MD_DIRTY, &mdev->flags);
	} else {
		/* this was a try anyways ... */
		ERR("meta data update failed!\n");

		drbd_chk_io_error(mdev, 1, TRUE);
		drbd_io_error(mdev, TRUE);
	}

	/* Update mdev->bc->md.la_size_sect,
	 * since we updated it on metadata. */
	mdev->bc->md.la_size_sect = drbd_get_capacity(mdev->this_bdev);

	up(&mdev->md_io_mutex);
	dec_local(mdev);
}

/**
 * drbd_md_read:
 * @bdev: describes the backing storage and the meta-data storage
 * Reads the meta data from bdev. Return 0 (NoError) on success, and an
 * enum ret_codes in case something goes wrong.
 * Currently only: MDIOError, MDInvalid.
 */
int drbd_md_read(struct drbd_conf *mdev, struct drbd_backing_dev *bdev)
{
	struct meta_data_on_disk *buffer;
	int i, rv = NoError;

	if (!inc_local_if_state(mdev, Attaching))
		return MDIOError;

	down(&mdev->md_io_mutex);
	buffer = (struct meta_data_on_disk *)page_address(mdev->md_io_page);

	if (!drbd_md_sync_page_io(mdev, bdev, bdev->md.md_offset, READ)) {
		/* NOTE: cant do normal error processing here as this is
		   called BEFORE disk is attached */
		ERR("Error while reading metadata.\n");
		rv = MDIOError;
		goto err;
	}

	if (be32_to_cpu(buffer->magic) != DRBD_MD_MAGIC) {
		ERR("Error while reading metadata, magic not found.\n");
		rv = MDInvalid;
		goto err;
	}
	if (be32_to_cpu(buffer->al_offset) != bdev->md.al_offset) {
		ERR("unexpected al_offset: %d (expected %d)\n",
		    be32_to_cpu(buffer->al_offset), bdev->md.al_offset);
		rv = MDInvalid;
		goto err;
	}
	if (be32_to_cpu(buffer->bm_offset) != bdev->md.bm_offset) {
		ERR("unexpected bm_offset: %d (expected %d)\n",
		    be32_to_cpu(buffer->bm_offset), bdev->md.bm_offset);
		rv = MDInvalid;
		goto err;
	}
	if (be32_to_cpu(buffer->md_size_sect) != bdev->md.md_size_sect) {
		ERR("unexpected md_size: %u (expected %u)\n",
		    be32_to_cpu(buffer->md_size_sect), bdev->md.md_size_sect);
		rv = MDInvalid;
		goto err;
	}

	if (be32_to_cpu(buffer->bm_bytes_per_bit) != BM_BLOCK_SIZE) {
		ERR("unexpected bm_bytes_per_bit: %u (expected %u)\n",
		    be32_to_cpu(buffer->bm_bytes_per_bit), BM_BLOCK_SIZE);
		rv = MDInvalid;
		goto err;
	}

	bdev->md.la_size_sect = be64_to_cpu(buffer->la_size);
	for (i = Current; i < UUID_SIZE; i++)
		bdev->md.uuid[i] = be64_to_cpu(buffer->uuid[i]);
	bdev->md.flags = be32_to_cpu(buffer->flags);
	mdev->sync_conf.al_extents = be32_to_cpu(buffer->al_nr_extents);
	bdev->md.device_uuid = be64_to_cpu(buffer->device_uuid);

	if (mdev->sync_conf.al_extents < 7)
		mdev->sync_conf.al_extents = 127;
		/* FIXME if this ever happens when reading meta data,
		 * it possibly screws up reading of the activity log?
		 */

 err:
	up(&mdev->md_io_mutex);
	dec_local(mdev);

	return rv;
}

/**
 * drbd_md_mark_dirty:
 * Call this function if you change enything that should be written to
 * the meta-data super block. This function sets MD_DIRTY, and starts a
 * timer that ensures that within five seconds you have to call drbd_md_sync().
 */
void drbd_md_mark_dirty(struct drbd_conf *mdev)
{
	set_bit(MD_DIRTY, &mdev->flags);
	mod_timer(&mdev->md_sync_timer, jiffies + 5*HZ);
}


STATIC void drbd_uuid_move_history(struct drbd_conf *mdev) __must_hold(local)
{
	int i;

	for (i = History_start; i < History_end; i++) {
		mdev->bc->md.uuid[i+1] = mdev->bc->md.uuid[i];

		MTRACE(TraceTypeUuid, TraceLvlAll,
		       drbd_print_uuid(mdev, i+1);
			);
	}
}

void _drbd_uuid_set(struct drbd_conf *mdev, int idx, u64 val) __must_hold(local)
{
	if (idx == Current) {
		if (mdev->state.role == Primary)
			val |= 1;
		else
			val &= ~((u64)1);

		drbd_set_ed_uuid(mdev, val);
	}

	mdev->bc->md.uuid[idx] = val;

	MTRACE(TraceTypeUuid, TraceLvlSummary,
	       drbd_print_uuid(mdev, idx);
		);

	drbd_md_mark_dirty(mdev);
}


void drbd_uuid_set(struct drbd_conf *mdev, int idx, u64 val) __must_hold(local)
{
	if (mdev->bc->md.uuid[idx]) {
		drbd_uuid_move_history(mdev);
		mdev->bc->md.uuid[History_start] = mdev->bc->md.uuid[idx];
		MTRACE(TraceTypeUuid, TraceLvlMetrics,
		       drbd_print_uuid(mdev, History_start);
			);
	}
	_drbd_uuid_set(mdev, idx, val);
}

/**
 * _drbd_uuid_new_current:
 * Creates a new current UUID, but does NOT rotate the old current
 * UUID into the bitmap slot (but into history). This causes a full
 * sync upon next connect. Aditionally the full sync is also requested
 * by the FullSync bit.
 */
void _drbd_uuid_new_current(struct drbd_conf *mdev) __must_hold(local)
{
	u64 uuid;

	/* Actually a seperate bit names DisklessPeer, would be
	   the right thing. But for now the FullSync bit is a
	   working substitute, to avoid repetitive generating
	   of new current UUIDs in case we loose connection
	   and reconnect in a loop. */
	if (mdev->bc->md.flags & MDF_FullSync)
		return;
	INFO("Creating new current UUID [no BitMap]\n");
	get_random_bytes(&uuid, sizeof(u64));
	drbd_uuid_set(mdev, Current, uuid);
	drbd_md_set_flag(mdev, MDF_FullSync);
}

/**
 * drbd_uuid_new_current:
 * Creates a new current UUID, and rotates the old current UUID into
 * the bitmap slot. Causes an incremental resync upon next connect.
 */
void drbd_uuid_new_current(struct drbd_conf *mdev) __must_hold(local)
{
	u64 val;

	INFO("Creating new current UUID\n");
	D_ASSERT(mdev->bc->md.uuid[Bitmap] == 0);
	mdev->bc->md.uuid[Bitmap] = mdev->bc->md.uuid[Current];
	MTRACE(TraceTypeUuid, TraceLvlMetrics,
	       drbd_print_uuid(mdev, Bitmap);
		);

	get_random_bytes(&val, sizeof(u64));
	_drbd_uuid_set(mdev, Current, val);
}

void drbd_uuid_set_bm(struct drbd_conf *mdev, u64 val) __must_hold(local)
{
	if (mdev->bc->md.uuid[Bitmap] == 0 && val == 0)
		return;

	if (val == 0) {
		drbd_uuid_move_history(mdev);
		mdev->bc->md.uuid[History_start] = mdev->bc->md.uuid[Bitmap];
		mdev->bc->md.uuid[Bitmap] = 0;

		MTRACE(TraceTypeUuid, TraceLvlMetrics,
		       drbd_print_uuid(mdev, History_start);
		       drbd_print_uuid(mdev, Bitmap);
			);
	} else {
		if (mdev->bc->md.uuid[Bitmap])
			drbd_WARN("bm UUID already set");

		mdev->bc->md.uuid[Bitmap] = val;
		mdev->bc->md.uuid[Bitmap] &= ~((u64)1);

		MTRACE(TraceTypeUuid, TraceLvlMetrics,
		       drbd_print_uuid(mdev, Bitmap);
			);
	}
	drbd_md_mark_dirty(mdev);
}

/**
 * drbd_bmio_set_n_write:
 * Is an io_fn for drbd_queue_bitmap_io() or drbd_bitmap_io() that sets
 * all bits in the bitmap and writes the whole bitmap to stable storage.
 */
int drbd_bmio_set_n_write(struct drbd_conf *mdev)
{
	int rv = -EIO;

	if (inc_local_if_state(mdev, Attaching)) {
		drbd_md_set_flag(mdev, MDF_FullSync);
		drbd_md_sync(mdev);
		drbd_bm_set_all(mdev);

		rv = drbd_bm_write(mdev);

		if (!rv) {
			drbd_md_clear_flag(mdev, MDF_FullSync);
			drbd_md_sync(mdev);
		}

		dec_local(mdev);
	}

	return rv;
}

/**
 * drbd_bmio_clear_n_write:
 * Is an io_fn for drbd_queue_bitmap_io() or drbd_bitmap_io() that clears
 * all bits in the bitmap and writes the whole bitmap to stable storage.
 */
int drbd_bmio_clear_n_write(struct drbd_conf *mdev)
{
	int rv = -EIO;

	if (inc_local_if_state(mdev, Attaching)) {
		drbd_bm_clear_all(mdev);
		rv = drbd_bm_write(mdev);
		dec_local(mdev);
	}

	return rv;
}

int w_bitmap_io(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct bm_io_work *work = (struct bm_io_work *)w;
	int rv;

	D_ASSERT(atomic_read(&mdev->ap_bio_cnt) == 0);

	drbd_bm_lock(mdev, work->why);
	rv = work->io_fn(mdev);
	drbd_bm_unlock(mdev);

	clear_bit(BITMAP_IO, &mdev->flags);
	wake_up(&mdev->misc_wait);

	if (work->done)
		work->done(mdev, rv);

	clear_bit(BITMAP_IO_QUEUED, &mdev->flags);
	work->why = NULL;

	return 1;
}

/**
 * drbd_queue_bitmap_io:
 * Queues an IO operation on the whole bitmap.
 * While IO on the bitmap happens we freeze appliation IO thus we ensure
 * that drbd_set_out_of_sync() can not be called.
 * This function MUST ONLY be called from worker context.
 * BAD API ALERT!
 * It MUST NOT be used while a previous such work is still pending!
 */
void drbd_queue_bitmap_io(struct drbd_conf *mdev,
			  int (*io_fn)(struct drbd_conf *),
			  void (*done)(struct drbd_conf *, int),
			  char *why)
{
	D_ASSERT(current == mdev->worker.task);

	D_ASSERT(!test_bit(BITMAP_IO_QUEUED, &mdev->flags));
	D_ASSERT(!test_bit(BITMAP_IO, &mdev->flags));
	D_ASSERT(list_empty(&mdev->bm_io_work.w.list));
	if (mdev->bm_io_work.why)
		ERR("FIXME going to queue '%s' but '%s' still pending?\n",
			why, mdev->bm_io_work.why);

	mdev->bm_io_work.io_fn = io_fn;
	mdev->bm_io_work.done = done;
	mdev->bm_io_work.why = why;

	set_bit(BITMAP_IO, &mdev->flags);
	if (atomic_read(&mdev->ap_bio_cnt) == 0) {
		if (list_empty(&mdev->bm_io_work.w.list)) {
			set_bit(BITMAP_IO_QUEUED, &mdev->flags);
			drbd_queue_work(&mdev->data.work, &mdev->bm_io_work.w);
		} else
			ERR("FIXME avoided double queuing bm_io_work\n");
	}
}

/**
 * drbd_bitmap_io:
 * Does an IO operation on the bitmap, freezing application IO while that
 * IO operations runs. This functions MUST NOT be called from worker context.
 */
int drbd_bitmap_io(struct drbd_conf *mdev, int (*io_fn)(struct drbd_conf *), char *why)
{
	int rv;

	D_ASSERT(current != mdev->worker.task);

	drbd_suspend_io(mdev);

	drbd_bm_lock(mdev, why);
	rv = io_fn(mdev);
	drbd_bm_unlock(mdev);

	drbd_resume_io(mdev);

	return rv;
}

void drbd_md_set_flag(struct drbd_conf *mdev, int flag) __must_hold(local)
{
	MUST_HOLD(mdev->req_lock);
	if ((mdev->bc->md.flags & flag) != flag) {
		drbd_md_mark_dirty(mdev);
		mdev->bc->md.flags |= flag;
	}
}

void drbd_md_clear_flag(struct drbd_conf *mdev, int flag) __must_hold(local)
{
	MUST_HOLD(mdev->req_lock);
	if ((mdev->bc->md.flags & flag) != 0) {
		drbd_md_mark_dirty(mdev);
		mdev->bc->md.flags &= ~flag;
	}
}
int drbd_md_test_flag(struct drbd_backing_dev *bdev, int flag)
{
	return (bdev->md.flags & flag) != 0;
}

STATIC void md_sync_timer_fn(unsigned long data)
{
	struct drbd_conf *mdev = (struct drbd_conf *) data;

	drbd_queue_work_front(&mdev->data.work, &mdev->md_sync_work);
}

STATIC int w_md_sync(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	drbd_WARN("md_sync_timer expired! Worker calls drbd_md_sync().\n");
	drbd_md_sync(mdev);

	return 1;
}

#ifdef DRBD_ENABLE_FAULTS
/* Fault insertion support including random number generator shamelessly
 * stolen from kernel/rcutorture.c */
struct fault_random_state {
	unsigned long state;
	unsigned long count;
};

#define FAULT_RANDOM_MULT 39916801  /* prime */
#define FAULT_RANDOM_ADD	479001701 /* prime */
#define FAULT_RANDOM_REFRESH 10000

/*
 * Crude but fast random-number generator.  Uses a linear congruential
 * generator, with occasional help from get_random_bytes().
 */
STATIC unsigned long
_drbd_fault_random(struct fault_random_state *rsp)
{
	long refresh;

	if (--rsp->count < 0) {
		get_random_bytes(&refresh, sizeof(refresh));
		rsp->state += refresh;
		rsp->count = FAULT_RANDOM_REFRESH;
	}
	rsp->state = rsp->state * FAULT_RANDOM_MULT + FAULT_RANDOM_ADD;
	return swahw32(rsp->state);
}

STATIC char *
_drbd_fault_str(unsigned int type) {
	static char *_faults[] = {
		"Meta-data write",
		"Meta-data read",
		"Resync write",
		"Resync read",
		"Data write",
		"Data read",
		"Data read ahead",
	};

	return (type < DRBD_FAULT_MAX) ? _faults[type] : "**Unknown**";
}

unsigned int
_drbd_insert_fault(struct drbd_conf *mdev, unsigned int type)
{
	static struct fault_random_state rrs = {0, 0};

	unsigned int ret = (
		(fault_devs == 0 ||
			((1 << mdev_to_minor(mdev)) & fault_devs) != 0) &&
		(((_drbd_fault_random(&rrs) % 100) + 1) <= fault_rate));

	if (ret) {
		fault_count++;

		if (printk_ratelimit())
			drbd_WARN("***Simulating %s failure\n",
				_drbd_fault_str(type));
	}

	return ret;
}
#endif

#ifdef ENABLE_DYNAMIC_TRACE

STATIC char *_drbd_uuid_str(unsigned int idx)
{
	static char *uuid_str[] = {
		"Current",
		"Bitmap",
		"History_start",
		"History_end",
		"UUID_SIZE",
		"UUID_FLAGS",
	};

	return (idx < EXT_UUID_SIZE) ? uuid_str[idx] : "*Unknown UUID index*";
}

/* Pretty print a UUID value */
void drbd_print_uuid(struct drbd_conf *mdev, unsigned int idx) __must_hold(local)
{
	INFO(" uuid[%s] now %016llX\n",
	     _drbd_uuid_str(idx), (unsigned long long)mdev->bc->md.uuid[idx]);
}


/*
 *
 * drbd_print_buffer
 *
 * This routine dumps binary data to the debugging output. Can be
 * called at interrupt level.
 *
 * Arguments:
 *
 *     prefix      - String is output at the beginning of each line output
 *     flags       - Control operation of the routine. Currently defined
 *                   Flags are:
 *                   DBGPRINT_BUFFADDR; if set, each line starts with the
 *                       virtual address of the line being outupt. If clear,
 *                       each line starts with the offset from the beginning
 *                       of the buffer.
 *     size        - Indicates the size of each entry in the buffer. Supported
 *                   values are sizeof(char), sizeof(short) and sizeof(int)
 *     buffer      - Start address of buffer
 *     buffer_va   - Virtual address of start of buffer (normally the same
 *                   as Buffer, but having it separate allows it to hold
 *                   file address for example)
 *     length      - length of buffer
 *
 */
void
drbd_print_buffer(const char *prefix, unsigned int flags, int size,
		  const void *buffer, const void *buffer_va,
		  unsigned int length)

#define LINE_SIZE       16
#define LINE_ENTRIES    (int)(LINE_SIZE/size)
{
	const unsigned char *pstart;
	const unsigned char *pstart_va;
	const unsigned char *pend;
	char bytes_str[LINE_SIZE*3+8], ascii_str[LINE_SIZE+8];
	char *pbytes = bytes_str, *pascii = ascii_str;
	int  offset = 0;
	long sizemask;
	int  field_width;
	int  index;
	const unsigned char *pend_str;
	const unsigned char *p;
	int count;

	/* verify size parameter */
	if (size != sizeof(char) &&
	    size != sizeof(short) &&
	    size != sizeof(int)) {
		printk(KERN_DEBUG "drbd_print_buffer: "
			"ERROR invalid size %d\n", size);
		return;
	}

	sizemask = size-1;
	field_width = size*2;

	/* Adjust start/end to be on appropriate boundary for size */
	buffer = (const char *)((long)buffer & ~sizemask);
	pend   = (const unsigned char *)
		(((long)buffer + length + sizemask) & ~sizemask);

	if (flags & DBGPRINT_BUFFADDR) {
		/* Move start back to nearest multiple of line size,
		 * if printing address. This results in nicely formatted output
		 * with addresses being on line size (16) byte boundaries */
		pstart = (const unsigned char *)((long)buffer & ~(LINE_SIZE-1));
	} else {
		pstart = (const unsigned char *)buffer;
	}

	/* Set value of start VA to print if addresses asked for */
	pstart_va = (const unsigned char *)buffer_va
		 - ((const unsigned char *)buffer-pstart);

	/* Calculate end position to nicely align right hand side */
	pend_str = pstart + (((pend-pstart) + LINE_SIZE-1) & ~(LINE_SIZE-1));

	/* Init strings */
	*pbytes = *pascii = '\0';

	/* Start at beginning of first line */
	p = pstart;
	count = 0;

	while (p < pend_str) {
		if (p < (const unsigned char *)buffer || p >= pend) {
			/* Before start of buffer or after end- print spaces */
			pbytes += sprintf(pbytes, "%*c ", field_width, ' ');
			pascii += sprintf(pascii, "%*c", size, ' ');
			p += size;
		} else {
			/* Add hex and ascii to strings */
			int val;
			switch (size) {
			default:
			case 1:
				val = *(unsigned char *)p;
				break;
			case 2:
				val = *(unsigned short *)p;
				break;
			case 4:
				val = *(unsigned int *)p;
				break;
			}

			pbytes += sprintf(pbytes, "%0*x ", field_width, val);

			for (index = size; index; index--) {
				*pascii++ = isprint(*p) ? *p : '.';
				p++;
			}
		}

		count++;

		if (count == LINE_ENTRIES || p >= pend_str) {
			/* Null terminate and print record */
			*pascii = '\0';
			printk(KERN_DEBUG "%s%8.8lx: %*s|%*s|\n",
			       prefix,
			       (flags & DBGPRINT_BUFFADDR)
			       ? (long)pstart_va : (long)offset,
			       LINE_ENTRIES*(field_width+1), bytes_str,
			       LINE_SIZE, ascii_str);

			/* Move onto next line */
			pstart_va += (p-pstart);
			pstart = p;
			count  = 0;
			offset += LINE_SIZE;

			/* Re-init strings */
			pbytes = bytes_str;
			pascii = ascii_str;
			*pbytes = *pascii = '\0';
		}
	}
}

#define PSM(A)							\
do {								\
	if (mask.A) {						\
		int i = snprintf(p, len, " " #A "( %s )",	\
				A##s_to_name(val.A));		\
		if (i >= len)					\
			return op;				\
		p += i;						\
		len -= i;					\
	}							\
} while (0)

STATIC char *dump_st(char *p, int len, union drbd_state_t mask, union drbd_state_t val)
{
	char *op = p;
	*p = '\0';
	PSM(role);
	PSM(peer);
	PSM(conn);
	PSM(disk);
	PSM(pdsk);

	return op;
}

#define INFOP(fmt, args...) \
do { \
	if (trace_level >= TraceLvlAll) { \
		INFO("%s:%d: %s [%d] %s %s " fmt , \
		     file, line, current->comm, current->pid, \
		     sockname, recv ? "<<<" : ">>>" , \
		     ## args); \
	} else { \
		INFO("%s %s " fmt, sockname, \
		     recv ? "<<<" : ">>>" , \
		     ## args); \
	} \
} while (0)

STATIC char *_dump_block_id(u64 block_id, char *buff)
{
	if (is_syncer_block_id(block_id))
		strcpy(buff, "SyncerId");
	else
		sprintf(buff, "%llx", (unsigned long long)block_id);

	return buff;
}

void
_dump_packet(struct drbd_conf *mdev, struct socket *sock,
	    int recv, union Drbd_Polymorph_Packet *p, char *file, int line)
{
	char *sockname = sock == mdev->meta.socket ? "meta" : "data";
	int cmd = (recv == 2) ? p->head.command : be16_to_cpu(p->head.command);
	char tmp[300];
	union drbd_state_t m, v;

	switch (cmd) {
	case HandShake:
		INFOP("%s (protocol %u-%u)\n", cmdname(cmd),
			be32_to_cpu(p->HandShake.protocol_min),
			be32_to_cpu(p->HandShake.protocol_max));
		break;

	case ReportBitMap: /* don't report this */
		break;

	case Data:
		INFOP("%s (sector %llus, id %s, seq %u, f %x)\n", cmdname(cmd),
		      (unsigned long long)be64_to_cpu(p->Data.sector),
		      _dump_block_id(p->Data.block_id, tmp),
		      be32_to_cpu(p->Data.seq_num),
		      be32_to_cpu(p->Data.dp_flags)
			);
		break;

	case DataReply:
	case RSDataReply:
		INFOP("%s (sector %llus, id %s)\n", cmdname(cmd),
		      (unsigned long long)be64_to_cpu(p->Data.sector),
		      _dump_block_id(p->Data.block_id, tmp)
			);
		break;

	case RecvAck:
	case WriteAck:
	case RSWriteAck:
	case DiscardAck:
	case NegAck:
	case NegRSDReply:
		INFOP("%s (sector %llus, size %u, id %s, seq %u)\n",
			cmdname(cmd),
		      (long long)be64_to_cpu(p->BlockAck.sector),
		      be32_to_cpu(p->BlockAck.blksize),
		      _dump_block_id(p->BlockAck.block_id, tmp),
		      be32_to_cpu(p->BlockAck.seq_num)
			);
		break;

	case DataRequest:
	case RSDataRequest:
		INFOP("%s (sector %llus, size %u, id %s)\n", cmdname(cmd),
		      (long long)be64_to_cpu(p->BlockRequest.sector),
		      be32_to_cpu(p->BlockRequest.blksize),
		      _dump_block_id(p->BlockRequest.block_id, tmp)
			);
		break;

	case Barrier:
	case BarrierAck:
		INFOP("%s (barrier %u)\n", cmdname(cmd), p->Barrier.barrier);
		break;

	case SyncParam:
	case SyncParam89:
		INFOP("%s (rate %u, verify-alg \"%.64s\", csums-alg \"%.64s\")\n",
			cmdname(cmd), be32_to_cpu(p->SyncParam89.rate),
			p->SyncParam89.verify_alg, p->SyncParam89.csums_alg);
		break;

	case ReportUUIDs:
		INFOP("%s Curr:%016llX, Bitmap:%016llX, "
		      "HisSt:%016llX, HisEnd:%016llX\n",
		      cmdname(cmd),
		      (unsigned long long)be64_to_cpu(p->GenCnt.uuid[Current]),
		      (unsigned long long)be64_to_cpu(p->GenCnt.uuid[Bitmap]),
		      (unsigned long long)be64_to_cpu(p->GenCnt.uuid[History_start]),
		      (unsigned long long)be64_to_cpu(p->GenCnt.uuid[History_end]));
		break;

	case ReportSizes:
		INFOP("%s (d %lluMiB, u %lluMiB, c %lldMiB, "
		      "max bio %x, q order %x)\n",
		      cmdname(cmd),
		      (long long)(be64_to_cpu(p->Sizes.d_size)>>(20-9)),
		      (long long)(be64_to_cpu(p->Sizes.u_size)>>(20-9)),
		      (long long)(be64_to_cpu(p->Sizes.c_size)>>(20-9)),
		      be32_to_cpu(p->Sizes.max_segment_size),
		      be32_to_cpu(p->Sizes.queue_order_type));
		break;

	case ReportState:
		v.i = be32_to_cpu(p->State.state);
		m.i = 0xffffffff;
		dump_st(tmp, sizeof(tmp), m, v);
		INFOP("%s (s %x {%s})\n", cmdname(cmd), v.i, tmp);
		break;

	case StateChgRequest:
		m.i = be32_to_cpu(p->ReqState.mask);
		v.i = be32_to_cpu(p->ReqState.val);
		dump_st(tmp, sizeof(tmp), m, v);
		INFOP("%s (m %x v %x {%s})\n", cmdname(cmd), m.i, v.i, tmp);
		break;

	case StateChgReply:
		INFOP("%s (ret %x)\n", cmdname(cmd),
		      be32_to_cpu(p->RqSReply.retcode));
		break;

	case Ping:
	case PingAck:
		/*
		 * Dont trace pings at summary level
		 */
		if (trace_level < TraceLvlAll)
			break;
		/* fall through... */
	default:
		INFOP("%s (%u)\n", cmdname(cmd), cmd);
		break;
	}
}

/* Debug routine to dump info about bio */

void _dump_bio(const char *pfx, struct drbd_conf *mdev, struct bio *bio, int complete, struct drbd_request *r)
{
#ifdef CONFIG_LBD
#define SECTOR_FORMAT "%Lx"
#else
#define SECTOR_FORMAT "%lx"
#endif
#define SECTOR_SHIFT 9

	unsigned long lowaddr = (unsigned long)(bio->bi_sector << SECTOR_SHIFT);
	char *faddr = (char *)(lowaddr);
	char rb[sizeof(void*)*2+6] = { 0, };
	struct bio_vec *bvec;
	int segno;

	const int rw = bio->bi_rw;
	const int biorw      = (rw & (RW_MASK|RWA_MASK));
	const int biobarrier = (rw & (1<<BIO_RW_BARRIER));
	const int biosync    = (rw & (1<<BIO_RW_SYNC));

	if (r)
		sprintf(rb,"Req:%p ", r);

	INFO("%s %s:%s%s%s Bio:%p %s- %soffset " SECTOR_FORMAT ", size %x\n",
	     complete ? "<<<" : ">>>",
	     pfx,
	     biorw == WRITE ? "Write" : "Read",
	     biobarrier ? " : B" : "",
	     biosync ? " : S" : "",
	     bio,
	     rb,
	     complete ? (drbd_bio_uptodate(bio) ? "Success, " : "Failed, ") : "",
	     bio->bi_sector << SECTOR_SHIFT,
	     bio->bi_size);

	if (trace_level >= TraceLvlMetrics &&
	    ((biorw == WRITE) ^ complete)) {
		printk(KERN_DEBUG "  ind     page   offset   length\n");
		__bio_for_each_segment(bvec, bio, segno, 0) {
			printk(KERN_DEBUG "  [%d] %p %8.8x %8.8x\n", segno,
			       bvec->bv_page, bvec->bv_offset, bvec->bv_len);

			if (trace_level >= TraceLvlAll) {
				char *bvec_buf;
				unsigned long flags;

				bvec_buf = bvec_kmap_irq(bvec, &flags);

				drbd_print_buffer("    ", DBGPRINT_BUFFADDR, 1,
						  bvec_buf,
						  faddr,
						  (bvec->bv_len <= 0x80)
						  ? bvec->bv_len : 0x80);

				bvec_kunmap_irq(bvec_buf, &flags);

				if (bvec->bv_len > 0x40)
					printk(KERN_DEBUG "    ....\n");

				faddr += bvec->bv_len;
			}
		}
	}
}
#endif

module_init(drbd_init)
module_exit(drbd_cleanup)
