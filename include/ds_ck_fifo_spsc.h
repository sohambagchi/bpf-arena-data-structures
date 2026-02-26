/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef DS_CK_FIFO_SPSC_H
#define DS_CK_FIFO_SPSC_H

#pragma once

#ifndef __BPF__
#include <linux/types.h>
#endif

#include "ds_api.h"

struct ds_ck_fifo_spsc_entry {
	void __arena *value;
	struct ds_ck_fifo_spsc_entry __arena *next;
	struct ds_kv kv;
};

struct ds_ck_fifo_spsc {
	struct ds_ck_fifo_spsc_entry __arena *head;
	struct ds_ck_fifo_spsc_entry __arena *tail;
	struct ds_ck_fifo_spsc_entry __arena *head_snapshot;
	struct ds_ck_fifo_spsc_entry __arena *garbage;
};

struct ds_ck_fifo_spsc_head {
	struct ds_ck_fifo_spsc fifo;
};

typedef struct ds_ck_fifo_spsc_head __arena ds_ck_fifo_spsc_head_t;
typedef struct ds_ck_fifo_spsc_entry __arena ds_ck_fifo_spsc_entry_t;

/* Relaxed pointer access — READ_ONCE/WRITE_ONCE in both BPF and userspace.
 * Do NOT use arena_atomic_load/arena_atomic_store (broken per GUIDE.md). */
#define ds_ck_fifo_spsc_ptr_load(ptr) READ_ONCE(*(ptr))
#define ds_ck_fifo_spsc_ptr_store(ptr, value) WRITE_ONCE(*(ptr), (value))

static inline void ds_ck_fifo_spsc_fifo_init(struct ds_ck_fifo_spsc __arena *fifo,
					     struct ds_ck_fifo_spsc_entry __arena *stub)
{
	cast_kern(fifo);
	cast_kern(stub);

	stub->next = NULL;
	fifo->head = stub;
	fifo->tail = stub;
	fifo->head_snapshot = stub;
	fifo->garbage = stub;
}

static inline void ds_ck_fifo_spsc_enqueue(struct ds_ck_fifo_spsc __arena *fifo,
					    struct ds_ck_fifo_spsc_entry __arena *entry,
					    void __arena *value)
{
	struct ds_ck_fifo_spsc_entry __arena *tail;

	cast_kern(fifo);
	cast_kern(entry);

	// WRITE_ONCE(entry->value, value);
	// WRITE_ONCE(entry->next, NULL);
	entry->value = value;
	entry->next = NULL;

	tail = READ_ONCE(fifo->tail);
	cast_kern(tail);
	smp_store_release(&tail->next, entry);
	// ds_ck_fifo_spsc_ptr_store(&fifo->tail, entry);
	fifo->tail = entry;
}

static inline bool ds_ck_fifo_spsc_dequeue(struct ds_ck_fifo_spsc __arena *fifo,
					   void __arena **value_out)
{
	struct ds_ck_fifo_spsc_entry __arena *head;
	struct ds_ck_fifo_spsc_entry __arena *entry;

	cast_kern(fifo);

	head = READ_ONCE(fifo->head);
	cast_kern(head);
	entry = smp_load_acquire(&head->next);
	cast_user(entry);
	if (!entry)
		return false;

	cast_kern(entry);
	if (value_out)
		*value_out = READ_ONCE(entry->value);

	smp_store_release(&fifo->head, entry);
	return true;
}

static inline bool ds_ck_fifo_spsc_isempty(struct ds_ck_fifo_spsc __arena *fifo)
{
	struct ds_ck_fifo_spsc_entry __arena *head;

	cast_kern(fifo);
	head = READ_ONCE(fifo->head);
	cast_kern(head);
	return READ_ONCE(head->next) == NULL;
}

static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_recycle(struct ds_ck_fifo_spsc __arena *fifo)
{
	struct ds_ck_fifo_spsc_entry __arena *garbage;

	cast_kern(fifo);

	if (fifo->head_snapshot == fifo->garbage) {
		fifo->head_snapshot = READ_ONCE(fifo->head);
		if (fifo->head_snapshot == fifo->garbage)
			return NULL;
	}

	garbage = fifo->garbage;
	cast_kern(garbage);
	fifo->garbage = garbage->next;
	return garbage;
}

static inline int ds_ck_fifo_spsc_init(struct ds_ck_fifo_spsc_head __arena *head)
{
	struct ds_ck_fifo_spsc_entry __arena *stub;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	stub = (struct ds_ck_fifo_spsc_entry __arena *)bpf_arena_alloc(sizeof(*stub));
	if (!stub)
		return DS_ERROR_NOMEM;

	cast_kern(stub);
	stub->value = NULL;

	WRITE_ONCE(stub->kv.key, 0);
	WRITE_ONCE(stub->kv.value, 0);

	ds_ck_fifo_spsc_fifo_init(&head->fifo, stub);
	return DS_SUCCESS;
}

static inline int ds_ck_fifo_spsc_insert(struct ds_ck_fifo_spsc_head __arena *head,
					 __u64 key, __u64 value)
{
	struct ds_ck_fifo_spsc_entry __arena *entry;
	struct ds_kv __arena *payload;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	entry = ds_ck_fifo_spsc_recycle(&head->fifo);
	if (!entry) {
		entry = (struct ds_ck_fifo_spsc_entry __arena *)bpf_arena_alloc(sizeof(*entry));
		if (!entry)
			return DS_ERROR_NOMEM;
	}

	cast_kern(entry);
	WRITE_ONCE(entry->kv.key, key);
	WRITE_ONCE(entry->kv.value, value);
	payload = &entry->kv;
	ds_ck_fifo_spsc_enqueue(&head->fifo, entry, payload);
	return DS_SUCCESS;
}

static inline int ds_ck_fifo_spsc_delete(struct ds_ck_fifo_spsc_head __arena *head,
					 struct ds_kv *out)
{
	void __arena *value;
	struct ds_kv __arena *payload;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	if (!ds_ck_fifo_spsc_dequeue(&head->fifo, &value))
		return DS_ERROR_NOT_FOUND;

	if (!out)
		return DS_SUCCESS;

	payload = (struct ds_kv __arena *)value;
	if (!payload)
		return DS_ERROR_CORRUPT;

	cast_kern(payload);
	out->key = READ_ONCE(payload->key);
	out->value = READ_ONCE(payload->value);
	return DS_SUCCESS;
}

static inline int ds_ck_fifo_spsc_pop(struct ds_ck_fifo_spsc_head __arena *head,
				      struct ds_kv *out)
{
	return ds_ck_fifo_spsc_delete(head, out);
}

static inline int ds_ck_fifo_spsc_search(struct ds_ck_fifo_spsc_head __arena *head,
					 __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

static inline int ds_ck_fifo_spsc_verify(struct ds_ck_fifo_spsc_head __arena *head)
{
	struct ds_ck_fifo_spsc_entry __arena *cursor;
	struct ds_ck_fifo_spsc_entry __arena *tail;
	unsigned int i;
	const unsigned int max_steps = 100000;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);

	if (!head->fifo.head || !head->fifo.tail || !head->fifo.garbage)
		return DS_ERROR_CORRUPT;

	cursor = ds_ck_fifo_spsc_ptr_load(&head->fifo.head);
	tail = ds_ck_fifo_spsc_ptr_load(&head->fifo.tail);
	if (!cursor || !tail)
		return DS_ERROR_CORRUPT;

	for (i = 0; i < max_steps && can_loop; i++) {
		if (cursor == tail)
			return DS_SUCCESS;
		cast_kern(cursor);
		cursor = ds_ck_fifo_spsc_ptr_load(&cursor->next);
		if (!cursor)
			return DS_ERROR_CORRUPT;
	}

	return DS_ERROR_CORRUPT;
}

#undef ds_ck_fifo_spsc_ptr_load
#undef ds_ck_fifo_spsc_ptr_store

#endif /* DS_CK_FIFO_SPSC_H */
