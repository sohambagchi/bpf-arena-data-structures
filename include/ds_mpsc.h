/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Vyukhov's MPSC Queue Implementation for BPF Arena
 * 
 * Based on Dmitry Vyukhov's intrusive MPSC node-based queue algorithm:
 * https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
 * 
 * Characteristics:
 * - Wait-free for producers (single XCHG instruction)
 * - Obstruction-free for consumer (may spin on stalled producer)
 * - Multiple producers, single consumer
 * - Unbounded (node-based, not ring buffer)
 * - Requires dummy/stub node
 * 
 * Design:
 * - Producers (BPF kernel): Multiple contexts can insert concurrently
 * - Consumer (Userspace): Single thread dequeues elements
 * - head: back of queue (where producers add)
 * - tail: front of queue (where consumer removes)
 * 
 * This implementation follows the ds_api.h template for the BPF Arena framework.
 */
#ifndef DS_MPSC_H
#define DS_MPSC_H

#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct ds_mpsc_node - Single node in the MPSC queue
 * @next: Pointer to next node
 * @data: User data (key-value pair)
 * 
 * Nodes are linked via the next pointer to form a singly-linked list.
 */
struct ds_mpsc_node {
	struct ds_mpsc_node __arena *next;
	struct ds_kv data;
};

/**
 * struct ds_mpsc_head - MPSC queue head structure
 * @head: Pointer to back of queue (producer target, atomically updated)
 * @tail: Pointer to front of queue (consumer target, single-writer)
 * @count: Current number of elements (approximate, for observability)
 * 
 * Invariants:
 * - head and tail are never NULL after initialization
 * - Empty queue: tail == head (both point to stub)
 * - Non-empty: tail->next != NULL
 */
struct ds_mpsc_head {
	/* Producer state (atomically updated) */
	struct ds_mpsc_node __arena *head;
	
	/* Consumer state (single-writer, no atomic needed) */
	struct ds_mpsc_node __arena *tail;
	
	/* Statistics (approximate) */
	__u64 count;
};

typedef struct ds_mpsc_head __arena ds_mpsc_head_t;
typedef struct ds_mpsc_node __arena ds_mpsc_node_t;

/* ========================================================================
 * HELPER MACROS
 * ======================================================================== */

/* Maximum retry attempts for operations (verifier safety) */
#define DS_MPSC_MAX_RETRIES 100

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

/**
 * ds_mpsc_init - Initialize the MPSC queue
 * @ctx: Queue head to initialize
 * 
 * Allocates a stub/dummy node and initializes the queue.
 * Both head and tail point to the stub initially.
 * 
 * Returns: DS_SUCCESS on success,
 *          DS_ERROR_INVALID if ctx is NULL,
 *          DS_ERROR_NOMEM if stub allocation fails
 */
static inline int ds_mpsc_init(struct ds_mpsc_head __arena *ctx)
{
	struct ds_mpsc_node __arena *stub;
	
	cast_kern(ctx);
	
	if (!ctx)
		return DS_ERROR_INVALID;
	
	/* Allocate the stub node */
	stub = bpf_arena_alloc(sizeof(*stub));
	if (!stub)
		return DS_ERROR_NOMEM;
	
	/* Cast is essential for BPF verifier to track arena pointer */
	cast_kern(stub);
	
	/* Initialize stub (data fields are zero, next is NULL) */
	WRITE_ONCE(stub->next, NULL);
	WRITE_ONCE(stub->data.key, 0);
	WRITE_ONCE(stub->data.value, 0);
	
	/* Both head and tail point to stub */
	ctx->head = stub;
	ctx->tail = stub;
	ctx->count = 0;
	
	return DS_SUCCESS;
}

/**
 * ds_mpsc_insert - Enqueue an element (wait-free for producers)
 * @ctx: Queue head
 * @key: Key to insert
 * @value: Value to insert
 * 
 * Wait-free producer operation using atomic XCHG.
 * Multiple producers can call this concurrently from BPF or userspace.
 * 
 * Algorithm:
 * 1. Allocate new node and initialize data
 * 2. Atomically swap head pointer (XCHG with RELEASE ordering)
 * 3. Link old head to new node (RELEASE store)
 * 
 * Memory ordering:
 * - RELEASE on XCHG: ensures node data is visible before publication
 * - RELEASE on link: ensures link is visible to consumer
 * 
 * Returns: DS_SUCCESS on success,
 *          DS_ERROR_INVALID if ctx is NULL,
 *          DS_ERROR_NOMEM if node allocation fails
 */
static inline int ds_mpsc_insert(struct ds_mpsc_head __arena *ctx, 
                                 __u64 key, __u64 value)
{
	struct ds_mpsc_node __arena *n;
	struct ds_mpsc_node __arena *prev;
	
	if (!ctx)
		return DS_ERROR_INVALID;
	
	cast_kern(ctx);
	
	/* 1. Allocate new node */
	n = bpf_arena_alloc(sizeof(*n));
	if (!n)
		return DS_ERROR_NOMEM;
	
	cast_kern(n);
	
	/* 2. Setup node (next=NULL means this will be the new terminator) */
	n->data.key = key;
	n->data.value = value;
	n->next = NULL;

	cast_user(n);	
	/* 3. Serialization point: Atomic Exchange
	 * RELEASE ordering ensures all writes to node are visible
	 * before the node is published to other threads.
	 * 'prev' becomes the node that was previously at the back.
	 */
	prev = arena_atomic_exchange(&ctx->head, n, ARENA_RELEASE);
	cast_kern(prev);
	
	/* 4. Link the old back to the new node
	 * Use smp_store_release to ensure the link is visible to the consumer.
	 * 
	 * CRITICAL WINDOW: If producer is preempted here, consumer sees:
	 * - tail != head (implies not empty)
	 * - BUT tail->next == NULL (link not yet visible)
	 * Consumer must handle this by returning DS_ERROR_BUSY.
	 */
	smp_store_release(&prev->next, n);
	
	/* Update statistics (RELAXED: only for observability) */
	arena_atomic_add(&ctx->count, 1, ARENA_RELAXED);
	
	return DS_SUCCESS;
}

/**
 * ds_mpsc_delete - Dequeue an element (consumer only, obstruction-free)
 * @ctx: Queue head
 * @output: Output parameter for dequeued key-value pair
 * 
 * Single-consumer dequeue operation. This is NOT safe for concurrent
 * consumers. In BPF context, this function returns DS_ERROR_INVALID
 * since BPF cannot handle the potential spin-wait on stalled producers.
 * 
 * Algorithm:
 * 1. Read tail and tail->next (ACQUIRE to see producer's link)
 * 2. If tail == head: queue is empty
 * 3. If tail->next == NULL but tail != head: producer is stalled (return BUSY)
 * 4. Otherwise: read data, advance tail, free old tail
 * 
 * Memory ordering:
 * - ACQUIRE on tail->next read: sees producer's link store
 * - No atomic needed for tail update: single consumer
 * 
 * Returns: DS_SUCCESS on success,
 *          DS_ERROR_INVALID if ctx or output is NULL, or called from BPF
 *          DS_ERROR_NOT_FOUND if queue is empty
 *          DS_ERROR_BUSY if producer is stalled (caller should retry)
 */
static inline int ds_mpsc_delete(struct ds_mpsc_head __arena *ctx, 
                                 struct ds_kv *output)
{
	struct ds_mpsc_node __arena *tail;
	struct ds_mpsc_node __arena *next;
	
	/* BPF cannot safely consume (no spin-wait allowed) */
#ifdef __BPF__
	return DS_ERROR_INVALID;
#endif
	
	if (!ctx || !output)
		return DS_ERROR_INVALID;
	
	cast_kern(ctx);
	tail = ctx->tail;
	cast_kern(tail);
	
	/* ACQUIRE load to see producer's link operation (prev->next = n) */
	next = smp_load_acquire(&tail->next);
	
	/* Case 1: Queue is logically empty */
	if (tail == ctx->head) {
		return DS_ERROR_NOT_FOUND;
	}
	
	/* Case 2: Producer stalled between XCHG and link
	 * Symptom: tail != head (not empty) but tail->next == NULL
	 * Resolution: Return BUSY so caller can retry
	 */
	if (next == NULL) {
		return DS_ERROR_BUSY;
	}
	
	/* Case 3: Valid dequeue
	 * The data is in 'next' (tail is the dummy/stub)
	 */
	cast_kern(next);
	
	/* Read the data */
	output->key = next->data.key;
	output->value = next->data.value;
	
	/* Move tail forward (only consumer modifies tail, no atomic needed) */
	ctx->tail = next;
	
	/* Free the old stub */
	bpf_arena_free(tail);
	
	/* Update statistics (RELAXED: only for observability) */
	arena_atomic_sub(&ctx->count, 1, ARENA_RELAXED);
	
	return DS_SUCCESS;
}

/**
 * ds_mpsc_pop - Pop an element from the queue (wrapper for delete)
 * @ctx: Queue head
 * @output: Output parameter for dequeued key-value pair
 * 
 * Convenience wrapper around ds_mpsc_delete() with retry logic for
 * the stalled producer case. Matches the pattern from ds_msqueue_pop.
 * 
 * Returns: 1 if element successfully dequeued (output is valid),
 *          0 if queue is empty (output is unchanged),
 *          negative error code for actual errors
 */
static inline int ds_mpsc_pop(struct ds_mpsc_head __arena *ctx, 
                              struct ds_kv *output)
{
	int result;
	int retries = 0;
	
	/* Retry loop for stalled producer case */
	while (retries < DS_MPSC_MAX_RETRIES && can_loop) {
		result = ds_mpsc_delete(ctx, output);
		
		if (result == DS_SUCCESS)
			return 1; /* Successfully dequeued */
		else if (result == DS_ERROR_NOT_FOUND)
			return 0; /* Empty queue - not an error */
		else if (result == DS_ERROR_BUSY) {
			/* Producer stalled, retry */
			retries++;
			continue;
		} else
			return result; /* Actual error */
	}
	
	/* Max retries exceeded on BUSY */
	return DS_ERROR_BUSY;
}

/**
 * ds_mpsc_search - Search for a key in the queue
 * @ctx: Queue head
 * @key: Key to search for
 * 
 * Note: Search is not a standard queue operation. This performs a
 * linear snapshot scan which may miss concurrent insertions/deletions.
 * Provided to satisfy the ds_api.h interface requirements.
 * 
 * The scan starts from tail->next (skipping the stub/dummy).
 * 
 * Returns: DS_SUCCESS if key is found,
 *          DS_ERROR_INVALID if ctx is NULL,
 *          DS_ERROR_NOT_FOUND if key not found
 */
static inline int ds_mpsc_search(struct ds_mpsc_head __arena *ctx, __u64 key)
{
	struct ds_mpsc_node __arena *curr;
	int count = 0;
	int max_iterations = 100000;
	
	if (!ctx)
		return DS_ERROR_INVALID;
	
	cast_kern(ctx);
	curr = ctx->tail;
	
	/* Traverse the queue */
	for (count = 0; count < max_iterations && can_loop; count++) {
		if (!curr)
			break;
		
		cast_kern(curr);
		
		/* Skip the dummy node (current tail) - data starts at tail->next */
		if (curr->data.key == key && curr != ctx->tail) {
			return DS_SUCCESS;
		}
		
		/* ACQUIRE load to safely follow the list */
		curr = smp_load_acquire(&curr->next);
	}
	
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_mpsc_verify - Verify queue structural integrity
 * @ctx: Queue head
 * 
 * Performs basic consistency checks:
 * - Verifies head and tail are not NULL
 * - Checks list is reachable from tail to head
 * - Detects cycles that would cause infinite loops
 * - Accepts transient stalled-producer state
 * 
 * Note: This operation is NOT thread-safe and should only be called when
 * no concurrent modifications are occurring (e.g., after quiescing).
 * 
 * Returns: DS_SUCCESS if queue structure is valid,
 *          DS_ERROR_INVALID if ctx is NULL,
 *          DS_ERROR_CORRUPT if structural corruption detected
 */
static inline int ds_mpsc_verify(struct ds_mpsc_head __arena *ctx)
{
	struct ds_mpsc_node __arena *curr;
	__u64 max_iter = 100000;
	__u64 i;
	
	if (!ctx)
		return DS_ERROR_INVALID;
	
	cast_kern(ctx);
	
	/* Head and tail must be non-NULL */
	if (!ctx->head || !ctx->tail)
		return DS_ERROR_CORRUPT;
	
	curr = ctx->tail;
	
	/* Traverse from tail to head */
	for (i = 0; i < max_iter && can_loop; i++) {
		if (!curr) {
			/* NULL before reaching head means broken list */
			if (curr != ctx->head)
				return DS_ERROR_CORRUPT;
			break;
		}
		
		cast_kern(curr);
		
		/* Reached the end successfully */
		if (curr == ctx->head) {
			return DS_SUCCESS;
		}
		
		/* ACQUIRE load to safely follow the list */
		curr = smp_load_acquire(&curr->next);
		
		/* Transient state: stalled producer
		 * tail != head but tail->next == NULL
		 * This is acceptable during concurrent operations
		 */
		if (curr == NULL && ctx->tail != ctx->head) {
			return DS_SUCCESS;
		}
	}
	
	/* Exceeded max iterations without reaching head - likely a cycle */
	return DS_ERROR_CORRUPT;
}

/**
 * ds_mpsc_stats - Get queue statistics
 * @ctx: Queue head
 * @stats: Output parameter for statistics
 * 
 * Returns: DS_SUCCESS on success,
 *          DS_ERROR_INVALID if ctx or stats is NULL
 */
static inline int ds_mpsc_stats(struct ds_mpsc_head __arena *ctx,
                                struct ds_stats *stats)
{
	if (!ctx || !stats)
		return DS_ERROR_INVALID;
	
	cast_kern(ctx);
	
	/* Read approximate count (RELAXED: only observability) */
	stats->current_elements = arena_atomic_load(&ctx->count, ARENA_RELAXED);
	
	/* Other stats not tracked in basic implementation */
	stats->max_elements = 0;
	stats->memory_used = 0;
	
	return DS_SUCCESS;
}

#endif /* DS_MPSC_H */
