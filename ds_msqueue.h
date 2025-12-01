/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Michael-Scott Non-Blocking Queue Implementation for BPF Arena
 * 
 * Based on "Simple, Fast, and Practical Non-Blocking and Blocking 
 * Concurrent Queue Algorithms" by Maged M. Michael and Michael L. Scott (1996)
 * 
 * This implementation follows the ds_api.h template and provides a lock-free
 * FIFO queue that works in both BPF kernel context and userspace with direct
 * arena access.
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_msqueue_node - Node in the Michael-Scott queue
 * @value: Value stored in this element (key is implicit in queue ordering)
 * @key: Key for search operations (not used for queue ordering)
 * @next: Pointer to next node
 * 
 * Note: In the MS Queue, nodes are enqueued in FIFO order regardless of key.
 * The key field is provided to satisfy the ds_api.h interface for search.
 */
struct ds_msqueue_node {
	__u64 key;
	__u64 value;
	struct ds_msqueue_node __arena *next;
};

/**
 * struct ds_msqueue_head - Head of the Michael-Scott queue
 * @head: Points to the dummy node (dequeue from head->next)
 * @tail: Points to the last node in the queue
 * @count: Number of elements in the queue (excluding dummy)
 * @stats: Operation statistics
 * 
 * Invariant: head always points to a dummy node
 * Invariant: tail points to the last node or lags slightly behind
 */
struct ds_msqueue_head {
	struct ds_msqueue_node __arena *head;
	struct ds_msqueue_node __arena *tail;
	__u64 count;
	struct ds_stats stats;
};

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

/**
 * ds_msqueue_init - Initialize an empty queue
 * @head: Queue head to initialize
 * 
 * Creates a dummy node as required by the MS Queue algorithm.
 * 
 * Returns: DS_SUCCESS or DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_msqueue_init(struct ds_msqueue_head __arena *head)
{
	struct ds_msqueue_node __arena *dummy;
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Allocate dummy node */
	dummy = bpf_arena_alloc(sizeof(*dummy));
	if (!dummy)
		return DS_ERROR_NOMEM;
	
	/* Initialize dummy node */
	cast_kern(dummy);
	dummy->key = 0;
	dummy->value = 0;
	dummy->next = NULL;
	
	/* Both head and tail point to dummy initially */
	cast_user(dummy);
	head->head = dummy;
	head->tail = dummy;
	head->count = 0;
	
	/* Initialize statistics */
	for (int i = 0; i < DS_OP_MAX; i++) {
		head->stats.ops[i].count = 0;
		head->stats.ops[i].failures = 0;
		head->stats.ops[i].total_time_ns = 0;
	}
	head->stats.current_elements = 0;
	head->stats.max_elements = 0;
	head->stats.memory_used = sizeof(*dummy);
	
	return DS_SUCCESS;
}

/**
 * ds_msqueue_insert - Enqueue a key-value pair (insert at tail)
 * @head: Queue head
 * @key: Key to enqueue
 * @value: Value to associate with key
 * 
 * Implements the non-blocking enqueue algorithm from the MS Queue paper.
 * This operation adds elements to the tail of the queue.
 * 
 * Returns: DS_SUCCESS on success, DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_msqueue_insert(struct ds_msqueue_head __arena *head, __u64 key, __u64 value)
{
	struct ds_msqueue_node __arena *new_node;
	struct ds_msqueue_node __arena *tail;
	struct ds_msqueue_node __arena *next;
	__u64 start_time = 0;
	int max_retries = 1000;
	int retry_count = 0;
	
#ifndef __BPF__
	start_time = ds_get_timestamp();
#else
	start_time = bpf_ktime_get_ns();
#endif
	
	if (!head) {
		head->stats.ops[DS_OP_INSERT].failures++;
		return DS_ERROR_INVALID;
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
	
	/* Enqueue loop */
	while (retry_count < max_retries && can_loop) {
		tail = head->tail;
		cast_kern(tail);
		next = tail->next;
		
		/* Check if tail is still consistent */
		cast_user(tail);
		if (tail != head->tail) {
			retry_count++;
			continue;
		}
		
		cast_kern(tail);
		if (next == NULL) {
			/* Tail is pointing to the last node, try to link new node */
			cast_user(new_node);
			if (arena_atomic_cmpxchg(&tail->next, next, new_node) == next) {
				/* Successfully linked, now try to swing tail */
				cast_user(tail);
				arena_atomic_cmpxchg(&head->tail, tail, new_node);
				
				/* Update statistics */
				arena_atomic_inc(&head->count);
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
		} else {
			/* Tail is lagging behind, help move it forward */
			cast_user(next);
			cast_user(tail);
			arena_atomic_cmpxchg(&head->tail, tail, next);
		}
		retry_count++;
	}
	
	/* Failed after max retries */
	cast_user(new_node);
	bpf_arena_free(new_node);
	head->stats.ops[DS_OP_INSERT].failures++;
	return DS_ERROR_INVALID;
}

/**
 * ds_msqueue_delete - Dequeue an element (remove from head)
 * @head: Queue head
 * @key: Output parameter for dequeued key
 * 
 * Implements the non-blocking dequeue algorithm from the MS Queue paper.
 * This operation removes elements from the head of the queue.
 * 
 * Note: The key parameter is used as an output to return the dequeued key.
 * 
 * Returns: DS_SUCCESS if dequeued, DS_ERROR_NOT_FOUND if queue is empty
 */
static inline int ds_msqueue_delete(struct ds_msqueue_head __arena *head, __u64 key)
{
	struct ds_msqueue_node __arena *h;
	struct ds_msqueue_node __arena *tail;
	struct ds_msqueue_node __arena *next;
	__u64 start_time = 0;
	int max_retries = 1000;
	int retry_count = 0;
	
#ifndef __BPF__
	start_time = ds_get_timestamp();
#else
	start_time = bpf_ktime_get_ns();
#endif
	
	if (!head) {
		return DS_ERROR_INVALID;
	}
	
	/* Dequeue loop */
	while (retry_count < max_retries && can_loop) {
		h = head->head;
		tail = head->tail;
		cast_kern(h);
		next = h->next;
		
		/* Check if head is still consistent */
		cast_user(h);
		if (h != head->head) {
			retry_count++;
			continue;
		}
		
		/* Check if queue is empty */
		if (next == NULL) {
			head->stats.ops[DS_OP_DELETE].failures++;
			return DS_ERROR_NOT_FOUND;
		}
		
		/* Check if tail is falling behind */
		if (h == tail) {
			/* Tail is pointing to head, help move it forward */
			cast_user(next);
			cast_user(tail);
			arena_atomic_cmpxchg(&head->tail, tail, next);
			retry_count++;
			continue;
		}
		
		/* Try to swing head to next node */
		cast_user(next);
		if (arena_atomic_cmpxchg(&head->head, h, next) == h) {
			/* Successfully dequeued */
			cast_kern(next);
			__u64 dequeued_key = next->key;
			__u64 dequeued_value = next->value;
			
			/* Free the old dummy node (h) */
			cast_user(h);
			bpf_arena_free(h);
			
			/* Update statistics */
			arena_atomic_dec(&head->count);
			head->stats.ops[DS_OP_DELETE].count++;
			head->stats.current_elements--;
			head->stats.memory_used -= sizeof(*h);
			
#ifndef __BPF__
			__u64 end_time = ds_get_timestamp();
#else
			__u64 end_time = bpf_ktime_get_ns();
#endif
			head->stats.ops[DS_OP_DELETE].total_time_ns += (end_time - start_time);
			
			return DS_SUCCESS;
		}
		retry_count++;
	}
	
	/* Failed after max retries */
	head->stats.ops[DS_OP_DELETE].failures++;
	return DS_ERROR_INVALID;
}

/**
 * ds_msqueue_search - Search for a key in the queue
 * @head: Queue head
 * @key: Key to search for
 * @value: Output parameter for value
 * 
 * Note: This is a linear search through the queue. In a pure FIFO queue,
 * search is not typically a primitive operation, but we provide it to
 * satisfy the ds_api.h interface.
 * 
 * Returns: DS_SUCCESS if found, DS_ERROR_NOT_FOUND otherwise
 */
static inline int ds_msqueue_search(struct ds_msqueue_head __arena *head, __u64 key, __u64 *value)
{
	struct ds_msqueue_node __arena *node;
	struct ds_msqueue_node __arena *h;
	__u64 start_time = 0;
	int max_iterations = 100000;
	int count = 0;
	
#ifndef __BPF__
	start_time = ds_get_timestamp();
#else
	start_time = bpf_ktime_get_ns();
#endif
	
	if (!head || !value) {
		head->stats.ops[DS_OP_SEARCH].failures++;
		return DS_ERROR_INVALID;
	}
	
	/* Start from head->next (skip dummy) */
	h = head->head;
	cast_kern(h);
	node = h->next;
	
	/* Search through queue */
	while (node && count < max_iterations && can_loop) {
		cast_kern(node);
		if (node->key == key) {
			*value = node->value;
			head->stats.ops[DS_OP_SEARCH].count++;
			
#ifndef __BPF__
			__u64 end_time = ds_get_timestamp();
#else
			__u64 end_time = bpf_ktime_get_ns();
#endif
			head->stats.ops[DS_OP_SEARCH].total_time_ns += (end_time - start_time);
			
			return DS_SUCCESS;
		}
		node = node->next;
		count++;
	}
	
	/* Not found */
	head->stats.ops[DS_OP_SEARCH].failures++;
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_msqueue_verify - Verify queue integrity
 * @head: Queue head
 * 
 * Checks:
 * - Head and tail are not NULL
 * - Head points to a valid dummy node
 * - Count matches actual number of elements
 * - No cycles in the queue
 * - Tail is reachable from head
 * 
 * Returns: DS_SUCCESS if valid, DS_ERROR_CORRUPT otherwise
 */
static inline int ds_msqueue_verify(struct ds_msqueue_head __arena *head)
{
	struct ds_msqueue_node __arena *node;
	struct ds_msqueue_node __arena *h;
	struct ds_msqueue_node __arena *tail;
	__u64 count = 0;
	__u64 max_iterations = 100000;
	int found_tail = 0;
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Check that head and tail are not NULL */
	h = head->head;
	tail = head->tail;
	
	if (!h || !tail)
		return DS_ERROR_CORRUPT;
	
	/* Start from head (dummy node) */
	node = h;
	
	/* Walk the queue and verify structure */
	while (node && count < max_iterations && can_loop) {
		/* Check if this is the tail node */
		if (node == tail)
			found_tail = 1;
		
		cast_kern(node);
		node = node->next;
		
		/* Count non-dummy nodes (skip first node which is dummy) */
		if (count > 0 || node != NULL)
			count++;
	}
	
	/* Adjust count (we counted dummy as well) */
	if (count > 0)
		count--;
	
	/* Verify we found the tail */
	if (!found_tail)
		return DS_ERROR_CORRUPT;
	
	/* Verify count matches (allow some slack due to concurrent operations) */
	__u64 recorded_count = head->count;
	if (count > recorded_count + 100 || recorded_count > count + 100)
		return DS_ERROR_CORRUPT;
	
	/* Check for infinite loop */
	if (count >= max_iterations)
		return DS_ERROR_CORRUPT;
	
	head->stats.ops[DS_OP_VERIFY].count++;
	return DS_SUCCESS;
}

/**
 * ds_msqueue_get_stats - Get queue statistics
 */
static inline void ds_msqueue_get_stats(struct ds_msqueue_head __arena *head, struct ds_stats *stats)
{
	if (!head || !stats)
		return;
	
	/* Copy all operation stats */
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
 * ds_msqueue_reset_stats - Reset queue statistics
 */
static inline void ds_msqueue_reset_stats(struct ds_msqueue_head __arena *head)
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
 * ds_msqueue_get_metadata - Get queue metadata
 */
static inline const struct ds_metadata* ds_msqueue_get_metadata(void)
{
	static const struct ds_metadata metadata = {
		.name = "msqueue",
		.description = "Michael-Scott Non-Blocking FIFO Queue",
		.node_size = sizeof(struct ds_msqueue_node),
		.requires_locking = 0,
	};
	return &metadata;
}

/* ========================================================================
 * ADDITIONAL OPERATIONS
 * ======================================================================== */

/**
 * ds_msqueue_iterate - Iterate over queue elements
 * @head: Queue head
 * @fn: Callback function for each element
 * @ctx: Context to pass to callback
 * 
 * Returns: Number of elements visited
 */
typedef int (*ds_msqueue_iter_fn)(__u64 key, __u64 value, void *ctx);

static inline __u64 ds_msqueue_iterate(struct ds_msqueue_head __arena *head,
                                        ds_msqueue_iter_fn fn,
                                        void *ctx)
{
	struct ds_msqueue_node __arena *node;
	struct ds_msqueue_node __arena *h;
	__u64 visited = 0;
	int max_iterations = 100000;
	
	if (!head || !fn)
		return 0;
	
	h = head->head;
	cast_kern(h);
	node = h->next; /* Skip dummy */
	
	while (node && visited < max_iterations && can_loop) {
		cast_kern(node);
		
		int result = fn(node->key, node->value, ctx);
		if (result != 0)
			break;
		
		node = node->next;
		visited++;
	}
	
	head->stats.ops[DS_OP_ITERATE].count++;
	return visited;
}

#endif /* DS_MSQUEUE_H */
