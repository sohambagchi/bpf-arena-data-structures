/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef DS_CK_RING_SPSC_H
#define DS_CK_RING_SPSC_H

#pragma once

#ifndef __BPF__
#include <linux/types.h>
#endif

#include "ds_api.h"

/*
 * CK-faithful SPSC ring:
 * - Producer index: p_tail
 * - Consumer index: c_head
 * - Power-of-two capacity with mask arithmetic
 * - One slot left empty to distinguish full vs empty
 */
struct ds_ck_ring_spsc_head {
	__u32 capacity;
	__u32 mask;

	/* Owned by consumer, read by producer. */
	__u32 c_head;

	/* Owned by producer, read by consumer. */
	__u32 p_tail;

	struct ds_kv __arena *slots;
};

typedef struct ds_ck_ring_spsc_head __arena ds_ck_ring_spsc_head_t;

static inline int ds_ck_ring_spsc_is_power_of_two(__u32 value)
{
	return value >= 2 && (value & (value - 1)) == 0;
}

static inline int ds_ck_ring_spsc_init(struct ds_ck_ring_spsc_head __arena *head,
				       __u32 capacity)
{
	struct ds_kv __arena *slots;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);

	if (!ds_ck_ring_spsc_is_power_of_two(capacity))
		return DS_ERROR_INVALID;

	slots = (struct ds_kv __arena *)bpf_arena_alloc(capacity * sizeof(*slots));
	if (!slots)
		return DS_ERROR_NOMEM;

	cast_kern(slots);

	head->capacity = capacity;
	head->mask = capacity - 1;
	head->c_head = 0;
	head->p_tail = 0;

	cast_user(slots);
	head->slots = slots;

	return DS_SUCCESS;
}

static inline int ds_ck_ring_spsc_insert(struct ds_ck_ring_spsc_head __arena *head,
					 __u64 key, __u64 value)
{
	__u32 producer;
	__u32 consumer;
	__u32 next;
	struct ds_kv __arena *slot;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	if (!head->slots)
		return DS_ERROR_INVALID;

	/* CK enqueue order: consumer(acquire), producer(relaxed), next/full test. */
	consumer = smp_load_acquire(&head->c_head);
	producer = head->p_tail;
	next = (producer + 1) & head->mask;

	if (next == consumer)
		return DS_ERROR_FULL;

	slot = &head->slots[producer];
	cast_kern(slot);
	slot->key = key;
	slot->value = value;
	smp_store_release(&head->p_tail, next);

	return DS_SUCCESS;
}

static inline int ds_ck_ring_spsc_delete(struct ds_ck_ring_spsc_head __arena *head,
					 struct ds_kv *out)
{
	__u32 consumer;
	__u32 producer;
	__u32 next;
	struct ds_kv __arena *slot;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	if (!head->slots)
		return DS_ERROR_INVALID;

	/* CK dequeue order: consumer(relaxed), producer(acquire), empty test. */
	consumer = head->c_head;
	producer = smp_load_acquire(&head->p_tail);

	if (consumer == producer)
		return DS_ERROR_NOT_FOUND;

	barrier();
	slot = &head->slots[consumer];
	cast_kern(slot);
	if (out) {
		out->key = slot->key;
		out->value = slot->value;
	}

	next = (consumer + 1) & head->mask;
	smp_store_release(&head->c_head, next);

	return DS_SUCCESS;
}

static inline int ds_ck_ring_spsc_pop(struct ds_ck_ring_spsc_head __arena *head,
				      struct ds_kv *out)
{
	return ds_ck_ring_spsc_delete(head, out);
}

static inline int ds_ck_ring_spsc_search(struct ds_ck_ring_spsc_head __arena *head,
					 __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

static inline __u32 ds_ck_ring_spsc_size(struct ds_ck_ring_spsc_head __arena *head)
{
	__u32 consumer;
	__u32 producer;

	cast_kern(head);
	consumer = smp_load_acquire(&head->c_head);
	producer = smp_load_acquire(&head->p_tail);
	return (producer - consumer) & head->mask;
}

static inline bool ds_ck_ring_spsc_is_empty(struct ds_ck_ring_spsc_head __arena *head)
{
	__u32 consumer;
	__u32 producer;

	cast_kern(head);
	consumer = READ_ONCE(head->c_head);
	producer = READ_ONCE(head->p_tail);
	return consumer == producer;
}

static inline bool ds_ck_ring_spsc_is_full(struct ds_ck_ring_spsc_head __arena *head)
{
	__u32 producer;
	__u32 consumer;
	__u32 next;

	cast_kern(head);
	consumer = smp_load_acquire(&head->c_head);
	producer = READ_ONCE(head->p_tail);
	next = (producer + 1) & head->mask;
	return next == consumer;
}

static inline int ds_ck_ring_spsc_verify(struct ds_ck_ring_spsc_head __arena *head)
{
	__u32 capacity;
	__u32 mask;
	__u32 consumer;
	__u32 producer;
	__u32 size;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);

	if (!head->slots)
		return DS_ERROR_CORRUPT;

	capacity = head->capacity;
	mask = head->mask;

	if (!ds_ck_ring_spsc_is_power_of_two(capacity))
		return DS_ERROR_CORRUPT;
	if (mask != capacity - 1)
		return DS_ERROR_CORRUPT;

	consumer = READ_ONCE(head->c_head);
	producer = READ_ONCE(head->p_tail);

	if ((consumer & mask) != consumer)
		return DS_ERROR_CORRUPT;
	if ((producer & mask) != producer)
		return DS_ERROR_CORRUPT;

	size = (producer - consumer) & mask;
	if (size > mask)
		return DS_ERROR_CORRUPT;

	return DS_SUCCESS;
}

#endif /* DS_CK_RING_SPSC_H */
