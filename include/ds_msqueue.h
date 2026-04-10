/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Michael-Scott Non-Blocking Queue Implementation for BPF Arena
 * 
 * Based on "Simple, Fast, and Practical Non-Blocking and Blocking 
 * Concurrent Queue Algorithms" by Maged M. Michael and Michael L. Scott (1996)
 * 
 * This is a reference implementation following the ds_api.h template.
 * It demonstrates a lock-free FIFO queue that works in both BPF kernel
 * context and userspace with direct arena access.
 */
#ifndef DS_MSQUEUE_H
#define DS_MSQUEUE_H

#pragma once

#include "ds_api.h"

#define DS_MSQUEUE_MAX_RETRIES 100

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

struct ds_msqueue_node;

typedef struct ds_msqueue_node __arena ds_msqueue_node_t;


/**
 * struct ds_msqueue_node - Node wrapper in the Michael-Scott queue
 * @elem: Embedded user data (key-value pair)
 * @next: Pointer to next node in the queue
 * 
 * This is the actual node structure used to build the linked list.
 * It wraps the user data (ds_msqueue_elem) and provides the next pointer
 * for linking nodes together in the queue.
 */
struct ds_msqueue_node {
	ds_msqueue_node_t *next;
};

/**
 * struct ds_msqueue_elem - Data payload for queue elements
 * @key: Key/identifier for this element (e.g., process ID)
 * @value: Associated data value (e.g., timestamp)
 * 
 * This structure contains the actual user data stored in each queue node.
 * It is embedded within ds_msqueue_node along with the next pointer.
 */
struct ds_msqueue_elem {
	struct ds_msqueue_node node;
	struct ds_kv data;
};
/**
 * struct ds_msqueue - Michael-Scott queue head structure
 * @head: Pointer to head node (always points to dummy node)
 * @tail: Pointer to tail node (last node, may lag during concurrent operations)
 * @count: Number of elements in queue (excluding the dummy node)
 * 
 * The queue maintains two key invariants:
 * 1. head always points to a dummy node; the first actual element is head->next
 * 2. tail points to the last node or may lag slightly behind during concurrent enqueues
 * 
 * This design enables lock-free enqueue and dequeue operations using compare-and-swap.
 */
struct ds_msqueue {
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	__u64 count;
};
typedef struct ds_msqueue __arena ds_msqueue_t;

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

#define __msqueue_list_entry(ptr, type, member) arena_container_of(ptr, type, member)

/**
 * ds_msqueue_init - Initialize an empty Michael-Scott queue
 * @queue: Pointer to queue structure to initialize
 * 
 * Allocates and initializes a dummy node as required by the MS Queue algorithm.
 * Both head and tail pointers are set to point to this dummy node.
 * The dummy node ensures the queue is never truly empty, simplifying the algorithm.
 * 
 * Returns: DS_SUCCESS on success,
 *          DS_ERROR_INVALID if queue pointer is NULL,
 *          DS_ERROR_NOMEM if dummy node allocation fails
 */
static inline int ds_msqueue_init_lkmm(struct ds_msqueue __arena *queue)
{
	struct ds_msqueue_elem __arena *dummy;
	
	cast_kern(queue);
	if (!queue)
		return DS_ERROR_INVALID;
	
	/* Allocate dummy element */
	dummy = bpf_arena_alloc(sizeof(*dummy));
	if (!dummy)
		return DS_ERROR_NOMEM;
	
	cast_kern(dummy);
	/* Initialize dummy element - NO cast_kern needed with clang-20 */
	WRITE_ONCE(dummy->node.next, NULL);
	WRITE_ONCE(dummy->data.key, 420);
	WRITE_ONCE(dummy->data.value, 69);
	
	/* Both head and tail point to dummy initially */
	cast_user(dummy);
	queue->head = dummy;
	queue->tail = dummy;
	queue->count = 0;
	
	return DS_SUCCESS;
}

#ifndef __BPF__
static inline int ds_msqueue_init_c(struct ds_msqueue __arena *queue)
{
	struct ds_msqueue_elem __arena *dummy;

	cast_kern(queue);
	if (!queue)
		return DS_ERROR_INVALID;

	dummy = bpf_arena_alloc(sizeof(*dummy));
	if (!dummy)
		return DS_ERROR_NOMEM;

	cast_kern(dummy);
	arena_atomic_store(&dummy->node.next, NULL, ARENA_RELAXED);
	arena_atomic_store(&dummy->data.key, 420, ARENA_RELAXED);
	arena_atomic_store(&dummy->data.value, 69, ARENA_RELAXED);

	cast_user(dummy);
	queue->head = dummy;
	queue->tail = dummy;
	queue->count = 0;

	return DS_SUCCESS;
}
#endif

static inline int ds_msqueue_init(struct ds_msqueue __arena *queue)
{
#ifdef __BPF__
	return ds_msqueue_init_lkmm(queue);
#else
	return ds_msqueue_init_c(queue);
#endif
}

/**
 * __msqueue_add_node - Helper to enqueue a node
 * @new_node: New node to add
 * @queue: Queue to which the node is added
 * 
 * Performs the while loop of the Michael-Scott enqueue algorithm.
 * Tries to link the new node at tail->next using compare-and-swap.
 * If tail has been modified by another thread, helps advance tail before retrying.
 * This cooperative behavior ensures progress even under high contention.
 * Note: This function assumes new_node is already initialized.
 * 
 * Returns: None
 * (Internal helper function) */
static inline int __msqueue_add_node_lkmm(struct ds_msqueue_elem __arena *new_node,
					  struct ds_msqueue __arena *queue) {
	struct ds_msqueue_elem __arena *tail;
	struct ds_msqueue_node __arena *next;
	int max_retries = DS_MSQUEUE_MAX_RETRIES;
	int retry_count = 0;

	/* Enqueue loop */
	while (retry_count < max_retries && can_loop) {
		/* Read tail */

		tail = READ_ONCE(queue->tail);
		
		cast_kern(tail);

		
		next = READ_ONCE(tail->node.next);
		
		cast_user(next);
		if (next != NULL) {
			// Tail is lagging
			struct ds_msqueue_elem __arena *next_elem;
			next_elem = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);

			cast_user(tail);
			(void)arena_atomic_cmpxchg(&queue->tail, tail, next_elem, ARENA_RELEASE, ARENA_RELAXED);
			retry_count++;
			continue;
		}

		cast_kern(new_node);
		if (arena_atomic_cmpxchg(&tail->node.next, next, &new_node->node, ARENA_RELEASE, ARENA_RELAXED) == next) {
			break;
		}

		retry_count++;
		continue;
	}

	if (retry_count >= max_retries) {
		return DS_ERROR_INVALID;
	}

	/* Update count (relaxed: just statistics) */
	arena_atomic_inc(&queue->count);
	
	cast_user(tail);
	cast_user(new_node);
	/* Successfully linked, now try to swing tail to new node */
	if (arena_atomic_cmpxchg(&queue->tail, tail, new_node, ARENA_RELEASE, ARENA_RELAXED) != tail) {
		/* Failed to update tail, but it's okay - another thread will help */
		return DS_SUCCESS;
	}
		
	return DS_SUCCESS;
}

#ifndef __BPF__
static inline int __msqueue_add_node_c(struct ds_msqueue_elem __arena *new_node,
				       struct ds_msqueue __arena *queue) {
	struct ds_msqueue_elem __arena *tail;
	struct ds_msqueue_node __arena *next;
	int max_retries = DS_MSQUEUE_MAX_RETRIES;
	int retry_count = 0;

	while (retry_count < max_retries && can_loop) {

		tail = arena_atomic_load(&queue->tail, ARENA_ACQUIRE);
		cast_kern(tail);

		next = arena_atomic_load(&tail->node.next, ARENA_ACQUIRE);
		cast_user(next);

		if (next != NULL) {
			struct ds_msqueue_elem __arena *next_elem;
			next_elem = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);

			cast_user(tail);
			(void)arena_atomic_cmpxchg(&queue->tail, tail, next_elem, ARENA_RELEASE, ARENA_RELAXED);
			retry_count++;
			continue;
		}

		cast_kern(new_node);
		if (arena_atomic_cmpxchg(&tail->node.next, next, &new_node->node,
						ARENA_RELEASE, ARENA_RELAXED) == next) {
			break;
		}

		retry_count++;
		continue;
	}

	if (retry_count >= max_retries)
		return DS_ERROR_INVALID;

	arena_atomic_inc(&queue->count);

	cast_user(tail);
	cast_user(new_node);
	if (arena_atomic_cmpxchg(&queue->tail, tail, new_node, ARENA_RELEASE, ARENA_RELAXED) != tail)
		return DS_SUCCESS;

	return DS_SUCCESS;
}
#endif


/**
 * ds_msqueue_insert - Enqueue a key-value pair at the tail
 * @queue: Pointer to queue structure
 * @key: Key to insert into the queue
 * @value: Value associated with the key
 * 
 * Implements the lock-free enqueue algorithm from the Michael-Scott queue paper.
 * Allocates a new node and attempts to link it at tail->next using compare-and-swap.
 * If tail has been modified by another thread, helps advance tail before retrying.
 * This cooperative behavior ensures progress even under high contention.
 * 
 * Returns: DS_SUCCESS on successful enqueue,
 *          DS_ERROR_INVALID if queue is NULL or operation fails after max retries,
 *          DS_ERROR_NOMEM if node allocation fails
 */
static inline int ds_msqueue_insert_lkmm(struct ds_msqueue __arena *queue, __u64 key, __u64 value)
{
	struct ds_msqueue_elem __arena *new_node;
	
	if (!queue)
		return DS_ERROR_INVALID;
	
	/* Allocate new element */
	new_node = bpf_arena_alloc(sizeof(*new_node));
	if (!new_node)
		return DS_ERROR_NOMEM;
	
	/* Initialize element */
	new_node->data.key = key;
	new_node->data.value = value;
	new_node->node.next = NULL;
	
	cast_user(new_node);
	if (__msqueue_add_node_lkmm(new_node, queue) == DS_SUCCESS) {
		return DS_SUCCESS;
	} else {
		cast_user(new_node);
		bpf_arena_free(new_node);
		return DS_ERROR_INVALID;
	}
}

#ifndef __BPF__
static inline int ds_msqueue_insert_c(struct ds_msqueue __arena *queue, __u64 key, __u64 value)
{
	struct ds_msqueue_elem __arena *new_node;

	if (!queue)
		return DS_ERROR_INVALID;

	new_node = bpf_arena_alloc(sizeof(*new_node));
	if (!new_node)
		return DS_ERROR_NOMEM;

	new_node->data.key = key;
	new_node->data.value = value;
	new_node->node.next = NULL;

	cast_user(new_node);
	if (__msqueue_add_node_c(new_node, queue) == DS_SUCCESS) {
		return DS_SUCCESS;
	} else {
		cast_user(new_node);
		bpf_arena_free(new_node);
		return DS_ERROR_INVALID;
	}
}
#endif

static inline int ds_msqueue_insert(struct ds_msqueue __arena *queue, __u64 key, __u64 value)
{
#ifdef __BPF__
	return ds_msqueue_insert_lkmm(queue, key, value);
#else
	return ds_msqueue_insert_c(queue, key, value);
#endif
}

/**
 * ds_msqueue_pop - Dequeue an element from the head
 * @queue: Pointer to queue structure
 * @data: Output buffer for dequeued key-value pair
 * 
 * Implements the lock-free dequeue algorithm from the Michael-Scott queue paper.
 * Attempts to swing the head pointer to head->next using compare-and-swap, effectively
 * removing the current dummy node. The old dummy is then freed, and what was head->next
 * becomes the new dummy. If tail is falling behind, helps advance it before retrying.
 * 
 * Returns: DS_SUCCESS if element successfully dequeued,
 *          DS_ERROR_INVALID if queue is NULL or operation fails after max retries,
 *          DS_ERROR_NOT_FOUND if queue is empty (head->next is NULL)
 */
static inline int ds_msqueue_pop_lkmm(struct ds_msqueue __arena *queue, struct ds_kv *data)
{
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	ds_msqueue_node_t *next;
	// ds_msqueue_node_t *next_tail;
	int max_retries = DS_MSQUEUE_MAX_RETRIES;
	int retry_count = 0;
	
	if (!queue || !data) {
		return DS_ERROR_INVALID;
	}

	/* Dequeue loop */
	while (retry_count < max_retries && can_loop) {
		/* Read Head, Tail, and next */

		head = READ_ONCE(queue->head);
		tail = READ_ONCE(queue->tail);
		
		cast_kern(head);
		next = READ_ONCE(head->node.next);
		
		cast_user(head);
		if ( READ_ONCE(queue->head) != head ) {
			retry_count++;
			continue;
		}

		cast_user(next);
		if ( next == NULL ) {
			/* Queue is empty */
			return DS_ERROR_NOT_FOUND;
		}

		cast_user(tail);
		if ( head == tail ) {
			// Tail is lagging
			struct ds_msqueue_elem __arena *next_elem_tail;
			next_elem_tail = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);
			(void)arena_atomic_cmpxchg(&queue->tail, tail, next_elem_tail, ARENA_RELEASE, ARENA_RELAXED);
			retry_count++;
			continue;
		}

		struct ds_msqueue_elem __arena *next_elem;
		next_elem = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);
		
		cast_kern(next_elem);
		data->key = next_elem->data.key;
		data->value = next_elem->data.value;

		cast_user(next_elem);
		/* LKMM: address dependency chain (head → head->next → next_elem →
		 * next_elem->data) ensures data visibility; relax CAS to RELAXED */
		if ( arena_atomic_cmpxchg(&queue->head, head, next_elem, ARENA_RELAXED, ARENA_RELAXED) == head) {
			cast_user(head);
			bpf_arena_free(head);
		
			/* Update count (relaxed: just statistics) */
			arena_atomic_dec(&queue->count);
			return DS_SUCCESS;
		}
		retry_count++;
		continue;
	}
		
	/* Failed after max retries */
	return DS_ERROR_INVALID;
}

#ifndef __BPF__
static inline int ds_msqueue_pop_c(struct ds_msqueue __arena *queue, struct ds_kv *data)
{
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	ds_msqueue_node_t *next;
	int max_retries = DS_MSQUEUE_MAX_RETRIES;
	int retry_count = 0;

	/* Guard userspace caller mistakes that can surface as runner SIGSEGV (-11). */
	if (!queue || !data)
		return DS_ERROR_INVALID;

	while (retry_count < max_retries && can_loop) {
		head = arena_atomic_load(&queue->head, ARENA_ACQUIRE);
		tail = arena_atomic_load(&queue->tail, ARENA_ACQUIRE);

		/* Defend rare metadata corruption/races under high runner load (SIGSEGV -11). */
		if (!head || !tail)
			return DS_ERROR_INVALID;

		cast_kern(head);

		next = arena_atomic_load(&head->node.next, ARENA_ACQUIRE);

		cast_user(head);
		if (arena_atomic_load(&queue->head, ARENA_ACQUIRE) != head) {
			retry_count++;
			continue;
		}

		cast_user(next);
		if (next == NULL)
			return DS_ERROR_NOT_FOUND;

		cast_user(tail);
		if (head == tail) {
			struct ds_msqueue_elem __arena *next_elem_tail;
			next_elem_tail = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);
			if (!next_elem_tail)
				return DS_ERROR_INVALID;
			(void)arena_atomic_cmpxchg(&queue->tail, tail, next_elem_tail, ARENA_RELEASE, ARENA_RELAXED);
			retry_count++;
			continue;
		}

		struct ds_msqueue_elem __arena *next_elem;
		next_elem = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);
		if (!next_elem)
			return DS_ERROR_INVALID;

		cast_kern(next_elem);
		data->key = next_elem->data.key;
		data->value = next_elem->data.value;

		cast_user(next_elem);
		if (arena_atomic_cmpxchg(&queue->head, head, next_elem, ARENA_ACQUIRE, ARENA_RELAXED) == head) {
			cast_user(head);
			bpf_arena_free(head);
			arena_atomic_dec(&queue->count);
			return DS_SUCCESS;
		}
		retry_count++;
		continue;
	}

	return DS_ERROR_INVALID;
}
#endif

static inline int ds_msqueue_pop(struct ds_msqueue __arena *queue, struct ds_kv *data)
{
#ifdef __BPF__
	return ds_msqueue_pop_lkmm(queue, data);
#else
	return ds_msqueue_pop_c(queue, data);
#endif
}

/**
 * ds_msqueue_search - Search for a key in the queue
 * @queue: Pointer to queue structure
 * @key: Key to search for
 * 
 * Performs a linear search starting from head->next (skipping the dummy node).
 * Note: Search is not a standard queue operation but is provided to satisfy
 * the ds_api.h interface requirements. This operation is NOT lock-free and may
 * observe inconsistent state during concurrent modifications.
 * 
 * Returns: DS_SUCCESS if key is found,
 *          DS_ERROR_INVALID if queue is NULL,
 *          DS_ERROR_NOT_FOUND if key not found or queue is empty
 */
static inline int ds_msqueue_search_lkmm(struct ds_msqueue __arena *queue, __u64 key)
{
	struct ds_msqueue_node __arena *next;
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *node;
	int max_iterations = 100000;
	int count = 0;
	
	if (!queue)
		return DS_ERROR_INVALID;
	
	/* Start from Head->next (skip dummy) */
	head = queue->head;
	cast_kern(head);
	next = head->node.next;
	
	/* Search through queue */
	while (next && count < max_iterations && can_loop) {
		cast_user(next);
		node = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);

		if (node->data.key == key)
			return DS_SUCCESS;
		
		cast_kern(node);
		/* LKMM: address dependency from next → node provides ordering */
		next = READ_ONCE(node->node.next);
		count++;
	}
	
	return DS_ERROR_NOT_FOUND;
}

#ifndef __BPF__
static inline int ds_msqueue_search_c(struct ds_msqueue __arena *queue, __u64 key)
{
	struct ds_msqueue_node __arena *next;
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *node;
	int max_iterations = 100000;
	int count = 0;

	if (!queue)
		return DS_ERROR_INVALID;

	head = queue->head;
	cast_kern(head);
	next = head->node.next;

	while (next && count < max_iterations && can_loop) {
		cast_user(next);
		node = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);

		if (node->data.key == key)
			return DS_SUCCESS;

		cast_kern(node);
		next = arena_atomic_load(&node->node.next, ARENA_ACQUIRE);
		count++;
	}

	return DS_ERROR_NOT_FOUND;
}
#endif

static inline int ds_msqueue_search(struct ds_msqueue __arena *queue, __u64 key)
{
#ifdef __BPF__
	return ds_msqueue_search_lkmm(queue, key);
#else
	return ds_msqueue_search_c(queue, key);
#endif
}

/**
 * ds_msqueue_verify - Verify queue structural integrity
 * @queue: Pointer to queue structure
 * 
 * Performs comprehensive consistency checks:
 * - Verifies head and tail pointers are not NULL
 * - Confirms tail is reachable from head (detects broken links)
 * - Validates element count matches actual queue length (with tolerance for concurrent ops)
 * - Detects cycles that would cause infinite loops
 * 
 * Note: This operation is NOT thread-safe and should only be called when
 * no concurrent modifications are occurring (e.g., during testing/debugging).
 * 
 * Returns: DS_SUCCESS if queue structure is valid,
 *          DS_ERROR_INVALID if queue is NULL,
 *          DS_ERROR_CORRUPT if structural corruption detected
 */
static inline int ds_msqueue_verify_lkmm(struct ds_msqueue __arena *queue)
{
	struct ds_msqueue_elem __arena *node;
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	__u64 count = 0;
	__u64 max_iterations = 100000;
	int found_tail = 0;
	
	if (!queue)
		return DS_ERROR_INVALID;
	
	/* Read Head and Tail */
	head = queue->head;
	tail = queue->tail;
	
	if (!head || !tail)
		return DS_ERROR_CORRUPT*5;
	
	/* Start from Head (dummy node) */
	cast_kern(head);
	/* LKMM: address dependency from node_ptr → node provides ordering */
	struct ds_msqueue_node __arena *node_ptr = READ_ONCE(head->node.next);
	if (head == tail && node_ptr == NULL)
		return DS_SUCCESS;
	node = (void __arena *)__msqueue_list_entry(node_ptr, struct ds_msqueue_elem, node);
	
	/* Walk the queue and verify structure */
	while (node && count < max_iterations && can_loop) {
		/* Check if this is the tail node */
		if (node == tail)
			found_tail = 1;
		
		cast_kern(node);
		/* LKMM: address dependency from node_ptr → node provides ordering */
		node_ptr = READ_ONCE(node->node.next);
		if (node_ptr) {
			node = (void __arena *)__msqueue_list_entry(node_ptr, struct ds_msqueue_elem, node);
		} else {
			node = 0;
		}
		
		/* Count non-dummy nodes (skip first node which is dummy) */
		if (count > 0 || node != 0)
			count++;
	}
	
	/* Adjust count (we counted dummy as well) */
	if (count > 0)
		count--;
	
	/* Verify we found the tail */
	if (!found_tail)
		return DS_ERROR_CORRUPT*2;
	
	/* Verify count matches (allow some slack due to concurrent operations) */
	__u64 recorded_count = queue->count;
	if (count > recorded_count + 100 || recorded_count > count + 100)
		return DS_ERROR_CORRUPT*3;
	
	/* Check for infinite loop */
	if (count >= max_iterations)
		return DS_ERROR_CORRUPT*4;
	
	return DS_SUCCESS;
}

#ifndef __BPF__
static inline int ds_msqueue_verify_c(struct ds_msqueue __arena *queue)
{
	struct ds_msqueue_elem __arena *node;
	struct ds_msqueue_elem __arena *head;
	struct ds_msqueue_elem __arena *tail;
	__u64 count = 0;
	__u64 max_iterations = 100000;
	int found_tail = 0;

	if (!queue)
		return DS_ERROR_INVALID;

	head = queue->head;
	tail = queue->tail;

	if (!head || !tail)
		return DS_ERROR_CORRUPT*5;

	cast_kern(head);
	struct ds_msqueue_node __arena *node_ptr = arena_atomic_load(&head->node.next, ARENA_ACQUIRE);
	if (head == tail && node_ptr == NULL)
		return DS_SUCCESS;
	node = (void __arena *)__msqueue_list_entry(node_ptr, struct ds_msqueue_elem, node);

	while (node && count < max_iterations && can_loop) {
		if (node == tail)
			found_tail = 1;

		cast_kern(node);
		node_ptr = arena_atomic_load(&node->node.next, ARENA_ACQUIRE);
		if (node_ptr) {
			node = (void __arena *)__msqueue_list_entry(node_ptr, struct ds_msqueue_elem, node);
		} else {
			node = 0;
		}

		if (count > 0 || node != 0)
			count++;
	}

	if (count > 0)
		count--;

	if (!found_tail)
		return DS_ERROR_CORRUPT*2;

	__u64 recorded_count = queue->count;
	if (count > recorded_count + 100 || recorded_count > count + 100)
		return DS_ERROR_CORRUPT*3;

	if (count >= max_iterations)
		return DS_ERROR_CORRUPT*4;

	return DS_SUCCESS;
}
#endif

static inline int ds_msqueue_verify(struct ds_msqueue __arena *queue)
{
#ifdef __BPF__
	return ds_msqueue_verify_lkmm(queue);
#else
	return ds_msqueue_verify_c(queue);
#endif
}

/**
 * ds_msqueue_get_metadata - Get Michael-Scott queue metadata
 * 
 * Returns a pointer to a static structure containing metadata about this
 * data structure implementation, including name, description, node size,
 * and locking requirements.
 * 
 * Returns: Pointer to static ds_metadata structure
 */
static inline const struct ds_metadata* ds_msqueue_get_metadata(void)
{
	static const struct ds_metadata metadata = {
		.name = "msqueue",
		.description = "Michael-Scott Non-Blocking FIFO Queue",
		.node_size = sizeof(struct ds_msqueue_node),
		.requires_locking = 0, /* Lock-free with proper atomics */
	};
	
	return &metadata;
}

/* ========================================================================
 * ITERATION HELPER
 * ======================================================================== */

/**
 * ds_msqueue_iterate - Iterate over all queue elements
 * @queue: Pointer to queue structure
 * @fn: Callback function invoked for each element (receives key, value, and context)
 * @ctx: User-provided context pointer passed to each callback invocation
 * 
 * Walks the queue from head->next to the end, invoking the callback function
 * for each element. Iteration stops early if the callback returns non-zero.
 * 
 * Note: This operation is NOT safe for concurrent modifications. It should only
 * be used when no other threads are modifying the queue.
 * 
 * Returns: Number of elements successfully visited before iteration ended
 */
typedef int (*ds_msqueue_iter_fn)(__u64 key, __u64 value, void *ctx);

static inline __u64 ds_msqueue_iterate_lkmm(struct ds_msqueue __arena *queue,
					    ds_msqueue_iter_fn fn,
					    void *ctx)
{
	struct ds_msqueue_elem __arena *node;
	struct ds_msqueue_elem __arena *head;
	__u64 visited = 0;
	__u64 max_iterations = 10;
	
	if (!queue || !fn)
		return 0;
	
	head = queue->head;
	cast_kern(head);
	struct ds_msqueue_node __arena *node_ptr = head->node.next;
	if (node_ptr) {
		node = (void __arena *)__msqueue_list_entry(node_ptr, struct ds_msqueue_elem, node);
	} else {
		return 0;
	}
	
	for (; node && visited < max_iterations && can_loop; visited++) {
		struct ds_msqueue_node __arena *next;
		
		cast_kern(node);
		
		int result = fn(node->data.key, node->data.value, ctx);
		if (result != 0)
			break;
		
		/* LKMM: address dependency from next → node provides ordering */
		next = READ_ONCE(node->node.next);
		if (next) {
			cast_user(next);
			node = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);
		} else {
			node = 0;
		}
	}
	
	return visited;
}

#ifndef __BPF__
static inline __u64 ds_msqueue_iterate_c(struct ds_msqueue __arena *queue,
					 ds_msqueue_iter_fn fn,
					 void *ctx)
{
	struct ds_msqueue_elem __arena *node;
	struct ds_msqueue_elem __arena *head;
	__u64 visited = 0;
	__u64 max_iterations = 10;

	if (!queue || !fn)
		return 0;

	head = queue->head;
	cast_kern(head);
	struct ds_msqueue_node __arena *node_ptr = head->node.next;
	if (node_ptr) {
		node = (void __arena *)__msqueue_list_entry(node_ptr, struct ds_msqueue_elem, node);
	} else {
		return 0;
	}

	for (; node && visited < max_iterations && can_loop; visited++) {
		struct ds_msqueue_node __arena *next;

		cast_kern(node);

		int result = fn(node->data.key, node->data.value, ctx);
		if (result != 0)
			break;

		next = arena_atomic_load(&node->node.next, ARENA_ACQUIRE);
		if (next) {
			cast_user(next);
			node = (void __arena *)__msqueue_list_entry(next, struct ds_msqueue_elem, node);
		} else {
			node = 0;
		}
	}

	return visited;
}
#endif

static inline __u64 ds_msqueue_iterate(struct ds_msqueue __arena *queue,
				       ds_msqueue_iter_fn fn,
				       void *ctx)
{
#ifdef __BPF__
	return ds_msqueue_iterate_lkmm(queue, fn, ctx);
#else
	return ds_msqueue_iterate_c(queue, fn, ctx);
#endif
}

#endif /* DS_MSQUEUE_H */
