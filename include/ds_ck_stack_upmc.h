/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef DS_CK_STACK_UPMC_H
#define DS_CK_STACK_UPMC_H

#pragma once

#include "ds_api.h"

struct ds_ck_stack_upmc_entry;

typedef struct ds_ck_stack_upmc_entry __arena ds_ck_stack_upmc_entry_t;

struct ds_ck_stack_upmc_entry {
	ds_ck_stack_upmc_entry_t *next;
	struct ds_kv data;
};

struct ds_ck_stack_upmc_head {
	ds_ck_stack_upmc_entry_t *head;
	__u64 count;
};

typedef struct ds_ck_stack_upmc_head __arena ds_ck_stack_upmc_head_t;

static inline void ds_ck_stack_upmc_init(ds_ck_stack_upmc_head_t *stack)
{
	if (!stack)
		return;

	cast_kern(stack);
	WRITE_ONCE(stack->head, NULL);
	stack->count = 0;
}

static inline bool ds_ck_stack_upmc_isempty(const ds_ck_stack_upmc_head_t *stack)
{
	if (!stack)
		return true;

	return smp_load_acquire(&stack->head) == NULL;
}

static inline void ds_ck_stack_upmc_push_upmc(ds_ck_stack_upmc_head_t *stack,
					      ds_ck_stack_upmc_entry_t *entry,
					      __u64 key,
					      __u64 value)
{
	ds_ck_stack_upmc_entry_t *head;
	ds_ck_stack_upmc_entry_t *observed;
	bool pushed = false;

	if (!stack || !entry)
		return;

	cast_kern(stack);
	cast_kern(entry);

	entry->data.key = key;
	entry->data.value = value;
	head = READ_ONCE(stack->head);

	do {
		entry->next = head;
		cast_user(entry);
		observed = arena_atomic_cmpxchg(&stack->head, head, entry,
					       ARENA_RELEASE, ARENA_RELAXED);
		if (observed == head) {
			pushed = true;
			break;
		}
		head = observed;
	} while (can_loop);

	if (pushed)
		arena_atomic_add(&stack->count, 1, ARENA_RELAXED);
}

static inline bool ds_ck_stack_upmc_trypush_upmc(ds_ck_stack_upmc_head_t *stack,
						  ds_ck_stack_upmc_entry_t *entry,
						  __u64 key,
						  __u64 value)
{
	ds_ck_stack_upmc_entry_t *head;

	if (!stack || !entry)
		return false;

	cast_kern(stack);
	cast_kern(entry);

	entry->data.key = key;
	entry->data.value = value;
	head = READ_ONCE(stack->head);
	entry->next = head;

	cast_user(entry);
	if (arena_atomic_cmpxchg(&stack->head, head, entry,
				 ARENA_RELEASE, ARENA_RELAXED) != head)
		return false;

	arena_atomic_add(&stack->count, 1, ARENA_RELAXED);
	return true;
}

static inline ds_ck_stack_upmc_entry_t *
ds_ck_stack_upmc_pop_upmc(ds_ck_stack_upmc_head_t *stack)
{
	ds_ck_stack_upmc_entry_t *head;
	ds_ck_stack_upmc_entry_t *next;
	ds_ck_stack_upmc_entry_t *observed;

	if (!stack)
		return NULL;

	cast_kern(stack);
	head = smp_load_acquire(&stack->head);

	while (head != NULL && can_loop) {
		cast_kern(head);
		next = head->next;
		observed = arena_atomic_cmpxchg(&stack->head, head, next,
					       ARENA_ACQUIRE, ARENA_RELAXED);
		if (observed == head) {
			arena_atomic_sub(&stack->count, 1, ARENA_RELAXED);
			return head;
		}
		head = observed;
	}

	return NULL;
}

static inline bool ds_ck_stack_upmc_trypop_upmc(ds_ck_stack_upmc_head_t *stack,
						ds_ck_stack_upmc_entry_t **entry_out)
{
	ds_ck_stack_upmc_entry_t *head;
	ds_ck_stack_upmc_entry_t *next;

	if (!stack)
		return false;

	cast_kern(stack);
	head = smp_load_acquire(&stack->head);
	if (head == NULL)
		return false;

	cast_kern(head);
	next = head->next;
	if (arena_atomic_cmpxchg(&stack->head, head, next,
				 ARENA_ACQUIRE, ARENA_RELAXED) != head)
		return false;

	arena_atomic_sub(&stack->count, 1, ARENA_RELAXED);
	if (entry_out)
		*entry_out = head;

	return true;
}

static inline int ds_ck_stack_upmc_insert(ds_ck_stack_upmc_head_t *stack,
					  __u64 key,
					  __u64 value)
{
	ds_ck_stack_upmc_entry_t *entry;

	if (!stack)
		return DS_ERROR_INVALID;

	entry = (ds_ck_stack_upmc_entry_t *)bpf_arena_alloc(sizeof(*entry));
	if (!entry)
		return DS_ERROR_NOMEM;

	cast_kern(entry);
	ds_ck_stack_upmc_push_upmc(stack, entry, key, value);
	return DS_SUCCESS;
}

static inline int ds_ck_stack_upmc_delete(ds_ck_stack_upmc_head_t *stack, __u64 key)
{
	if (!stack)
		return DS_ERROR_INVALID;

	(void)key;
	return DS_ERROR_INVALID;
}

static inline int ds_ck_stack_upmc_pop(ds_ck_stack_upmc_head_t *stack, struct ds_kv *out)
{
	ds_ck_stack_upmc_entry_t *entry;

	if (!stack || !out)
		return DS_ERROR_INVALID;

	entry = ds_ck_stack_upmc_pop_upmc(stack);
	if (!entry)
		return DS_ERROR_NOT_FOUND;

	cast_kern(entry);
	out->key = entry->data.key;
	out->value = entry->data.value;

	return DS_SUCCESS;
}

static inline int ds_ck_stack_upmc_search(ds_ck_stack_upmc_head_t *stack, __u64 key)
{
	ds_ck_stack_upmc_entry_t *cursor;
	int iterations;

	if (!stack)
		return DS_ERROR_INVALID;

	cursor = smp_load_acquire(&stack->head);
	for (iterations = 0; cursor != NULL && iterations < 100000 && can_loop; iterations++) {
		cast_kern(cursor);
		if (cursor->data.key == key)
			return DS_SUCCESS;
		cursor = smp_load_acquire(&cursor->next);
	}

	return DS_ERROR_NOT_FOUND;
}

static inline int ds_ck_stack_upmc_verify(ds_ck_stack_upmc_head_t *stack)
{
	ds_ck_stack_upmc_entry_t *slow;
	ds_ck_stack_upmc_entry_t *fast;

	if (!stack)
		return DS_ERROR_INVALID;

	slow = smp_load_acquire(&stack->head);
	fast = slow;

	while (fast != NULL && can_loop) {
		ds_ck_stack_upmc_entry_t *fast_next;

		cast_kern(fast);
		fast_next = smp_load_acquire(&fast->next);
		if (fast_next == NULL)
			return DS_SUCCESS;

		cast_kern(fast_next);
		fast = smp_load_acquire(&fast_next->next);

		if (slow != NULL) {
			cast_kern(slow);
			slow = smp_load_acquire(&slow->next);
		}

		if (slow != NULL && slow == fast)
			return DS_ERROR_CORRUPT;
	}

	return DS_SUCCESS;
}

static inline int ds_ck_stack_upmc_stats(ds_ck_stack_upmc_head_t *stack, struct ds_stats *stats)
{
	if (!stack || !stats)
		return DS_ERROR_INVALID;

	stats->current_elements = arena_atomic_load(&stack->count, ARENA_RELAXED);
	stats->max_elements = 0;
	stats->memory_used = 0;
	return DS_SUCCESS;
}

#endif
