/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017
 *					All rights reserved
 *
 *  This file is part of GPAC / filters sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terfsess of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "filter_session.h"

typedef struct __lf_item
{
	struct __lf_item *next;
	void *data;
} GF_LFQItem;

typedef struct __gf_filter_queue
{
	//head element is dummy, never swaped
	GF_LFQItem *head;
	GF_LFQItem *tail;

	GF_LFQItem *res_head;
	GF_LFQItem *res_tail;

	volatile u32 nb_items;

	GF_Mutex *mx;
} GF_FilterQueue;




GF_FilterQueue *gf_fq_new(const GF_Mutex *mx)
{
	GF_FilterQueue *q;
	GF_SAFEALLOC(q, GF_FilterQueue);
	if (!q) return NULL;

	q->mx = (GF_Mutex *) mx;
	if (mx) return q;


	//lock-free mode, create dummuy slot for head
	GF_SAFEALLOC(q->head, GF_LFQItem);
	if (!q->head) {
		gf_free(q);
		return NULL;
	}
	q->tail = q->head;

	//lock-free mode, create dummuy slot for reservoir head
	GF_SAFEALLOC(q->res_head, GF_LFQItem);
	if (!q->res_head) {
		gf_free(q->head);
		gf_free(q);
		return NULL;
	}
	q->res_tail = q->res_head;
	return q;
}


void gf_fq_del(GF_FilterQueue *q, void (*item_delete)(void *) )
{
	GF_LFQItem *it = q->head;
	//first item is dummy if lock-free mode, doesn't hold a valid pointer
	if (! q->mx) it->data=NULL;

	while (it) {
		GF_LFQItem *ptr = it;
		it = it->next;
		if (ptr->data && item_delete) item_delete(ptr->data);
		gf_free(ptr);
	}

	it = q->res_head;
	while (it) {
		GF_LFQItem *ptr = it;
		it = it->next;
		gf_free(ptr);
	}
	gf_free(q);
}

static void gf_fq_lockfree_enqueue(GF_LFQItem *it, GF_LFQItem **tail_ptr)
{
	GF_LFQItem *next, *tail;

	while (1) {
		tail = *tail_ptr;
		next = tail->next;
		if (next == tail->next) {
			if (next==NULL) {
				if (__sync_bool_compare_and_swap(&tail->next, next, it)) {
					break; // Enqueue is done.  Exit loop
				}
			} else {
				//tail not pointing at last node, move it
				__sync_bool_compare_and_swap(tail_ptr, tail, next);
			}
		}
	}
	__sync_bool_compare_and_swap(tail_ptr, tail, it);
}

static void *gf_fq_lockfree_dequeue(GF_LFQItem **head_ptr, GF_LFQItem **tail_ptr, GF_LFQItem **prev_head)
{
	void *data=NULL;
	GF_LFQItem *tail, *next, *head;
	next = NULL;
	*prev_head = NULL;

	while (1 ) {
		head = *head_ptr;
		tail = *tail_ptr;
		next = head->next;
		//state seefsess OK
		if (head == *head_ptr) {
			//head is at tail, we have an empty queue or some state issue
			if (head == tail) {
				//empty queue (first slot is dummy, we need to check next)
				if (next == NULL)
					return NULL;

				//swap back tail at next
				__sync_bool_compare_and_swap(tail_ptr, tail, next);
			} else {
				data = next->data;
				//try to advance q->head to next
				if (__sync_bool_compare_and_swap(head_ptr, head, next))
					break; //success!
			}
		}
	}
	*prev_head = head;
	return data;
}

void gf_lfq_add(GF_FilterQueue *q, void *item)
{
	GF_LFQItem *it=NULL;

	gf_fq_lockfree_dequeue( &q->res_head, &q->res_tail, &it);
	if (!it) {
		GF_SAFEALLOC(it, GF_LFQItem);
		if (!it) return;
	} else {
		it->next = NULL;
	}
	it->data=item;
	gf_fq_lockfree_enqueue(it, &q->tail);
	safe_int_inc(&q->nb_items);
}

void *gf_lfq_pop(GF_FilterQueue *q)
{
	GF_LFQItem *slot=NULL;
	void *data = gf_fq_lockfree_dequeue( &q->head, &q->tail, &slot);
	if (!data) return NULL;
	assert(slot);

	slot->data = NULL;
	slot->next = NULL;
	gf_fq_lockfree_enqueue(slot, &q->res_tail);
	safe_int_dec(&q->nb_items);

	return data;
}

u32 gf_fq_count(GF_FilterQueue *q)
{
	return q->nb_items;
}

//TODO - check performances vs function pointer
void gf_fq_add(GF_FilterQueue *fq, void *item)
{
	GF_LFQItem *it;

#if 1
	u32 k, c = gf_fq_count(fq);
	for (k=0; k<c; k++) {
		void *a = gf_fq_get(fq, k);
		assert(a != item);
	}
#endif

	if (! fq->mx) {
		gf_lfq_add(fq, item);
	} else {
		gf_mx_p(fq->mx);

		it = fq->res_head;
		if (it) {
			fq->res_head = fq->res_head->next;
			it->next = NULL;
		} else {
			GF_SAFEALLOC(it, GF_LFQItem);
			if (!it) return;
		}
		if (! fq->res_head) fq->res_tail = NULL;

		it->data = item;
		if (!fq->tail) {
			fq->tail = fq->head = it;
		} else {
			fq->tail->next = it;
			fq->tail = it;
		}
		fq->nb_items++;
		gf_mx_v(fq->mx);
	}
}

void *gf_fq_pop(GF_FilterQueue *fq)
{
	GF_LFQItem *it;
	void *data=NULL;
	if (! fq->mx) {
		return gf_lfq_pop(fq);
	}

	gf_mx_p(fq->mx);
	it = fq->head;
	if (it) {
		fq->head = it->next;
		data = it->data;

		it->data = NULL;
		it->next = NULL;

		if (fq->res_tail) {
			fq->res_tail->next = it;
			fq->res_tail = it;
		} else {
			fq->res_head = fq->res_tail = it;
		}

		fq->nb_items--;

		if (! fq->head) fq->tail = NULL;

	}
	gf_mx_v(fq->mx);
	return data;
}


void *gf_fq_head(GF_FilterQueue *fq)
{
	void *data;
	if (fq->mx) {
		gf_mx_p(fq->mx);
		data = fq->head ? fq->head->data : NULL;
		gf_mx_v(fq->mx);
	} else {
		data = fq->head->next ? fq->head->next->data : NULL;
	}
	return data;
}


void *gf_fq_get(GF_FilterQueue *fq, u32 idx)
{
	void *data;
	GF_LFQItem *it;

	if (fq->mx) {
		gf_mx_p(fq->mx);
		it = fq->head;

		while (idx) {
			it = it->next;
			idx--;
			if (!it) break;
		}
		data = it ? it->data : NULL;
		gf_mx_v(fq->mx);
	} else {
		it = fq->head->next;
		while (idx) {
			it = it->next;
			idx--;
			if (!it) break;
		}
		data = it ? it->data : NULL;
	}

	return data;
}