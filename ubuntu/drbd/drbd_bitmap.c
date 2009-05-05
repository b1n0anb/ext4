/*
-*- linux-c -*-
   drbd_bitmap.c
   Kernel module for 2.6.x Kernels

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2004-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 2004-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2004-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

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

#include <linux/bitops.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/drbd.h>
#include "drbd_int.h"

/* OPAQUE outside this file!
 * interface defined in drbd_int.h

 * convetion:
 * function name drbd_bm_... => used elsewhere, "public".
 * function name      bm_... => internal to implementation, "private".

 * Note that since find_first_bit returns int, at the current granularity of
 * the bitmap (4KB per byte), this implementation "only" supports up to
 * 1<<(32+12) == 16 TB...
 *
 * we will eventually change the implementation to not allways hold the full
 * bitmap in memory, but only some 'lru_cache' of the on disk bitmap.

 * THINK
 * I'm not yet sure whether this file should be bits only,
 * or wether I want it to do all the sector<->bit calculation in here.
 */

/*
 * NOTE
 *  Access to the *bm_pages is protected by bm_lock.
 *  It is safe to read the other members within the lock.
 *
 *  drbd_bm_set_bits is called from bio_endio callbacks,
 *  We may be called with irq already disabled,
 *  so we need spin_lock_irqsave().
 *  And we need the kmap_atomic.
 * FIXME
 *  for performance reasons, when we _know_ we have irq disabled, we should
 *  probably introduce some _in_irq variants, so we know to only spin_lock().
 *
 * FIXME
 *  Actually you need to serialize all resize operations.
 *  but then, resize is a drbd state change, and it should be serialized
 *  already. Unfortunately it is not (yet), so two concurrent resizes, like
 *  attach storage (drbdsetup) and receive the peers size (drbd receiver)
 *  may eventually blow things up.
 * Therefore,
 *  you may only change the other members when holding
 *  the bm_change mutex _and_ the bm_lock.
 *  thus reading them holding either is safe.
 *  this is sort of overkill, but I rather do it right
 *  than have two resize operations interfere somewhen.
 */
struct drbd_bitmap {
	struct page **bm_pages;
	spinlock_t bm_lock;
	/* WARNING unsigned long bm_fo and friends:
	 * 32bit number of bit offset is just enough for 512 MB bitmap.
	 * it will blow up if we make the bitmap bigger...
	 * not that it makes much sense to have a bitmap that large,
	 * rather change the granularity to 16k or 64k or something.
	 * (that implies other problems, however...)
	 */
	unsigned long bm_fo;        /* next offset for drbd_bm_find_next */
	unsigned long bm_set;       /* nr of set bits; THINK maybe atomic_t? */
	unsigned long bm_bits;
	size_t   bm_words;
	size_t   bm_number_of_pages;
	sector_t bm_dev_capacity;
	struct semaphore bm_change; /* serializes resize operations */

	atomic_t bm_async_io;
	wait_queue_head_t bm_io_wait;

	unsigned long  bm_flags;

	/* debugging aid, in case we are still racy somewhere */
	char          *bm_why;
	struct task_struct *bm_task;
};

/* definition of bits in bm_flags */
#define BM_LOCKED 0
#define BM_MD_IO_ERROR (BITS_PER_LONG-1) /* 31? 63? */

static inline int bm_is_locked(struct drbd_bitmap *b)
{
	return test_bit(BM_LOCKED, &b->bm_flags);
}

#define bm_print_lock_info(m) __bm_print_lock_info(m, __func__)
static void __bm_print_lock_info(struct drbd_conf *mdev, const char *func)
{
	struct drbd_bitmap *b = mdev->bitmap;
	if (!DRBD_ratelimit(5*HZ, 5))
		return;
	ERR("FIXME %s in %s, bitmap locked for '%s' by %s\n",
	    current == mdev->receiver.task ? "receiver" :
	    current == mdev->asender.task  ? "asender"  :
	    current == mdev->worker.task   ? "worker"   : current->comm,
	    func, b->bm_why ?: "?",
	    b->bm_task == mdev->receiver.task ? "receiver" :
	    b->bm_task == mdev->asender.task  ? "asender"  :
	    b->bm_task == mdev->worker.task   ? "worker"   : "?");
}

void drbd_bm_lock(struct drbd_conf *mdev, char *why)
{
	struct drbd_bitmap *b = mdev->bitmap;
	int trylock_failed;

	if (!b) {
		ERR("FIXME no bitmap in drbd_bm_lock!?\n");
		return;
	}

	trylock_failed = down_trylock(&b->bm_change);

	if (trylock_failed) {
		DBG("%s going to '%s' but bitmap already locked for '%s' by %s\n",
		    current == mdev->receiver.task ? "receiver" :
		    current == mdev->asender.task  ? "asender"  :
		    current == mdev->worker.task   ? "worker"   : "?",
		    why, b->bm_why ?: "?",
		    b->bm_task == mdev->receiver.task ? "receiver" :
		    b->bm_task == mdev->asender.task  ? "asender"  :
		    b->bm_task == mdev->worker.task   ? "worker"   : "?");
		down(&b->bm_change);
	}
	if (__test_and_set_bit(BM_LOCKED, &b->bm_flags))
		ERR("FIXME bitmap already locked in bm_lock\n");

	b->bm_why  = why;
	b->bm_task = current;
}

void drbd_bm_unlock(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	if (!b) {
		ERR("FIXME no bitmap in drbd_bm_unlock!?\n");
		return;
	}

	if (!__test_and_clear_bit(BM_LOCKED, &mdev->bitmap->bm_flags))
		ERR("FIXME bitmap not locked in bm_unlock\n");

	b->bm_why  = NULL;
	b->bm_task = NULL;
	up(&b->bm_change);
}

#define bm_end_info(ignored...)	((void)(0))

#if 0
#define catch_oob_access_start() do {	\
	do {				\
		if ((bm-p_addr) >= PAGE_SIZE/sizeof(long)) { \
			printk(KERN_ALERT "drbd_bitmap.c:%u %s: p_addr:%p bm:%p %d\n", \
					__LINE__ , __func__ , p_addr, bm, (bm-p_addr)); \
			break;		\
		}
#define catch_oob_access_end()	\
	} while (0); } while (0)
#else
#define catch_oob_access_start() do {
#define catch_oob_access_end() } while (0)
#endif

/* word offset to long pointer */
STATIC unsigned long * bm_map_paddr(struct drbd_bitmap *b, unsigned long offset)
{
	struct page *page;
	unsigned long page_nr;

	/* page_nr = (word*sizeof(long)) >> PAGE_SHIFT; */
	page_nr = offset >> (PAGE_SHIFT - LN2_BPL + 3);
	BUG_ON(page_nr >= b->bm_number_of_pages);
	page = b->bm_pages[page_nr];

	return (unsigned long *) kmap_atomic(page, KM_IRQ1);
}

STATIC void bm_unmap(unsigned long *p_addr)
{
	kunmap_atomic(p_addr, KM_IRQ1);
};

/* long word offset of _bitmap_ sector */
#define S2W(s)	((s)<<(BM_EXT_SIZE_B-BM_BLOCK_SIZE_B-LN2_BPL))
/* word offset from start of bitmap to word number _in_page_
 * modulo longs per page
#define MLPP(X) ((X) % (PAGE_SIZE/sizeof(long))
 hm, well, Philipp thinks gcc might not optimze the % into & (... - 1)
 so do it explicitly:
 */
#define MLPP(X) ((X) & ((PAGE_SIZE/sizeof(long))-1))

/* Long words per page */
#define LWPP (PAGE_SIZE/sizeof(long))

/*
 * actually most functions herein should take a struct drbd_bitmap*, not a
 * struct drbd_conf*, but for the debug macros I like to have the mdev around
 * to be able to report device specific.
 */

/* FIXME TODO sometimes I use "int offset" as index into the bitmap.
 * since we currently are LIMITED to (128<<11)-64-8 sectors of bitmap,
 * this is ok [as long as we dont run on a 24 bit arch :)].
 * But it is NOT strictly ok.
 */

STATIC void bm_free_pages(struct page **pages, unsigned long number)
{
	unsigned long i;
	if (!pages)
		return;

	for (i = 0; i < number; i++) {
		if (!pages[i]) {
			printk(KERN_ALERT "drbd: bm_free_pages tried to free "
					  "a NULL pointer; i=%lu n=%lu\n",
					  i, number);
			continue;
		}
		__free_page(pages[i]);
		pages[i] = NULL;
	}
}

/*
 * "have" and "want" are NUMBER OF PAGES.
 */
STATIC struct page **bm_realloc_pages(struct page **old_pages,
				       unsigned long have,
				       unsigned long want)
{
	struct page** new_pages, *page;
	unsigned int i, bytes;

	BUG_ON(have == 0 && old_pages != NULL);
	BUG_ON(have != 0 && old_pages == NULL);

	if (have == want)
		return old_pages;

	/* To use kmalloc here is ok, as long as we support 4TB at max...
	 * otherwise this might become bigger than 128KB, which is
	 * the maximum for kmalloc.
	 *
	 * no, it is not: on 64bit boxes, sizeof(void*) == 8,
	 * 128MB bitmap @ 4K pages -> 256K of page pointers.
	 * ==> use vmalloc for now again.
	 * then again, we could do something like
	 *   if (nr_pages > watermark) vmalloc else kmalloc :*> ...
	 * or do cascading page arrays:
	 *   one page for the page array of the page array,
	 *   those pages for the real bitmap pages.
	 *   there we could even add some optimization members,
	 *   so we won't need to kmap_atomic in bm_find_next_bit just to see
	 *   that the page has no bits set ...
	 * or we can try a "huge" page ;-)
	 */
	bytes = sizeof(struct page*)*want;
	new_pages = vmalloc(bytes);
	if (!new_pages)
		return NULL;

	memset(new_pages, 0, bytes);
	if (want >= have) {
		for (i = 0; i < have; i++)
			new_pages[i] = old_pages[i];
		for (; i < want; i++) {
			if (!(page = alloc_page(GFP_HIGHUSER))) {
				bm_free_pages(new_pages + have, i - have);
				vfree(new_pages);
				return NULL;
			}
			new_pages[i] = page;
		}
	} else {
		for (i = 0; i < want; i++)
			new_pages[i] = old_pages[i];
		/* NOT HERE, we are outside the spinlock!
		bm_free_pages(old_pages + want, have - want);
		*/
	}

	return new_pages;
}

/*
 * called on driver init only. TODO call when a device is created.
 * allocates the drbd_bitmap, and stores it in mdev->bitmap.
 */
int drbd_bm_init(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	WARN_ON(b != NULL);
	b = kzalloc(sizeof(struct drbd_bitmap), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	spin_lock_init(&b->bm_lock);
	init_MUTEX(&b->bm_change);
	init_waitqueue_head(&b->bm_io_wait);

	mdev->bitmap = b;

	return 0;
}

sector_t drbd_bm_capacity(struct drbd_conf *mdev)
{
	ERR_IF(!mdev->bitmap) return 0;
	return mdev->bitmap->bm_dev_capacity;
}

/* called on driver unload. TODO: call when a device is destroyed.
 */
void drbd_bm_cleanup(struct drbd_conf *mdev)
{
	ERR_IF (!mdev->bitmap) return;
	/* FIXME I think we should explicitly change the device size to zero
	 * before this...
	 *
	WARN_ON(mdev->bitmap->bm);
	 */
	bm_free_pages(mdev->bitmap->bm_pages, mdev->bitmap->bm_number_of_pages);
	vfree(mdev->bitmap->bm_pages);
	kfree(mdev->bitmap);
	mdev->bitmap = NULL;
}

/*
 * since (b->bm_bits % BITS_PER_LONG) != 0,
 * this masks out the remaining bits.
 * Rerturns the number of bits cleared.
 */
STATIC int bm_clear_surplus(struct drbd_bitmap *b)
{
	const unsigned long mask = (1UL << (b->bm_bits & (BITS_PER_LONG-1))) - 1;
	size_t w = b->bm_bits >> LN2_BPL;
	int cleared = 0;
	unsigned long *p_addr, *bm;

	p_addr = bm_map_paddr(b, w);
	bm = p_addr + MLPP(w);
	if (w < b->bm_words) {
		catch_oob_access_start();
		cleared = hweight_long(*bm & ~mask);
		*bm &= mask;
		catch_oob_access_end();
		w++; bm++;
	}

	if (w < b->bm_words) {
		catch_oob_access_start();
		cleared += hweight_long(*bm);
		*bm = 0;
		catch_oob_access_end();
	}
	bm_unmap(p_addr);
	return cleared;
}

STATIC void bm_set_surplus(struct drbd_bitmap *b)
{
	const unsigned long mask = (1UL << (b->bm_bits & (BITS_PER_LONG-1))) - 1;
	size_t w = b->bm_bits >> LN2_BPL;
	unsigned long *p_addr, *bm;

	p_addr = bm_map_paddr(b, w);
	bm = p_addr + MLPP(w);
	if (w < b->bm_words) {
		catch_oob_access_start();
		*bm |= ~mask;
		bm++; w++;
		catch_oob_access_end();
	}

	if (w < b->bm_words) {
		catch_oob_access_start();
		*bm = ~(0UL);
		catch_oob_access_end();
	}
	bm_unmap(p_addr);
}

STATIC unsigned long __bm_count_bits(struct drbd_bitmap *b, const int swap_endian)
{
	unsigned long *p_addr, *bm, offset = 0;
	unsigned long bits = 0;
	unsigned long i, do_now;

	while (offset < b->bm_words) {
		i = do_now = min_t(size_t, b->bm_words-offset, LWPP);
		p_addr = bm_map_paddr(b, offset);
		bm = p_addr + MLPP(offset);
		while (i--) {
			catch_oob_access_start();
#ifndef __LITTLE_ENDIAN
			if (swap_endian)
				*bm = lel_to_cpu(*bm);
#endif
			bits += hweight_long(*bm++);
			catch_oob_access_end();
		}
		bm_unmap(p_addr);
		offset += do_now;
	}

	return bits;
}

static inline unsigned long bm_count_bits(struct drbd_bitmap *b)
{
	return __bm_count_bits(b, 0);
}

static inline unsigned long bm_count_bits_swap_endian(struct drbd_bitmap *b)
{
	return __bm_count_bits(b, 1);
}

void _drbd_bm_recount_bits(struct drbd_conf *mdev, char *file, int line)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long flags, bits;

	ERR_IF(!b) return;

	/* IMO this should be inside drbd_bm_lock/unlock.
	 * Unfortunately it is used outside of the locks.
	 * And I'm not yet sure where we need to place the
	 * lock/unlock correctly.
	 */

	spin_lock_irqsave(&b->bm_lock, flags);
	bits = bm_count_bits(b);
	if (bits != b->bm_set) {
		ERR("bm_set was %lu, corrected to %lu. %s:%d\n",
		    b->bm_set, bits, file, line);
		b->bm_set = bits;
	}
	spin_unlock_irqrestore(&b->bm_lock, flags);
}

/* offset and len in long words.*/
STATIC void bm_memset(struct drbd_bitmap * b, size_t offset, int c, size_t len)
{
	unsigned long *p_addr, *bm;
	size_t do_now, end;

#define BM_SECTORS_PER_BIT (BM_BLOCK_SIZE/512)

	end = offset + len;

	if (end > b->bm_words) {
		printk(KERN_ALERT "drbd: bm_memset end > bm_words\n");
		return;
	}

	while (offset < end) {
		do_now = min_t(size_t, ALIGN(offset + 1, LWPP), end) - offset;
		p_addr = bm_map_paddr(b, offset);
		bm = p_addr + MLPP(offset);
		catch_oob_access_start();
		if (bm+do_now > p_addr + LWPP) {
			printk(KERN_ALERT "drbd: BUG BUG BUG! p_addr:%p bm:%p do_now:%d\n",
			       p_addr, bm, (int)do_now);
			break; /* breaks to after catch_oob_access_end() only! */
		}
		memset(bm, c, do_now * sizeof(long));
		catch_oob_access_end();
		bm_unmap(p_addr);
		offset += do_now;
	}
}

/*
 * make sure the bitmap has enough room for the attached storage,
 * if neccessary, resize.
 * called whenever we may have changed the device size.
 * returns -ENOMEM if we could not allocate enough memory, 0 on success.
 * In case this is actually a resize, we copy the old bitmap into the new one.
 * Otherwise, the bitmap is initiallized to all bits set.
 */
int drbd_bm_resize(struct drbd_conf *mdev, sector_t capacity)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long bits, words, owords, obits, *p_addr, *bm;
	unsigned long want, have, onpages; /* number of pages */
	struct page **npages, **opages = NULL;
	int err = 0, growing;

	ERR_IF(!b) return -ENOMEM;

	drbd_bm_lock(mdev, "resize");

	INFO("drbd_bm_resize called with capacity == %llu\n",
			(unsigned long long)capacity);

	if (capacity == b->bm_dev_capacity)
		goto out;

	if (capacity == 0) {
		spin_lock_irq(&b->bm_lock);
		opages = b->bm_pages;
		onpages = b->bm_number_of_pages;
		owords = b->bm_words;
		b->bm_pages = NULL;
		b->bm_number_of_pages =
		b->bm_fo    =
		b->bm_set   =
		b->bm_bits  =
		b->bm_words =
		b->bm_dev_capacity = 0;
		spin_unlock_irq(&b->bm_lock);
		bm_free_pages(opages, onpages);
		vfree(opages);
		goto out;
	}
	bits  = BM_SECT_TO_BIT(ALIGN(capacity, BM_SECT_PER_BIT));

	/* if we would use
	   words = ALIGN(bits,BITS_PER_LONG) >> LN2_BPL;
	   a 32bit host could present the wrong number of words
	   to a 64bit host.
	*/
	words = ALIGN(bits, 64) >> LN2_BPL;

	if (inc_local(mdev)) {
		D_ASSERT((u64)bits <= (((u64)mdev->bc->md.md_size_sect-MD_BM_OFFSET) << 12));
		dec_local(mdev);
	}

	/* one extra long to catch off by one errors */
	want = ALIGN((words+1)*sizeof(long), PAGE_SIZE) >> PAGE_SHIFT;
	have = b->bm_number_of_pages;
	if (want == have) {
		D_ASSERT(b->bm_pages != NULL);
		npages = b->bm_pages;
	} else
		npages = bm_realloc_pages(b->bm_pages, have, want);

	if (!npages) {
		err = -ENOMEM;
		goto out;
	}

	spin_lock_irq(&b->bm_lock);
	opages = b->bm_pages;
	owords = b->bm_words;
	obits  = b->bm_bits;

	growing = bits > obits;
	if (opages)
		bm_set_surplus(b);

	b->bm_pages = npages;
	b->bm_number_of_pages = want;
	b->bm_bits  = bits;
	b->bm_words = words;
	b->bm_dev_capacity = capacity;

	if (growing) {
		bm_memset(b, owords, 0xff, words-owords);
		b->bm_set += bits - obits;
	}

	if (want < have) {
		/* implicit: (opages != NULL) && (opages != npages) */
		bm_free_pages(opages + want, have - want);
	}

	p_addr = bm_map_paddr(b, words);
	bm = p_addr + MLPP(words);
	catch_oob_access_start();
	*bm = DRBD_MAGIC;
	catch_oob_access_end();
	bm_unmap(p_addr);

	(void)bm_clear_surplus(b);
	if (!growing)
		b->bm_set = bm_count_bits(b);

	bm_end_info(mdev, __FUNCTION__);
	spin_unlock_irq(&b->bm_lock);
	if (opages != npages)
		vfree(opages);
	INFO("resync bitmap: bits=%lu words=%lu\n", bits, words);

 out:
	drbd_bm_unlock(mdev);
	return err;
}

/* inherently racy:
 * if not protected by other means, return value may be out of date when
 * leaving this function...
 * we still need to lock it, since it is important that this returns
 * bm_set == 0 precisely.
 *
 * maybe bm_set should be atomic_t ?
 */
unsigned long drbd_bm_total_weight(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long s;
	unsigned long flags;

	ERR_IF(!b) return 0;
	ERR_IF(!b->bm_pages) return 0;

	spin_lock_irqsave(&b->bm_lock, flags);
	s = b->bm_set;
	spin_unlock_irqrestore(&b->bm_lock, flags);

	return s;
}

size_t drbd_bm_words(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	ERR_IF(!b) return 0;
	ERR_IF(!b->bm_pages) return 0;

	return b->bm_words;
}

unsigned long drbd_bm_bits(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	ERR_IF(!b) return 0;

	return b->bm_bits;
}

/* merge number words from buffer into the bitmap starting at offset.
 * buffer[i] is expected to be little endian unsigned long.
 * bitmap must be locked by drbd_bm_lock.
 * currently only used from receive_bitmap.
 */
void drbd_bm_merge_lel(struct drbd_conf *mdev, size_t offset, size_t number,
			unsigned long *buffer)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long *p_addr, *bm;
	unsigned long word, bits;
	size_t end, do_now;

	end = offset + number;

	ERR_IF(!b) return;
	ERR_IF(!b->bm_pages) return;
	if (number == 0)
		return;
	WARN_ON(offset >= b->bm_words);
	WARN_ON(end    >  b->bm_words);

	spin_lock_irq(&b->bm_lock);
	while (offset < end) {
		do_now = min_t(size_t, ALIGN(offset+1, LWPP), end) - offset;
		p_addr = bm_map_paddr(b, offset);
		bm = p_addr + MLPP(offset);
		offset += do_now;
		while (do_now--) {
			catch_oob_access_start();
			bits = hweight_long(*bm);
			word = *bm | lel_to_cpu(*buffer++);
			*bm++ = word;
			b->bm_set += hweight_long(word) - bits;
			catch_oob_access_end();
		}
		bm_unmap(p_addr);
	}
	/* with 32bit <-> 64bit cross-platform connect
	 * this is only correct for current usage,
	 * where we _know_ that we are 64 bit aligned,
	 * and know that this function is used in this way, too...
	 */
	if (end == b->bm_words) {
		b->bm_set -= bm_clear_surplus(b);
		bm_end_info(mdev, __func__);
	}
	spin_unlock_irq(&b->bm_lock);
}

/* copy number words from the bitmap starting at offset into the buffer.
 * buffer[i] will be little endian unsigned long.
 */
void drbd_bm_get_lel(struct drbd_conf *mdev, size_t offset, size_t number,
		     unsigned long *buffer)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long *p_addr, *bm;
	size_t end, do_now;

	end = offset + number;

	ERR_IF(!b) return;
	ERR_IF(!b->bm_pages) return;

	spin_lock_irq(&b->bm_lock);
	if ((offset >= b->bm_words) ||
	    (end    >  b->bm_words) ||
	    (number <= 0))
		ERR("offset=%lu number=%lu bm_words=%lu\n",
			(unsigned long)	offset,
			(unsigned long)	number,
			(unsigned long) b->bm_words);
	else {
		while (offset < end) {
			do_now = min_t(size_t, ALIGN(offset+1, LWPP), end) - offset;
			p_addr = bm_map_paddr(b, offset);
			bm = p_addr + MLPP(offset);
			offset += do_now;
			while (do_now--) {
				catch_oob_access_start();
				*buffer++ = cpu_to_lel(*bm++);
				catch_oob_access_end();
			}
			bm_unmap(p_addr);
		}
	}
	spin_unlock_irq(&b->bm_lock);
}

/* set all bits in the bitmap */
void drbd_bm_set_all(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	ERR_IF(!b) return;
	ERR_IF(!b->bm_pages) return;

	spin_lock_irq(&b->bm_lock);
	bm_memset(b, 0, 0xff, b->bm_words);
	(void)bm_clear_surplus(b);
	b->bm_set = b->bm_bits;
	spin_unlock_irq(&b->bm_lock);
}

/* clear all bits in the bitmap */
void drbd_bm_clear_all(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	ERR_IF(!b) return;
	ERR_IF(!b->bm_pages) return;

	spin_lock_irq(&b->bm_lock);
	bm_memset(b, 0, 0, b->bm_words);
	b->bm_set = 0;
	spin_unlock_irq(&b->bm_lock);
}

static BIO_ENDIO_TYPE bm_async_io_complete BIO_ENDIO_ARGS(struct bio *bio, int error)
{
	struct drbd_bitmap *b = bio->bi_private;
	int uptodate = bio_flagged(bio, BIO_UPTODATE);

	BIO_ENDIO_FN_START;

	/* strange behaviour of some lower level drivers...
	 * fail the request by clearing the uptodate flag,
	 * but do not return any error?!
	 * do we want to WARN() on this? */
	if (!error && !uptodate)
		error = -EIO;

	if (error) {
		/* doh. what now?
		 * for now, set all bits, and flag MD_IO_ERROR
		 */
		/* FIXME kmap_atomic memset etc. pp. */
		__set_bit(BM_MD_IO_ERROR, &b->bm_flags);
	}
	if (atomic_dec_and_test(&b->bm_async_io))
		wake_up(&b->bm_io_wait);

	bio_put(bio);

	BIO_ENDIO_FN_RETURN;
}

STATIC void bm_page_io_async(struct drbd_conf *mdev, struct drbd_bitmap *b, int page_nr, int rw) __must_hold(local)
{
	/* we are process context. we always get a bio */
	struct bio *bio = bio_alloc(GFP_KERNEL, 1);
	unsigned int len;
	sector_t on_disk_sector =
		mdev->bc->md.md_offset + mdev->bc->md.bm_offset;
	on_disk_sector += ((sector_t)page_nr) << (PAGE_SHIFT-9);

	/* this might happen with very small
	 * flexible external meta data device */
	len = min_t(unsigned int, PAGE_SIZE,
		(drbd_md_last_sector(mdev->bc) - on_disk_sector + 1)<<9);

	D_DUMPLU(on_disk_sector);
	D_DUMPI(len);

	bio->bi_bdev = mdev->bc->md_bdev;
	bio->bi_sector = on_disk_sector;
	bio_add_page(bio, b->bm_pages[page_nr], len, 0);
	bio->bi_private = b;
	bio->bi_end_io = bm_async_io_complete;

	if (FAULT_ACTIVE(mdev, (rw & WRITE) ? DRBD_FAULT_MD_WR : DRBD_FAULT_MD_RD)) {
		bio->bi_rw |= rw;
		bio_endio(bio, -EIO);
	} else {
		submit_bio(rw, bio);
	}
}

# if defined(__LITTLE_ENDIAN)
	/* nothing to do, on disk == in memory */
# define bm_cpu_to_lel(x) ((void)0)
# else
void bm_cpu_to_lel(struct drbd_bitmap *b)
{
	/* need to cpu_to_lel all the pages ...
	 * this may be optimized by using
	 * cpu_to_lel(-1) == -1 and cpu_to_lel(0) == 0;
	 * the following is still not optimal, but better than nothing */
	if (b->bm_set == 0) {
		/* no page at all; avoid swap if all is 0 */
		i = b->bm_number_of_pages;
	} else if (b->bm_set == b->bm_bits) {
		/* only the last page */
		i = b->bm_number_of_pages -1;
	} else {
		/* all pages */
		i = 0;
	}
	for (; i < b->bm_number_of_pages; i++) {
		unsigned long *bm;
		/* if you'd want to use kmap_atomic, you'd have to disable irq! */
		p_addr = kmap(b->bm_pages[i]);
		for (bm = p_addr; bm < p_addr + PAGE_SIZE/sizeof(long); bm++) {
			*bm = cpu_to_lel(*bm);
		}
		kunmap(p_addr);
	}
}
# endif
/* lel_to_cpu == cpu_to_lel */
# define bm_lel_to_cpu(x) bm_cpu_to_lel(x)

/*
 * bm_rw: read/write the whole bitmap from/to its on disk location.
 */
STATIC int bm_rw(struct drbd_conf *mdev, int rw) __must_hold(local)
{
	struct drbd_bitmap *b = mdev->bitmap;
	/* sector_t sector; */
	int bm_words, num_pages, i;
	unsigned long now;
	char ppb[10];
	int err = 0;

	WARN_ON(!bm_is_locked(b));

	/* no spinlock here, the drbd_bm_lock should be enough! */

	bm_words  = drbd_bm_words(mdev);
	num_pages = (bm_words*sizeof(long) + PAGE_SIZE-1) >> PAGE_SHIFT;

	/* on disk bitmap is little endian */
	if (rw == WRITE)
		bm_cpu_to_lel(b);

	now = jiffies;
	atomic_set(&b->bm_async_io, num_pages);
	__clear_bit(BM_MD_IO_ERROR, &b->bm_flags);

	/* let the layers below us try to merge these bios... */
	for (i = 0; i < num_pages; i++)
		bm_page_io_async(mdev, b, i, rw);

	drbd_blk_run_queue(bdev_get_queue(mdev->bc->md_bdev));
	wait_event(b->bm_io_wait, atomic_read(&b->bm_async_io) == 0);

	MTRACE(TraceTypeMDIO, TraceLvlSummary,
	       INFO("%s of bitmap took %lu jiffies\n",
		    rw == READ ? "reading" : "writing", jiffies - now);
	       );

	if (test_bit(BM_MD_IO_ERROR, &b->bm_flags)) {
		ALERT("we had at least one MD IO ERROR during bitmap IO\n");
		drbd_chk_io_error(mdev, 1, TRUE);
		drbd_io_error(mdev, TRUE);
		err = -EIO;
	}

	now = jiffies;
	if (rw == WRITE) {
		/* swap back endianness */
		bm_lel_to_cpu(b);
		/* flush bitmap to stable storage */
		drbd_md_flush(mdev);
	} else /* rw == READ */ {
		/* just read, if neccessary adjust endianness */
		b->bm_set = bm_count_bits_swap_endian(b);
		INFO("recounting of set bits took additional %lu jiffies\n",
		     jiffies - now);
	}
	now = b->bm_set;

	INFO("%s (%lu bits) marked out-of-sync by on disk bit-map.\n",
	     ppsize(ppb, now << (BM_BLOCK_SIZE_B-10)), now);

	return err;
}

/**
 * drbd_bm_read: Read the whole bitmap from its on disk location.
 *
 * currently only called from "drbd_nl_disk_conf"
 */
int drbd_bm_read(struct drbd_conf *mdev) __must_hold(local)
{
	return bm_rw(mdev, READ);
}

/**
 * drbd_bm_write: Write the whole bitmap to its on disk location.
 *
 * called at various occasions.
 */
int drbd_bm_write(struct drbd_conf *mdev) __must_hold(local)
{
	return bm_rw(mdev, WRITE);
}

/**
 * drbd_bm_write_sect: Writes a 512 byte piece of the bitmap to its
 * on disk location. On disk bitmap is little endian.
 *
 * @enr: The _sector_ offset from the start of the bitmap.
 *
 */
int drbd_bm_write_sect(struct drbd_conf *mdev, unsigned long enr) __must_hold(local)
{
	sector_t on_disk_sector = enr + mdev->bc->md.md_offset
				      + mdev->bc->md.bm_offset;
	int bm_words, num_words, offset;
	int err = 0;

	down(&mdev->md_io_mutex);
	bm_words  = drbd_bm_words(mdev);
	offset    = S2W(enr);	/* word offset into bitmap */
	num_words = min(S2W(1), bm_words - offset);
#if DUMP_MD >= 3
	INFO("write_sect: sector=%lu offset=%u num_words=%u\n",
			enr, offset, num_words);
#endif
	if (num_words < S2W(1))
		memset(page_address(mdev->md_io_page), 0, MD_HARDSECT);
	drbd_bm_get_lel(mdev, offset, num_words,
			page_address(mdev->md_io_page));
	if (!drbd_md_sync_page_io(mdev, mdev->bc, on_disk_sector, WRITE)) {
		int i;
		err = -EIO;
		ERR("IO ERROR writing bitmap sector %lu "
		    "(meta-disk sector %llus)\n",
		    enr, (unsigned long long)on_disk_sector);
		drbd_chk_io_error(mdev, 1, TRUE);
		drbd_io_error(mdev, TRUE);
		for (i = 0; i < AL_EXT_PER_BM_SECT; i++)
			drbd_bm_ALe_set_all(mdev, enr*AL_EXT_PER_BM_SECT+i);
	}
	mdev->bm_writ_cnt++;
	up(&mdev->md_io_mutex);
	return err;
}

void drbd_bm_reset_find(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;

	ERR_IF(!b) return;

	spin_lock_irq(&b->bm_lock);
	if (bm_is_locked(b))
		bm_print_lock_info(mdev);
	b->bm_fo = 0;
	spin_unlock_irq(&b->bm_lock);

}

/* NOTE
 * find_first_bit returns int, we return unsigned long.
 * should not make much difference anyways, but ...
 *
 * this returns a bit number, NOT a sector!
 */
#define BPP_MASK ((1UL << (PAGE_SHIFT+3)) - 1)
unsigned long drbd_bm_find_next(struct drbd_conf *mdev)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long i = -1UL;
	unsigned long *p_addr;
	unsigned long bit_offset; /* bit offset of the mapped page. */

	ERR_IF(!b) return i;
	ERR_IF(!b->bm_pages) return i;

	spin_lock_irq(&b->bm_lock);
	if (bm_is_locked(b))
		bm_print_lock_info(mdev);
	if (b->bm_fo > b->bm_bits) {
		ERR("bm_fo=%lu bm_bits=%lu\n", b->bm_fo, b->bm_bits);
	} else {
		while (b->bm_fo < b->bm_bits) {
			unsigned long offset;
			bit_offset = b->bm_fo & ~BPP_MASK; /* bit offset of the page */
			offset = bit_offset >> LN2_BPL;    /* word offset of the page */
			p_addr = bm_map_paddr(b, offset);
			i = find_next_bit(p_addr, PAGE_SIZE*8, b->bm_fo & BPP_MASK);
			bm_unmap(p_addr);
			if (i < PAGE_SIZE*8) {
				i = bit_offset + i;
				if (i >= b->bm_bits)
					break;
				b->bm_fo = i+1;
				goto found;
			}
			b->bm_fo = bit_offset + PAGE_SIZE*8;
		}
		i = -1UL;
		/* leave b->bm_fo unchanged. */
	}
 found:
	spin_unlock_irq(&b->bm_lock);
	return i;
}

void drbd_bm_set_find(struct drbd_conf *mdev, unsigned long i)
{
	struct drbd_bitmap *b = mdev->bitmap;

	spin_lock_irq(&b->bm_lock);

	b->bm_fo = min_t(unsigned long, i, b->bm_bits);

	spin_unlock_irq(&b->bm_lock);
}

int drbd_bm_rs_done(struct drbd_conf *mdev)
{
	D_ASSERT(mdev->bitmap);
	return mdev->bitmap->bm_fo >= mdev->bitmap->bm_bits;
}

/* returns number of bits actually changed.
 * for val != 0, we change 0 -> 1, return code positiv
 * for val == 0, we change 1 -> 0, return code negative
 * wants bitnr, not sector */
static int bm_change_bits_to(struct drbd_conf *mdev, const unsigned long s,
	const unsigned long e, int val)
{
	unsigned long flags;
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long *p_addr = NULL;
	unsigned long bitnr;
	unsigned long last_page_nr = -1UL;
	int c = 0;

	ERR_IF(!b) return 1;
	ERR_IF(!b->bm_pages) return 0;

	spin_lock_irqsave(&b->bm_lock, flags);
	if (bm_is_locked(b))
		bm_print_lock_info(mdev);
	for (bitnr = s; bitnr <= e; bitnr++) {
		ERR_IF (bitnr >= b->bm_bits) {
			ERR("bitnr=%lu bm_bits=%lu\n", bitnr, b->bm_bits);
		} else {
			unsigned long offset = bitnr>>LN2_BPL;
			unsigned long page_nr = offset >> (PAGE_SHIFT - LN2_BPL + 3);
			if (page_nr != last_page_nr) {
				if (p_addr)
					bm_unmap(p_addr);
				p_addr = bm_map_paddr(b, offset);
				last_page_nr = page_nr;
			}
			if (val)
				c += (0 == __test_and_set_bit(bitnr & BPP_MASK, p_addr));
			else
				c -= (0 != __test_and_clear_bit(bitnr & BPP_MASK, p_addr));
		}
	}
	if (p_addr)
		bm_unmap(p_addr);
	b->bm_set += c;
	spin_unlock_irqrestore(&b->bm_lock, flags);
	return c;
}

/* returns number of bits changed 0 -> 1 */
int drbd_bm_set_bits(struct drbd_conf *mdev, const unsigned long s, const unsigned long e)
{
	return bm_change_bits_to(mdev, s, e, 1);
}

/* returns number of bits changed 1 -> 0 */
int drbd_bm_clear_bits(struct drbd_conf *mdev, const unsigned long s, const unsigned long e)
{
	return -bm_change_bits_to(mdev, s, e, 0);
}

/* returns bit state
 * wants bitnr, NOT sector.
 * inherently racy... area needs to be locked by means of {al,rs}_lru
 *  1 ... bit set
 *  0 ... bit not set
 * -1 ... first out of bounds access, stop testing for bits!
 */
int drbd_bm_test_bit(struct drbd_conf *mdev, const unsigned long bitnr)
{
	unsigned long flags;
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long *p_addr;
	int i;

	ERR_IF(!b) return 0;
	ERR_IF(!b->bm_pages) return 0;

	spin_lock_irqsave(&b->bm_lock, flags);
	if (bm_is_locked(b))
		bm_print_lock_info(mdev);
	if (bitnr < b->bm_bits) {
		unsigned long offset = bitnr>>LN2_BPL;
		p_addr = bm_map_paddr(b, offset);
		i = test_bit(bitnr & BPP_MASK, p_addr) ? 1 : 0;
		bm_unmap(p_addr);
	} else if (bitnr == b->bm_bits) {
		i = -1;
	} else { /* (bitnr > b->bm_bits) */
		ERR("bitnr=%lu > bm_bits=%lu\n", bitnr, b->bm_bits);
		i = 0;
	}

	spin_unlock_irqrestore(&b->bm_lock, flags);
	return i;
}

/* returns number of bits set */
int drbd_bm_count_bits(struct drbd_conf *mdev, const unsigned long s, const unsigned long e)
{
	unsigned long flags;
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long *p_addr = NULL, page_nr = -1;
	unsigned long bitnr;
	int c = 0;
	size_t w;

	/* If this is called without a bitmap, that is a bug.  But just to be
	 * robust in case we screwed up elsewhere, in that case pretend there
	 * was one dirty bit in the requested area, so we won't try to do a
	 * local read there (no bitmap probably implies no disk) */
	ERR_IF(!b) return 1;
	ERR_IF(!b->bm_pages) return 1;

	spin_lock_irqsave(&b->bm_lock, flags);
	for (bitnr = s; bitnr <= e; bitnr++) {
		w = bitnr >> LN2_BPL;
		if (page_nr != w >> (PAGE_SHIFT - LN2_BPL + 3)) {
			page_nr = w >> (PAGE_SHIFT - LN2_BPL + 3);
			if (p_addr)
				bm_unmap(p_addr);
			p_addr = bm_map_paddr(b, w);
		}
		ERR_IF (bitnr >= b->bm_bits) {
			ERR("bitnr=%lu bm_bits=%lu\n", bitnr, b->bm_bits);
		} else {
			c += (0 != test_bit(bitnr - (page_nr << (PAGE_SHIFT+3)), p_addr));
		}
	}
	if (p_addr)
		bm_unmap(p_addr);
	spin_unlock_irqrestore(&b->bm_lock, flags);
	return c;
}


/* inherently racy...
 * return value may be already out-of-date when this function returns.
 * but the general usage is that this is only use during a cstate when bits are
 * only cleared, not set, and typically only care for the case when the return
 * value is zero, or we already "locked" this "bitmap extent" by other means.
 *
 * enr is bm-extent number, since we chose to name one sector (512 bytes)
 * worth of the bitmap a "bitmap extent".
 *
 * TODO
 * I think since we use it like a reference count, we should use the real
 * reference count of some bitmap extent element from some lru instead...
 *
 */
int drbd_bm_e_weight(struct drbd_conf *mdev, unsigned long enr)
{
	struct drbd_bitmap *b = mdev->bitmap;
	int count, s, e;
	unsigned long flags;
	unsigned long *p_addr, *bm;

	ERR_IF(!b) return 0;
	ERR_IF(!b->bm_pages) return 0;

	spin_lock_irqsave(&b->bm_lock, flags);
	if (bm_is_locked(b))
		bm_print_lock_info(mdev);

	s = S2W(enr);
	e = min((size_t)S2W(enr+1), b->bm_words);
	count = 0;
	if (s < b->bm_words) {
		int n = e-s;
		p_addr = bm_map_paddr(b, s);
		bm = p_addr + MLPP(s);
		while (n--) {
			catch_oob_access_start();
			count += hweight_long(*bm++);
			catch_oob_access_end();
		}
		bm_unmap(p_addr);
	} else {
		ERR("start offset (%d) too large in drbd_bm_e_weight\n", s);
	}
	spin_unlock_irqrestore(&b->bm_lock, flags);
#if DUMP_MD >= 3
	INFO("enr=%lu weight=%d e=%d s=%d\n", enr, count, e, s);
#endif
	return count;
}

/* set all bits covered by the AL-extent al_enr */
unsigned long drbd_bm_ALe_set_all(struct drbd_conf *mdev, unsigned long al_enr)
{
	struct drbd_bitmap *b = mdev->bitmap;
	unsigned long *p_addr, *bm;
	unsigned long weight;
	int count, s, e, i, do_now;
	ERR_IF(!b) return 0;
	ERR_IF(!b->bm_pages) return 0;

	spin_lock_irq(&b->bm_lock);
	if (bm_is_locked(b))
		bm_print_lock_info(mdev);
	weight = b->bm_set;

	s = al_enr * BM_WORDS_PER_AL_EXT;
	e = min_t(size_t, s + BM_WORDS_PER_AL_EXT, b->bm_words);
	/* assert that s and e are on the same page */
	D_ASSERT((e-1) >> (PAGE_SHIFT - LN2_BPL + 3)
	      ==  s    >> (PAGE_SHIFT - LN2_BPL + 3));
	count = 0;
	if (s < b->bm_words) {
		i = do_now = e-s;
		p_addr = bm_map_paddr(b, s);
		bm = p_addr + MLPP(s);
		while (i--) {
			catch_oob_access_start();
			count += hweight_long(*bm);
			*bm = -1UL;
			catch_oob_access_end();
			bm++;
		}
		bm_unmap(p_addr);
		b->bm_set += do_now*BITS_PER_LONG - count;
		if (e == b->bm_words)
			b->bm_set -= bm_clear_surplus(b);
	} else {
		ERR("start offset (%d) too large in drbd_bm_ALe_set_all\n", s);
	}
	weight = b->bm_set - weight;
	spin_unlock_irq(&b->bm_lock);
	return weight;
}
