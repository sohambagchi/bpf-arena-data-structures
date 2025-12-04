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

struct ds_list_node;

typedef struct ds_list_node __arena ds_list_node_t;

/**
 * struct ds_list_node - Node in the doubly-linked list
 * @next: Pointer to next node
 * @pprev: Pointer to pointer to this node (for efficient deletion)
 * @key: Key for this element
 * @value: Value stored in this element
 */
struct ds_list_node {
	ds_list_node_t *next;
	ds_list_node_t * __arena *pprev;
};

/**
 * struct ds_list_head - Head of the doubly-linked list
 * @first: Pointer to first element
 * @count: Number of elements in the list
 */
struct ds_list_head {
	struct ds_list_elem __arena *first;
	__u64 count;
};
typedef struct ds_list_head __arena ds_list_head_t;

struct ds_list_elem {
	struct ds_list_node node;
	__u64 key; // timestamp
	__u64 value;
	// struct ds_element value;
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
 * @member: Member name of the list_node in the struct
 * 
 * Safe for deletion during iteration.
 */
#define list_for_each_entry(pos, head, member)					\
	for (void * ___tmp = (pos = list_entry_safe((head)->first,		\
						    typeof(*(pos)), member),	\
			      (void *)0);					\
	     pos && ({ ___tmp = (void *)pos->member.next; 1; }) && can_loop;    \
	     pos = list_entry_safe((void __arena *)___tmp, typeof(*(pos)), member))

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

/**
 * __list_add_head - Add element to head of list (internal)
 */
static inline void __list_add_head(struct ds_list_elem __arena *elem,
                                   struct ds_list_head __arena *h)
{
	struct ds_list_elem __arena *first = h->first;
	struct ds_list_node __arena * __arena *tmp;
	ds_list_node_t *n = &elem->node;

	cast_user(first);
	cast_kern(n);
	if (first)
		WRITE_ONCE(n->next, &first->node);
	else
		WRITE_ONCE(n->next, NULL);
	cast_kern(first);
	if (first) {
		tmp = &n->next;
		cast_user(tmp);
		WRITE_ONCE(first->node.pprev, tmp);
	}
	cast_user(elem);
	WRITE_ONCE(h->first, elem);
	tmp = (struct ds_list_node __arena * __arena *)&h->first;
	cast_user(tmp);
	cast_kern(n);
	WRITE_ONCE(n->pprev, tmp);
}

/**
 * __list_del - Delete node from list (internal)
 */
static inline void __list_del(ds_list_node_t *n)
{
	ds_list_node_t *next = n->next;
	ds_list_node_t **tmp_ptr;
	ds_list_node_t * __arena *pprev = n->pprev;

	cast_user(next);
	cast_kern(pprev);
	tmp_ptr = (ds_list_node_t **)pprev;
	cast_kern(tmp_ptr);
	WRITE_ONCE(*tmp_ptr, next);
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
	cast_kern(head);
	if (!head)
		return DS_ERROR_INVALID;
	
	// cast_kern(head);
	head->first = NULL;
	head->count = 0;
	
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
// static inline int ds_list_insert(struct ds_list_head __arena *head, __u64 key, const struct ds_element *value)
// {
// 	struct ds_list_elem __arena *n;
	
// 	if (!head)
// 		return DS_ERROR_INVALID;
	
// 	/* Check if key already exists */
// 	list_for_each_entry(n, head, node) {
// 		cast_kern(n);
// 		if (n->key == key) {
// 			/* Key exists - update value */
// 			n->value.pid = value->pid;
// 			__builtin_memcpy(n->value.comm, value->comm, sizeof(n->value.comm));
// 			__builtin_memcpy(n->value.path, value->path, sizeof(n->value.path));
// 			return DS_SUCCESS;
// 		}
// 	}
	
// 	/* Allocate new node - returns __arena pointer */
// 	struct ds_list_elem __arena *new_node = bpf_arena_alloc(sizeof(*new_node));
// 	if (!new_node)
// 		return DS_ERROR_NOMEM;
	
// 	/* Initialize node - NO cast_kern needed with clang-20 */
// 	new_node->key = key;
// 	new_node->value.pid = value->pid;
// 	__builtin_memcpy(new_node->value.comm, value->comm, sizeof(new_node->value.comm));
// 	__builtin_memcpy(new_node->value.path, value->path, sizeof(new_node->value.path));
// 	/* Add to head of list */
// 	__list_add_head(new_node, head);
// 	head->count++;
	
// 	return DS_SUCCESS;
// }

static inline int ds_list_insert(struct ds_list_head __arena *head, __u64 key, __u64 value)
{
	struct ds_list_elem __arena *n;
	
	if (!head)
		ds_list_init(head);
	
	/* Check if key already exists */
	list_for_each_entry(n, head, node) {
		cast_kern(n);
		if (n->key == key) {
			/* Key exists - update value */
			n->value = value;
			return DS_SUCCESS;
		}
	}
	
	/* Allocate new node - returns __arena pointer */
	struct ds_list_elem __arena *new_node = bpf_arena_alloc(sizeof(*new_node));
	if (!new_node)
		return DS_ERROR_NOMEM;
	
	/* Initialize node - NO cast_kern needed with clang-20 */
	new_node->key = key;
	new_node->value = value;
	/* Add to head of list */
	__list_add_head(new_node, head);
	head->count++;
	
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
	struct ds_list_elem __arena *n;
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Search for key */
	list_for_each_entry(n, head, node) {
		cast_kern(n);
		if (n->key == key) {
			/* Found it - delete */
			__list_del(&n->node);
			bpf_arena_free(n);
			head->count--;
			return DS_SUCCESS;
		}
	}
	
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
// static inline int ds_list_search(struct ds_list_head __arena *head, const char *target_dir)
// {
// 	struct ds_list_elem __arena *n;
	
// 	if (!head || !target_dir)
// 		return DS_ERROR_INVALID;
	
// 	/* Search for key */
// 	list_for_each_entry(n, head, node) {
// 		cast_kern(n);
// 		if (__builtin_strncmp(n->value.path, target_dir, sizeof(n->value.path)) == 0) {
// 			return DS_SUCCESS;
// 		}
// 	}
	
// 	return DS_ERROR_NOT_FOUND;
// }
static inline int ds_list_search(struct ds_list_head __arena *head, __u64 key)
{
	struct ds_list_elem __arena *n;
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Search for key */
	list_for_each_entry(n, head, node) {
		cast_kern(n);
		if (n->key == key) {
			return DS_SUCCESS;
		}
	}
	
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
	struct ds_list_elem __arena *elem;
	struct ds_list_node __arena * __arena *expected_pprev = (struct ds_list_node __arena * __arena *)&head->first;
	__u64 count = 0;
	__u64 max_iterations = 100000; /* Prevent infinite loops */
	
	if (!head)
		return DS_ERROR_INVALID;
	
	cast_kern(head);
	
	list_for_each_entry(elem, head, node) {
		cast_kern(elem);
		count++;
		
		/* Check pprev pointer */
		if (elem->node.pprev != expected_pprev)
			return DS_ERROR_CORRUPT;
		
		/* Prevent infinite loops */
		if (count >= max_iterations)
			return DS_ERROR_CORRUPT;
		
		expected_pprev = &elem->node.next;
	}
	
	/* Check count */
	if (count != head->count)
		return DS_ERROR_CORRUPT;
	
	return DS_SUCCESS;
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
// typedef int (*ds_list_iter_fn)(__u64 key, struct ds_element value, void *ctx);

static inline __u64 ds_list_iterate(struct ds_list_head __arena *head,
                                     ds_list_iter_fn fn,
                                     void *ctx)
{
	struct ds_list_elem __arena *elem;
	__u64 count = 0;
	
	if (!head || !fn)
		return 0;
	
	list_for_each_entry(elem, head, node) {
		cast_kern(elem);
		
		int ret = fn(elem->key, elem->value, ctx);
		if (ret != 0)
			break;
		
		count++;
	}
	
	return count;
}
