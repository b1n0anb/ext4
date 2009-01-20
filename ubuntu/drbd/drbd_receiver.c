/*
-*- linux-c -*-
   drbd_receiver.c
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

#include <asm/uaccess.h>
#include <net/sock.h>

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/drbd_config.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/pkt_sched.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#ifdef HAVE_LINUX_SCATTERLIST_H
/* 2.6.11 (suse 9.3, fc4) does not include requisites
 * from linux/scatterlist.h :( */
#include <asm/scatterlist.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#endif
#include <linux/drbd.h>
#include "drbd_int.h"
#include "drbd_req.h"

struct flush_work {
	struct drbd_work w;
	struct drbd_epoch *epoch;
};

enum epoch_event {
	EV_put,
	EV_got_barrier_nr,
	EV_barrier_done,
	EV_became_last,
	EV_cleanup = 32, /* used as flag */
};

enum finish_epoch {
	FE_still_live,
	FE_destroyed,
	FE_recycled,
};

STATIC enum finish_epoch drbd_may_finish_epoch(struct drbd_conf *, struct drbd_epoch *, enum epoch_event);
STATIC int e_end_block(struct drbd_conf *, struct drbd_work *, int);
static inline struct drbd_epoch *previous_epoch(struct drbd_conf *mdev, struct drbd_epoch *epoch)
{
	struct drbd_epoch *prev;
	spin_lock(&mdev->epoch_lock);
	prev = list_entry(epoch->list.prev, struct drbd_epoch, list);
	if (prev == epoch || prev == mdev->current_epoch)
		prev = NULL;
	spin_unlock(&mdev->epoch_lock);
	return prev;
}

#ifdef DBG_ASSERTS
void drbd_assert_breakpoint(struct drbd_conf *mdev, char *exp,
			    char *file, int line)
{
	ERR("ASSERT( %s ) in %s:%d\n", exp, file, line);
}
#endif

#define GFP_TRY	(__GFP_HIGHMEM | __GFP_NOWARN)

/**
 * drbd_bp_alloc: Returns a page. Fails only if a signal comes in.
 */
STATIC struct page *drbd_pp_alloc(struct drbd_conf *mdev, gfp_t gfp_mask)
{
	unsigned long flags = 0;
	struct page *page;
	DEFINE_WAIT(wait);

	/* FIXME Add some usefull watermark again to "kick_lo", if pages get
	 * used up too quickly. The watermark that had been in place here did
	 * not make sense.
	 */

	spin_lock_irqsave(&drbd_pp_lock, flags);
	/* This lock needs to lock out irq because we might call drbd_pp_free()
	   from IRQ context.
	   FIXME but why irq _save_ ?
	   this is only called from drbd_alloc_ee,
	   and that is strictly process context! */
	page = drbd_pp_pool;
	if (page) {
		drbd_pp_pool = (struct page *)page_private(page);
		set_page_private(page, 0); /* just to be polite */
		drbd_pp_vacant--;
	}
	spin_unlock_irqrestore(&drbd_pp_lock, flags);
	if (page)
		goto got_page;

	drbd_kick_lo(mdev);

	for (;;) {
		prepare_to_wait(&drbd_pp_wait, &wait, TASK_INTERRUPTIBLE);

		/* try the pool again, maybe the drbd_kick_lo set some free */
		spin_lock_irqsave(&drbd_pp_lock, flags);
		page = drbd_pp_pool;
		if (page) {
			drbd_pp_pool = (struct page *)page_private(page);
			drbd_pp_vacant--;
		}
		spin_unlock_irqrestore(&drbd_pp_lock, flags);

		if (page)
			break;

		/* hm. pool was empty. try to allocate from kernel.
		 * don't wait, if none is available, though.
		 */
		if (atomic_read(&mdev->pp_in_use)
					< mdev->net_conf->max_buffers) {
			page = alloc_page(GFP_TRY);
			if (page)
				break;
		}

		/* doh. still no page.
		 * either used up the configured maximum number,
		 * or we are low on memory.
		 * wait for someone to return a page into the pool.
		 * unless, of course, someone signalled us.
		 */
		if (signal_pending(current)) {
			drbd_WARN("drbd_pp_alloc interrupted!\n");
			finish_wait(&drbd_pp_wait, &wait);
			return NULL;
		}
		drbd_kick_lo(mdev);
		if (!(gfp_mask & __GFP_WAIT)) {
			finish_wait(&drbd_pp_wait, &wait);
			return NULL;
		}
		schedule();
	}
	finish_wait(&drbd_pp_wait, &wait);

 got_page:
	atomic_inc(&mdev->pp_in_use);
	return page;
}

STATIC void drbd_pp_free(struct drbd_conf *mdev, struct page *page)
{
	unsigned long flags = 0;
	int free_it;

	spin_lock_irqsave(&drbd_pp_lock, flags);
	if (drbd_pp_vacant > (DRBD_MAX_SEGMENT_SIZE/PAGE_SIZE)*minor_count) {
		free_it = 1;
	} else {
		set_page_private(page, (unsigned long)drbd_pp_pool);
		drbd_pp_pool = page;
		drbd_pp_vacant++;
		free_it = 0;
	}
	spin_unlock_irqrestore(&drbd_pp_lock, flags);

	atomic_dec(&mdev->pp_in_use);

	if (free_it)
		__free_page(page);

	/*
	 * FIXME
	 * typically there are no waiters.
	 * we should try to avoid any unnecessary call to wake_up.
	 */
	wake_up(&drbd_pp_wait);
}

/*
You need to hold the req_lock:
 drbd_free_ee()
 _drbd_wait_ee_list_empty()

You must not have the req_lock:
 drbd_alloc_ee()
 drbd_init_ee()
 drbd_release_ee()
 drbd_ee_fix_bhs()
 drbd_process_done_ee()
 drbd_clear_done_ee()
 drbd_wait_ee_list_empty()
*/

struct Tl_epoch_entry *drbd_alloc_ee(struct drbd_conf *mdev,
				     u64 id,
				     sector_t sector,
				     unsigned int data_size,
				     gfp_t gfp_mask) __must_hold(local)
{
	struct request_queue *q;
	struct Tl_epoch_entry *e;
	struct bio_vec *bvec;
	struct page *page;
	struct bio *bio;
	unsigned int ds;
	int i;

	e = mempool_alloc(drbd_ee_mempool, gfp_mask & ~__GFP_HIGHMEM);
	if (!e) {
		if (!(gfp_mask & __GFP_NOWARN))
			ERR("alloc_ee: Allocation of an EE failed\n");
		return NULL;
	}

	bio = bio_alloc(gfp_mask & ~__GFP_HIGHMEM, div_ceil(data_size, PAGE_SIZE));
	if (!bio) {
		if (!(gfp_mask & __GFP_NOWARN))
			ERR("alloc_ee: Allocation of a bio failed\n");
		goto fail1;
	}

	bio->bi_bdev = mdev->bc->backing_bdev;
	bio->bi_sector = sector;

	ds = data_size;
	while (ds) {
		page = drbd_pp_alloc(mdev, gfp_mask);
		if (!page) {
			if (!(gfp_mask & __GFP_NOWARN))
				ERR("alloc_ee: Allocation of a page failed\n");
			goto fail2;
		}
		if (!bio_add_page(bio, page, min_t(int, ds, PAGE_SIZE), 0)) {
			drbd_pp_free(mdev, page);
			ERR("alloc_ee: bio_add_page(s=%llu,"
			    "data_size=%u,ds=%u) failed\n",
			    (unsigned long long)sector, data_size, ds);

			q = bdev_get_queue(bio->bi_bdev);
			if (q->merge_bvec_fn) {
#ifdef HAVE_bvec_merge_data
				struct bvec_merge_data bvm = {
					.bi_bdev = bio->bi_bdev,
					.bi_sector = bio->bi_sector,
					.bi_size = bio->bi_size,
					.bi_rw = bio->bi_rw,
				};
				int l = q->merge_bvec_fn(q, &bvm,
						&bio->bi_io_vec[bio->bi_vcnt]);
#else
				int l = q->merge_bvec_fn(q, bio,
						&bio->bi_io_vec[bio->bi_vcnt]);
#endif
				ERR("merge_bvec_fn() = %d\n", l);
			}

			/* dump more of the bio. */
			DUMPI(bio->bi_max_vecs);
			DUMPI(bio->bi_vcnt);
			DUMPI(bio->bi_size);
			DUMPI(bio->bi_phys_segments);

			goto fail2;
			break;
		}
		ds -= min_t(int, ds, PAGE_SIZE);
	}

	D_ASSERT(data_size == bio->bi_size);

	bio->bi_private = e;
	e->mdev = mdev;
	e->sector = sector;
	e->size = bio->bi_size;

	e->private_bio = bio;
	e->block_id = id;
	INIT_HLIST_NODE(&e->colision);
	e->epoch = NULL;
	e->flags = 0;

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("allocated EE sec=%llus size=%u ee=%p\n",
		    (unsigned long long)sector, data_size, e);
	       );

	return e;

 fail2:
	__bio_for_each_segment(bvec, bio, i, 0) {
		drbd_pp_free(mdev, bvec->bv_page);
	}
	bio_put(bio);
 fail1:
	mempool_free(e, drbd_ee_mempool);

	return NULL;
}

void drbd_free_ee(struct drbd_conf *mdev, struct Tl_epoch_entry *e)
{
	struct bio *bio = e->private_bio;
	struct bio_vec *bvec;
	int i;

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("Free EE sec=%llus size=%u ee=%p\n",
		    (unsigned long long)e->sector, e->size, e);
	       );

	__bio_for_each_segment(bvec, bio, i, 0) {
		drbd_pp_free(mdev, bvec->bv_page);
	}

	bio_put(bio);

	D_ASSERT(hlist_unhashed(&e->colision));

	mempool_free(e, drbd_ee_mempool);
}

/* currently on module unload only */
int drbd_release_ee(struct drbd_conf *mdev, struct list_head *list)
{
	int count = 0;
	struct Tl_epoch_entry *e;
	struct list_head *le;

	spin_lock_irq(&mdev->req_lock);
	while (!list_empty(list)) {
		le = list->next;
		list_del(le);
		e = list_entry(le, struct Tl_epoch_entry, w.list);
		drbd_free_ee(mdev, e);
		count++;
	}
	spin_unlock_irq(&mdev->req_lock);

	return count;
}


STATIC void reclaim_net_ee(struct drbd_conf *mdev)
{
	struct Tl_epoch_entry *e;
	struct list_head *le, *tle;

	/* The EEs are always appended to the end of the list. Since
	   they are sent in order over the wire, they have to finish
	   in order. As soon as we see the first not finished we can
	   stop to examine the list... */

	list_for_each_safe(le, tle, &mdev->net_ee) {
		e = list_entry(le, struct Tl_epoch_entry, w.list);
		if (drbd_bio_has_active_page(e->private_bio))
			break;
		list_del(le);
		drbd_free_ee(mdev, e);
	}
}


/*
 * This function is called from _asender only_
 * but see also comments in _req_mod(,barrier_acked)
 * and receive_Barrier.
 *
 * Move entries from net_ee to done_ee, if ready.
 * Grab done_ee, call all callbacks, free the entries.
 * The callbacks typically send out ACKs.
 */
STATIC int drbd_process_done_ee(struct drbd_conf *mdev)
{
	LIST_HEAD(work_list);
	struct Tl_epoch_entry *e, *t;
	int ok = 1;

	spin_lock_irq(&mdev->req_lock);
	reclaim_net_ee(mdev);
	list_splice_init(&mdev->done_ee, &work_list);
	spin_unlock_irq(&mdev->req_lock);

	/* possible callbacks here:
	 * e_end_block, and e_end_resync_block, e_send_discard_ack.
	 * all ignore the last argument.
	 */
	list_for_each_entry_safe(e, t, &work_list, w.list) {
		MTRACE(TraceTypeEE, TraceLvlAll,
		       INFO("Process EE on done_ee sec=%llus size=%u ee=%p\n",
			    (unsigned long long)e->sector, e->size, e);
			);
		/* list_del not necessary, next/prev members not touched */
		if (e->w.cb(mdev, &e->w, 0) == 0)
			ok = 0;
		drbd_free_ee(mdev, e);
	}
	wake_up(&mdev->ee_wait);

	return ok;
}



/* clean-up helper for drbd_disconnect */
void _drbd_clear_done_ee(struct drbd_conf *mdev)
{
	struct list_head *le;
	struct Tl_epoch_entry *e;
	struct drbd_epoch *epoch;
	int n = 0;

	MUST_HOLD(&mdev->req_lock);

	reclaim_net_ee(mdev);

	while (!list_empty(&mdev->done_ee)) {
		le = mdev->done_ee.next;
		list_del(le);
		e = list_entry(le, struct Tl_epoch_entry, w.list);
		if (mdev->net_conf->wire_protocol == DRBD_PROT_C
		|| is_syncer_block_id(e->block_id))
			++n;

		if (!hlist_unhashed(&e->colision))
			hlist_del_init(&e->colision);

		if (e->epoch) {
			if (e->flags & EE_IS_BARRIER) {
				epoch = previous_epoch(mdev, e->epoch);
				if (epoch)
					drbd_may_finish_epoch(mdev, epoch, EV_barrier_done + EV_cleanup);
			}
			drbd_may_finish_epoch(mdev, e->epoch, EV_put + EV_cleanup);
		}
		drbd_free_ee(mdev, e);
	}

	sub_unacked(mdev, n);
}

void _drbd_wait_ee_list_empty(struct drbd_conf *mdev, struct list_head *head)
{
	DEFINE_WAIT(wait);
	MUST_HOLD(&mdev->req_lock);

	/* avoids spin_lock/unlock
	 * and calling prepare_to_wait in the fast path */
	while (!list_empty(head)) {
		prepare_to_wait(&mdev->ee_wait, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irq(&mdev->req_lock);
		drbd_kick_lo(mdev);
		schedule();
		finish_wait(&mdev->ee_wait, &wait);
		spin_lock_irq(&mdev->req_lock);
	}
}

void drbd_wait_ee_list_empty(struct drbd_conf *mdev, struct list_head *head)
{
	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev, head);
	spin_unlock_irq(&mdev->req_lock);
}

#ifdef DEFINE_SOCK_CREATE_KERN
/* if there is no sock_create_kern,
 * tthere is also sock_create_lite missing */
int sock_create_lite(int family, int type, int protocol, struct socket **res)
{
	int err = 0;
	struct socket *sock = NULL;

	sock = sock_alloc();
	if (!sock)
		err = -ENOMEM;
	else
		sock->type = type;

	*res = sock;
	return err;
}
#endif

/* see also kernel_accept; which is only present since 2.6.18.
 * also we want to log which part of it failed, exactly */
STATIC int drbd_accept(struct drbd_conf *mdev, const char **what,
		struct socket *sock, struct socket **newsock)
{
	struct sock *sk = sock->sk;
	int err = 0;

	*what = "listen";
	err = sock->ops->listen(sock, 5);
	if (err < 0)
		goto out;

	*what = "sock_create_lite";
	err = sock_create_lite(sk->sk_family, sk->sk_type, sk->sk_protocol,
			       newsock);
	if (err < 0)
		goto out;

	*what = "accept";
	err = sock->ops->accept(sock, *newsock, 0);
	if (err < 0) {
		sock_release(*newsock);
		*newsock = NULL;
		goto out;
	}
	(*newsock)->ops  = sock->ops;

out:
	return err;
}

STATIC int drbd_recv_short(struct drbd_conf *mdev, struct socket *sock,
		    void *buf, size_t size, int flags)
{
	mm_segment_t oldfs;
	struct kvec iov = {
		.iov_base = buf,
		.iov_len = size,
	};
	struct msghdr msg = {
		.msg_iovlen = 1,
		.msg_iov = (struct iovec *)&iov,
		.msg_flags = (flags ? flags : MSG_WAITALL | MSG_NOSIGNAL)
	};
	int rv;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	rv = sock_recvmsg(sock, &msg, size, msg.msg_flags);
	set_fs(oldfs);

	return rv;
}

STATIC int drbd_recv(struct drbd_conf *mdev, void *buf, size_t size)
{
	mm_segment_t oldfs;
	struct kvec iov = {
		.iov_base = buf,
		.iov_len = size,
	};
	struct msghdr msg = {
		.msg_iovlen = 1,
		.msg_iov = (struct iovec *)&iov,
		.msg_flags = MSG_WAITALL | MSG_NOSIGNAL
	};
	int rv;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	for (;;) {
		rv = sock_recvmsg(mdev->data.socket, &msg, size, msg.msg_flags);
		if (rv == size)
			break;

		/* Note:
		 * ECONNRESET	other side closed the connection
		 * ERESTARTSYS	(on  sock) we got a signal
		 */

		if (rv < 0) {
			if (rv == -ECONNRESET)
				INFO("sock was reset by peer\n");
			else if (rv != -ERESTARTSYS)
				ERR("sock_recvmsg returned %d\n", rv);
			break;
		} else if (rv == 0) {
			INFO("sock was shut down by peer\n");
			break;
		} else	{
			/* signal came in, or peer/link went down,
			 * after we read a partial message
			 */
			/* D_ASSERT(signal_pending(current)); */
			break;
		}
	};

	set_fs(oldfs);

	if (rv != size)
		drbd_force_state(mdev, NS(conn, BrokenPipe));

	return rv;
}

STATIC struct socket *drbd_try_connect(struct drbd_conf *mdev)
{
	const char *what;
	struct socket *sock;
	struct sockaddr_in6 src_in6;
	int err;
	int disconnect_on_error = 1;

	if (!inc_net(mdev))
		return NULL;

	what = "sock_create_kern";
	err = sock_create_kern(((struct sockaddr *)mdev->net_conf->my_addr)->sa_family,
		SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err < 0) {
		sock = NULL;
		goto out;
	}

	sock->sk->sk_rcvtimeo =
	sock->sk->sk_sndtimeo =  mdev->net_conf->try_connect_int*HZ;

       /* explicitly bind to the configured IP as source IP
	*  for the outgoing connections.
	*  This is needed for multihomed hosts and to be
	*  able to use lo: interfaces for drbd.
	* Make sure to use 0 as portnumber, so linux selects
	*  a free one dynamically.
	*/
	memcpy(&src_in6, mdev->net_conf->my_addr,
	       min_t(int, mdev->net_conf->my_addr_len, sizeof(src_in6)));
	if(((struct sockaddr *)mdev->net_conf->my_addr)->sa_family == AF_INET6)
		src_in6.sin6_port = 0;
	else
		((struct sockaddr_in *)&src_in6)->sin_port = 0; /* AF_INET & AF_SCI */

	what = "bind before connect";
	err = sock->ops->bind(sock,
			      (struct sockaddr *) &src_in6,
			      mdev->net_conf->my_addr_len);
	if (err < 0)
		goto out;

	/* connect may fail, peer not yet available.
	 * stay WFConnection, don't go Disconnecting! */
	disconnect_on_error = 0;
	what = "connect";
	err = sock->ops->connect(sock,
				 (struct sockaddr *)mdev->net_conf->peer_addr,
				 mdev->net_conf->peer_addr_len, 0);

out:
	if (err < 0) {
		if (sock) {
			sock_release(sock);
			sock = NULL;
		}
		switch (-err) {
			/* timeout, busy, signal pending */
		case ETIMEDOUT: case EAGAIN: case EINPROGRESS:
		case EINTR: case ERESTARTSYS:
			/* peer not (yet) available, network problem */
		case ECONNREFUSED: case ENETUNREACH:
		case EHOSTDOWN:    case EHOSTUNREACH:
#if 0
			DBG("%s failure ignored, err = %d\n",
					what, err);
#endif
			disconnect_on_error = 0;
			break;
		default:
			ERR("%s failed, err = %d\n", what, err);
		}
		if (disconnect_on_error)
			drbd_force_state(mdev, NS(conn, Disconnecting));
	}
	dec_net(mdev);
	return sock;
}

STATIC struct socket *drbd_wait_for_connect(struct drbd_conf *mdev)
{
	int err;
	struct socket *s_estab = NULL, *s_listen;
	const char *what;

	if (!inc_net(mdev))
		return NULL;

	what = "sock_create_kern";
	err = sock_create_kern(((struct sockaddr *)mdev->net_conf->my_addr)->sa_family,
		SOCK_STREAM, IPPROTO_TCP, &s_listen);
	if (err) {
		s_listen = NULL;
		goto out;
	}

	s_listen->sk->sk_reuse    = 1; /* SO_REUSEADDR */
	s_listen->sk->sk_rcvtimeo =
	s_listen->sk->sk_sndtimeo =  mdev->net_conf->try_connect_int*HZ;

	what = "bind before listen";
	err = s_listen->ops->bind(s_listen,
			      (struct sockaddr *) mdev->net_conf->my_addr,
			      mdev->net_conf->my_addr_len);
	if (err < 0)
		goto out;

	err = drbd_accept(mdev, &what, s_listen, &s_estab);

out:
	if (s_listen)
		sock_release(s_listen);
	if (err < 0) {
		if (err != -EAGAIN && err != -EINTR && err != -ERESTARTSYS) {
			ERR("%s failed, err = %d\n", what, err);
			drbd_force_state(mdev, NS(conn, Disconnecting));
		}
#if 0
		else {
			DBG("%s failure ignored, err = %d, not Disconnecting\n",
					what, err);
		}
#endif
	}
	dec_net(mdev);

	return s_estab;
}

int drbd_do_handshake(struct drbd_conf *mdev);
int drbd_do_auth(struct drbd_conf *mdev);

STATIC int drbd_send_fp(struct drbd_conf *mdev,
	struct socket *sock, enum Drbd_Packet_Cmd cmd)
{
	struct Drbd_Header *h = (struct Drbd_Header *) &mdev->data.sbuf.head;

	return _drbd_send_cmd(mdev, sock, cmd, h, sizeof(*h), 0);
}

STATIC enum Drbd_Packet_Cmd drbd_recv_fp(struct drbd_conf *mdev, struct socket *sock)
{
	struct Drbd_Header *h = (struct Drbd_Header *) &mdev->data.sbuf.head;
	int rr;

	rr = drbd_recv_short(mdev, sock, h, sizeof(*h), 0);

	if (rr == sizeof(*h) && h->magic == BE_DRBD_MAGIC)
		return be16_to_cpu(h->command);

	return 0xffff;
}

/**
 * drbd_socket_okay:
 * Tests if the connection behind the socket still exists. If not it frees
 * the socket.
 */
static int drbd_socket_okay(struct drbd_conf *mdev, struct socket **sock)
{
	int rr;
	char tb[4];

	rr = drbd_recv_short(mdev, *sock, tb, 4, MSG_DONTWAIT | MSG_PEEK);

	if (rr > 0 || rr == -EAGAIN) {
		return TRUE;
	} else {
		sock_release(*sock);
		*sock = NULL;
		return FALSE;
	}
}

/*
 * return values:
 *   1 yess, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 *  -2 We do not have a network config...
 */
STATIC int drbd_connect(struct drbd_conf *mdev)
{
	struct socket *s, *sock, *msock;
	int try, h, ok;

	D_ASSERT(!mdev->data.socket);

	if (test_and_clear_bit(CREATE_BARRIER, &mdev->flags))
		ERR("CREATE_BARRIER flag was set in drbd_connect - now cleared!\n");

	if (drbd_request_state(mdev, NS(conn, WFConnection)) < SS_Success)
		return -2;

	clear_bit(DISCARD_CONCURRENT, &mdev->flags);

	sock  = NULL;
	msock = NULL;

	do {
		for (try = 0;;) {
			/* 3 tries, this should take less than a second! */
			s = drbd_try_connect(mdev);
			if (s || ++try >= 3)
				break;
			/* give the other side time to call bind() & listen() */
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
		}

		if (s) {
			if (!sock) {
				drbd_send_fp(mdev, s, HandShakeS);
				sock = s;
				s = NULL;
			} else if (!msock) {
				drbd_send_fp(mdev, s, HandShakeM);
				msock = s;
				s = NULL;
			} else {
				ERR("Logic error in drbd_connect()\n");
				return -1;
			}
		}

		if (sock && msock) {
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
			ok = drbd_socket_okay(mdev, &sock);
			ok = drbd_socket_okay(mdev, &msock) && ok;
			if (ok)
				break;
		}

		s = drbd_wait_for_connect(mdev);
		if (s) {
			switch (drbd_recv_fp(mdev, s)) {
			case HandShakeS:
				if (sock)
					sock_release(sock);
				sock = s;
				break;
			case HandShakeM:
				if (msock)
					sock_release(msock);
				msock = s;
				set_bit(DISCARD_CONCURRENT, &mdev->flags);
				break;
			default:
				drbd_WARN("Error receiving initial packet\n");
				sock_release(s);
			}
		}

		if (mdev->state.conn <= Disconnecting)
			return -1;
		if (signal_pending(current)) {
			flush_signals(current);
			smp_rmb();
			if (get_t_state(&mdev->receiver) == Exiting) {
				if (sock)
					sock_release(sock);
				if (msock)
					sock_release(msock);
				return -1;
			}
		}

		if (sock && msock) {
			ok = drbd_socket_okay(mdev, &sock);
			ok = drbd_socket_okay(mdev, &msock) && ok;
			if (ok)
				break;
		}
	} while (1);

	msock->sk->sk_reuse = 1; /* SO_REUSEADDR */
	sock->sk->sk_reuse = 1; /* SO_REUSEADDR */

	sock->sk->sk_allocation = GFP_NOIO;
	msock->sk->sk_allocation = GFP_NOIO;

	sock->sk->sk_priority = TC_PRIO_INTERACTIVE_BULK;
	msock->sk->sk_priority = TC_PRIO_INTERACTIVE;

	if (mdev->net_conf->sndbuf_size) {
		/* FIXME fold to limits. should be done during configuration */
		/* this is setsockopt SO_SNDBUFFORCE and SO_RCVBUFFORCE,
		 * done directly. */
		sock->sk->sk_sndbuf = mdev->net_conf->sndbuf_size;
		sock->sk->sk_rcvbuf = mdev->net_conf->sndbuf_size;
		sock->sk->sk_userlocks |= SOCK_SNDBUF_LOCK | SOCK_RCVBUF_LOCK;
	}

#if 0 /* don't pin the msock bufsize, autotuning should work better */
	msock->sk->sk_sndbuf = 2*32767;
	msock->sk->sk_userlocks |= SOCK_SNDBUF_LOCK;
#endif

	/* NOT YET ...
	 * sock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/10;
	 * sock->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
	 * first set it to the HandShake timeout, wich is hardcoded for now: */
	sock->sk->sk_sndtimeo =
	sock->sk->sk_rcvtimeo = 2*HZ;

	msock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/10;
	msock->sk->sk_rcvtimeo = mdev->net_conf->ping_int*HZ;

	/* we don't want delays.
	 * we use TCP_CORK where apropriate, though */
	drbd_tcp_nodelay(sock);
	drbd_tcp_nodelay(msock);

	mdev->data.socket = sock;
	mdev->meta.socket = msock;
	mdev->last_received = jiffies;

	D_ASSERT(mdev->asender.task == NULL);

	h = drbd_do_handshake(mdev);
	if (h <= 0)
		return h;

	if (mdev->cram_hmac_tfm) {
		/* drbd_request_state(mdev, NS(conn, WFAuth)); */
		if (!drbd_do_auth(mdev)) {
			ERR("Authentication of peer failed\n");
			return -1;
		}
	}

	if (drbd_request_state(mdev, NS(conn, WFReportParams)) < SS_Success)
		return 0;

	sock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/10;
	sock->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;

	atomic_set(&mdev->packet_seq, 0);
	mdev->peer_seq = 0;

	drbd_thread_start(&mdev->asender);

	drbd_send_protocol(mdev);
	drbd_send_sync_param(mdev, &mdev->sync_conf);
	drbd_send_sizes(mdev);
	drbd_send_uuids(mdev);
	drbd_send_state(mdev);
	clear_bit(USE_DEGR_WFC_T, &mdev->flags);

	return 1;
}

STATIC int drbd_recv_header(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	int r;

	r = drbd_recv(mdev, h, sizeof(*h));

	if (unlikely(r != sizeof(*h))) {
		ERR("short read expecting header on sock: r=%d\n", r);
		return FALSE;
	};
	h->command = be16_to_cpu(h->command);
	h->length  = be16_to_cpu(h->length);
	if (unlikely(h->magic != BE_DRBD_MAGIC)) {
		ERR("magic?? on data m: 0x%lx c: %d l: %d\n",
		    (long)be32_to_cpu(h->magic),
		    h->command, h->length);
		return FALSE;
	}
	mdev->last_received = jiffies;

	return TRUE;
}

STATIC enum finish_epoch drbd_flush_after_epoch(struct drbd_conf *mdev, struct drbd_epoch *epoch)
{
	int rv;

	if (mdev->write_ordering >= WO_bdev_flush && inc_local(mdev)) {
		rv = blkdev_issue_flush(mdev->bc->backing_bdev, NULL);
		if (rv) {
			ERR("local disk flush failed with status %d\n",rv);
			/* would rather check on EOPNOTSUPP, but that is not reliable.
			 * don't try again for ANY return value != 0
			 * if (rv == -EOPNOTSUPP) */
			drbd_bump_write_ordering(mdev, WO_drain_io);
		}
		dec_local(mdev);
	}

	return drbd_may_finish_epoch(mdev, epoch, EV_barrier_done);
}

/**
 * w_flush: Checks if an epoch can be closed and therefore might
 * close and/or free the epoch object.
 */
STATIC int w_flush(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct flush_work *fw = (struct flush_work *)w;
	struct drbd_epoch *epoch = fw->epoch;

	kfree(w);

	if (!test_and_set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags))
		drbd_flush_after_epoch(mdev, epoch);

	drbd_may_finish_epoch(mdev, epoch, EV_put |
			      (mdev->state.conn < Connected ? EV_cleanup : 0));

	return 1;
}

/**
 * drbd_may_finish_epoch: Checks if an epoch can be closed and therefore might
 * close and/or free the epoch object.
 */
STATIC enum finish_epoch drbd_may_finish_epoch(struct drbd_conf *mdev,
					       struct drbd_epoch *epoch,
					       enum epoch_event ev)
{
	int finish, epoch_size;
	struct drbd_epoch *next_epoch;
	int schedule_flush = 0;
	enum finish_epoch rv = FE_still_live;

	static char *epoch_event_str[] = {
		[EV_put] = "put",
		[EV_got_barrier_nr] = "got_barrier_nr",
		[EV_barrier_done] = "barrier_done",
		[EV_became_last] = "became_last",
	};

	spin_lock(&mdev->epoch_lock);
	do {
		next_epoch = NULL;
		finish = 0;

		epoch_size = atomic_read(&epoch->epoch_size);

		switch (ev & ~EV_cleanup) {
		case EV_put:
			atomic_dec(&epoch->active);
			break;
		case EV_got_barrier_nr:
			set_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags);

			/* Special case: If we just switched from WO_bio_barrier to
			   WO_bdev_flush we should not finish the current epoch */
			if (test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags) && epoch_size == 1 &&
			    mdev->write_ordering != WO_bio_barrier &&
			    epoch == mdev->current_epoch)
				clear_bit(DE_CONTAINS_A_BARRIER, &epoch->flags);
			break;
		case EV_barrier_done:
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags);
			break;
		case EV_became_last:
			/* nothing to do*/
			break;
		}

		MTRACE(TraceTypeEpochs, TraceLvlAll,
		       INFO("Update epoch  %p/%d { size=%d active=%d %c%c n%c%c } ev=%s\n",
			    epoch, epoch->barrier_nr, epoch_size, atomic_read(&epoch->active),
			    test_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags) ? 'n' : '-',
			    test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags) ? 'b' : '-',
			    test_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags) ? 'i' : '-',
			    test_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags) ? 'd' : '-',
			    epoch_event_str[ev]);
			);

		if (epoch_size != 0 &&
		    atomic_read(&epoch->active) == 0 &&
		    test_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags) &&
		    epoch->list.prev == &mdev->current_epoch->list &&
		    !test_bit(DE_IS_FINISHING, &epoch->flags)) {
			/* Nearly all conditions are met to finish that epoch... */
			if (test_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags) ||
			    mdev->write_ordering == WO_none ||
			    (epoch_size == 1 && test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags)) ||
			    ev & EV_cleanup) {
				finish = 1;
				set_bit(DE_IS_FINISHING, &epoch->flags);
			} else if (!test_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags) &&
				 mdev->write_ordering == WO_bio_barrier) {
				atomic_inc(&epoch->active);
				schedule_flush = 1;
			}
		}
		if (finish) {
			if (!(ev & EV_cleanup)) {
				spin_unlock(&mdev->epoch_lock);
				drbd_send_b_ack(mdev, epoch->barrier_nr, epoch_size);
				spin_lock(&mdev->epoch_lock);
			}
			dec_unacked(mdev);

			if (mdev->current_epoch != epoch) {
				next_epoch = list_entry(epoch->list.next, struct drbd_epoch, list);
				list_del(&epoch->list);
				ev = EV_became_last | (ev & EV_cleanup);
				mdev->epochs--;
				MTRACE(TraceTypeEpochs, TraceLvlSummary,
				       INFO("Freeing epoch %p/%d { size=%d } nr_epochs=%d\n",
					    epoch, epoch->barrier_nr, epoch_size, mdev->epochs);
					);
				kfree(epoch);

				if (rv == FE_still_live)
					rv = FE_destroyed;
			} else {
				epoch->flags = 0;
				atomic_set(&epoch->epoch_size, 0);
				/* atomic_set(&epoch->active, 0); is alrady zero */
				if (rv == FE_still_live)
					rv = FE_recycled;
			}
		}

		if (!next_epoch)
			break;

		epoch = next_epoch;
	} while (1);

	spin_unlock(&mdev->epoch_lock);

	if (schedule_flush) {
		struct flush_work *fw;
		fw = kmalloc(sizeof(*fw), GFP_ATOMIC);
		if (fw) {
			MTRACE(TraceTypeEpochs, TraceLvlMetrics,
			       INFO("Schedul flush %p/%d { size=%d } nr_epochs=%d\n",
				    epoch, epoch->barrier_nr, epoch_size, mdev->epochs);
				);
			fw->w.cb = w_flush;
			fw->epoch = epoch;
			drbd_queue_work(&mdev->data.work, &fw->w);
		} else {
			drbd_WARN("Could not kmalloc a flush_work obj\n");
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
			/* That is not a recursion, only one level */
			drbd_may_finish_epoch(mdev, epoch, EV_barrier_done);
			drbd_may_finish_epoch(mdev, epoch, EV_put);
		}
	}

	return rv;
}

/**
 * drbd_bump_write_ordering: It turned out that the current mdev->write_ordering
 * method does not work on the backing block device. Try the next allowed method.
 */
void drbd_bump_write_ordering(struct drbd_conf *mdev, enum write_ordering_e wo) __must_hold(local)
{
	enum write_ordering_e pwo;
	static char *write_ordering_str[] = {
		[WO_none] = "none",
		[WO_drain_io] = "drain",
		[WO_bdev_flush] = "flush",
		[WO_bio_barrier] = "barrier",
	};

	pwo = mdev->write_ordering;
	wo = min(pwo, wo);
	if (wo == WO_bio_barrier && mdev->bc->dc.no_disk_barrier)
		wo = WO_bdev_flush;
	if (wo == WO_bdev_flush && mdev->bc->dc.no_disk_flush)
		wo = WO_drain_io;
	if (wo == WO_drain_io && mdev->bc->dc.no_disk_drain)
		wo = WO_none;
	mdev->write_ordering = wo;
	if (pwo != mdev->write_ordering || wo == WO_bio_barrier)
		INFO("Method to ensure write ordering: %s\n", write_ordering_str[mdev->write_ordering]);
}

/**
 * w_e_reissue: In case the IO subsystem delivered an error for an BIO with the
 * BIO_RW_BARRIER flag set, retry that bio without the barrier flag set.
 */
int w_e_reissue(struct drbd_conf *mdev, struct drbd_work *w, int cancel) __releases(local)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	struct bio* bio = e->private_bio;

	/* We leave DE_CONTAINS_A_BARRIER and EE_IS_BARRIER in place,
	   (and DE_BARRIER_IN_NEXT_EPOCH_ISSUED in the previous Epoch)
	   so that we can finish that epoch in drbd_may_finish_epoch().
	   That is necessary if we already have a long chain of Epochs, before
	   we realize that BIO_RW_BARRIER is actually not supported */

	/* As long as the -ENOTSUPP on the barrier is reported immediately
	   that will never trigger. It it is reported late, we will just
	   print that warning an continue corretly for all future requests
	   with WO_bdev_flush */
	if (previous_epoch(mdev, e->epoch))
		drbd_WARN("Write ordering was not enforced (one time event)\n");

	/* prepare bio for re-submit,
	 * re-init volatile members */
	/* we still have a local reference,
	 * inc_local was done in receive_Data. */
	bio->bi_bdev = mdev->bc->backing_bdev;
	bio->bi_sector = e->sector;
	bio->bi_size = e->size;
	bio->bi_idx = 0;

	bio->bi_flags &= ~(BIO_POOL_MASK - 1);
	bio->bi_flags |= 1 << BIO_UPTODATE;

	/* don't know whether this is necessary: */
	bio->bi_phys_segments = 0;
	bio->bi_next = NULL;

	/* these should be unchanged: */
	/* bio->bi_end_io = drbd_endio_write_sec; */
	/* bio->bi_vcnt = whatever; */

	e->w.cb = e_end_block;

	/* This is no longer a barrier request. */
	bio->bi_rw &= ~(1UL << BIO_RW_BARRIER);

	drbd_generic_make_request(mdev, DRBD_FAULT_DT_WR, bio);

	return 1;
}

STATIC int receive_Barrier(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	int rv, issue_flush;
	struct Drbd_Barrier_Packet *p = (struct Drbd_Barrier_Packet *)h;
	struct drbd_epoch *epoch;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;

	rv = drbd_recv(mdev, h->payload, h->length);
	ERR_IF(rv != h->length) return FALSE;

	inc_unacked(mdev);

	if (mdev->net_conf->wire_protocol != DRBD_PROT_C)
		drbd_kick_lo(mdev);

	mdev->current_epoch->barrier_nr = p->barrier;
	rv = drbd_may_finish_epoch(mdev, mdev->current_epoch, EV_got_barrier_nr);

	/* BarrierAck may imply that the corresponding extent is dropped from
	 * the activity log, which means it would not be resynced in case the
	 * Primary crashes now.
	 * Therefore we must send the barrier_ack after the barrier request was
	 * completed. */
	switch (mdev->write_ordering) {
	case WO_bio_barrier:
	case WO_none:
		if (rv == FE_recycled)
			return TRUE;
		break;

	case WO_bdev_flush:
	case WO_drain_io:
		D_ASSERT(rv == FE_still_live);
		set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &mdev->current_epoch->flags);
		drbd_wait_ee_list_empty(mdev, &mdev->active_ee);
		rv = drbd_flush_after_epoch(mdev, mdev->current_epoch);
		if (rv == FE_recycled)
			return TRUE;

		/* The asender will send all the ACKs and barrier ACKs out, since
		   all EEs moved from the active_ee to the done_ee. We need to
		   provide a new epoch object for the EEs that come in soon */
		break;
	}

	epoch = kmalloc(sizeof(struct drbd_epoch), GFP_KERNEL);
	if (!epoch) {
		drbd_WARN("Allocation of an epoch failed, slowing down\n");
		issue_flush = !test_and_set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
		drbd_wait_ee_list_empty(mdev, &mdev->active_ee);
		if (issue_flush) {
			rv = drbd_flush_after_epoch(mdev, mdev->current_epoch);
			if (rv == FE_recycled)
				return TRUE;
		}

		drbd_wait_ee_list_empty(mdev, &mdev->done_ee);

		return TRUE;
	}

	epoch->flags = 0;
	atomic_set(&epoch->epoch_size, 0);
	atomic_set(&epoch->active, 0);

	spin_lock(&mdev->epoch_lock);
	if (atomic_read(&mdev->current_epoch->epoch_size)) {
		list_add(&epoch->list, &mdev->current_epoch->list);
		mdev->current_epoch = epoch;
		mdev->epochs++;
		MTRACE(TraceTypeEpochs, TraceLvlMetrics,
		       INFO("Allocat epoch %p/xxxx { } nr_epochs=%d\n", epoch, mdev->epochs);
			);
	} else {
		/* The current_epoch got recycled while we allocated this one... */
		kfree(epoch);
	}
	spin_unlock(&mdev->epoch_lock);

	return TRUE;
}

/* used from receive_RSDataReply (recv_resync_read)
 * and from receive_Data */
STATIC struct Tl_epoch_entry *
read_in_block(struct drbd_conf *mdev, u64 id, sector_t sector, int data_size) __must_hold(local)
{
	struct Tl_epoch_entry *e;
	struct bio_vec *bvec;
	struct page *page;
	struct bio *bio;
	int dgs, ds, i, rr;
	void *dig_in = mdev->int_dig_in;
	void *dig_vv = mdev->int_dig_vv;

	dgs = (mdev->agreed_pro_version >= 87 && mdev->integrity_r_tfm) ?
		crypto_hash_digestsize(mdev->integrity_r_tfm) : 0;

	if (dgs) {
		rr = drbd_recv(mdev, dig_in, dgs);
		if (rr != dgs) {
			drbd_WARN("short read receiving data digest: read %d expected %d\n",
			     rr, dgs);
			return NULL;
		}
	}

	data_size -= dgs;

	ERR_IF(data_size &  0x1ff) return NULL;
	ERR_IF(data_size >  DRBD_MAX_SEGMENT_SIZE) return NULL;

	e = drbd_alloc_ee(mdev, id, sector, data_size, GFP_KERNEL);
	if (!e)
		return NULL;
	bio = e->private_bio;
	ds = data_size;
	bio_for_each_segment(bvec, bio, i) {
		page = bvec->bv_page;
		rr = drbd_recv(mdev, kmap(page), min_t(int, ds, PAGE_SIZE));
		kunmap(page);
		if (rr != min_t(int, ds, PAGE_SIZE)) {
			drbd_free_ee(mdev, e);
			drbd_WARN("short read receiving data: read %d expected %d\n",
			     rr, min_t(int, ds, PAGE_SIZE));
			return NULL;
		}
		ds -= rr;
	}

	if (dgs) {
		drbd_csum(mdev, mdev->integrity_r_tfm, bio, dig_vv);
		if (memcmp(dig_in,dig_vv,dgs)) {
			ERR("Digest integrity check FAILED.\n");
			drbd_bcast_ee(mdev, "digest failed",
					dgs, dig_in, dig_vv, e);
			drbd_free_ee(mdev, e);
			return NULL;
		}
	}
	mdev->recv_cnt += data_size>>9;
	return e;
}

/* drbd_drain_block() just takes a data block
 * out of the socket input buffer, and discards it.
 */
STATIC int drbd_drain_block(struct drbd_conf *mdev, int data_size)
{
	struct page *page;
	int rr, rv = 1;
	void *data;

	page = drbd_pp_alloc(mdev, GFP_KERNEL);

	data = kmap(page);
	while (data_size) {
		rr = drbd_recv(mdev, data, min_t(int, data_size, PAGE_SIZE));
		if (rr != min_t(int, data_size, PAGE_SIZE)) {
			rv = 0;
			drbd_WARN("short read receiving data: read %d expected %d\n",
			     rr, min_t(int, data_size, PAGE_SIZE));
			break;
		}
		data_size -= rr;
	}
	kunmap(page);
	drbd_pp_free(mdev, page);
	return rv;
}

/* kick lower level device, if we have more than (arbitrary number)
 * reference counts on it, which typically are locally submitted io
 * requests.  don't use unacked_cnt, so we speed up proto A and B, too. */
static void maybe_kick_lo(struct drbd_conf *mdev)
{
	/* FIXME hysteresis ?? */
	if (atomic_read(&mdev->local_cnt) >= mdev->net_conf->unplug_watermark)
		drbd_kick_lo(mdev);
}

STATIC int recv_dless_read(struct drbd_conf *mdev, struct drbd_request *req,
			   sector_t sector, int data_size)
{
	struct bio_vec *bvec;
	struct bio *bio;
	int dgs, rr, i, expect;
	void *dig_in = mdev->int_dig_in;
	void *dig_vv = mdev->int_dig_vv;

	dgs = (mdev->agreed_pro_version >= 87 && mdev->integrity_r_tfm) ?
		crypto_hash_digestsize(mdev->integrity_r_tfm) : 0;

	if (dgs) {
		rr = drbd_recv(mdev, dig_in, dgs);
		if (rr != dgs) {
			drbd_WARN("short read receiving data reply digest: read %d expected %d\n",
			     rr, dgs);
			return 0;
		}
	}

	data_size -= dgs;

	bio = req->master_bio;
	D_ASSERT(sector == bio->bi_sector);

	bio_for_each_segment(bvec, bio, i) {
		expect = min_t(int, data_size, bvec->bv_len);
		rr = drbd_recv(mdev,
			     kmap(bvec->bv_page)+bvec->bv_offset,
			     expect);
		kunmap(bvec->bv_page);
		if (rr != expect) {
			drbd_WARN("short read receiving data reply: "
			     "read %d expected %d\n",
			     rr, expect);
			return 0;
		}
		data_size -= rr;
	}

	if (dgs) {
		drbd_csum(mdev, mdev->integrity_r_tfm, bio, dig_vv);
		if (memcmp(dig_in,dig_vv,dgs)) {
			ERR("Digest integrity check FAILED. Broken NICs?\n");
			return 0;
		}
	}

	D_ASSERT(data_size == 0);
	/* FIXME recv_cnt accounting ?? */
	return 1;
}

/* e_end_resync_block() is called via
 * drbd_process_done_ee() by asender only */
STATIC int e_end_resync_block(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	sector_t sector = e->sector;
	int ok;

	D_ASSERT(hlist_unhashed(&e->colision));

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		drbd_set_in_sync(mdev, sector, e->size);
		ok = drbd_send_ack(mdev, RSWriteAck, e);
	} else {
		/* Record failure to sync */
		drbd_rs_failed_io(mdev, sector, e->size);

		ok  = drbd_send_ack(mdev, NegAck, e);
		ok &= drbd_io_error(mdev, FALSE);
	}
	dec_unacked(mdev);

	return ok;
}

STATIC int recv_resync_read(struct drbd_conf *mdev, sector_t sector, int data_size) __releases(local)
{
	struct Tl_epoch_entry *e;

	e = read_in_block(mdev, ID_SYNCER, sector, data_size);
	if (!e) {
		dec_local(mdev);
		return FALSE;
	}

	dec_rs_pending(mdev);

	e->private_bio->bi_end_io = drbd_endio_write_sec;
	e->private_bio->bi_rw = WRITE;
	e->w.cb = e_end_resync_block;

	inc_unacked(mdev);
	/* corresponding dec_unacked() in e_end_resync_block()
	 * respective _drbd_clear_done_ee */

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list, &mdev->sync_ee);
	spin_unlock_irq(&mdev->req_lock);

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("submit EE (RS)WRITE sec=%llus size=%u ee=%p\n",
		    (unsigned long long)e->sector, e->size, e);
	       );
	dump_internal_bio("Sec", mdev, e->private_bio, 0);
	drbd_generic_make_request(mdev, DRBD_FAULT_RS_WR, e->private_bio);
	/* accounting done in endio */

	maybe_kick_lo(mdev);
	return TRUE;
}

STATIC int receive_DataReply(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct drbd_request *req;
	sector_t sector;
	unsigned int header_size, data_size;
	int ok;
	struct Drbd_Data_Packet *p = (struct Drbd_Data_Packet *)h;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	sector = be64_to_cpu(p->sector);

	spin_lock_irq(&mdev->req_lock);
	req = _ar_id_to_req(mdev, p->block_id, sector);
	spin_unlock_irq(&mdev->req_lock);
	if (unlikely(!req)) {
		ERR("Got a corrupt block_id/sector pair(1).\n");
		return FALSE;
	}

	/* hlist_del(&req->colision) is done in _req_may_be_done, to avoid
	 * special casing it there for the various failure cases.
	 * still no race with drbd_fail_pending_reads */
	ok = recv_dless_read(mdev, req, sector, data_size);

	if (ok)
		req_mod(req, data_received, 0);
	/* else: nothing. handled from drbd_disconnect...
	 * I don't think we may complete this just yet
	 * in case we are "on-disconnect: freeze" */

	return ok;
}

STATIC int receive_RSDataReply(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	sector_t sector;
	unsigned int header_size, data_size;
	int ok;
	struct Drbd_Data_Packet *p = (struct Drbd_Data_Packet *)h;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	sector = be64_to_cpu(p->sector);
	D_ASSERT(p->block_id == ID_SYNCER);

	if (inc_local(mdev)) {
		/* data is submitted to disk within recv_resync_read.
		 * corresponding dec_local done below on error,
		 * or in drbd_endio_write_sec. */
		/* FIXME paranoia:
		 * verify that the corresponding bit is set.
		 * in case we are Primary SyncTarget,
		 * verify there are no pending write request to that area.
		 */
		ok = recv_resync_read(mdev, sector, data_size);
	} else {
		if (DRBD_ratelimit(5*HZ, 5))
			ERR("Can not write resync data to local disk.\n");

		ok = drbd_drain_block(mdev, data_size);

		drbd_send_ack_dp(mdev, NegAck, p);
	}

	return ok;
}

/* e_end_block() is called via drbd_process_done_ee().
 * this means this function only runs in the asender thread
 */
STATIC int e_end_block(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	sector_t sector = e->sector;
	struct drbd_epoch *epoch;
	int ok = 1, pcmd;

	if (e->flags & EE_IS_BARRIER) {
		epoch = previous_epoch(mdev, e->epoch);
		if (epoch)
			drbd_may_finish_epoch(mdev, epoch, EV_barrier_done);
	}

	if (mdev->net_conf->wire_protocol == DRBD_PROT_C) {
		if (likely(drbd_bio_uptodate(e->private_bio))) {
			pcmd = (mdev->state.conn >= SyncSource &&
				mdev->state.conn <= PausedSyncT &&
				e->flags & EE_MAY_SET_IN_SYNC) ?
				RSWriteAck : WriteAck;
			ok &= drbd_send_ack(mdev, pcmd, e);
			if (pcmd == RSWriteAck)
				drbd_set_in_sync(mdev, sector, e->size);
		} else {
			/* FIXME I think we should send a NegAck regardless of
			 * which protocol is in effect.
			 * In which case we would need to make sure that any
			 * NegAck is sent. Basically that means that
			 * drbd_process_done_ee may not list_del() the ee
			 * before this callback did run...
			 * maybe even move the list_del(e) in here... */
			ok  = drbd_send_ack(mdev, NegAck, e);
			ok &= drbd_io_error(mdev, FALSE);
			/* we expect it to be marked out of sync anyways...
			 * maybe assert this?  */
		}
		dec_unacked(mdev);
	} else if (unlikely(!drbd_bio_uptodate(e->private_bio))) {
		ok = drbd_io_error(mdev, FALSE);
	}

	/* we delete from the conflict detection hash _after_ we sent out the
	 * WriteAck / NegAck, to get the sequence number right.  */
	if (mdev->net_conf->two_primaries) {
		spin_lock_irq(&mdev->req_lock);
		D_ASSERT(!hlist_unhashed(&e->colision));
		hlist_del_init(&e->colision);
		spin_unlock_irq(&mdev->req_lock);
	} else {
		D_ASSERT(hlist_unhashed(&e->colision));
	}

	drbd_may_finish_epoch(mdev, e->epoch, EV_put);

	return ok;
}

STATIC int e_send_discard_ack(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct Tl_epoch_entry *e = (struct Tl_epoch_entry *)w;
	int ok = 1;

	D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
	ok = drbd_send_ack(mdev, DiscardAck, e);

	spin_lock_irq(&mdev->req_lock);
	D_ASSERT(!hlist_unhashed(&e->colision));
	hlist_del_init(&e->colision);
	spin_unlock_irq(&mdev->req_lock);

	dec_unacked(mdev);

	return ok;
}

/* Called from receive_Data.
 * Synchronize packets on sock with packets on msock.
 *
 * This is here so even when a Data packet traveling via sock overtook an Ack
 * packet traveling on msock, they are still processed in the order they have
 * been sent.
 *
 * Note: we don't care for Ack packets overtaking Data packets.
 *
 * In case packet_seq is larger than mdev->peer_seq number, there are
 * outstanding packets on the msock. We wait for them to arrive.
 * In case we are the logically next packet, we update mdev->peer_seq
 * ourselves. Correctly handles 32bit wrap around.
 * FIXME verify that atomic_t guarantees 32bit wrap around,
 * otherwise we have to play tricks with << ...
 *
 * Assume we have a 10 GBit connection, that is about 1<<30 byte per second,
 * about 1<<21 sectors per second. So "worst" case, we have 1<<3 == 8 seconds
 * for the 24bit wrap (historical atomic_t guarantee on some archs), and we have
 * 1<<9 == 512 seconds aka ages for the 32bit wrap around...
 *
 * returns 0 if we may process the packet,
 * -ERESTARTSYS if we were interrupted (by disconnect signal). */
static int drbd_wait_peer_seq(struct drbd_conf *mdev, const u32 packet_seq)
{
	DEFINE_WAIT(wait);
	unsigned int p_seq;
	long timeout;
	int ret = 0;
	spin_lock(&mdev->peer_seq_lock);
	for (;;) {
		prepare_to_wait(&mdev->seq_wait, &wait, TASK_INTERRUPTIBLE);
		if (seq_le(packet_seq, mdev->peer_seq+1))
			break;
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		p_seq = mdev->peer_seq;
		spin_unlock(&mdev->peer_seq_lock);
		timeout = schedule_timeout(30*HZ);
		spin_lock(&mdev->peer_seq_lock);
		if (timeout == 0 && p_seq == mdev->peer_seq) {
			ret = -ETIMEDOUT;
			ERR("ASSERT FAILED waited 30 seconds for sequence update, forcing reconnect\n");
			break;
		}
	}
	finish_wait(&mdev->seq_wait, &wait);
	if (mdev->peer_seq+1 == packet_seq)
		mdev->peer_seq++;
	spin_unlock(&mdev->peer_seq_lock);
	return ret;
}

/* mirrored write */
STATIC int receive_Data(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	sector_t sector;
	struct Tl_epoch_entry *e;
	struct Drbd_Data_Packet *p = (struct Drbd_Data_Packet *)h;
	int header_size, data_size;
	int rw = WRITE;
	u32 dp_flags;

	/* FIXME merge this code dups into some helper function */
	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	if (!inc_local(mdev)) {
		/* data is submitted to disk at the end of this function.
		 * corresponding dec_local done either below (on error),
		 * or in drbd_endio_write_sec. */
		if (DRBD_ratelimit(5*HZ, 5))
			ERR("Can not write mirrored data block "
			    "to local disk.\n");
		spin_lock(&mdev->peer_seq_lock);
		if (mdev->peer_seq+1 == be32_to_cpu(p->seq_num))
			mdev->peer_seq++;
		spin_unlock(&mdev->peer_seq_lock);

		drbd_send_ack_dp(mdev, NegAck, p);
		atomic_inc(&mdev->current_epoch->epoch_size);
		return drbd_drain_block(mdev, data_size);
	}

	sector = be64_to_cpu(p->sector);
	e = read_in_block(mdev, p->block_id, sector, data_size);
	if (!e) {
		dec_local(mdev);
		return FALSE;
	}

	e->private_bio->bi_end_io = drbd_endio_write_sec;
	e->w.cb = e_end_block;

	spin_lock(&mdev->epoch_lock);
	e->epoch = mdev->current_epoch;
	atomic_inc(&e->epoch->epoch_size);
	atomic_inc(&e->epoch->active);

	if (mdev->write_ordering == WO_bio_barrier && atomic_read(&e->epoch->epoch_size) == 1) {
		struct drbd_epoch *epoch;
		/* Issue a barrier if we start a new epoch, and the previous epoch
		   was not a epoch containing a single request which already was
		   a Barrier. */
		epoch = list_entry(e->epoch->list.prev, struct drbd_epoch, list);
		if (epoch == e->epoch) {
			MTRACE(TraceTypeEpochs, TraceLvlMetrics,
			       INFO("Add barrier   %p/%d\n",
				    epoch, epoch->barrier_nr);
				);
			set_bit(DE_CONTAINS_A_BARRIER, &e->epoch->flags);
			rw |= (1<<BIO_RW_BARRIER);
			e->flags |= EE_IS_BARRIER;
		} else {
			if (atomic_read(&epoch->epoch_size) > 1 ||
			    !test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags)) {
				MTRACE(TraceTypeEpochs, TraceLvlMetrics,
				       INFO("Add barrier   %p/%d, setting bi in %p/%d\n",
					    e->epoch, e->epoch->barrier_nr,
					    epoch, epoch->barrier_nr);
					);
				set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
				set_bit(DE_CONTAINS_A_BARRIER, &e->epoch->flags);
				rw |= (1<<BIO_RW_BARRIER);
				e->flags |= EE_IS_BARRIER;
			}
		}
	}
	spin_unlock(&mdev->epoch_lock);

	dp_flags = be32_to_cpu(p->dp_flags);
	if (dp_flags & DP_HARDBARRIER)
		rw |= (1<<BIO_RW_BARRIER);
	if (dp_flags & DP_RW_SYNC)
		rw |= (1<<BIO_RW_SYNC);
	if (dp_flags & DP_MAY_SET_IN_SYNC)
		e->flags |= EE_MAY_SET_IN_SYNC;

	/* I'm the receiver, I do hold a net_cnt reference. */
	if (!mdev->net_conf->two_primaries) {
		spin_lock_irq(&mdev->req_lock);
	} else {
		/* don't get the req_lock yet,
		 * we may sleep in drbd_wait_peer_seq */
		const int size = e->size;
		const int discard = test_bit(DISCARD_CONCURRENT, &mdev->flags);
		DEFINE_WAIT(wait);
		struct drbd_request *i;
		struct hlist_node *n;
		struct hlist_head *slot;
		int first;

		D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
		BUG_ON(mdev->ee_hash == NULL);
		BUG_ON(mdev->tl_hash == NULL);

		/* conflict detection and handling:
		 * 1. wait on the sequence number,
		 *    in case this data packet overtook ACK packets.
		 * 2. check our hash tables for conflicting requests.
		 *    we only need to walk the tl_hash, since an ee can not
		 *    have a conflict with an other ee: on the submitting
		 *    node, the corresponding req had already been conflicting,
		 *    and a conflicting req is never sent.
		 *
		 * Note: for two_primaries, we are protocol C,
		 * so there cannot be any request that is DONE
		 * but still on the transfer log.
		 *
		 * unconditionally add to the ee_hash.
		 *
		 * if no conflicting request is found:
		 *    submit.
		 *
		 * if any conflicting request is found
		 * that has not yet been acked,
		 * AND I have the "discard concurrent writes" flag:
		 *	 queue (via done_ee) the DiscardAck; OUT.
		 *
		 * if any conflicting request is found:
		 *	 block the receiver, waiting on misc_wait
		 *	 until no more conflicting requests are there,
		 *	 or we get interrupted (disconnect).
		 *
		 *	 we do not just write after local io completion of those
		 *	 requests, but only after req is done completely, i.e.
		 *	 we wait for the DiscardAck to arrive!
		 *
		 *	 then proceed normally, i.e. submit.
		 */
		if (drbd_wait_peer_seq(mdev, be32_to_cpu(p->seq_num)))
			goto out_interrupted;

		spin_lock_irq(&mdev->req_lock);

		hlist_add_head(&e->colision, ee_hash_slot(mdev, sector));

#define OVERLAPS overlaps(i->sector, i->size, sector, size)
		slot = tl_hash_slot(mdev, sector);
		first = 1;
		for (;;) {
			int have_unacked = 0;
			int have_conflict = 0;
			prepare_to_wait(&mdev->misc_wait, &wait,
				TASK_INTERRUPTIBLE);
			hlist_for_each_entry(i, n, slot, colision) {
				if (OVERLAPS) {
					/* only ALERT on first iteration,
					 * we may be woken up early... */
					if (first)
						ALERT("%s[%u] Concurrent local write detected!"
						      "	new: %llus +%u; pending: %llus +%u\n",
						      current->comm, current->pid,
						      (unsigned long long)sector, size,
						      (unsigned long long)i->sector, i->size);
					if (i->rq_state & RQ_NET_PENDING)
						++have_unacked;
					++have_conflict;
				}
			}
#undef OVERLAPS
			if (!have_conflict)
				break;

			/* Discard Ack only for the _first_ iteration */
			if (first && discard && have_unacked) {
				ALERT("Concurrent write! [DISCARD BY FLAG] sec=%llus\n",
				     (unsigned long long)sector);
				inc_unacked(mdev);
				e->w.cb = e_send_discard_ack;
				list_add_tail(&e->w.list, &mdev->done_ee);

				spin_unlock_irq(&mdev->req_lock);

				/* we could probably send that DiscardAck ourselves,
				 * but I don't like the receiver using the msock */

				dec_local(mdev);
				wake_asender(mdev);
				finish_wait(&mdev->misc_wait, &wait);
				return TRUE;
			}

			if (signal_pending(current)) {
				hlist_del_init(&e->colision);

				spin_unlock_irq(&mdev->req_lock);

				finish_wait(&mdev->misc_wait, &wait);
				goto out_interrupted;
			}

			spin_unlock_irq(&mdev->req_lock);
			if (first) {
				first = 0;
				ALERT("Concurrent write! [W AFTERWARDS] "
				     "sec=%llus\n", (unsigned long long)sector);
			} else if (discard) {
				/* we had none on the first iteration.
				 * there must be none now. */
				D_ASSERT(have_unacked == 0);
			}
			schedule();
			spin_lock_irq(&mdev->req_lock);
		}
		finish_wait(&mdev->misc_wait, &wait);
	}

	list_add(&e->w.list, &mdev->active_ee);
	spin_unlock_irq(&mdev->req_lock);

	switch (mdev->net_conf->wire_protocol) {
	case DRBD_PROT_C:
		inc_unacked(mdev);
		/* corresponding dec_unacked() in e_end_block()
		 * respective _drbd_clear_done_ee */
		break;
	case DRBD_PROT_B:
		/* I really don't like it that the receiver thread
		 * sends on the msock, but anyways */
		drbd_send_ack(mdev, RecvAck, e);
		break;
	case DRBD_PROT_A:
		/* nothing to do */
		break;
	}

	if (mdev->state.pdsk == Diskless) {
		/* In case we have the only disk of the cluster, */
		drbd_set_out_of_sync(mdev, e->sector, e->size);
		e->flags |= EE_CALL_AL_COMPLETE_IO;
		drbd_al_begin_io(mdev, e->sector);
	}

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("submit EE (DATA)WRITE sec=%llus size=%u ee=%p\n",
		    (unsigned long long)e->sector, e->size, e);
	       );
	/* FIXME drbd_al_begin_io in case we have two primaries... */
	e->private_bio->bi_rw = rw;
	dump_internal_bio("Sec", mdev, e->private_bio, 0);
	drbd_generic_make_request(mdev, DRBD_FAULT_DT_WR, e->private_bio);
	/* accounting done in endio */

	maybe_kick_lo(mdev);
	return TRUE;

out_interrupted:
	/* yes, the epoch_size now is imbalanced.
	 * but we drop the connection anyways, so we don't have a chance to
	 * receive a barrier... atomic_inc(&mdev->epoch_size); */
	dec_local(mdev);
	drbd_free_ee(mdev, e);
	return FALSE;
}

STATIC int receive_DataRequest(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	sector_t sector;
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);
	struct Tl_epoch_entry *e;
	struct digest_info *di;
	int size,digest_size;
	unsigned int fault_type;
	struct Drbd_BlockRequest_Packet *p =
		(struct Drbd_BlockRequest_Packet *)h;
	const int brps = sizeof(*p)-sizeof(*h);

	if (drbd_recv(mdev, h->payload, brps) != brps)
		return FALSE;

	sector = be64_to_cpu(p->sector);
	size   = be32_to_cpu(p->blksize);

	if (size <= 0 || (size & 0x1ff) != 0 || size > DRBD_MAX_SEGMENT_SIZE) {
		ERR("%s:%d: sector: %llus, size: %u\n", __FILE__, __LINE__,
				(unsigned long long)sector, size);
		return FALSE;
	}
	if (sector + (size>>9) > capacity) {
		ERR("%s:%d: sector: %llus, size: %u\n", __FILE__, __LINE__,
				(unsigned long long)sector, size);
		return FALSE;
	}

	if (!inc_local_if_state(mdev, UpToDate)) {
		if (DRBD_ratelimit(5*HZ, 5))
			ERR("Can not satisfy peer's read request, "
			    "no local data.\n");
		drbd_send_ack_rp(mdev, h->command == DataRequest ? NegDReply :
				 NegRSDReply , p);
		return TRUE;
	}

	e = drbd_alloc_ee(mdev, p->block_id, sector, size, GFP_KERNEL);
	if (!e) {
		dec_local(mdev);
		return FALSE;
	}

	/* FIXME actually, it could be a READA originating from the peer,
	 * also it could have set some flags (e.g. BIO_RW_SYNC) ... */
	e->private_bio->bi_rw = READ;
	e->private_bio->bi_end_io = drbd_endio_read_sec;

	switch (h->command) {
	case DataRequest:
		e->w.cb = w_e_end_data_req;
		fault_type = DRBD_FAULT_DT_RD;
		break;
	case RSDataRequest:
		e->w.cb = w_e_end_rsdata_req;
		fault_type = DRBD_FAULT_RS_RD;
		/* Eventually this should become asynchrously. Currently it
		 * blocks the whole receiver just to delay the reading of a
		 * resync data block.
		 * the drbd_work_queue mechanism is made for this...
		 */
		if (!drbd_rs_begin_io(mdev, sector)) {
			/* we have been interrupted,
			 * probably connection lost! */
			D_ASSERT(signal_pending(current));
			dec_local(mdev);
			drbd_free_ee(mdev, e);
			return 0;
		}
		break;

	case OVReply:
	case CsumRSRequest:
		fault_type = DRBD_FAULT_RS_RD;
		digest_size = h->length - brps ;
		di = kmalloc(sizeof(*di) + digest_size ,GFP_KERNEL);
		if (!di) {
			dec_local(mdev);
			drbd_free_ee(mdev,e);
			return 0;
		}

		di->digest_size = digest_size;
		di->digest = (((char *)di)+sizeof(struct digest_info));

		if (drbd_recv(mdev, di->digest, digest_size) != digest_size) {
			dec_local(mdev);
			drbd_free_ee(mdev,e);
			kfree(di);
			return FALSE;
		}

		e->block_id = (u64)(unsigned long)di;
		if (h->command == CsumRSRequest) {
			D_ASSERT(mdev->agreed_pro_version >= 89);
			e->w.cb = w_e_end_csum_rs_req;
		} else if (h->command == OVReply) {
			e->w.cb = w_e_end_ov_reply;
			dec_rs_pending(mdev);
			break;
		}

		if (!drbd_rs_begin_io(mdev,sector)) {
			// we have been interrupted, probably connection lost!
			D_ASSERT(signal_pending(current));
			drbd_free_ee(mdev,e);
			kfree(di);
			dec_local(mdev);
			return FALSE;
		}
		break;

	case OVRequest:
		e->w.cb = w_e_end_ov_req;
		fault_type = DRBD_FAULT_RS_RD;
		/* Eventually this should become asynchrously. Currently it
		 * blocks the whole receiver just to delay the reading of a
		 * resync data block.
		 * the drbd_work_queue mechanism is made for this...
		 */
		if (!drbd_rs_begin_io(mdev,sector)) {
			/* we have been interrupted,
			 * probably connection lost! */
			D_ASSERT(signal_pending(current));
			dec_local(mdev);
			drbd_free_ee(mdev,e);
			return 0;
		}
		break;


	default:
		ERR("unexpected command (%s) in receive_DataRequest\n",
		    cmdname(h->command));
		fault_type = DRBD_FAULT_MAX;
	}

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list, &mdev->read_ee);
	spin_unlock_irq(&mdev->req_lock);

	inc_unacked(mdev);

	MTRACE(TraceTypeEE, TraceLvlAll,
	       INFO("submit EE READ sec=%llus size=%u ee=%p\n",
		    (unsigned long long)e->sector, e->size, e);
	       );

	dump_internal_bio("Sec", mdev, e->private_bio, 0);
	drbd_generic_make_request(mdev, fault_type, e->private_bio);
	maybe_kick_lo(mdev);

	return TRUE;
}

STATIC int drbd_asb_recover_0p(struct drbd_conf *mdev) __must_hold(local)
{
	int self, peer, rv = -100;
	unsigned long ch_self, ch_peer;

	self = mdev->bc->md.uuid[Bitmap] & 1;
	peer = mdev->p_uuid[Bitmap] & 1;

	ch_peer = mdev->p_uuid[UUID_SIZE];
	ch_self = mdev->comm_bm_set;

	switch (mdev->net_conf->after_sb_0p) {
	case Consensus:
	case DiscardSecondary:
	case CallHelper:
		ERR("Configuration error.\n");
		break;
	case Disconnect:
		break;
	case DiscardYoungerPri:
		if (self == 0 && peer == 1) { rv = -1; break; }
		if (self == 1 && peer == 0) { rv =  1; break; }
		/* Else fall through to one of the other strategies... */
	case DiscardOlderPri:
		if (self == 0 && peer == 1) { rv =  1; break; }
		if (self == 1 && peer == 0) { rv = -1; break; }
		/* Else fall through to one of the other strategies... */
		drbd_WARN("Discard younger/older primary did not found a decision\n"
		     "Using discard-least-changes instead\n");
	case DiscardZeroChg:
		if (ch_peer == 0 && ch_self == 0) {
			rv = test_bit(DISCARD_CONCURRENT, &mdev->flags)
				? -1 : 1;
			break;
		} else {
			if (ch_peer == 0) { rv =  1; break; }
			if (ch_self == 0) { rv = -1; break; }
		}
		if (mdev->net_conf->after_sb_0p == DiscardZeroChg)
			break;
	case DiscardLeastChg:
		if	(ch_self < ch_peer)
			rv = -1;
		else if (ch_self > ch_peer)
			rv =  1;
		else /* ( ch_self == ch_peer ) */
		     /* Well, then use something else. */
			rv = test_bit(DISCARD_CONCURRENT, &mdev->flags)
				? -1 : 1;
		break;
	case DiscardLocal:
		rv = -1;
		break;
	case DiscardRemote:
		rv =  1;
	}

	return rv;
}

STATIC int drbd_asb_recover_1p(struct drbd_conf *mdev) __must_hold(local)
{
	int self, peer, hg, rv = -100;

	self = mdev->bc->md.uuid[Bitmap] & 1;
	peer = mdev->p_uuid[Bitmap] & 1;

	switch (mdev->net_conf->after_sb_1p) {
	case DiscardYoungerPri:
	case DiscardOlderPri:
	case DiscardLeastChg:
	case DiscardLocal:
	case DiscardRemote:
		ERR("Configuration error.\n");
		break;
	case Disconnect:
		break;
	case Consensus:
		hg = drbd_asb_recover_0p(mdev);
		if (hg == -1 && mdev->state.role == Secondary)
			rv = hg;
		if (hg == 1  && mdev->state.role == Primary)
			rv = hg;
		break;
	case Violently:
		rv = drbd_asb_recover_0p(mdev);
		break;
	case DiscardSecondary:
		return mdev->state.role == Primary ? 1 : -1;
	case CallHelper:
		hg = drbd_asb_recover_0p(mdev);
		if (hg == -1 && mdev->state.role == Primary) {
			self = drbd_set_role(mdev, Secondary, 0);
			if (self != SS_Success) {
				drbd_khelper(mdev, "pri-lost-after-sb");
			} else {
				drbd_WARN("Sucessfully gave up primary role.\n");
				rv = hg;
			}
		} else
			rv = hg;
	}

	return rv;
}

STATIC int drbd_asb_recover_2p(struct drbd_conf *mdev) __must_hold(local)
{
	int self, peer, hg, rv = -100;

	self = mdev->bc->md.uuid[Bitmap] & 1;
	peer = mdev->p_uuid[Bitmap] & 1;

	switch (mdev->net_conf->after_sb_2p) {
	case DiscardYoungerPri:
	case DiscardOlderPri:
	case DiscardLeastChg:
	case DiscardLocal:
	case DiscardRemote:
	case Consensus:
	case DiscardSecondary:
		ERR("Configuration error.\n");
		break;
	case Violently:
		rv = drbd_asb_recover_0p(mdev);
		break;
	case Disconnect:
		break;
	case CallHelper:
		hg = drbd_asb_recover_0p(mdev);
		if (hg == -1) {
			self = drbd_set_role(mdev, Secondary, 0);
			if (self != SS_Success) {
				drbd_khelper(mdev, "pri-lost-after-sb");
			} else {
				drbd_WARN("Sucessfully gave up primary role.\n");
				rv = hg;
			}
		} else
			rv = hg;
	}

	return rv;
}

STATIC void drbd_uuid_dump(struct drbd_conf *mdev, char *text, u64 *uuid)
{
	if (!uuid) {
		INFO("%s uuid info vanished while I was looking!\n", text);
		return;
	}
	INFO("%s %016llX:%016llX:%016llX:%016llX\n",
	     text,
	     (unsigned long long)uuid[Current],
	     (unsigned long long)uuid[Bitmap],
	     (unsigned long long)uuid[History_start],
	     (unsigned long long)uuid[History_end]);
}

/*
  100	after split brain try auto recover
    2	SyncSource set BitMap
    1	SyncSource use BitMap
    0	no Sync
   -1	SyncTarget use BitMap
   -2	SyncTarget set BitMap
 -100	after split brain, disconnect
-1000	unrelated data
 */
STATIC int drbd_uuid_compare(struct drbd_conf *mdev, int *rule_nr) __must_hold(local)
{
	u64 self, peer;
	int i, j;

	self = mdev->bc->md.uuid[Current] & ~((u64)1);
	peer = mdev->p_uuid[Current] & ~((u64)1);

	*rule_nr = 1;
	if (self == UUID_JUST_CREATED &&
	    peer == UUID_JUST_CREATED) return 0;

	*rule_nr = 2;
	if ((self == UUID_JUST_CREATED || self == (u64)0) &&
	     peer != UUID_JUST_CREATED) return -2;

	*rule_nr = 3;
	if ( self != UUID_JUST_CREATED &&
	    (peer == UUID_JUST_CREATED || peer == (u64)0)) return 2;

	*rule_nr = 4;
	if (self == peer) { /* Common power [off|failure] */
		int rct, dc; /* roles at crash time */

		rct = (test_bit(CRASHED_PRIMARY, &mdev->flags) ? 1 : 0) +
			(mdev->p_uuid[UUID_FLAGS] & 2);
		/* lowest bit is set when we were primary,
		 * next bit (weight 2) is set when peer was primary */

		MTRACE(TraceTypeUuid, TraceLvlMetrics, DUMPI(rct); );

		switch (rct) {
		case 0: /* !self_pri && !peer_pri */ return 0;
		case 1: /*  self_pri && !peer_pri */ return 1;
		case 2: /* !self_pri &&  peer_pri */ return -1;
		case 3: /*  self_pri &&  peer_pri */
			dc = test_bit(DISCARD_CONCURRENT, &mdev->flags);
			MTRACE(TraceTypeUuid, TraceLvlMetrics, DUMPI(dc); );
			return dc ? -1 : 1;
		}
	}

	*rule_nr = 5;
	peer = mdev->p_uuid[Bitmap] & ~((u64)1);
	if (self == peer)
		return -1;

	*rule_nr = 6;
	for (i = History_start; i <= History_end; i++) {
		peer = mdev->p_uuid[i] & ~((u64)1);
		if (self == peer)
			return -2;
	}

	*rule_nr = 7;
	self = mdev->bc->md.uuid[Bitmap] & ~((u64)1);
	peer = mdev->p_uuid[Current] & ~((u64)1);
	if (self == peer)
		return 1;

	*rule_nr = 8;
	for (i = History_start; i <= History_end; i++) {
		self = mdev->bc->md.uuid[i] & ~((u64)1);
		if (self == peer)
			return 2;
	}

	*rule_nr = 9;
	self = mdev->bc->md.uuid[Bitmap] & ~((u64)1);
	peer = mdev->p_uuid[Bitmap] & ~((u64)1);
	if (self == peer && self != ((u64)0))
		return 100;

	*rule_nr = 10;
	for (i = History_start; i <= History_end; i++) {
		self = mdev->p_uuid[i] & ~((u64)1);
		for (j = History_start; j <= History_end; j++) {
			peer = mdev->p_uuid[j] & ~((u64)1);
			if (self == peer)
				return -100;
		}
	}

	return -1000;
}

/* drbd_sync_handshake() returns the new conn state on success, or
   conn_mask (-1) on failure.
 */
STATIC enum drbd_conns drbd_sync_handshake(struct drbd_conf *mdev, enum drbd_role peer_role,
					   enum drbd_disk_state peer_disk) __must_hold(local)
{
	int hg, rule_nr;
	enum drbd_conns rv = conn_mask;
	enum drbd_disk_state mydisk;

	mydisk = mdev->state.disk;
	if (mydisk == Negotiating)
		mydisk = mdev->new_state_tmp.disk;

	hg = drbd_uuid_compare(mdev, &rule_nr);

	INFO("drbd_sync_handshake:\n");
	drbd_uuid_dump(mdev, "self", mdev->bc->md.uuid);
	drbd_uuid_dump(mdev, "peer", mdev->p_uuid);
	INFO("uuid_compare()=%d by rule %d\n", hg, rule_nr);

	if (hg == -1000) {
		ALERT("Unrelated data, aborting!\n");
		return conn_mask;
	}

	if (   (mydisk == Inconsistent && peer_disk > Inconsistent) ||
	    (peer_disk == Inconsistent && mydisk    > Inconsistent))  {
		int f = (hg == -100) || abs(hg) == 2;
		hg = mydisk > Inconsistent ? 1 : -1;
		if (f)
			hg = hg*2;
		INFO("Becoming sync %s due to disk states.\n",
		     hg > 0 ? "source" : "target");
	}

	if (hg == 100 || (hg == -100 && mdev->net_conf->always_asbp)) {
		int pcount = (mdev->state.role == Primary)
			   + (peer_role == Primary);
		int forced = (hg == -100);

		switch (pcount) {
		case 0:
			hg = drbd_asb_recover_0p(mdev);
			break;
		case 1:
			hg = drbd_asb_recover_1p(mdev);
			break;
		case 2:
			hg = drbd_asb_recover_2p(mdev);
			break;
		}
		if (abs(hg) < 100) {
			drbd_WARN("Split-Brain detected, %d primaries, "
			     "automatically solved. Sync from %s node\n",
			     pcount, (hg < 0) ? "peer" : "this");
			if (forced) {
				drbd_WARN("Doing a full sync, since"
				     " UUIDs where ambiguous.\n");
				drbd_uuid_dump(mdev, "self", mdev->bc->md.uuid);
				drbd_uuid_dump(mdev, "peer", mdev->p_uuid);
				hg = hg*2;
			}
		}
	}

	if (hg == -100) {
		if (mdev->net_conf->want_lose && !(mdev->p_uuid[UUID_FLAGS]&1))
			hg = -1;
		if (!mdev->net_conf->want_lose && (mdev->p_uuid[UUID_FLAGS]&1))
			hg = 1;

		if (abs(hg) < 100)
			drbd_WARN("Split-Brain detected, manually solved. "
			     "Sync from %s node\n",
			     (hg < 0) ? "peer" : "this");
	}

	if (hg == -100) {
		ALERT("Split-Brain detected, dropping connection!\n");
		drbd_uuid_dump(mdev, "self", mdev->bc->md.uuid);
		drbd_uuid_dump(mdev, "peer", mdev->p_uuid);
		drbd_khelper(mdev, "split-brain");
		return conn_mask;
	}

	if (hg > 0 && mydisk <= Inconsistent) {
		ERR("I shall become SyncSource, but I am inconsistent!\n");
		return conn_mask;
	}

	if (hg < 0 && /* by intention we do not use mydisk here. */
	    mdev->state.role == Primary && mdev->state.disk >= Consistent) {
		switch (mdev->net_conf->rr_conflict) {
		case CallHelper:
			drbd_khelper(mdev, "pri-lost");
			/* fall through */
		case Disconnect:
			ERR("I shall become SyncTarget, but I am primary!\n");
			return conn_mask;
		case Violently:
			drbd_WARN("Becoming SyncTarget, violating the stable-data"
			     "assumption\n");
		}
	}

	if (abs(hg) >= 2) {
		INFO("Writing the whole bitmap, full sync required after drbd_sync_handshake.\n");
		if (drbd_bitmap_io(mdev, &drbd_bmio_set_n_write, "set_n_write from sync_handshake"))
			return conn_mask;
	}

	if (hg > 0) { /* become sync source. */
		rv = WFBitMapS;
	} else if (hg < 0) { /* become sync target */
		rv = WFBitMapT;
	} else {
		rv = Connected;
		if (drbd_bm_total_weight(mdev)) {
			INFO("No resync, but %lu bits in bitmap!\n",
			     drbd_bm_total_weight(mdev));
		}
	}

	drbd_bm_recount_bits(mdev);

	return rv;
}

/* returns 1 if invalid */
STATIC int cmp_after_sb(enum after_sb_handler peer, enum after_sb_handler self)
{
	/* DiscardRemote - DiscardLocal is valid */
	if ((peer == DiscardRemote && self == DiscardLocal) ||
	    (self == DiscardRemote && peer == DiscardLocal))
		return 0;

	/* any other things with DiscardRemote or DiscardLocal are invalid */
	if (peer == DiscardRemote || peer == DiscardLocal ||
	    self == DiscardRemote || self == DiscardLocal)
		return 1;

	/* everything else is valid if they are equal on both sides. */
	if (peer == self)
		return 0;

	/* everything es is invalid. */
	return 1;
}

STATIC int receive_protocol(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_Protocol_Packet *p = (struct Drbd_Protocol_Packet *)h;
	int header_size, data_size;
	int p_proto, p_after_sb_0p, p_after_sb_1p, p_after_sb_2p;
	int p_want_lose, p_two_primaries;
	char p_integrity_alg[SHARED_SECRET_MAX] = "";

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	p_proto		= be32_to_cpu(p->protocol);
	p_after_sb_0p	= be32_to_cpu(p->after_sb_0p);
	p_after_sb_1p	= be32_to_cpu(p->after_sb_1p);
	p_after_sb_2p	= be32_to_cpu(p->after_sb_2p);
	p_want_lose	= be32_to_cpu(p->want_lose);
	p_two_primaries = be32_to_cpu(p->two_primaries);

	if (p_proto != mdev->net_conf->wire_protocol) {
		ERR("incompatible communication protocols\n");
		goto disconnect;
	}

	if (cmp_after_sb(p_after_sb_0p, mdev->net_conf->after_sb_0p)) {
		ERR("incompatible after-sb-0pri settings\n");
		goto disconnect;
	}

	if (cmp_after_sb(p_after_sb_1p, mdev->net_conf->after_sb_1p)) {
		ERR("incompatible after-sb-1pri settings\n");
		goto disconnect;
	}

	if (cmp_after_sb(p_after_sb_2p, mdev->net_conf->after_sb_2p)) {
		ERR("incompatible after-sb-2pri settings\n");
		goto disconnect;
	}

	if (p_want_lose && mdev->net_conf->want_lose) {
		ERR("both sides have the 'want_lose' flag set\n");
		goto disconnect;
	}

	if (p_two_primaries != mdev->net_conf->two_primaries) {
		ERR("incompatible setting of the two-primaries options\n");
		goto disconnect;
	}

	if (mdev->agreed_pro_version >= 87) {
		unsigned char *my_alg = mdev->net_conf->integrity_alg;

		if (drbd_recv(mdev, p_integrity_alg, data_size) != data_size)
			return FALSE;

		p_integrity_alg[SHARED_SECRET_MAX-1] = 0;
		if (strcmp(p_integrity_alg, my_alg)) {
			ERR("incompatible setting of the data-integrity-alg\n");
			goto disconnect;
		}
		INFO("data-integrity-alg: %s\n",
		     my_alg[0] ? my_alg : (unsigned char *)"<not-used>");
	}

	return TRUE;

disconnect:
	drbd_force_state(mdev, NS(conn, Disconnecting));
	return FALSE;
}

/* helper function
 * input: alg name, feature name
 * return: NULL (alg name was "")
 *         ERR_PTR(error) if something goes wrong
 *         or the crypto hash ptr, if it worked out ok. */
struct crypto_hash *drbd_crypto_alloc_digest_safe(const struct drbd_conf *mdev,
		const char *alg, const char *name)
{
	struct crypto_hash *tfm;

	if (!alg[0])
		return NULL;

	tfm = crypto_alloc_hash(alg, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		ERR("Can not allocate \"%s\" as %s (reason: %ld)\n",
				alg, name, PTR_ERR(tfm));
		return tfm;
	}
	if (crypto_tfm_alg_type(crypto_hash_tfm(tfm)) != CRYPTO_ALG_TYPE_DIGEST) {
		crypto_free_hash(tfm);
		ERR("\"%s\" is not a digest (%s)\n", alg, name);
		return ERR_PTR(-EINVAL);
	}
	return tfm;
}

STATIC int receive_SyncParam(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	int ok = TRUE;
	struct Drbd_SyncParam89_Packet *p = (struct Drbd_SyncParam89_Packet *)h;
	unsigned int header_size, data_size, exp_max_sz;
	struct crypto_hash *verify_tfm = NULL;
	struct crypto_hash *csums_tfm = NULL;
	const int apv = mdev->agreed_pro_version;

	exp_max_sz  = apv <= 87 ? sizeof(struct Drbd_SyncParam_Packet)
		    : apv == 88 ? sizeof(struct Drbd_SyncParam_Packet)
					+ SHARED_SECRET_MAX
		    : /* 89 */    sizeof(struct Drbd_SyncParam89_Packet);

	if (h->length > exp_max_sz) {
		ERR("SyncParam packet too long: received %u, expected <= %u bytes\n",
		    h->length, exp_max_sz);
		return FALSE;
	}

	if (apv <= 88) {
		header_size = sizeof(struct Drbd_SyncParam_Packet) - sizeof(*h);
		data_size   = h->length  - header_size;
	} else /* apv >= 89 */ {
		header_size = sizeof(struct Drbd_SyncParam89_Packet) - sizeof(*h);
		data_size   = h->length  - header_size;
		D_ASSERT(data_size == 0);
	}

	/* initialize verify_alg and csums_alg */
	memset(p->verify_alg, 0, 2 * SHARED_SECRET_MAX);

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	mdev->sync_conf.rate	  = be32_to_cpu(p->rate);

	if (apv >= 88) {
		if (apv == 88) {
			if (data_size > SHARED_SECRET_MAX) {
				ERR("verify-alg too long, "
				    "peer wants %u, accepting only %u byte\n",
						data_size, SHARED_SECRET_MAX);
				return FALSE;
			}

			if (drbd_recv(mdev, p->verify_alg, data_size) != data_size)
				return FALSE;

			/* we expect NUL terminated string */
			/* but just in case someone tries to be evil */
			D_ASSERT(p->verify_alg[data_size-1] == 0);
			p->verify_alg[data_size-1] = 0;

		} else /* apv >= 89 */ {
			/* we still expect NUL terminated strings */
			/* but just in case someone tries to be evil */
			D_ASSERT(p->verify_alg[SHARED_SECRET_MAX-1] == 0);
			D_ASSERT(p->csums_alg[SHARED_SECRET_MAX-1] == 0);
			p->verify_alg[SHARED_SECRET_MAX-1] = 0;
			p->csums_alg[SHARED_SECRET_MAX-1] = 0;
		}

		if (strcmp(mdev->sync_conf.verify_alg, p->verify_alg)) {
			if (mdev->state.conn == WFReportParams) {
				ERR("Different verify-alg settings. me=\"%s\" peer=\"%s\"\n",
				    mdev->sync_conf.verify_alg, p->verify_alg);
				goto disconnect;
			}
			verify_tfm = drbd_crypto_alloc_digest_safe(mdev,
					p->verify_alg, "verify-alg");
			if (IS_ERR(verify_tfm))
				goto disconnect;
		}

		if (apv >= 89 && strcmp(mdev->sync_conf.csums_alg, p->csums_alg)) {
			if (mdev->state.conn == WFReportParams) {
				ERR("Different csums-alg settings. me=\"%s\" peer=\"%s\"\n",
				    mdev->sync_conf.csums_alg, p->csums_alg);
				goto disconnect;
			}
			csums_tfm = drbd_crypto_alloc_digest_safe(mdev,
					p->csums_alg, "csums-alg");
			if (IS_ERR(csums_tfm))
				goto disconnect;
		}


		spin_lock(&mdev->peer_seq_lock);
		/* lock against drbd_nl_syncer_conf() */
		if (verify_tfm) {
			strcpy(mdev->sync_conf.verify_alg, p->verify_alg);
			mdev->sync_conf.verify_alg_len = strlen(p->verify_alg) + 1;
			crypto_free_hash(mdev->verify_tfm);
			mdev->verify_tfm = verify_tfm;
			INFO("using verify-alg: \"%s\"\n", p->verify_alg);
		}
		if (csums_tfm) {
			strcpy(mdev->sync_conf.csums_alg, p->csums_alg);
			mdev->sync_conf.csums_alg_len = strlen(p->csums_alg) + 1;
			crypto_free_hash(mdev->csums_tfm);
			mdev->csums_tfm = csums_tfm;
			INFO("using csums-alg: \"%s\"\n", p->csums_alg);
		}
		spin_unlock(&mdev->peer_seq_lock);
	}

	return ok;
disconnect:
	crypto_free_hash(verify_tfm);
	drbd_force_state(mdev, NS(conn, Disconnecting));
	return FALSE;
}

STATIC void drbd_setup_order_type(struct drbd_conf *mdev, int peer)
{
	/* sorry, we currently have no working implementation
	 * of distributed TCQ */
}

/* warn if the arguments differ by more than 12.5% */
static void warn_if_differ_considerably(struct drbd_conf *mdev,
	const char *s, sector_t a, sector_t b)
{
	sector_t d;
	if (a == 0 || b == 0)
		return;
	d = (a > b) ? (a - b) : (b - a);
	if (d > (a>>3) || d > (b>>3))
		drbd_WARN("Considerable difference in %s: %llus vs. %llus\n", s,
		     (unsigned long long)a, (unsigned long long)b);
}

STATIC int receive_sizes(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_Sizes_Packet *p = (struct Drbd_Sizes_Packet *)h;
	enum determin_dev_size_enum dd = unchanged;
	unsigned int max_seg_s;
	sector_t p_size, p_usize, my_usize;
	int ldsc = 0; /* local disk size changed */
	enum drbd_conns nconn;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	p_size = be64_to_cpu(p->d_size);
	p_usize = be64_to_cpu(p->u_size);

	if (p_size == 0 && mdev->state.disk == Diskless) {
		ERR("some backing storage is needed\n");
		drbd_force_state(mdev, NS(conn, Disconnecting));
		return FALSE;
	}

	/* just store the peer's disk size for now.
	 * we still need to figure out wether we accept that. */
	mdev->p_size = p_size;

#define min_not_zero(l, r) (l == 0) ? r : ((r == 0) ? l : min(l, r))
	if (inc_local(mdev)) {
		warn_if_differ_considerably(mdev, "lower level device sizes",
			   p_size, drbd_get_max_capacity(mdev->bc));
		warn_if_differ_considerably(mdev, "user requested size",
					    p_usize, mdev->bc->dc.disk_size);

		/* if this is the first connect, or an otherwise expected
		 * param exchange, choose the minimum */
		if (mdev->state.conn == WFReportParams)
			p_usize = min_not_zero((sector_t)mdev->bc->dc.disk_size,
					     p_usize);

		my_usize = mdev->bc->dc.disk_size;

		if (mdev->bc->dc.disk_size != p_usize) {
			mdev->bc->dc.disk_size = p_usize;
			INFO("Peer sets u_size to %lu sectors\n",
			     (unsigned long)mdev->bc->dc.disk_size);
		}

		/* Never shrink a device with usable data during connect.
		   But allow online shrinking if we are connected. */
		if (drbd_new_dev_size(mdev, mdev->bc) <
		   drbd_get_capacity(mdev->this_bdev) &&
		   mdev->state.disk >= Outdated &&
		   mdev->state.conn < Connected) {
			ERR("The peer's disk size is too small!\n");
			drbd_force_state(mdev, NS(conn, Disconnecting));
			mdev->bc->dc.disk_size = my_usize;
			dec_local(mdev);
			return FALSE;
		}
		dec_local(mdev);
	}
#undef min_not_zero

	if (inc_local(mdev)) {
		dd = drbd_determin_dev_size(mdev);
		dec_local(mdev);
		if (dd == dev_size_error)
			return FALSE;
		drbd_md_sync(mdev);
	} else {
		/* I am diskless, need to accept the peer's size. */
		drbd_set_my_capacity(mdev, p_size);
	}

	if (mdev->p_uuid && mdev->state.conn <= Connected && inc_local(mdev)) {
		nconn = drbd_sync_handshake(mdev,
				mdev->state.peer, mdev->state.pdsk);
		dec_local(mdev);

		if (nconn == conn_mask) {
			drbd_force_state(mdev, NS(conn, Disconnecting));
			return FALSE;
		}

		if (drbd_request_state(mdev, NS(conn, nconn)) < SS_Success) {
			drbd_force_state(mdev, NS(conn, Disconnecting));
			return FALSE;
		}
	}

	if (inc_local(mdev)) {
		if (mdev->bc->known_size != drbd_get_capacity(mdev->bc->backing_bdev)) {
			mdev->bc->known_size = drbd_get_capacity(mdev->bc->backing_bdev);
			ldsc = 1;
		}

		max_seg_s = be32_to_cpu(p->max_segment_size);
		if (max_seg_s != mdev->rq_queue->max_segment_size)
			drbd_setup_queue_param(mdev, max_seg_s);

		drbd_setup_order_type(mdev, be32_to_cpu(p->queue_order_type));
		dec_local(mdev);
	}

	if (mdev->state.conn > WFReportParams) {
		if (be64_to_cpu(p->c_size) !=
		    drbd_get_capacity(mdev->this_bdev) || ldsc) {
			/* we have different sizes, probabely peer
			 * needs to know my new size... */
			drbd_send_sizes(mdev);
		}
		if (dd == grew && mdev->state.conn == Connected) {
			if (mdev->state.pdsk >= Inconsistent &&
			    mdev->state.disk >= Inconsistent)
				resync_after_online_grow(mdev);
			else
				set_bit(RESYNC_AFTER_NEG, &mdev->flags);
		}
	}

	return TRUE;
}

STATIC int receive_uuids(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_GenCnt_Packet *p = (struct Drbd_GenCnt_Packet *)h;
	u64 *p_uuid;
	int i;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	p_uuid = kmalloc(sizeof(u64)*EXT_UUID_SIZE, GFP_KERNEL);

	for (i = Current; i < EXT_UUID_SIZE; i++)
		p_uuid[i] = be64_to_cpu(p->uuid[i]);

	kfree(mdev->p_uuid);
	mdev->p_uuid = p_uuid;

	if (mdev->state.conn < Connected &&
	    mdev->state.disk < Inconsistent &&
	    mdev->state.role == Primary &&
	    (mdev->ed_uuid & ~((u64)1)) != (p_uuid[Current] & ~((u64)1))) {
		ERR("Can only connect to data with current UUID=%016llX\n",
		    (unsigned long long)mdev->ed_uuid);
		drbd_force_state(mdev, NS(conn, Disconnecting));
		return FALSE;
	}

	/* Before we test for the disk state, we should wait until an eventually
	   ongoing cluster wide state change is finished. That is important if
	   we are primary and are detaching from our disk. We need to see the
	   new disk state... */
	wait_event(mdev->misc_wait, !test_bit(CLUSTER_ST_CHANGE, &mdev->flags));
	if (mdev->state.conn >= Connected && mdev->state.disk < Inconsistent)
		drbd_set_ed_uuid(mdev, p_uuid[Current]);

	return TRUE;
}

/**
 * convert_state:
 * Switches the view of the state.
 */
STATIC union drbd_state_t convert_state(union drbd_state_t ps)
{
	union drbd_state_t ms;

	static enum drbd_conns c_tab[] = {
		[Connected] = Connected,

		[StartingSyncS] = StartingSyncT,
		[StartingSyncT] = StartingSyncS,
		[Disconnecting] = TearDown, /* NetworkFailure, */
		[VerifyS]       = VerifyT,
		[conn_mask]   = conn_mask,
	};

	ms.i = ps.i;

	ms.conn = c_tab[ps.conn];
	ms.peer = ps.role;
	ms.role = ps.peer;
	ms.pdsk = ps.disk;
	ms.disk = ps.pdsk;
	ms.peer_isp = (ps.aftr_isp | ps.user_isp);

	return ms;
}

STATIC int receive_req_state(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_Req_State_Packet *p = (struct Drbd_Req_State_Packet *)h;
	union drbd_state_t mask, val;
	int rv;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	mask.i = be32_to_cpu(p->mask);
	val.i = be32_to_cpu(p->val);

	if (test_bit(DISCARD_CONCURRENT, &mdev->flags) &&
	    test_bit(CLUSTER_ST_CHANGE, &mdev->flags)) {
		drbd_send_sr_reply(mdev, SS_ConcurrentStChg);
		return TRUE;
	}

	mask = convert_state(mask);
	val = convert_state(val);

	DRBD_STATE_DEBUG_INIT_VAL(val);
	rv = drbd_change_state(mdev, ChgStateVerbose, mask, val);

	drbd_send_sr_reply(mdev, rv);
	drbd_md_sync(mdev);

	return TRUE;
}

STATIC int receive_state(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_State_Packet *p = (struct Drbd_State_Packet *)h;
	enum drbd_conns nconn, oconn;
	union drbd_state_t ns, peer_state;
	enum drbd_disk_state real_peer_disk;
	int rv;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h)))
		return FALSE;

	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	peer_state.i = be32_to_cpu(p->state);

	real_peer_disk = peer_state.disk;
	if (peer_state.disk == Negotiating) {
		real_peer_disk = mdev->p_uuid[UUID_FLAGS] & 4 ? Inconsistent : Consistent;
		INFO("real peer disk state = %s\n", disks_to_name(real_peer_disk));
	}

	spin_lock_irq(&mdev->req_lock);
 retry:
	oconn = nconn = mdev->state.conn;
	spin_unlock_irq(&mdev->req_lock);

	if (nconn == WFReportParams)
		nconn = Connected;

	if (mdev->p_uuid && peer_state.disk >= Negotiating &&
	    inc_local_if_state(mdev, Negotiating)) {
		int cr; /* consider resync */

		cr  = (oconn < Connected);
		cr |= (oconn == Connected &&
		       (peer_state.disk == Negotiating ||
			mdev->state.disk == Negotiating));
		cr |= test_bit(CONSIDER_RESYNC, &mdev->flags); /* peer forced */
		cr |= (oconn == Connected && peer_state.conn > Connected);

		if (cr)
			nconn = drbd_sync_handshake(mdev, peer_state.role, real_peer_disk);

		dec_local(mdev);
		if (nconn == conn_mask) {
			if (mdev->state.disk == Negotiating) {
				drbd_force_state(mdev, NS(disk, Diskless));
				nconn = Connected;
			} else if (peer_state.disk == Negotiating) {
				ERR("Disk attach process on the peer node was aborted.\n");
				peer_state.disk = Diskless;
			} else {
				D_ASSERT(oconn == WFReportParams);
				drbd_force_state(mdev, NS(conn, Disconnecting));
				return FALSE;
			}
		}
	}

	spin_lock_irq(&mdev->req_lock);
	if (mdev->state.conn != oconn)
		goto retry;
	clear_bit(CONSIDER_RESYNC, &mdev->flags);
	ns.i = mdev->state.i;
	ns.conn = nconn;
	ns.peer = peer_state.role;
	ns.pdsk = real_peer_disk;
	ns.peer_isp = (peer_state.aftr_isp | peer_state.user_isp);
	if ((nconn == Connected || nconn == WFBitMapS) && ns.disk == Negotiating)
		ns.disk = mdev->new_state_tmp.disk;
	DRBD_STATE_DEBUG_INIT_VAL(ns);
	rv = _drbd_set_state(mdev, ns, ChgStateVerbose | ChgStateHard, NULL);
	ns = mdev->state;
	spin_unlock_irq(&mdev->req_lock);

	if (rv < SS_Success) {
		drbd_force_state(mdev, NS(conn, Disconnecting));
		return FALSE;
	}

	if (oconn > WFReportParams) {
		if (nconn > Connected && peer_state.conn <= Connected &&
		    peer_state.disk != Negotiating ) {
			/* we want resync, peer has not yet decided to sync... */
			/* Nowadays only used when forcing a node into primary role and
			   setting its disk to UpTpDate with that */
			drbd_send_uuids(mdev);
			drbd_send_state(mdev);
		}
	}

	mdev->net_conf->want_lose = 0;

	/* FIXME assertion for (gencounts do not diverge) */
	drbd_md_sync(mdev); /* update connected indicator, la_size, ... */

	return TRUE;
}

STATIC int receive_sync_uuid(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_SyncUUID_Packet *p = (struct Drbd_SyncUUID_Packet *)h;

	wait_event(mdev->misc_wait,
		   mdev->state.conn < Connected ||
		   mdev->state.conn == WFSyncUUID);

	/* D_ASSERT( mdev->state.conn == WFSyncUUID ); */

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	/* Here the _drbd_uuid_ functions are right, current should
	   _not_ be rotated into the history */
	if (inc_local_if_state(mdev, Negotiating)) {
		_drbd_uuid_set(mdev, Current, be64_to_cpu(p->uuid));
		_drbd_uuid_set(mdev, Bitmap, 0UL);

		drbd_start_resync(mdev, SyncTarget);

		dec_local(mdev);
	} else
		ERR("Ignoring SyncUUID packet!\n");

	return TRUE;
}

/* Since we are processing the bitfild from lower addresses to higher,
   it does not matter if the process it in 32 bit chunks or 64 bit
   chunks as long as it is little endian. (Understand it as byte stream,
   beginning with the lowest byte...) If we would use big endian
   we would need to process it from the highest address to the lowest,
   in order to be agnostic to the 32 vs 64 bits issue.

   returns 0 on failure, 1 if we suceessfully received it. */
STATIC int receive_bitmap(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	size_t bm_words, bm_i, want, num_words;
	unsigned long *buffer;
	int ok = FALSE;

	wait_event(mdev->misc_wait, !atomic_read(&mdev->ap_bio_cnt));

	drbd_bm_lock(mdev, "receive bitmap");

	bm_words = drbd_bm_words(mdev);
	bm_i	 = 0;
	/* maybe we should use some per thread scratch page,
	 * and allocate that during initial device creation? */
	buffer	 = (unsigned long *) __get_free_page(GFP_NOIO);
	if (!buffer) {
		ERR("failed to allocate one page buffer in %s\n", __func__);
		goto out;
	}

	while (1) {
		num_words = min_t(size_t, BM_PACKET_WORDS, bm_words-bm_i);
		want = num_words * sizeof(long);
		ERR_IF(want != h->length) goto out;
		if (want == 0)
			break;
		if (drbd_recv(mdev, buffer, want) != want)
			goto out;

		drbd_bm_merge_lel(mdev, bm_i, num_words, buffer);
		bm_i += num_words;

		if (!drbd_recv_header(mdev, h))
			goto out;
		D_ASSERT(h->command == ReportBitMap);
	}

	if (mdev->state.conn == WFBitMapT) {
		ok = !drbd_send_bitmap(mdev);
		if (!ok)
			goto out;
		/* Omit ChgOrdered with this state transition to avoid deadlocks. */
		ok = _drbd_request_state(mdev, NS(conn, WFSyncUUID), ChgStateVerbose);
		D_ASSERT(ok == SS_Success);
	} else if (mdev->state.conn != WFBitMapS) {
		/* admin may have requested Disconnecting,
		 * other threads may have noticed network errors */
		INFO("unexpected cstate (%s) in receive_bitmap\n",
		    conns_to_name(mdev->state.conn));
	}

	ok = TRUE;
 out:
	drbd_bm_unlock(mdev);
	if (ok && mdev->state.conn == WFBitMapS)
		drbd_start_resync(mdev, SyncSource);
	free_page((unsigned long) buffer);
	return ok;
}

STATIC int receive_skip(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	/* TODO zero copy sink :) */
	static char sink[128];
	int size, want, r;

	drbd_WARN("skipping unknown optional packet type %d, l: %d!\n",
	     h->command, h->length);

	size = h->length;
	while (size > 0) {
		want = min_t(int, size, sizeof(sink));
		r = drbd_recv(mdev, sink, want);
		ERR_IF(r < 0) break;
		size -= r;
	}
	return size == 0;
}

STATIC int receive_UnplugRemote(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	if (mdev->state.disk >= Inconsistent)
		drbd_kick_lo(mdev);

	/* Make sure we've acked all the TCP data associated
	 * with the data requests being unplugged */
	drbd_tcp_quickack(mdev->data.socket);

	return TRUE;
}

typedef int (*drbd_cmd_handler_f)(struct drbd_conf *, struct Drbd_Header *);

static drbd_cmd_handler_f drbd_default_handler[] = {
	[Data]		   = receive_Data,
	[DataReply]	   = receive_DataReply,
	[RSDataReply]	   = receive_RSDataReply,
	[Barrier]	   = receive_Barrier,
	[ReportBitMap]	   = receive_bitmap,
	[UnplugRemote]	   = receive_UnplugRemote,
	[DataRequest]	   = receive_DataRequest,
	[RSDataRequest]    = receive_DataRequest,
	[SyncParam]	   = receive_SyncParam,
	[SyncParam89]	   = receive_SyncParam,
	[ReportProtocol]   = receive_protocol,
	[ReportUUIDs]	   = receive_uuids,
	[ReportSizes]	   = receive_sizes,
	[ReportState]	   = receive_state,
	[StateChgRequest]  = receive_req_state,
	[ReportSyncUUID]   = receive_sync_uuid,
	[OVRequest]        = receive_DataRequest,
	[OVReply]          = receive_DataRequest,
	[CsumRSRequest]    = receive_DataRequest,
	/* anything missing from this table is in
	 * the asender_tbl, see get_asender_cmd */
	[MAX_CMD]	   = NULL,
};

static drbd_cmd_handler_f *drbd_cmd_handler = drbd_default_handler;
static drbd_cmd_handler_f *drbd_opt_cmd_handler;

STATIC void drbdd(struct drbd_conf *mdev)
{
	drbd_cmd_handler_f handler;
	struct Drbd_Header *header = &mdev->data.rbuf.head;

	while (get_t_state(&mdev->receiver) == Running) {
		drbd_thread_current_set_cpu(mdev);
		if (!drbd_recv_header(mdev, header))
			break;

		if (header->command < MAX_CMD)
			handler = drbd_cmd_handler[header->command];
		else if (MayIgnore < header->command
		     && header->command < MAX_OPT_CMD)
			handler = drbd_opt_cmd_handler[header->command-MayIgnore];
		else if (header->command > MAX_OPT_CMD)
			handler = receive_skip;
		else
			handler = NULL;

		if (unlikely(!handler)) {
			ERR("unknown packet type %d, l: %d!\n",
			    header->command, header->length);
			drbd_force_state(mdev, NS(conn, ProtocolError));
			break;
		}
		if (unlikely(!handler(mdev, header))) {
			ERR("error receiving %s, l: %d!\n",
			    cmdname(header->command), header->length);
			drbd_force_state(mdev, NS(conn, ProtocolError));
			break;
		}

		dump_packet(mdev, mdev->data.socket, 2, &mdev->data.rbuf,
				__FILE__, __LINE__);
	}
}

/* FIXME how should freeze-io be handled? */
STATIC void drbd_fail_pending_reads(struct drbd_conf *mdev)
{
	struct hlist_head *slot;
	struct hlist_node *pos;
	struct hlist_node *tmp;
	struct drbd_request *req;
	int i;

	/*
	 * Application READ requests
	 */
	spin_lock_irq(&mdev->req_lock);
	for (i = 0; i < APP_R_HSIZE; i++) {
		slot = mdev->app_reads_hash+i;
		hlist_for_each_entry_safe(req, pos, tmp, slot, colision) {
			/* it may (but should not any longer!)
			 * be on the work queue; if that assert triggers,
			 * we need to also grab the
			 * spin_lock_irq(&mdev->data.work.q_lock);
			 * and list_del_init here. */
			D_ASSERT(list_empty(&req->w.list));
			_req_mod(req, connection_lost_while_pending, 0);
		}
	}
	for (i = 0; i < APP_R_HSIZE; i++)
		if (!hlist_empty(mdev->app_reads_hash+i))
			drbd_WARN("ASSERT FAILED: app_reads_hash[%d].first: "
				"%p, should be NULL\n", i, mdev->app_reads_hash[i].first);

	memset(mdev->app_reads_hash, 0, APP_R_HSIZE*sizeof(void *));
	spin_unlock_irq(&mdev->req_lock);
}

STATIC void drbd_disconnect(struct drbd_conf *mdev)
{
	struct drbd_work prev_work_done;
	enum fencing_policy fp;
	union drbd_state_t os, ns;
	int rv = SS_UnknownError;
	unsigned int i;

	if (mdev->state.conn == StandAlone)
		return;
	/* FIXME verify that:
	 * the state change magic prevents us from becoming >= Connected again
	 * while we are still cleaning up.
	 */
	if (mdev->state.conn >= WFConnection)
		ERR("ASSERT FAILED cstate = %s, expected < WFConnection\n",
				conns_to_name(mdev->state.conn));

	/* asender does not clean up anything. it must not interfere, either */
	drbd_thread_stop(&mdev->asender);

	down(&mdev->data.mutex);
	drbd_free_sock(mdev);
	up(&mdev->data.mutex);

	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev, &mdev->active_ee);
	_drbd_wait_ee_list_empty(mdev, &mdev->sync_ee);
	_drbd_clear_done_ee(mdev);
	_drbd_wait_ee_list_empty(mdev, &mdev->read_ee);
	reclaim_net_ee(mdev);
	spin_unlock_irq(&mdev->req_lock);

	/* We do not have data structures that would allow us to
	 * get the rs_pending_cnt down to 0 again.
	 *  * On SyncTarget we do not have any data structures describing
	 *    the pending RSDataRequest's we have sent.
	 *  * On SyncSource there is no data structure that tracks
	 *    the RSDataReply blocks that we sent to the SyncTarget.
	 *  And no, it is not the sum of the reference counts in the
	 *  resync_LRU. The resync_LRU tracks the whole operation including
	 *  the disk-IO, while the rs_pending_cnt only tracks the blocks
	 *  on the fly. */
	drbd_rs_cancel_all(mdev);
	mdev->rs_total = 0;
	mdev->rs_failed = 0;
	atomic_set(&mdev->rs_pending_cnt, 0);
	wake_up(&mdev->misc_wait);

	/* make sure syncer is stopped and w_resume_next_sg queued */
	del_timer_sync(&mdev->resync_timer);
	set_bit(STOP_SYNC_TIMER, &mdev->flags);
	resync_timer_fn((unsigned long)mdev);

	/* wait for all w_e_end_data_req, w_e_end_rsdata_req, w_send_barrier,
	 * w_make_resync_request etc. which may still be on the worker queue
	 * to be "canceled" */
	set_bit(WORK_PENDING, &mdev->flags);
	prev_work_done.cb = w_prev_work_done;
	drbd_queue_work(&mdev->data.work, &prev_work_done);
	wait_event(mdev->misc_wait, !test_bit(WORK_PENDING, &mdev->flags));

	kfree(mdev->p_uuid);
	mdev->p_uuid = NULL;

	/* queue cleanup for the worker.
	 * FIXME this should go into after_state_ch  */
	if (!mdev->state.susp)
		tl_clear(mdev);

	/* FIXME: fail pending reads?
	 * when we are configured for freeze io,
	 * we could retry them once we un-freeze. */
	drbd_fail_pending_reads(mdev);

	INFO("Connection closed\n");

	drbd_md_sync(mdev);

	fp = DontCare;
	if (inc_local(mdev)) {
		fp = mdev->bc->dc.fencing;
		dec_local(mdev);
	}

	if (mdev->state.role == Primary) {
		if (fp >= Resource && mdev->state.pdsk >= DUnknown) {
			enum drbd_disk_state nps = drbd_try_outdate_peer(mdev);
			drbd_request_state(mdev, NS(pdsk, nps));
		}
	}

	spin_lock_irq(&mdev->req_lock);
	os = mdev->state;
	if (os.conn >= Unconnected) {
		/* Do not restart in case we are Disconnecting */
		ns = os;
		ns.conn = Unconnected;
		DRBD_STATE_DEBUG_INIT_VAL(ns);
		rv = _drbd_set_state(mdev, ns, ChgStateVerbose, NULL);
	}
	spin_unlock_irq(&mdev->req_lock);

	if (os.conn == Disconnecting) {
		struct hlist_head *h;
		wait_event(mdev->misc_wait, atomic_read(&mdev->net_cnt) == 0);

		/* we must not free the tl_hash
		 * while application io is still on the fly */
		wait_event(mdev->misc_wait, atomic_read(&mdev->ap_bio_cnt) == 0);

		spin_lock_irq(&mdev->req_lock);
		/* paranoia code */
		for (h = mdev->ee_hash; h < mdev->ee_hash + mdev->ee_hash_s; h++)
			if (h->first)
				ERR("ASSERT FAILED ee_hash[%u].first == %p, expected NULL\n",
						(int)(h - mdev->ee_hash), h->first);
		kfree(mdev->ee_hash);
		mdev->ee_hash = NULL;
		mdev->ee_hash_s = 0;

		/* paranoia code */
		for (h = mdev->tl_hash; h < mdev->tl_hash + mdev->tl_hash_s; h++)
			if (h->first)
				ERR("ASSERT FAILED tl_hash[%u] == %p, expected NULL\n",
						(int)(h - mdev->tl_hash), h->first);
		kfree(mdev->tl_hash);
		mdev->tl_hash = NULL;
		mdev->tl_hash_s = 0;
		spin_unlock_irq(&mdev->req_lock);

		crypto_free_hash(mdev->cram_hmac_tfm);
		mdev->cram_hmac_tfm = NULL;

		kfree(mdev->net_conf);
		mdev->net_conf = NULL;
		drbd_request_state(mdev, NS(conn, StandAlone));
	}

	/* they do trigger all the time.
	 * hm. why won't tcp release the page references,
	 * we already released the socket!? */
	i = atomic_read(&mdev->pp_in_use);
	if (i)
		DBG("pp_in_use = %u, expected 0\n", i);
	if (!list_empty(&mdev->net_ee))
		DBG("net_ee not empty!\n");

	D_ASSERT(list_empty(&mdev->read_ee));
	D_ASSERT(list_empty(&mdev->active_ee));
	D_ASSERT(list_empty(&mdev->sync_ee));
	D_ASSERT(list_empty(&mdev->done_ee));

	/* ok, no more ee's on the fly, it is safe to reset the epoch_size */
	atomic_set(&mdev->current_epoch->epoch_size, 0);
	D_ASSERT(list_empty(&mdev->current_epoch->list));
}

/*
 * We support PRO_VERSION_MIN to PRO_VERSION_MAX. The protocol version
 * we can agree on is stored in agreed_pro_version.
 *
 * feature flags and the reserved array should be enough room for future
 * enhancements of the handshake protocol, and possible plugins...
 *
 * for now, they are expected to be zero, but ignored.
 */
STATIC int drbd_send_handshake(struct drbd_conf *mdev)
{
	/* ASSERT current == mdev->receiver ... */
	struct Drbd_HandShake_Packet *p = &mdev->data.sbuf.HandShake;
	int ok;

	if (down_interruptible(&mdev->data.mutex)) {
		ERR("interrupted during initial handshake\n");
		return 0; /* interrupted. not ok. */
	}
	/* FIXME do we need to verify this here? */
	if (mdev->data.socket == NULL) {
		up(&mdev->data.mutex);
		return 0;
	}

	memset(p, 0, sizeof(*p));
	p->protocol_min = cpu_to_be32(PRO_VERSION_MIN);
	p->protocol_max = cpu_to_be32(PRO_VERSION_MAX);
	ok = _drbd_send_cmd( mdev, mdev->data.socket, HandShake,
			     (struct Drbd_Header *)p, sizeof(*p), 0 );
	up(&mdev->data.mutex);
	return ok;
}

/*
 * return values:
 *   1 yess, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 */
int drbd_do_handshake(struct drbd_conf *mdev)
{
	/* ASSERT current == mdev->receiver ... */
	struct Drbd_HandShake_Packet *p = &mdev->data.rbuf.HandShake;
	const int expect = sizeof(struct Drbd_HandShake_Packet)
			  -sizeof(struct Drbd_Header);
	int rv;

	rv = drbd_send_handshake(mdev);
	if (!rv)
		return 0;

	rv = drbd_recv_header(mdev, &p->head);
	if (!rv)
		return 0;

	if (p->head.command != HandShake) {
		ERR("expected HandShake packet, received: %s (0x%04x)\n",
		     cmdname(p->head.command), p->head.command);
		return -1;
	}

	if (p->head.length != expect) {
		ERR("expected HandShake length: %u, received: %u\n",
		     expect, p->head.length);
		return -1;
	}

	rv = drbd_recv(mdev, &p->head.payload, expect);

	if (rv != expect) {
		ERR("short read receiving handshake packet: l=%u\n", rv);
		return 0;
	}

	dump_packet(mdev, mdev->data.socket, 2, &mdev->data.rbuf,
			__FILE__, __LINE__);

	p->protocol_min = be32_to_cpu(p->protocol_min);
	p->protocol_max = be32_to_cpu(p->protocol_max);
	if (p->protocol_max == 0)
		p->protocol_max = p->protocol_min;

	if (PRO_VERSION_MAX < p->protocol_min ||
	    PRO_VERSION_MIN > p->protocol_max)
		goto incompat;

	mdev->agreed_pro_version = min_t(int,PRO_VERSION_MAX,p->protocol_max);

	INFO("Handshake successful: "
	     "Agreed network protocol version %d\n", mdev->agreed_pro_version);

	return 1;

 incompat:
	ERR("incompatible DRBD dialects: "
	    "I support %d-%d, peer supports %d-%d\n",
	    PRO_VERSION_MIN, PRO_VERSION_MAX,
	    p->protocol_min, p->protocol_max);
	return -1;
}

#if !defined(CONFIG_CRYPTO_HMAC) && !defined(CONFIG_CRYPTO_HMAC_MODULE)
int drbd_do_auth(struct drbd_conf *mdev)
{
	ERR("This kernel was build without CONFIG_CRYPTO_HMAC.\n");
	ERR("You need to disable 'cram-hmac-alg' in drbd.conf.\n");
	return 0;
}
#else
#define CHALLENGE_LEN 64
int drbd_do_auth(struct drbd_conf *mdev)
{
	char my_challenge[CHALLENGE_LEN];  /* 64 Bytes... */
	struct scatterlist sg;
	char *response = NULL;
	char *right_response = NULL;
	char *peers_ch = NULL;
	struct Drbd_Header p;
	unsigned int key_len = strlen(mdev->net_conf->shared_secret);
	unsigned int resp_size;
	struct hash_desc desc;
	int rv;

	desc.tfm = mdev->cram_hmac_tfm;
	desc.flags = 0;

	rv = crypto_hash_setkey(mdev->cram_hmac_tfm,
				(u8 *)mdev->net_conf->shared_secret, key_len);
	if (rv) {
		ERR("crypto_hash_setkey() failed with %d\n", rv);
		rv = 0;
		goto fail;
	}

	get_random_bytes(my_challenge, CHALLENGE_LEN);

	rv = drbd_send_cmd2(mdev, AuthChallenge, my_challenge, CHALLENGE_LEN);
	if (!rv)
		goto fail;

	rv = drbd_recv_header(mdev, &p);
	if (!rv)
		goto fail;

	if (p.command != AuthChallenge) {
		ERR("expected AuthChallenge packet, received: %s (0x%04x)\n",
		    cmdname(p.command), p.command);
		rv = 0;
		goto fail;
	}

	if (p.length > CHALLENGE_LEN*2) {
		ERR("expected AuthChallenge payload too big.\n");
		rv = 0;
		goto fail;
	}

	peers_ch = kmalloc(p.length, GFP_KERNEL);
	if (peers_ch == NULL) {
		ERR("kmalloc of peers_ch failed\n");
		rv = 0;
		goto fail;
	}

	rv = drbd_recv(mdev, peers_ch, p.length);

	if (rv != p.length) {
		ERR("short read AuthChallenge: l=%u\n", rv);
		rv = 0;
		goto fail;
	}

	resp_size = crypto_hash_digestsize(mdev->cram_hmac_tfm);
	response = kmalloc(resp_size, GFP_KERNEL);
	if (response == NULL) {
		ERR("kmalloc of response failed\n");
		rv = 0;
		goto fail;
	}

	sg_init_table(&sg, 1);
	sg_set_buf(&sg, peers_ch, p.length);

	rv = crypto_hash_digest(&desc, &sg, sg.length, response);
	if (rv) {
		ERR("crypto_hash_digest() failed with %d\n", rv);
		rv = 0;
		goto fail;
	}

	rv = drbd_send_cmd2(mdev, AuthResponse, response, resp_size);
	if (!rv)
		goto fail;

	rv = drbd_recv_header(mdev, &p);
	if (!rv)
		goto fail;

	if (p.command != AuthResponse) {
		ERR("expected AuthResponse packet, received: %s (0x%04x)\n",
		    cmdname(p.command), p.command);
		rv = 0;
		goto fail;
	}

	if (p.length != resp_size) {
		ERR("expected AuthResponse payload of wrong size\n");
		rv = 0;
		goto fail;
	}

	rv = drbd_recv(mdev, response , resp_size);

	if (rv != resp_size) {
		ERR("short read receiving AuthResponse: l=%u\n", rv);
		rv = 0;
		goto fail;
	}

	right_response = kmalloc(resp_size, GFP_KERNEL);
	if (response == NULL) {
		ERR("kmalloc of right_response failed\n");
		rv = 0;
		goto fail;
	}

	sg_set_buf(&sg, my_challenge, CHALLENGE_LEN);

	rv = crypto_hash_digest(&desc, &sg, sg.length, right_response);
	if (rv) {
		ERR("crypto_hash_digest() failed with %d\n", rv);
		rv = 0;
		goto fail;
	}

	rv = !memcmp(response, right_response, resp_size);

	if (rv)
		INFO("Peer authenticated using %d bytes of '%s' HMAC\n",
		     resp_size, mdev->net_conf->cram_hmac_alg);

 fail:
	kfree(peers_ch);
	kfree(response);
	kfree(right_response);

	return rv;
}
#endif

STATIC int drbdd_init(struct Drbd_thread *thi)
{
	struct drbd_conf *mdev = thi->mdev;
	int minor = mdev_to_minor(mdev);
	int h;

	sprintf(current->comm, "drbd%d_receiver", minor);

	INFO("receiver (re)started\n");

	do {
		h = drbd_connect(mdev);
		if (h == 0) {
			drbd_disconnect(mdev);
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
		}
		if (h == -1) {
			drbd_WARN("Discarding network configuration.\n");
			drbd_force_state(mdev, NS(conn, Disconnecting));
		}
	} while (h == 0);

	if (h > 0) {
		if (inc_net(mdev)) {
			drbdd(mdev);
			dec_net(mdev);
		}
	}

	drbd_disconnect(mdev);

	INFO("receiver terminated\n");
	return 0;
}

/* ********* acknowledge sender ******** */

STATIC int got_RqSReply(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_RqS_Reply_Packet *p = (struct Drbd_RqS_Reply_Packet *)h;

	int retcode = be32_to_cpu(p->retcode);

	if (retcode >= SS_Success) {
		set_bit(CL_ST_CHG_SUCCESS, &mdev->flags);
	} else {
		set_bit(CL_ST_CHG_FAIL, &mdev->flags);
		ERR("Requested state change failed by peer: %s (%d)\n",
		    set_st_err_name(retcode), retcode);
	}
	wake_up(&mdev->state_wait);

	return TRUE;
}

STATIC int got_Ping(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	return drbd_send_ping_ack(mdev);

}

STATIC int got_PingAck(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	/* restore idle timeout */
	mdev->meta.socket->sk->sk_rcvtimeo = mdev->net_conf->ping_int*HZ;

	return TRUE;
}

STATIC int got_IsInSync(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_BlockAck_Packet *p = (struct Drbd_BlockAck_Packet *)h;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);

	D_ASSERT(mdev->agreed_pro_version >= 89);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	drbd_rs_complete_io(mdev, sector);
	drbd_set_in_sync(mdev, sector, blksize);
	/* rs_same_csums is supposed to count in units of BM_BLOCK_SIZE */
	mdev->rs_same_csum += (blksize >> BM_BLOCK_SIZE_B);
	dec_rs_pending(mdev);

	return TRUE;
}

STATIC int got_BlockAck(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct drbd_request *req;
	struct Drbd_BlockAck_Packet *p = (struct Drbd_BlockAck_Packet *)h;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	if (is_syncer_block_id(p->block_id)) {
		drbd_set_in_sync(mdev, sector, blksize);
		dec_rs_pending(mdev);
	} else {
		spin_lock_irq(&mdev->req_lock);
		req = _ack_id_to_req(mdev, p->block_id, sector);

		if (unlikely(!req)) {
			spin_unlock_irq(&mdev->req_lock);
			ERR("Got a corrupt block_id/sector pair(2).\n");
			return FALSE;
		}

		switch (be16_to_cpu(h->command)) {
		case RSWriteAck:
			D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
			_req_mod(req, write_acked_by_peer_and_sis, 0);
			break;
		case WriteAck:
			D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
			_req_mod(req, write_acked_by_peer, 0);
			break;
		case RecvAck:
			D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_B);
			_req_mod(req, recv_acked_by_peer, 0);
			break;
		case DiscardAck:
			D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
			ALERT("Got DiscardAck packet %llus +%u!"
			      " DRBD is not a random data generator!\n",
			      (unsigned long long)req->sector, req->size);
			_req_mod(req, conflict_discarded_by_peer, 0);
			break;
		default:
			D_ASSERT(0);
		}
		spin_unlock_irq(&mdev->req_lock);
	}
	/* dec_ap_pending is handled within _req_mod */

	return TRUE;
}

STATIC int got_NegAck(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_BlockAck_Packet *p = (struct Drbd_BlockAck_Packet *)h;
	sector_t sector = be64_to_cpu(p->sector);
	struct drbd_request *req;

	if (DRBD_ratelimit(5*HZ, 5))
		drbd_WARN("Got NegAck packet. Peer is in troubles?\n");

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	if (is_syncer_block_id(p->block_id)) {
		int size = be32_to_cpu(p->blksize);

		dec_rs_pending(mdev);

		drbd_rs_failed_io(mdev, sector, size);
	} else {
		spin_lock_irq(&mdev->req_lock);
		req = _ack_id_to_req(mdev, p->block_id, sector);

		if (unlikely(!req)) {
			spin_unlock_irq(&mdev->req_lock);
			ERR("Got a corrupt block_id/sector pair(2).\n");
			return FALSE;
		}

		_req_mod(req, neg_acked, 0);
		spin_unlock_irq(&mdev->req_lock);
	}

	return TRUE;
}

STATIC int got_NegDReply(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct drbd_request *req;
	struct Drbd_BlockAck_Packet *p = (struct Drbd_BlockAck_Packet *)h;
	sector_t sector = be64_to_cpu(p->sector);

	spin_lock_irq(&mdev->req_lock);
	req = _ar_id_to_req(mdev, p->block_id, sector);
	if (unlikely(!req)) {
		spin_unlock_irq(&mdev->req_lock);
		ERR("Got a corrupt block_id/sector pair(3).\n");
		return FALSE;
	}

	_req_mod(req, neg_acked, 0);
	spin_unlock_irq(&mdev->req_lock);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	ERR("Got NegDReply; Sector %llus, len %u; Fail original request.\n",
	    (unsigned long long)sector, be32_to_cpu(p->blksize));

	return TRUE;
}

STATIC int got_NegRSDReply(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	sector_t sector;
	int size;
	struct Drbd_BlockAck_Packet *p = (struct Drbd_BlockAck_Packet *)h;

	sector = be64_to_cpu(p->sector);
	size = be32_to_cpu(p->blksize);
	D_ASSERT(p->block_id == ID_SYNCER);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	dec_rs_pending(mdev);

	if (inc_local_if_state(mdev, Failed)) {
		drbd_rs_complete_io(mdev, sector);
		drbd_rs_failed_io(mdev, sector, size);
		dec_local(mdev);
	}

	return TRUE;
}

STATIC int got_BarrierAck(struct drbd_conf *mdev, struct Drbd_Header *h)
{
	struct Drbd_BarrierAck_Packet *p = (struct Drbd_BarrierAck_Packet *)h;

	tl_release(mdev, p->barrier, be32_to_cpu(p->set_size));

	return TRUE;
}

STATIC int got_OVResult(struct drbd_conf *mdev, struct Drbd_Header* h)
{
	struct Drbd_BlockAck_Packet *p = (struct Drbd_BlockAck_Packet*)h;
	struct drbd_work* w;
	sector_t sector;
	int size;

	sector = be64_to_cpu(p->sector);
	size = be32_to_cpu(p->blksize);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	if (be64_to_cpu(p->block_id) == ID_OUT_OF_SYNC) {
		drbd_ov_oos_found(mdev, sector, size);
	} else ov_oos_print(mdev);

	drbd_rs_complete_io(mdev, sector);
	dec_rs_pending(mdev);

	if (--mdev->ov_left == 0) {
		w = kmalloc(sizeof(*w), GFP_KERNEL);
		if (w) {
			w->cb = w_ov_finished;
			drbd_queue_work_front(&mdev->data.work, w);
		} else {
			ERR("kmalloc(w) failed.");
			drbd_resync_finished(mdev);
		}
	}
	return TRUE;
}

struct asender_cmd {
	size_t pkt_size;
	int (*process)(struct drbd_conf *mdev, struct Drbd_Header *h);
};

static struct asender_cmd *get_asender_cmd(int cmd)
{
	static struct asender_cmd asender_tbl[] = {
		/* anything missing from this table is in
		 * the drbd_cmd_handler (drbd_default_handler) table,
		 * see the beginning of drbdd() */
	[Ping]		= { sizeof(struct Drbd_Header), got_Ping },
	[PingAck]	= { sizeof(struct Drbd_Header),	got_PingAck },
	[RecvAck]	= { sizeof(struct Drbd_BlockAck_Packet), got_BlockAck },
	[WriteAck]	= { sizeof(struct Drbd_BlockAck_Packet), got_BlockAck },
	[RSWriteAck]	= { sizeof(struct Drbd_BlockAck_Packet), got_BlockAck },
	[DiscardAck]	= { sizeof(struct Drbd_BlockAck_Packet), got_BlockAck },
	[NegAck]	= { sizeof(struct Drbd_BlockAck_Packet), got_NegAck },
	[NegDReply]	= { sizeof(struct Drbd_BlockAck_Packet), got_NegDReply },
	[NegRSDReply]	= { sizeof(struct Drbd_BlockAck_Packet), got_NegRSDReply},
	[OVResult]      = { sizeof(struct Drbd_BlockAck_Packet),  got_OVResult },
	[BarrierAck]	= { sizeof(struct Drbd_BarrierAck_Packet), got_BarrierAck },
	[StateChgReply] = { sizeof(struct Drbd_RqS_Reply_Packet), got_RqSReply },
	[RSIsInSync]	= { sizeof(struct Drbd_BlockAck_Packet), got_IsInSync },
	[MAX_CMD]	= { 0, NULL },
	};
	if (cmd > MAX_CMD)
		return NULL;
	return &asender_tbl[cmd];
}

STATIC int drbd_asender(struct Drbd_thread *thi)
{
	struct drbd_conf *mdev = thi->mdev;
	struct Drbd_Header *h = &mdev->meta.rbuf.head;
	struct asender_cmd *cmd = NULL;

	int rv, len;
	void *buf    = h;
	int received = 0;
	int expect   = sizeof(struct Drbd_Header);
	int empty;

	sprintf(current->comm, "drbd%d_asender", mdev_to_minor(mdev));

	current->policy = SCHED_RR;  /* Make this a realtime task! */
	current->rt_priority = 2;    /* more important than all other tasks */

	while (get_t_state(thi) == Running) {
		drbd_thread_current_set_cpu(mdev);
		if (test_and_clear_bit(SEND_PING, &mdev->flags)) {
			ERR_IF(!drbd_send_ping(mdev)) goto reconnect;
			mdev->meta.socket->sk->sk_rcvtimeo =
				mdev->net_conf->ping_timeo*HZ/10;
		}

		/* conditionally cork;
		 * it may hurt latency if we cork without much to send */
		if (!mdev->net_conf->no_cork &&
			3 < atomic_read(&mdev->unacked_cnt))
			drbd_tcp_cork(mdev->meta.socket);
		while (1) {
			clear_bit(SIGNAL_ASENDER, &mdev->flags);
			flush_signals(current);
			if (!drbd_process_done_ee(mdev)) {
				ERR("process_done_ee() = NOT_OK\n");
				goto reconnect;
			}
			/* to avoid race with newly queued ACKs */
			set_bit(SIGNAL_ASENDER, &mdev->flags);
			spin_lock_irq(&mdev->req_lock);
			empty = list_empty(&mdev->done_ee);
			spin_unlock_irq(&mdev->req_lock);
			/* new ack may have been queued right here,
			 * but then there is also a signal pending,
			 * and we start over... */
			if (empty)
				break;
		}
		/* but unconditionally uncork unless disabled */
		if (!mdev->net_conf->no_cork)
			drbd_tcp_uncork(mdev->meta.socket);

		/* short circuit, recv_msg would return EINTR anyways. */
		if (signal_pending(current))
			continue;

		rv = drbd_recv_short(mdev, mdev->meta.socket,
				     buf, expect-received, 0);
		clear_bit(SIGNAL_ASENDER, &mdev->flags);

		flush_signals(current);

		/* Note:
		 * -EINTR	 (on meta) we got a signal
		 * -EAGAIN	 (on meta) rcvtimeo expired
		 * -ECONNRESET	 other side closed the connection
		 * -ERESTARTSYS  (on data) we got a signal
		 * rv <  0	 other than above: unexpected error!
		 * rv == expected: full header or command
		 * rv <  expected: "woken" by signal during receive
		 * rv == 0	 : "connection shut down by peer"
		 */
		if (likely(rv > 0)) {
			received += rv;
			buf	 += rv;
		} else if (rv == 0) {
			ERR("meta connection shut down by peer.\n");
			goto reconnect;
		} else if (rv == -EAGAIN) {
			if (mdev->meta.socket->sk->sk_rcvtimeo ==
			    mdev->net_conf->ping_timeo*HZ/10) {
				ERR("PingAck did not arrive in time.\n");
				goto reconnect;
			}
			set_bit(SEND_PING, &mdev->flags);
			continue;
		} else if (rv == -EINTR) {
			continue;
		} else {
			ERR("sock_recvmsg returned %d\n", rv);
			goto reconnect;
		}

		if (received == expect && cmd == NULL) {
			if (unlikely(h->magic != BE_DRBD_MAGIC)) {
				ERR("magic?? on meta m: 0x%lx c: %d l: %d\n",
				    (long)be32_to_cpu(h->magic),
				    h->command, h->length);
				goto reconnect;
			}
			cmd = get_asender_cmd(be16_to_cpu(h->command));
			len = be16_to_cpu(h->length);
			if (unlikely(cmd == NULL)) {
				ERR("unknown command?? on meta m: 0x%lx c: %d l: %d\n",
				    (long)be32_to_cpu(h->magic),
				    h->command, h->length);
				goto disconnect;
			}
			expect = cmd->pkt_size;
			ERR_IF(len != expect-sizeof(struct Drbd_Header)) {
				dump_packet(mdev, mdev->meta.socket, 1, (void *)h, __FILE__, __LINE__);
				DUMPI(expect);
				goto reconnect;
			}
		}
		if (received == expect) {
			D_ASSERT(cmd != NULL);
			dump_packet(mdev, mdev->meta.socket, 1, (void *)h, __FILE__, __LINE__);
			if (!cmd->process(mdev, h))
				goto reconnect;

			buf	 = h;
			received = 0;
			expect	 = sizeof(struct Drbd_Header);
			cmd	 = NULL;
		}
	}

	if (0) {
reconnect:
		drbd_force_state(mdev, NS(conn, NetworkFailure));
	}
	if (0) {
disconnect:
		drbd_force_state(mdev, NS(conn, Disconnecting));
	}
	clear_bit(SIGNAL_ASENDER, &mdev->flags);

	D_ASSERT(mdev->state.conn < Connected);
	INFO("asender terminated\n");

	return 0;
}
