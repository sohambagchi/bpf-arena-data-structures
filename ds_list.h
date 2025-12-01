/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Doubly-Linked List Implementation for BPF Arena
 * 
 * This is a reference implementation following the ds_api.h template.
 * It demonstrates how to implement a concurrent data structure that works
 * in both BPF kernel context and userspace with direct arena access.
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_list_node - Node in the doubly-linked list
 * @next: Pointer to next node
 * @pprev: Pointer to pointer to this node (for efficient deletion)
 * @key: Key for this element
 * @value: Value stored in this element
 */
struct ds_list_node {
	struct ds_list_node __arena *next;
	struct ds_list_node __arena * __arena *pprev;
	__u64 key;
	__u64 value;
};

/**
 * struct ds_list_head - Head of the doubly-linked list
 * @first: Pointer to first node
 * @count: Number of elements in the list
 * @stats: Operation statistics
 */
struct ds_list_head {
	struct ds_list_node __arena *first;
	__u64 count;
	struct ds_stats stats;
};

/* ========================================================================
 * HELPER MACROS
 * ======================================================================== */

#define list_entry(ptr, type, member) arena_container_of(ptr, type, member)

#define list_entry_safe(ptr, type, member) \
	({ typeof(*ptr) * ___ptr = (ptr); \
	 ___ptr ? ({ cast_kern(___ptr); list_entry(___ptr, type, member); }) : NULL; \
	 })

/**
 * list_for_each_entry - Iterate over list entries
 * @pos: Iterator variable
 * @head: List head
 * 
 * Safe for deletion during iteration.
 */
#define list_for_each_entry(pos, head)						\
	for (void * ___tmp = (pos = list_entry_safe((head)->first,		\
						    typeof(*(pos)), node),	\
			      (void *)0);					\
	     pos && ({ ___tmp = (void *)pos->next; 1; }) && can_loop;		\
	     pos = list_entry_safe((void __arena *)___tmp, typeof(*(pos)), node))

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

/**
 * __list_add_head - Add node to head of list (internal)
 */
static inline void __list_add_head(struct ds_list_node __arena *n,
                                    struct ds_list_head __arena *h)
{
	struct ds_list_node __arena *first = h->first;
	struct ds_list_node __arena * __arena *tmp;

	cast_user(first);
	cast_kern(n);
	WRITE_ONCE(n->next, first);
	cast_kern(first);
	if (first) {
		tmp = &n->next;
		cast_user(tmp);
		WRITE_ONCE(first->pprev, tmp);
	}
	cast_user(n);
	WRITE_ONCE(h->first, n);

	cast_kern(n);
	n->pprev = &h->first;
}

/**
 * __list_del - Delete node from list (internal)
 */
static inline void __list_del(struct ds_list_node __arena *n)
{
	struct ds_list_node __arena *next = n->next;
	struct ds_list_node __arena *tmp;
	struct ds_list_node __arena * __arena *pprev = n->pprev;

	cast_user(next);
	cast_kern(pprev);
	tmp = *pprev;
	cast_kern(tmp);
	WRITE_ONCE(tmp, next);
	if (next) {
		cast_user(pprev);
		cast_kern(next);
		WRITE_ONCE(next->pprev, pprev);
	}
}

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

/**
 * ds_list_init - Initialize an empty list
 * @head: List head to initialize
 * 
 * Returns: DS_SUCCESS
 */
static inline int ds_list_init(struct ds_list_head __arena *head)
{
	if (!head)
		return DS_ERROR_INVALID;
	
	head->first = NULL;
	head->count = 0;
	
	/* Initialize statistics */
	for (int i = 0; i < DS_OP_MAX; i++) {
		head->stats.ops[i].count = 0;
		head->stats.ops[i].failures = 0;
		head->stats.ops[i].total_time_ns = 0;
	}
	head->stats.current_elements = 0;
	head->stats.max_elements = 0;
	head->stats.memory_used = 0;
	
	return DS_SUCCESS;
}

/**
 * ds_list_insert - Insert a key-value pair into the list
 * @head: List head
 * @key: Key to insert
 * @value: Value to associate with key
 * 
 * If key already exists, updates the value.
 * 
 * Returns: DS_SUCCESS on success, DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_list_insert(struct ds_list_head __arena *head, __u64 key, __u64 value)
{
	struct ds_list_node __arena *n, *new_node;
	__u64 start_time = 0;
	
#ifndef __BPF__
	start_time = ds_get_timestamp();
#else
	start_time = bpf_ktime_get_ns();
#endif
	
	if (!head) {
		head->stats.ops[DS_OP_INSERT].failures++;
		return DS_ERROR_INVALID;
	}
	
	/* Check if key already exists */
	n = head->first;
	while (n && can_loop) {
		cast_kern(n);
		if (n->key == key) {
			/* Update existing value */
			n->value = value;
			head->stats.ops[DS_OP_INSERT].count++;
			return DS_SUCCESS;
		}
		n = n->next;
	}
	
	/* Allocate new node */
	new_node = bpf_arena_alloc(sizeof(*new_node));
	if (!new_node) {
		head->stats.ops[DS_OP_INSERT].failures++;
		return DS_ERROR_NOMEM;
	}
	
	/* Initialize node */
	cast_kern(new_node);
	new_node->key = key;
	new_node->value = value;
	new_node->next = NULL;
	new_node->pprev = NULL;
	
	/* Add to head of list */
	__list_add_head(new_node, head);
	head->count++;
	
	/* Update statistics */
	head->stats.ops[DS_OP_INSERT].count++;
	head->stats.current_elements++;
	if (head->stats.current_elements > head->stats.max_elements)
		head->stats.max_elements = head->stats.current_elements;
	head->stats.memory_used += sizeof(*new_node);
	
#ifndef __BPF__
	__u64 end_time = ds_get_timestamp();
#else
	__u64 end_time = bpf_ktime_get_ns();
#endif
	head->stats.ops[DS_OP_INSERT].total_time_ns += (end_time - start_time);
	
	return DS_SUCCESS;
}

/**
 * ds_list_delete - Delete a key from the list
 * @head: List head
 * @key: Key to delete
 * 
 * Returns: DS_SUCCESS if deleted, DS_ERROR_NOT_FOUND if key doesn't exist
 */
static inline int ds_list_delete(struct ds_list_head __arena *head, __u64 key)
{
	struct ds_list_node __arena *n;
	__u64 start_time = 0;
	
#ifndef __BPF__
	start_time = ds_get_timestamp();
#else
	start_time = bpf_ktime_get_ns();
#endif
	
	if (!head) {
		return DS_ERROR_INVALID;
	}
	
	/* Search for key */
	n = head->first;
	while (n && can_loop) {
		cast_kern(n);
		if (n->key == key) {
			/* Found it - delete */
			__list_del(n);
			bpf_arena_free(n);
			head->count--;
			
			/* Update statistics */
			head->stats.ops[DS_OP_DELETE].count++;
			head->stats.current_elements--;
			head->stats.memory_used -= sizeof(*n);
			
#ifndef __BPF__
			__u64 end_time = ds_get_timestamp();
#else
			__u64 end_time = bpf_ktime_get_ns();
#endif
			head->stats.ops[DS_OP_DELETE].total_time_ns += (end_time - start_time);
			
			return DS_SUCCESS;
		}
		n = n->next;
	}
	
	/* Not found */
	head->stats.ops[DS_OP_DELETE].failures++;
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_list_search - Search for a key in the list
 * @head: List head
 * @key: Key to search for
 * @value: Output parameter for value
 * 
 * Returns: DS_SUCCESS if found, DS_ERROR_NOT_FOUND otherwise
 */
static inline int ds_list_search(struct ds_list_head __arena *head, __u64 key, __u64 *value)
{
	struct ds_list_node __arena *n;
	__u64 start_time = 0;
	
#ifndef __BPF__
	start_time = ds_get_timestamp();
#else
	start_time = bpf_ktime_get_ns();
#endif
	
	if (!head || !value) {
		if (head)
			head->stats.ops[DS_OP_SEARCH].failures++;
		return DS_ERROR_INVALID;
	}
	
	/* Search for key */
	n = head->first;
	while (n && can_loop) {
		cast_kern(n);
		if (n->key == key) {
			*value = n->value;
			head->stats.ops[DS_OP_SEARCH].count++;
			
#ifndef __BPF__
			__u64 end_time = ds_get_timestamp();
#else
			__u64 end_time = bpf_ktime_get_ns();
#endif
			head->stats.ops[DS_OP_SEARCH].total_time_ns += (end_time - start_time);
			
			return DS_SUCCESS;
		}
		n = n->next;
	}
	
	/* Not found */
	head->stats.ops[DS_OP_SEARCH].failures++;
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_list_verify - Verify list integrity
 * @head: List head
 * 
 * Checks:
 * - Count matches actual number of elements
 * - pprev pointers are correct
 * - No cycles
 * 
 * Returns: DS_SUCCESS if valid, DS_ERROR_CORRUPT otherwise
 */
static inline int ds_list_verify(struct ds_list_head __arena *head)
{
	struct ds_list_node __arena *n;
	struct ds_list_node __arena * __arena *expected_pprev;
	__u64 count = 0;
	__u64 max_iterations = 100000; /* Prevent infinite loops */
	
	if (!head)
		return DS_ERROR_INVALID;
	
	cast_kern(head);
	n = head->first;
	
	while (n && count < max_iterations && can_loop) {
		cast_kern(n);
		count++;
		
		/* Check pprev pointer */
		if (n->pprev != expected_pprev) {
			head->stats.ops[DS_OP_VERIFY].failures++;
			return DS_ERROR_CORRUPT;
		}
		
		expected_pprev = &n->next;
		n = n->next;
	}
	
	/* Check count */
	if (count != head->count) {
		head->stats.ops[DS_OP_VERIFY].failures++;
		return DS_ERROR_CORRUPT;
	}
	
	/* Check for cycle (if we hit max_iterations) */
	if (count >= max_iterations && n != NULL) {
		head->stats.ops[DS_OP_VERIFY].failures++;
		return DS_ERROR_CORRUPT;
	}
	
	head->stats.ops[DS_OP_VERIFY].count++;
	return DS_SUCCESS;
}

/**
 * ds_list_get_stats - Get operation statistics
 * @head: List head
 * @stats: Output parameter for statistics
 */
static inline void ds_list_get_stats(struct ds_list_head __arena *head, struct ds_stats *stats)
{
	if (!head || !stats)
		return;
	
	/* Copy statistics */
	for (int i = 0; i < DS_OP_MAX; i++) {
		stats->ops[i].count = head->stats.ops[i].count;
		stats->ops[i].failures = head->stats.ops[i].failures;
		stats->ops[i].total_time_ns = head->stats.ops[i].total_time_ns;
	}
	
	stats->current_elements = head->stats.current_elements;
	stats->max_elements = head->stats.max_elements;
	stats->memory_used = head->stats.memory_used;
}

/**
 * ds_list_reset_stats - Reset operation statistics
 * @head: List head
 */
static inline void ds_list_reset_stats(struct ds_list_head __arena *head)
{
	if (!head)
		return;
	
	for (int i = 0; i < DS_OP_MAX; i++) {
		head->stats.ops[i].count = 0;
		head->stats.ops[i].failures = 0;
		head->stats.ops[i].total_time_ns = 0;
	}
}

/**
 * ds_list_get_metadata - Get data structure metadata
 * 
 * Returns: Pointer to metadata structure
 */
static inline const struct ds_metadata* ds_list_get_metadata(void)
{
	static const struct ds_metadata metadata = {
		.name = "list",
		.description = "Doubly-linked list",
		.node_size = sizeof(struct ds_list_node),
		.requires_locking = 0, /* Lock-free with proper atomics */
	};
	
	return &metadata;
}

/* ========================================================================
 * ITERATION HELPER
 * ======================================================================== */

/**
 * ds_list_iterate - Iterate over all elements
 * @head: List head
 * @callback: Function to call for each element
 * @ctx: Context to pass to callback
 * 
 * Callback should return 0 to continue, non-zero to stop.
 * 
 * Returns: Number of elements visited
 */
typedef int (*ds_list_iter_fn)(__u64 key, __u64 value, void *ctx);

static inline __u64 ds_list_iterate(struct ds_list_head __arena *head,
                                     ds_list_iter_fn fn,
                                     void *ctx)
{
	struct ds_list_node __arena *n;
	__u64 count = 0;
	
	if (!head || !fn)
		return 0;
	
	n = head->first;
	while (n && can_loop) {
		cast_kern(n);
		
		int ret = fn(n->key, n->value, ctx);
		if (ret != 0)
			break;
		
		count++;
		n = n->next;
	}
	
	head->stats.ops[DS_OP_ITERATE].count++;
	return count;
}
