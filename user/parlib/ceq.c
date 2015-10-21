/* Copyright (c) 2015 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Coalescing Event Queue: encapuslates the essence of epoll/kqueue in shared
 * memory: a dense array of sticky status bits.
 *
 * User side (consumer).
 *
 * When initializing, the nr_events is the maximum count of events you are
 * tracking, e.g. 100 FDs being tapped, but not the actual FD numbers.
 *
 * The ring_sz is a rough guess of the number of concurrent events.  It's not a
 * big deal what you pick, but it must be a power of 2.  Otherwise the kernel
 * will probably scribble over your memory.  If you pick a value that is too
 * small, then the ring may overflow, triggering an O(n) scan of the events
 * array.  You could make it the nearest power of 2 >= nr_events, for reasonable
 * behavior at the expense of memory.  It'll be very rare for the ring to have
 * more entries than the array has events. */

#include <parlib/ceq.h>
#include <parlib/arch/atomic.h>
#include <parlib/vcore.h>
#include <parlib/assert.h>
#include <parlib/spinlock.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

void ceq_init(struct ceq *ceq, uint8_t op, size_t nr_events, size_t ring_sz)
{
	/* In case they already had an mbox initialized, cleanup whatever was there
	 * so we don't leak memory.  They better not have asked for events before
	 * doing this init call... */
	ceq_cleanup(ceq);
	ceq->events = malloc(sizeof(struct ceq_event) * nr_events);
	memset(ceq->events, 0, sizeof(struct ceq_event) * nr_events);
	ceq->nr_events = nr_events;
	assert(IS_PWR2(ring_sz));
	ceq->ring = malloc(sizeof(int32_t) * ring_sz);
	memset(ceq->ring, 0xff, sizeof(int32_t) * ring_sz);
	ceq->ring_sz = ring_sz;
	ceq->operation = op;
	ceq->ring_overflowed = FALSE;
	atomic_init(&ceq->prod_idx, 0);
	atomic_init(&ceq->cons_pub_idx, 0);
	atomic_init(&ceq->cons_pvt_idx, 0);
	static_assert(sizeof(struct spin_pdr_lock) <= sizeof(ceq->u_lock));
	spin_pdr_init((struct spin_pdr_lock*)&ceq->u_lock);
}

/* Helper, returns an index into the events array from the ceq ring.  -1 if the
 * ring was empty when we looked (could be filled right after we looked).  This
 * is the same algorithm used with BCQs, but with a magic value (-1) instead of
 * a bool to track whether or not the slot is ready for consumption. */
static int32_t get_ring_idx(struct ceq *ceq)
{
	long pvt_idx, prod_idx;
	int32_t ret;
	do {
		prod_idx = atomic_read(&ceq->prod_idx);
		pvt_idx = atomic_read(&ceq->cons_pvt_idx);
		if (__ring_empty(prod_idx, pvt_idx))
			return -1;
	} while (!atomic_cas(&ceq->cons_pvt_idx, pvt_idx, pvt_idx + 1));
	/* We claimed our slot, which is pvt_idx.  The new cons_pvt_idx is advanced
	 * by 1 for the next consumer.  Now we need to wait on the kernel to fill
	 * the value: */
	while ((ret = ceq->ring[pvt_idx & (ceq->ring_sz - 1)]) == -1)
		cpu_relax();
	/* Set the value back to -1 for the next time the slot is used */
	ceq->ring[pvt_idx & (ceq->ring_sz - 1)] = -1;
	/* We now have our entry.  We need to make sure the pub_idx is updated.  All
	 * consumers are doing this.  We can just wait on all of them to update the
	 * cons_pub to our location, then we update it to the next.
	 *
	 * We're waiting on other vcores, but we don't know which one(s). */
	while (atomic_read(&ceq->cons_pub_idx) != pvt_idx)
		cpu_relax_vc(vcore_id());	/* wait on all of them */
	/* This is the only time we update cons_pub.  We also know no one else is
	 * updating it at this moment; the while loop acts as a lock, such that
	 * no one gets to this point until pub == their pvt_idx, all of which are
	 * unique. */
	/* No rwmb needed, it's the same variable (con_pub) */
	atomic_set(&ceq->cons_pub_idx, pvt_idx + 1);
	return ret;
}

/* Helper, extracts a message from a ceq[idx], returning TRUE if there was a
 * message.  Note that there might have been nothing in the message (coal == 0).
 * still, that counts; it's more about idx_posted.  A concurrent reader could
 * have swapped out the coal contents (imagine two consumers, each gets past the
 * idx_posted check).  If having an "empty" coal is a problem, then higher level
 * software can ask for another event.
 *
 * Implied in all of that is that idx_posted is also racy.  The consumer blindly
 * sets it to false.  So long as it extracts coal after doing so, we're fine. */
static bool extract_ceq_msg(struct ceq *ceq, int32_t idx, struct event_msg *msg)
{
	struct ceq_event *ceq_ev = &ceq->events[idx];
	if (!ceq_ev->idx_posted)
		return FALSE;
	/* Once we clear this flag, any new coalesces will trigger another ring
	 * event, so we don't need to worry about missing anything.  It is possible
	 * that this CEQ event will get those new coalesces as part of this message,
	 * and future messages will have nothing.  That's fine. */
	ceq_ev->idx_posted = FALSE;
	cmb();	/* order the read after the flag write.  swap provides cpu_mb */
	/* We extract the existing coals and reset the collection to 0; now the
	 * collected events are in our msg. */
	msg->ev_arg2 = atomic_swap(&ceq_ev->coalesce, 0);
	/* if the user wants access to user_data, they can peak in the event array
	 * via ceq->events[msg->ev_type].user_data. */
	msg->ev_type = idx;
	msg->ev_arg3 = (void*)ceq_ev->blob_data;
	ceq_ev->blob_data = 0;	/* racy, but there are no blob guarantees */
	return TRUE;
}

/* Consumer side, returns TRUE on success and fills *msg with the ev_msg.  If
 * the ceq appears empty, it will return FALSE.  Messages may have arrived after
 * we started getting that we do not receive. */
bool get_ceq_msg(struct ceq *ceq, struct event_msg *msg)
{
	int32_t idx = get_ring_idx(ceq);
	if (idx == -1) {
		if (!ceq->ring_overflowed)
			return FALSE;
		/* We didn't get anything via the ring, but if we're overflowed, then we
		 * need to look in the array directly.  Note that we only handle
		 * overflow when we failed to get something.  Eventually, we'll deal
		 * with overflow (which should be very rare).  Also note that while we
		 * are dealing with overflow, the kernel could be producing and using
		 * the ring, and we could have consumers consuming from the ring.
		 *
		 * Overall, we need to clear the overflow flag, make sure the list is
		 * empty, and turn the flag back on if it isn't.  That'll make sure
		 * overflow is set if there's a chance there is a message in the array
		 * that doesn't have an idx in the ring.
		 *
		 * However, if we do that, there's a time when overflow isn't set and
		 * the ring is empty.  A concurrent consumer could think that the ring
		 * is empty, when in fact it isn't.  That's bad, since we could miss a
		 * message (i.e. sleep when we have a message we needed).  So we'll need
		 * to deal with concurrent consumers, and whatever we do will also need
		 * to deal with concurrent conusmers who handle overflow too.  Easiest
		 * thing is to just lock.  If the lock is set, then that also means the
		 * mailbox isn't empty. */
		spin_pdr_lock((struct spin_pdr_lock*)&ceq->u_lock);
		/* Check again - someone may have handled it while we were waiting on
		 * the lock */
		if (!ceq->ring_overflowed) {
			spin_pdr_unlock((struct spin_pdr_lock*)&ceq->u_lock);
			return FALSE;
		}
		ceq->ring_overflowed = FALSE;
		wrmb(); /* clear overflowed before reading event entries */
		for (int i = 0; i < ceq->nr_events; i++) {
			if (extract_ceq_msg(ceq, i, msg)) {
				/* We found something.  There might be more, but a future
				 * consumer will have to deal with it, or verify there isn't. */
				ceq->ring_overflowed = TRUE;
				spin_pdr_unlock((struct spin_pdr_lock*)&ceq->u_lock);
				return TRUE;
			}
		}
		/* made it to the end, looks like there was no overflow left.  there
		 * could be new ones added behind us (they'd be in the ring or overflow
		 * would be turned on again), but those message were added after we
		 * started consuming, and therefore not our obligation to extract. */
		spin_pdr_unlock((struct spin_pdr_lock*)&ceq->u_lock);
		return FALSE;
	}
	if (!extract_ceq_msg(ceq, idx, msg))
		return FALSE;
	return TRUE;
}

/* pvt_idx is the next slot that a new consumer will try to consume.  when
 * pvt_idx != pub_idx, pub_idx is lagging, and it represents consumptions in
 * progress. */
static bool __ceq_ring_is_empty(struct ceq *ceq)
{
	return __ring_empty(atomic_read(&ceq->prod_idx),
	                    atomic_read(&ceq->cons_pvt_idx));
}

bool ceq_is_empty(struct ceq *ceq)
{
	if (!__ceq_ring_is_empty(ceq) ||
	    ceq->ring_overflowed ||
	    spin_pdr_locked((struct spin_pdr_lock*)&ceq->u_lock)) {
		return FALSE;
	}
	return TRUE;
}

void ceq_cleanup(struct ceq *ceq)
{
	free(ceq->events);
	free(ceq->ring);
}
