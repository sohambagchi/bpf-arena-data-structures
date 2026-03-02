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

/* Relaxed pointer access helpers. */
static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_ptr_load_lkmm(struct ds_ck_fifo_spsc_entry __arena * __arena *ptr)
{
	return READ_ONCE(*ptr);
}

static inline void ds_ck_fifo_spsc_ptr_store_lkmm(
	struct ds_ck_fifo_spsc_entry __arena * __arena *ptr,
	struct ds_ck_fifo_spsc_entry __arena *value)
{
	WRITE_ONCE(*ptr, value);
}

#ifndef __BPF__
static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_ptr_load_c(struct ds_ck_fifo_spsc_entry __arena * __arena *ptr)
{
	return arena_atomic_load(ptr, ARENA_RELAXED);
}

static inline void ds_ck_fifo_spsc_ptr_store_c(
	struct ds_ck_fifo_spsc_entry __arena * __arena *ptr,
	struct ds_ck_fifo_spsc_entry __arena *value)
{
	arena_atomic_store(ptr, value, ARENA_RELAXED);
}
#endif

static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_ptr_load(struct ds_ck_fifo_spsc_entry __arena * __arena *ptr)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_ptr_load_lkmm(ptr);
#else
	return ds_ck_fifo_spsc_ptr_load_c(ptr);
#endif
}

static inline void ds_ck_fifo_spsc_ptr_store(
	struct ds_ck_fifo_spsc_entry __arena * __arena *ptr,
	struct ds_ck_fifo_spsc_entry __arena *value)
{
#ifdef __BPF__
	ds_ck_fifo_spsc_ptr_store_lkmm(ptr, value);
#else
	ds_ck_fifo_spsc_ptr_store_c(ptr, value);
#endif
}

static inline void ds_ck_fifo_spsc_fifo_init_lkmm(struct ds_ck_fifo_spsc __arena *fifo,
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

#ifndef __BPF__
static inline void ds_ck_fifo_spsc_fifo_init_c(struct ds_ck_fifo_spsc __arena *fifo,
					       struct ds_ck_fifo_spsc_entry __arena *stub)
{
	cast_kern(fifo);
	cast_kern(stub);

	arena_atomic_store(&stub->next, NULL, ARENA_RELAXED);
	arena_atomic_store(&fifo->head, stub, ARENA_RELAXED);
	arena_atomic_store(&fifo->tail, stub, ARENA_RELAXED);
	arena_atomic_store(&fifo->head_snapshot, stub, ARENA_RELAXED);
	arena_atomic_store(&fifo->garbage, stub, ARENA_RELAXED);
}
#endif

static inline void ds_ck_fifo_spsc_fifo_init(struct ds_ck_fifo_spsc __arena *fifo,
					     struct ds_ck_fifo_spsc_entry __arena *stub)
{
#ifdef __BPF__
	ds_ck_fifo_spsc_fifo_init_lkmm(fifo, stub);
#else
	ds_ck_fifo_spsc_fifo_init_c(fifo, stub);
#endif
}

static inline void ds_ck_fifo_spsc_enqueue_lkmm(struct ds_ck_fifo_spsc __arena *fifo,
						 struct ds_ck_fifo_spsc_entry __arena *entry,
						 void __arena *value)
{
	struct ds_ck_fifo_spsc_entry __arena *tail;

	cast_kern(fifo);
	cast_kern(entry);

	entry->value = value;
	entry->next = NULL;

	tail = READ_ONCE(fifo->tail);
	cast_kern(tail);
	smp_store_release(&tail->next, entry);
	fifo->tail = entry;
}

#ifndef __BPF__
static inline void ds_ck_fifo_spsc_enqueue_c(struct ds_ck_fifo_spsc __arena *fifo,
					     struct ds_ck_fifo_spsc_entry __arena *entry,
					     void __arena *value)
{
	struct ds_ck_fifo_spsc_entry __arena *tail;

	cast_kern(fifo);
	cast_kern(entry);

	arena_atomic_store(&entry->value, value, ARENA_RELAXED);
	arena_atomic_store(&entry->next, NULL, ARENA_RELAXED);

	tail = arena_atomic_load(&fifo->tail, ARENA_RELAXED);
	cast_kern(tail);
	arena_atomic_store(&tail->next, entry, ARENA_RELEASE);
	arena_atomic_store(&fifo->tail, entry, ARENA_RELAXED);
}
#endif

static inline void ds_ck_fifo_spsc_enqueue(struct ds_ck_fifo_spsc __arena *fifo,
					    struct ds_ck_fifo_spsc_entry __arena *entry,
					    void __arena *value)
{
#ifdef __BPF__
	ds_ck_fifo_spsc_enqueue_lkmm(fifo, entry, value);
#else
	ds_ck_fifo_spsc_enqueue_c(fifo, entry, value);
#endif
}

static inline bool ds_ck_fifo_spsc_dequeue_lkmm(struct ds_ck_fifo_spsc __arena *fifo,
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

#ifndef __BPF__
static inline bool ds_ck_fifo_spsc_dequeue_c(struct ds_ck_fifo_spsc __arena *fifo,
					     void __arena **value_out)
{
	struct ds_ck_fifo_spsc_entry __arena *head;
	struct ds_ck_fifo_spsc_entry __arena *entry;

	cast_kern(fifo);

	head = arena_atomic_load(&fifo->head, ARENA_RELAXED);
	cast_kern(head);
	entry = arena_atomic_load(&head->next, ARENA_ACQUIRE);
	cast_user(entry);
	if (!entry)
		return false;

	cast_kern(entry);
	if (value_out)
		*value_out = arena_atomic_load(&entry->value, ARENA_RELAXED);

	arena_atomic_store(&fifo->head, entry, ARENA_RELEASE);
	return true;
}
#endif

static inline bool ds_ck_fifo_spsc_dequeue(struct ds_ck_fifo_spsc __arena *fifo,
				   void __arena **value_out)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_dequeue_lkmm(fifo, value_out);
#else
	return ds_ck_fifo_spsc_dequeue_c(fifo, value_out);
#endif
}

static inline bool ds_ck_fifo_spsc_isempty_lkmm(struct ds_ck_fifo_spsc __arena *fifo)
{
	struct ds_ck_fifo_spsc_entry __arena *head;

	cast_kern(fifo);
	head = READ_ONCE(fifo->head);
	cast_kern(head);
	return READ_ONCE(head->next) == NULL;
}

#ifndef __BPF__
static inline bool ds_ck_fifo_spsc_isempty_c(struct ds_ck_fifo_spsc __arena *fifo)
{
	struct ds_ck_fifo_spsc_entry __arena *head;

	cast_kern(fifo);
	head = arena_atomic_load(&fifo->head, ARENA_RELAXED);
	cast_kern(head);
	return arena_atomic_load(&head->next, ARENA_RELAXED) == NULL;
}
#endif

static inline bool ds_ck_fifo_spsc_isempty(struct ds_ck_fifo_spsc __arena *fifo)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_isempty_lkmm(fifo);
#else
	return ds_ck_fifo_spsc_isempty_c(fifo);
#endif
}

static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_recycle_lkmm(struct ds_ck_fifo_spsc __arena *fifo)
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

#ifndef __BPF__
static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_recycle_c(struct ds_ck_fifo_spsc __arena *fifo)
{
	struct ds_ck_fifo_spsc_entry __arena *garbage;

	cast_kern(fifo);

	if (arena_atomic_load(&fifo->head_snapshot, ARENA_RELAXED) ==
	    arena_atomic_load(&fifo->garbage, ARENA_RELAXED)) {
		arena_atomic_store(&fifo->head_snapshot,
				   arena_atomic_load(&fifo->head, ARENA_RELAXED),
				   ARENA_RELAXED);
		if (arena_atomic_load(&fifo->head_snapshot, ARENA_RELAXED) ==
		    arena_atomic_load(&fifo->garbage, ARENA_RELAXED))
			return NULL;
	}

	garbage = arena_atomic_load(&fifo->garbage, ARENA_RELAXED);
	cast_kern(garbage);
	arena_atomic_store(&fifo->garbage,
			   arena_atomic_load(&garbage->next, ARENA_RELAXED),
			   ARENA_RELAXED);
	return garbage;
}
#endif

static inline struct ds_ck_fifo_spsc_entry __arena *
ds_ck_fifo_spsc_recycle(struct ds_ck_fifo_spsc __arena *fifo)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_recycle_lkmm(fifo);
#else
	return ds_ck_fifo_spsc_recycle_c(fifo);
#endif
}

static inline int ds_ck_fifo_spsc_init_lkmm(struct ds_ck_fifo_spsc_head __arena *head)
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

	ds_ck_fifo_spsc_fifo_init_lkmm(&head->fifo, stub);
	return DS_SUCCESS;
}

#ifndef __BPF__
static inline int ds_ck_fifo_spsc_init_c(struct ds_ck_fifo_spsc_head __arena *head)
{
	struct ds_ck_fifo_spsc_entry __arena *stub;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	stub = (struct ds_ck_fifo_spsc_entry __arena *)bpf_arena_alloc(sizeof(*stub));
	if (!stub)
		return DS_ERROR_NOMEM;

	cast_kern(stub);
	arena_atomic_store(&stub->value, NULL, ARENA_RELAXED);

	arena_atomic_store(&stub->kv.key, 0, ARENA_RELAXED);
	arena_atomic_store(&stub->kv.value, 0, ARENA_RELAXED);

	ds_ck_fifo_spsc_fifo_init_c(&head->fifo, stub);
	return DS_SUCCESS;
}
#endif

static inline int ds_ck_fifo_spsc_init(struct ds_ck_fifo_spsc_head __arena *head)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_init_lkmm(head);
#else
	return ds_ck_fifo_spsc_init_c(head);
#endif
}

static inline int ds_ck_fifo_spsc_insert_lkmm(struct ds_ck_fifo_spsc_head __arena *head,
					      __u64 key, __u64 value)
{
	struct ds_ck_fifo_spsc_entry __arena *entry;
	struct ds_kv __arena *payload;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	entry = ds_ck_fifo_spsc_recycle_lkmm(&head->fifo);
	if (!entry) {
		entry = (struct ds_ck_fifo_spsc_entry __arena *)bpf_arena_alloc(sizeof(*entry));
		if (!entry)
			return DS_ERROR_NOMEM;
	}

	cast_kern(entry);
	WRITE_ONCE(entry->kv.key, key);
	WRITE_ONCE(entry->kv.value, value);
	payload = &entry->kv;
	ds_ck_fifo_spsc_enqueue_lkmm(&head->fifo, entry, payload);
	return DS_SUCCESS;
}

#ifndef __BPF__
static inline int ds_ck_fifo_spsc_insert_c(struct ds_ck_fifo_spsc_head __arena *head,
					   __u64 key, __u64 value)
{
	struct ds_ck_fifo_spsc_entry __arena *entry;
	struct ds_kv __arena *payload;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	entry = ds_ck_fifo_spsc_recycle_c(&head->fifo);
	if (!entry) {
		entry = (struct ds_ck_fifo_spsc_entry __arena *)bpf_arena_alloc(sizeof(*entry));
		if (!entry)
			return DS_ERROR_NOMEM;
	}

	cast_kern(entry);
	arena_atomic_store(&entry->kv.key, key, ARENA_RELAXED);
	arena_atomic_store(&entry->kv.value, value, ARENA_RELAXED);
	payload = &entry->kv;
	ds_ck_fifo_spsc_enqueue_c(&head->fifo, entry, payload);
	return DS_SUCCESS;
}
#endif

static inline int ds_ck_fifo_spsc_insert(struct ds_ck_fifo_spsc_head __arena *head,
					 __u64 key, __u64 value)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_insert_lkmm(head, key, value);
#else
	return ds_ck_fifo_spsc_insert_c(head, key, value);
#endif
}

static inline int ds_ck_fifo_spsc_delete_lkmm(struct ds_ck_fifo_spsc_head __arena *head,
					      struct ds_kv *out)
{
	void __arena *value;
	struct ds_kv __arena *payload;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	if (!ds_ck_fifo_spsc_dequeue_lkmm(&head->fifo, &value))
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

#ifndef __BPF__
static inline int ds_ck_fifo_spsc_delete_c(struct ds_ck_fifo_spsc_head __arena *head,
					   struct ds_kv *out)
{
	void __arena *value;
	struct ds_kv __arena *payload;

	if (!head)
		return DS_ERROR_INVALID;

	cast_kern(head);
	if (!ds_ck_fifo_spsc_dequeue_c(&head->fifo, &value))
		return DS_ERROR_NOT_FOUND;

	if (!out)
		return DS_SUCCESS;

	payload = (struct ds_kv __arena *)value;
	if (!payload)
		return DS_ERROR_CORRUPT;

	cast_kern(payload);
	out->key = arena_atomic_load(&payload->key, ARENA_RELAXED);
	out->value = arena_atomic_load(&payload->value, ARENA_RELAXED);
	return DS_SUCCESS;
}
#endif

static inline int ds_ck_fifo_spsc_delete(struct ds_ck_fifo_spsc_head __arena *head,
					 struct ds_kv *out)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_delete_lkmm(head, out);
#else
	return ds_ck_fifo_spsc_delete_c(head, out);
#endif
}

static inline int ds_ck_fifo_spsc_pop_lkmm(struct ds_ck_fifo_spsc_head __arena *head,
					   struct ds_kv *out)
{
	return ds_ck_fifo_spsc_delete_lkmm(head, out);
}

#ifndef __BPF__
static inline int ds_ck_fifo_spsc_pop_c(struct ds_ck_fifo_spsc_head __arena *head,
					struct ds_kv *out)
{
	return ds_ck_fifo_spsc_delete_c(head, out);
}
#endif

static inline int ds_ck_fifo_spsc_pop(struct ds_ck_fifo_spsc_head __arena *head,
				      struct ds_kv *out)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_pop_lkmm(head, out);
#else
	return ds_ck_fifo_spsc_pop_c(head, out);
#endif
}

static inline int ds_ck_fifo_spsc_search_lkmm(struct ds_ck_fifo_spsc_head __arena *head,
					      __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

#ifndef __BPF__
static inline int ds_ck_fifo_spsc_search_c(struct ds_ck_fifo_spsc_head __arena *head,
					   __u64 key)
{
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}
#endif

static inline int ds_ck_fifo_spsc_search(struct ds_ck_fifo_spsc_head __arena *head,
					 __u64 key)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_search_lkmm(head, key);
#else
	return ds_ck_fifo_spsc_search_c(head, key);
#endif
}

static inline int ds_ck_fifo_spsc_verify_lkmm(struct ds_ck_fifo_spsc_head __arena *head)
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

	cursor = ds_ck_fifo_spsc_ptr_load_lkmm(&head->fifo.head);
	tail = ds_ck_fifo_spsc_ptr_load_lkmm(&head->fifo.tail);
	if (!cursor || !tail)
		return DS_ERROR_CORRUPT;

	for (i = 0; i < max_steps && can_loop; i++) {
		if (cursor == tail)
			return DS_SUCCESS;
		cast_kern(cursor);
		cursor = ds_ck_fifo_spsc_ptr_load_lkmm(&cursor->next);
		if (!cursor)
			return DS_ERROR_CORRUPT;
	}

	return DS_ERROR_CORRUPT;
}

#ifndef __BPF__
static inline int ds_ck_fifo_spsc_verify_c(struct ds_ck_fifo_spsc_head __arena *head)
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

	cursor = ds_ck_fifo_spsc_ptr_load_c(&head->fifo.head);
	tail = ds_ck_fifo_spsc_ptr_load_c(&head->fifo.tail);
	if (!cursor || !tail)
		return DS_ERROR_CORRUPT;

	for (i = 0; i < max_steps && can_loop; i++) {
		if (cursor == tail)
			return DS_SUCCESS;
		cast_kern(cursor);
		cursor = ds_ck_fifo_spsc_ptr_load_c(&cursor->next);
		if (!cursor)
			return DS_ERROR_CORRUPT;
	}

	return DS_ERROR_CORRUPT;
}
#endif

static inline int ds_ck_fifo_spsc_verify(struct ds_ck_fifo_spsc_head __arena *head)
{
#ifdef __BPF__
	return ds_ck_fifo_spsc_verify_lkmm(head);
#else
	return ds_ck_fifo_spsc_verify_c(head);
#endif
}

#endif /* DS_CK_FIFO_SPSC_H */
