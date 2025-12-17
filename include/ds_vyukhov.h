/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Vyukhov's Bounded MPMC Queue Implementation for BPF Arena
 * 
 * Based on Dmitry Vyukov's lock-free bounded MPMC queue algorithm:
 * https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 * 
 * Characteristics:
 * - Lock-free multi-producer multi-consumer queue
 * - Fixed capacity (power of 2)
 * - Single CAS operation in fast path
 * - Sequence numbers for coordination and ABA protection
 * 
 * This implementation follows the ds_api.h template for the BPF Arena framework.
 */
#ifndef DS_VYUKHOV_H
#define DS_VYUKHOV_H

#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_vyukhov_node - Single cell in the ring buffer
 * @sequence: Coordination counter for lock-free access
 * @key: User data key
 * @value: User data value
 * 
 * The sequence field acts as both a coordination mechanism and ABA protection:
 * - Producer can write if: sequence == enqueue_pos
 * - Consumer can read if: sequence == dequeue_pos + 1
 * - After write: sequence = pos + 1
 * - After read: sequence = pos + mask + 1 (wraps to next lap)
 */
struct ds_vyukhov_node {
	__u64 sequence;
	struct ds_kv data;
};

/**
 * struct ds_vyukhov_head - MPMC queue head structure
 * @enqueue_pos: Atomic counter for producers
 * @pad1: Cache line padding to prevent false sharing
 * @dequeue_pos: Atomic counter for consumers
 * @pad2: Cache line padding to prevent false sharing
 * @buffer_mask: Capacity - 1 (for fast modulo)
 * @buffer: Pointer to the ring buffer array
 * @count: Current number of elements (approximate, for observability)
 * 
 * The padding ensures that enqueue_pos and dequeue_pos reside on different
 * cache lines to minimize contention between producers and consumers.
 */
struct ds_vyukhov_head {
	/* Producer state */
	__u64 enqueue_pos;
	char pad1[56];  /* Pad to 64 bytes (cache line) */
	
	/* Consumer state */
	__u64 dequeue_pos;
	char pad2[56];  /* Pad to 64 bytes (cache line) */
	
	/* Constants and memory */
	__u64 buffer_mask;
	struct ds_vyukhov_node __arena *buffer;
	
	/* Statistics (approximate) */
	__u64 count;
};

typedef struct ds_vyukhov_head __arena ds_vyukhov_head_t;
typedef struct ds_vyukhov_node __arena ds_vyukhov_node_t;

/* ========================================================================
 * HELPER MACROS
 * ======================================================================== */

/* Maximum retry attempts for CAS operations (verifier safety) */
#define DS_VYUKHOV_MAX_RETRIES 100

/* Default capacity if not specified */
#define DS_VYUKHOV_DEFAULT_CAPACITY 128

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

/**
 * ds_vyukhov_init - Initialize the MPMC queue
 * @head: Queue head to initialize
 * @capacity: Queue capacity (must be power of 2, e.g., 1024)
 * 
 * Allocates the ring buffer and initializes sequence numbers.
 * Each cell's sequence is set to its index for the first lap.
 * 
 * Returns: DS_SUCCESS on success, DS_ERROR_INVALID if capacity is invalid,
 *          DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_vyukhov_init(struct ds_vyukhov_head __arena *head, __u32 capacity)
{
	cast_kern(head);
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Capacity must be power of 2 and at least 2 */
	if (capacity < 2 || (capacity & (capacity - 1)) != 0)
		return DS_ERROR_INVALID;
	
	/* Initialize control fields */
	head->buffer_mask = capacity - 1;
	WRITE_ONCE(head->enqueue_pos, 0);
	WRITE_ONCE(head->dequeue_pos, 0);
	WRITE_ONCE(head->count, 0);
	// head->enqueue_pos = 0;
	// head->dequeue_pos = 0;
	// head->count = 0;
	
	/* Allocate the ring buffer */
	head->buffer = bpf_arena_alloc(capacity * sizeof(struct ds_vyukhov_node));
	if (!head->buffer)
		return DS_ERROR_NOMEM;
	
	cast_kern(head->buffer);
	
	/* Initialize sequence counters - cell i gets sequence i 
	 * Note: data fields are already zeroed by arena allocation */
	for (__u32 i = 0; i < capacity && can_loop; i++) {
		struct ds_vyukhov_node __arena *cell = &head->buffer[i];
		cast_kern(cell);
		WRITE_ONCE(cell->sequence, i);
	}
	
	return DS_SUCCESS;
}

/**
 * ds_vyukhov_insert - Enqueue an element (insert)
 * @head: Queue head
 * @key: Key to insert
 * @value: Value to insert
 * 
 * Producers compete to reserve a slot via CAS on enqueue_pos.
 * A slot is available when cell->sequence == pos.
 * After writing data, sets sequence to pos + 1 to signal consumers.
 * 
 * Returns: DS_SUCCESS on success
 *          DS_ERROR_NOMEM if queue is full
 *          DS_ERROR_BUSY if max retries exceeded
 */
static inline int ds_vyukhov_insert(struct ds_vyukhov_head __arena *head,
                                     __u64 key, __u64 value)
{
	struct ds_vyukhov_node __arena *cell;
	__u64 pos;
	__u64 mask;
	int retries = 0;
	
	if (!head || !head->buffer)
		return DS_ERROR_INVALID;
	
	pos = READ_ONCE(head->enqueue_pos);
	mask = head->buffer_mask;
	
	/* Retry loop with verifier-safe bound */
	for (; retries < DS_VYUKHOV_MAX_RETRIES && can_loop; retries++) {
		cell = &head->buffer[pos & mask];
		cast_kern(cell);
		
		__u64 seq = smp_load_acquire(&cell->sequence);
		__s64 dif = (__s64)seq - (__s64)pos;
		
		if (dif == 0) {
			/* Cell is ready for write. Try to claim it. */
			__u64 old_pos = arena_atomic_cmpxchg(&head->enqueue_pos, pos, pos + 1,
			                                     ARENA_RELAXED, ARENA_RELAXED);
			
			if (old_pos == pos) {
				/* Success! We own this slot. Write data. */
				cell->data.key = key;
				cell->data.value = value;
				
				/* Release to consumer: sequence = pos + 1 */
				// arena_atomic_exchange(&cell->sequence, pos + 1, ARENA_RELEASE);
				smp_store_release(&cell->sequence, pos + 1);
				
				/* Update approximate count (relaxed: just statistics) */
				arena_atomic_inc(&head->count);
				
				return DS_SUCCESS;
			}
			/* CAS failed, another producer claimed it. Retry. */
		}
		else if (dif < 0) {
			/* Sequence < pos: Queue is full */
			return DS_ERROR_NOMEM;
		}
		/* else: dif > 0, rare race condition, reload and retry */
		
		/* Reload position and try again */
		pos = READ_ONCE(head->enqueue_pos);
	}
	
	/* Max retries exceeded */
	return DS_ERROR_BUSY;
}

/**
 * ds_vyukhov_delete - Dequeue an element (delete/pop)
 * @head: Queue head
 * @data: Output parameter for dequeued key-value pair
 * 
 * Consumers compete to claim data via CAS on dequeue_pos.
 * Data is available when cell->sequence == pos + 1.
 * After reading, sets sequence to pos + mask + 1 for next lap.
 * 
 * Returns: DS_SUCCESS on success
 *          DS_ERROR_INVALID if head or data is NULL
 *          DS_ERROR_NOT_FOUND if queue is empty
 *          DS_ERROR_BUSY if max retries exceeded
 */
static inline int ds_vyukhov_delete(struct ds_vyukhov_head __arena *head, struct ds_kv *data)
{
	struct ds_vyukhov_node __arena *cell;
	__u64 pos;
	__u64 mask;
	int retries = 0;
	
	if (!head || !head->buffer || !data)
		return DS_ERROR_INVALID;
	
	pos = READ_ONCE(head->dequeue_pos);
	mask = head->buffer_mask;
	
	/* Retry loop with verifier-safe bound */
	for (; retries < DS_VYUKHOV_MAX_RETRIES && can_loop; retries++) {
		cell = &head->buffer[pos & mask];
		cast_kern(cell);
		
		// __u64 seq = READ_ONCE(cell->sequence); // TODO: Should be an acquire? 
		__u64 seq = smp_load_acquire(&cell->sequence);
		__s64 dif = (__s64)seq - (__s64)(pos + 1);
		
		if (dif == 0) {
			/* Cell has data. Try to claim it. */
			__u64 old_pos = arena_atomic_cmpxchg(&head->dequeue_pos, pos, pos + 1,
			                                     ARENA_RELAXED, ARENA_RELAXED);
			
			if (old_pos == pos) {
				/* Success! We own this data. Read and return it. */
				cast_kern(cell);
				data->key = cell->data.key;
				data->value = cell->data.value;
				
				/* Release to producer: sequence = pos + mask + 1 (next lap) */
				// arena_atomic_exchange(&cell->sequence, pos + mask + 1, ARENA_RELEASE);
				smp_store_release(&cell->sequence, pos + mask + 1);
				
				/* Update approximate count (relaxed: just statistics) */
				arena_atomic_dec(&head->count);
				
				return DS_SUCCESS;
			}
			/* CAS failed, another consumer claimed it. Retry. */
		}
		else if (dif < 0) {
			/* Sequence < pos + 1: Queue is empty */
			return DS_ERROR_NOT_FOUND;
		}
		/* else: dif > 0, rare race condition, reload and retry */
		
		/* Reload position and try again */
		pos = READ_ONCE(head->dequeue_pos);
	}
	
	/* Max retries exceeded */
	return DS_ERROR_BUSY;
}

/**
 * ds_vyukhov_pop - Pop an element from the queue (wrapper for delete)
 * @head: Queue head
 * @data: Output parameter for dequeued key-value pair
 * 
 * This is a convenience wrapper around ds_vyukhov_delete() that provides
 * more intuitive naming for FIFO pop operations.
 * 
 * Returns: DS_SUCCESS on success, error code otherwise
 */
static inline int ds_vyukhov_pop(struct ds_vyukhov_head __arena *head, struct ds_kv *data)
{
	return ds_vyukhov_delete(head, data);
}

/**
 * ds_vyukhov_search - Search for a key in the queue
 * @head: Queue head
 * @key: Key to search for
 * 
 * Note: Queues don't support efficient random access.
 * This performs a linear snapshot scan which may miss concurrent insertions/deletions.
 * The scan is not atomic with respect to queue modifications.
 * 
 * Returns: DS_SUCCESS if found, DS_ERROR_NOT_FOUND otherwise
 */
static inline int ds_vyukhov_search(struct ds_vyukhov_head __arena *head, __u64 key)
{
	__u64 start, end, mask;
	
	if (!head || !head->buffer)
		return DS_ERROR_INVALID;
	
	/* Take snapshot of queue bounds */
	start = READ_ONCE(head->dequeue_pos);
	end = READ_ONCE(head->enqueue_pos);
	mask = head->buffer_mask;
	
	/* Scan from dequeue to enqueue position */
	for (__u64 i = start; i < end && can_loop; i++) {
		struct ds_vyukhov_node __arena *cell = &head->buffer[i & mask];
		cast_kern(cell);
		
		if (cell->data.key == key)
			return DS_SUCCESS;
	}
	
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_vyukhov_verify - Verify queue integrity
 * @head: Queue head
 * 
 * Checks:
 * - Head pointer is valid
 * - Buffer pointer is valid
 * - dequeue_pos <= enqueue_pos (basic sanity)
 * - Approximate count is reasonable
 * 
 * Returns: DS_SUCCESS if valid, DS_ERROR_CORRUPT otherwise
 */
static inline int ds_vyukhov_verify(struct ds_vyukhov_head __arena *head)
{
	if (!head)
		return DS_ERROR_INVALID;
	
	cast_kern(head);
	
	/* Check buffer allocation */
	if (!head->buffer)
		return DS_ERROR_CORRUPT;
	
	/* Check basic invariant: dequeue <= enqueue */
	__u64 enq = READ_ONCE(head->enqueue_pos);
	__u64 deq = READ_ONCE(head->dequeue_pos);
	
	if (deq > enq)
		return DS_ERROR_CORRUPT;
	
	/* Check that current size doesn't exceed capacity */
	__u64 size = enq - deq;
	__u64 capacity = head->buffer_mask + 1;
	
	if (size > capacity)
		return DS_ERROR_CORRUPT;
	
	/* Approximate count check (may be slightly off due to races) */
	if (head->count > capacity)
		return DS_ERROR_CORRUPT;
	
	return DS_SUCCESS;
}

/**
 * ds_vyukhov_get_metadata - Get data structure metadata
 * 
 * Returns: Pointer to metadata structure
 */
static inline const struct ds_metadata* ds_vyukhov_get_metadata(void)
{
	static const struct ds_metadata metadata = {
		.name = "vyukhov",
		.description = "Bounded MPMC Queue (Vyukhov 1024cores)",
		.node_size = sizeof(struct ds_vyukhov_node),
		.requires_locking = 0,  /* Lock-free */
	};
	
	return &metadata;
}

/* ========================================================================
 * ITERATION HELPER
 * ======================================================================== */

/**
 * ds_vyukhov_iterate - Iterate over queue elements
 * @head: Queue head
 * @fn: Callback function to call for each element
 * @ctx: Context to pass to callback
 * 
 * Iterates from dequeue_pos to enqueue_pos (snapshot).
 * Note: Not atomic - queue may change during iteration.
 * 
 * Returns: Number of elements visited
 */
typedef int (*ds_vyukhov_iter_fn)(__u64 key, __u64 value, void *ctx);

static inline __u64 ds_vyukhov_iterate(struct ds_vyukhov_head __arena *head,
                                        ds_vyukhov_iter_fn fn,
                                        void *ctx)
{
	__u64 count = 0;
	
	if (!head || !fn || !head->buffer)
		return 0;
	
	/* Take snapshot of queue bounds */
	__u64 start = READ_ONCE(head->dequeue_pos);
	__u64 end = READ_ONCE(head->enqueue_pos);
	__u64 mask = head->buffer_mask;
	
	/* Iterate through valid range */
	for (__u64 i = start; i < end && can_loop; i++) {
		struct ds_vyukhov_node __arena *cell = &head->buffer[i & mask];
		cast_kern(cell);
		
		int ret = fn(cell->data.key, cell->data.value, ctx);
		if (ret != 0)
			break;
		
		count++;
	}
	
	return count;
}

#endif /* DS_VYUKHOV_H */
