/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Folly SPSC Queue Implementation for BPF Arena
 * 
 * This implements a Single-Producer Single-Consumer (SPSC) lock-free queue
 * based on Folly's ProducerConsumerQueue design. It uses a fixed-size ring
 * buffer with atomic indices and explicit memory ordering for high-throughput
 * telemetry between kernel (producer) and userspace (consumer).
 * 
 * Algorithm Characteristics:
 * - Lock-Free: Uses arena_atomic primitives with acquire/release ordering
 * - Wait-Free: Operations complete in bounded steps
 * - Cache-Friendly: Read/write indices on separate cache lines
 * - Fixed Capacity: Ring buffer with (size - 1) usable slots
 * 
 * Memory Ordering:
 * - Producer: writes data → RELEASE write_idx
 * - Consumer: ACQUIRE write_idx → reads data
 * - Consumer: reads data → RELEASE read_idx  
 * - Producer: ACQUIRE read_idx → checks space
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

#define CACHE_LINE_SIZE 64

/* Ring buffer uses ds_kv directly for elements */

/**
 * struct ds_spsc_queue_head - SPSC queue control structure
 * @write_idx: Producer's write position (aligned to prevent false sharing)
 * @read_idx: Consumer's read position (aligned to prevent false sharing)
 * @size: Total number of slots (capacity + 1, one slot always empty)
 * @records: Pointer to contiguous array of nodes in arena
 * 
 * INVARIANTS:
 * - size must be >= 2
 * - Usable capacity is (size - 1)
 * - 0 <= read_idx.idx < size
 * - 0 <= write_idx.idx < size
 * - Empty: read_idx == write_idx
 * - Full: (write_idx + 1) % size == read_idx
 */
struct ds_spsc_queue_head {
	/* Producer writes to write_idx, Consumer reads it */
	struct {
		__u32 idx;
	} write_idx __attribute__((aligned(CACHE_LINE_SIZE)));

	/* Consumer writes to read_idx, Producer reads it */
	struct {
		__u32 idx;
	} read_idx __attribute__((aligned(CACHE_LINE_SIZE)));

	__u32 size;             /* Total number of slots (capacity + 1) */
	struct ds_kv __arena *records; /* Pointer to contiguous array */
};

typedef struct ds_spsc_queue_head __arena ds_spsc_queue_head_t;

/* ========================================================================
 * API FUNCTIONS
 * ======================================================================== */

/**
 * ds_spsc_init - Initialize SPSC queue
 * @head: Queue head to initialize
 * @size: Total size of ring buffer (must be >= 2, usable capacity is size-1)
 * 
 * Allocates the fixed-size array and initializes indices to 0.
 * 
 * Returns: DS_SUCCESS or DS_ERROR_*
 */
static inline __attribute__((unused))
int ds_spsc_init(struct ds_spsc_queue_head __arena *head, __u32 size)
{
	struct ds_kv __arena *records;
	
	cast_kern(head);
	if (size < 2)
		return DS_ERROR_INVALID;

	/* Allocate contiguous array for records */
	records = bpf_arena_alloc(size * sizeof(struct ds_kv));
	if (!records)
		return DS_ERROR_NOMEM;
	
	cast_kern(records);
	/* records array is already zeroed by arena allocation */
	
	/* Initialize head structure */
	head->size = size;
	/* Use WRITE_ONCE for initialization, not arena_atomic_store */
	WRITE_ONCE(head->read_idx.idx, 0);
	WRITE_ONCE(head->write_idx.idx, 0);
	
	/* Assign records pointer (cast_user before assigning to arena field) */
	cast_user(records);
	head->records = records;
	
	return DS_SUCCESS;
}

/**
 * ds_spsc_insert - Insert element into queue (PRODUCER ONLY)
 * @head: Queue head
 * @key: Key to insert
 * @value: Value to insert
 * 
 * Producer writes data then updates write_idx with RELEASE semantics.
 * Must be called by only ONE producer thread.
 * 
 * Returns: DS_SUCCESS or DS_ERROR_FULL
 */
static inline __attribute__((unused))
int ds_spsc_insert(struct ds_spsc_queue_head __arena *head, __u64 key, __u64 value)
{
	cast_kern(head);
	
	/* Load write index (RELAXED: only this thread writes to it) */
	__u32 current_write = READ_ONCE(head->write_idx.idx);
	
	/* Calculate next write index */
	__u32 next_record = current_write + 1;
	if (next_record >= head->size) {
		next_record = 0;
	}

	/* Check against read index (ACQUIRE: synchronize with Consumer) */
	__u32 current_read = smp_load_acquire(&head->read_idx.idx);
	
	if (next_record != current_read) {
		/* Space available. Perform the write. */
		struct ds_kv __arena *node = &head->records[current_write];
		cast_kern(node);
		
		node->key = key;
		node->value = value;

		/* Commit the write (RELEASE: make payload visible before index update) */
		smp_store_release(&head->write_idx.idx, next_record);
		return DS_SUCCESS;
	}

	/* Queue is full */
	return DS_ERROR_FULL;
}

/**
 * ds_spsc_delete - Dequeue element from queue (CONSUMER ONLY)
 * @head: Queue head
 * @data: Output structure to receive key/value pair
 * 
 * Consumer reads data then updates read_idx with RELEASE semantics.
 * Must be called by only ONE consumer thread.
 * 
 * Note: This is a FIFO pop operation. The 'key' parameter in standard
 * ds_delete(key) is ignored - this always dequeues the front element.
 * 
 * Returns: DS_SUCCESS or DS_ERROR_NOT_FOUND (empty)
 */
static inline __attribute__((unused))
int ds_spsc_delete(struct ds_spsc_queue_head __arena *head, struct ds_kv *data)
{
	cast_kern(head);

	/* Load read index (RELAXED: only this thread writes to it) */
	__u32 current_read = READ_ONCE(head->read_idx.idx);
	
	/* Check against write index (ACQUIRE: synchronize with Producer) */
	__u32 current_write = smp_load_acquire(&head->write_idx.idx);

	if (current_read == current_write) {
		/* Queue is empty */
		return DS_ERROR_NOT_FOUND;
	}

	/* Retrieve data */
	struct ds_kv __arena *node = &head->records[current_read];
	cast_kern(node);
	if (data) {
		data->key = node->key;
		data->value = node->value;
	}

	/* Calculate next read index */
	__u32 next_record = current_read + 1;
	if (next_record >= head->size) {
		next_record = 0;
	}

	/* Commit the read (RELEASE: signal slot is free) */
	smp_store_release(&head->read_idx.idx, next_record);
	return DS_SUCCESS;
}

/**
 * ds_spsc_pop - Alias for ds_spsc_delete for API compatibility
 */
static inline __attribute__((unused))
int ds_spsc_pop(struct ds_spsc_queue_head __arena *head, struct ds_kv *data)
{
	return ds_spsc_delete(head, data);
}

/**
 * ds_spsc_search - Search for element with given key
 * @head: Queue head
 * @key: Key to search for
 * 
 * NOT IMPLEMENTED: Linear search would require iterating through the ring
 * buffer from read_idx to write_idx, which is rarely useful for SPSC queues.
 * 
 * Returns: DS_ERROR_INVALID (unsupported operation)
 */
static inline __attribute__((unused))
int ds_spsc_search(struct ds_spsc_queue_head __arena *head, __u64 key)
{
	/* Search is not practical for SPSC queue - it's a FIFO structure */
	(void)head;
	(void)key;
	return DS_ERROR_INVALID;
}

/**
 * ds_spsc_verify - Verify queue integrity
 * @head: Queue head
 * 
 * Checks that indices are within bounds and size calculation is consistent.
 * 
 * Returns: DS_SUCCESS or DS_ERROR_CORRUPT
 */
static inline __attribute__((unused))
int ds_spsc_verify(struct ds_spsc_queue_head __arena *head)
{
	cast_kern(head);
	
	__u32 r = READ_ONCE(head->read_idx.idx);
	__u32 w = READ_ONCE(head->write_idx.idx);
	__u32 s = head->size;

	/* Check indices are within bounds */
	if (r >= s || w >= s)
		return DS_ERROR_CORRUPT;
	
	/* Calculate current size (accounting for wrap-around) */
	int size_guess = (int)w - (int)r;
	if (size_guess < 0)
		size_guess += s;
	
	/* Size should never exceed capacity (size - 1) */
	if (size_guess > (int)(s - 1))
		return DS_ERROR_CORRUPT;
	
	return DS_SUCCESS;
}

/**
 * ds_spsc_size - Get current number of elements in queue
 * @head: Queue head
 * 
 * Returns: Number of elements currently in queue
 */
static inline __attribute__((unused))
__u32 ds_spsc_size(struct ds_spsc_queue_head __arena *head)
{
	cast_kern(head);
	
	__u32 r = READ_ONCE(head->read_idx.idx);
	__u32 w = READ_ONCE(head->write_idx.idx);
	__u32 s = head->size;
	
	int size = (int)w - (int)r;
	if (size < 0)
		size += s;
	
	return (__u32)size;
}

/**
 * ds_spsc_is_empty - Check if queue is empty
 * @head: Queue head
 * 
 * Returns: true if empty, false otherwise
 */
static inline __attribute__((unused))
bool ds_spsc_is_empty(struct ds_spsc_queue_head __arena *head)
{
	cast_kern(head);
	
	__u32 r = READ_ONCE(head->read_idx.idx);
	__u32 w = READ_ONCE(head->write_idx.idx);
	
	return r == w;
}

/**
 * ds_spsc_is_full - Check if queue is full
 * @head: Queue head
 * 
 * Returns: true if full, false otherwise
 */
static inline __attribute__((unused))
bool ds_spsc_is_full(struct ds_spsc_queue_head __arena *head)
{
	cast_kern(head);
	
	__u32 r = smp_load_acquire(&head->read_idx.idx);
	__u32 w = READ_ONCE(head->write_idx.idx);
	__u32 s = head->size;
	
	__u32 next_write = w + 1;
	if (next_write >= s)
		next_write = 0;
	
	return next_write == r;
}
