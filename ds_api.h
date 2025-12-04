/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Data Structure API Template for Concurrent Testing Framework */
#ifndef DS_API_H
#define DS_API_H

#pragma once

#include "libarena_ds.h"

/**
 * OVERVIEW
 * ========
 * This header defines the standard API that all data structures must implement
 * to work with the concurrent testing framework. Each data structure should
 * provide both BPF (kernel) and userspace implementations of these operations.
 * 
 * NAMING CONVENTIONS
 * ==================
 * - Data structure files: ds_<name>.h (e.g., ds_list.h, ds_tree.h)
 * - Operation functions: ds_<name>_<operation> (e.g., ds_list_insert)
 * - Types: ds_<name>_<type> (e.g., ds_list_head, ds_list_node)
 * 
 * REQUIRED OPERATIONS
 * ===================
 * Every data structure MUST implement these operations:
 * - init:   Initialize the data structure
 * - insert: Add an element
 * - delete: Remove an element
 * - search: Find an element
 * - verify: Check data structure integrity
 * - stats:  Return operation statistics
 */

/* ========================================================================
 * OPERATION RESULT CODES
 * ======================================================================== */

enum ds_result {
	DS_SUCCESS = 0,
	DS_ERROR_NOT_FOUND = -1,
	DS_ERROR_EXISTS = -2,
	DS_ERROR_NOMEM = -3,
	DS_ERROR_INVALID = -4,
	DS_ERROR_CORRUPT = -5,
};

/* ========================================================================
 * OPERATION TYPES (for statistics and dispatch)
 * ======================================================================== */

enum ds_op_type {
	DS_OP_INIT = 0,
	DS_OP_INSERT,
	DS_OP_DELETE,
	DS_OP_SEARCH,
	DS_OP_VERIFY,
	DS_OP_ITERATE,
	DS_OP_MAX
};

/* ========================================================================
 * DATA STRUCTURE METADATA
 * ======================================================================== */

struct ds_metadata {
	const char *name;              /* Data structure name */
	const char *description;       /* Brief description */
	__u32 node_size;              /* Size of a single node */
	__u32 requires_locking;       /* 1 if not lock-free */
};

/* ========================================================================
 * OPERATION STATISTICS
 * ======================================================================== */

struct ds_op_stats {
	__u64 count;                  /* Number of operations */
	__u64 failures;               /* Number of failed operations */
	__u64 total_time_ns;         /* Total time in nanoseconds */
};

struct ds_stats {
	struct ds_op_stats ops[DS_OP_MAX];
	__u64 current_elements;       /* Current number of elements */
	__u64 max_elements;           /* Maximum elements reached */
	__u64 memory_used;            /* Bytes of arena memory used */
};

/* ========================================================================
 * STANDARD API INTERFACE DEFINITION
 * ======================================================================== */

/**
 * DS_API_DECLARE - Declare data structure API
 * @name: Short name of the data structure (e.g., list, tree)
 * 
 * This macro declares the standard function prototypes that every
 * data structure implementation must provide.
 * 
 * Example usage in ds_list.h:
 *   DS_API_DECLARE(list)
 * 
 * This will generate declarations for:
 *   - ds_list_init()
 *   - ds_list_insert()
 *   - ds_list_delete()
 *   - etc.
 */
#define DS_API_DECLARE(name) \
	typedef struct ds_##name##_head ds_##name##_head_t; \
	typedef struct ds_##name##_node ds_##name##_node_t; \
	\
	static inline int ds_##name##_init(ds_##name##_head_t *head); \
	static inline int ds_##name##_insert(ds_##name##_head_t *head, __u64 key, __u64 value); \
	static inline int ds_##name##_delete(ds_##name##_head_t *head, __u64 key); \
	static inline int ds_##name##_search(ds_##name##_head_t *head, __u64 key); \
	static inline int ds_##name##_verify(ds_##name##_head_t *head); \
	static inline void ds_##name##_get_stats(ds_##name##_head_t *head, struct ds_stats *stats); \
	static inline void ds_##name##_reset_stats(ds_##name##_head_t *head); \
	static inline const struct ds_metadata* ds_##name##_get_metadata(void);
	
// static inline int ds_##name##_insert(ds_##name##_head_t *head, __u64 key, const struct ds_element *value); 
// static inline int ds_##name##_search(ds_##name##_head_t *head, __u64 key, struct ds_element *value); 

/**
 * DS_API_IMPL_INIT - Implement init operation
 * @name: Data structure name
 * @head_type: Type of the head structure
 * @code: Implementation code block
 * 
 * Example:
 *   DS_API_IMPL_INIT(list, struct ds_list_head, {
 *       head->first = NULL;
 *       head->count = 0;
 *       return DS_SUCCESS;
 *   })
 */
#define DS_API_IMPL_INIT(name, head_type, code) \
	static inline int ds_##name##_init(head_type *head) code

/**
 * DS_API_IMPL_INSERT - Implement insert operation
 * @name: Data structure name
 * @head_type: Type of the head structure
 * @code: Implementation code block
 */
#define DS_API_IMPL_INSERT(name, head_type, code) \
	static inline int ds_##name##_insert(head_type *head, __u64 key, __u64 value) code
	// static inline int ds_##name##_insert(head_type *head, __u64 key, const struct ds_element *value) code

/**
 * DS_API_IMPL_DELETE - Implement delete operation
 */
#define DS_API_IMPL_DELETE(name, head_type, code) \
	static inline int ds_##name##_delete(head_type *head, __u64 key) code

/**
 * DS_API_IMPL_SEARCH - Implement search operation
 */
#define DS_API_IMPL_SEARCH(name, head_type, code) \
	static inline int ds_##name##_search(head_type *head, __u64 key) code
	// static inline int ds_##name##_search(head_type *head, __u64 key, struct ds_element *value) code

/**
 * DS_API_IMPL_VERIFY - Implement verify operation
 */
#define DS_API_IMPL_VERIFY(name, head_type, code) \
	static inline int ds_##name##_verify(head_type *head) code

/* ========================================================================
 * OPERATION DISPATCH HELPER
 * ======================================================================== */

/**
 * struct ds_operation - Encapsulates a single operation for dispatch
 * 
 * Used to pass operations from userspace to kernel or between threads.
 */
struct ds_operation {
	enum ds_op_type type;
	__u64 key;             /* pid */
	__u64 value;           /* timestamp */

	int result;                  /* Operation result */
};

/* ========================================================================
 * USERSPACE-SPECIFIC HELPERS
 * ======================================================================== */

#ifndef __BPF__

#include <time.h>

/**
 * ds_get_timestamp - Get current timestamp in nanoseconds
 */
static inline __u64 ds_get_timestamp(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
}

/**
 * ds_print_stats - Print data structure statistics
 */
static inline void ds_print_stats(const char *name, struct ds_stats *stats)
{
	const char *op_names[] = {
		"INIT", "INSERT", "DELETE", "SEARCH", "VERIFY", "ITERATE"
	};
	
	printf("\n=== %s Statistics ===\n", name);
	printf("Elements: %llu (max: %llu)\n", stats->current_elements, stats->max_elements);
	printf("Memory: %llu bytes\n", stats->memory_used);
	printf("\nOperations:\n");
	
	for (int i = 0; i < DS_OP_MAX; i++) {
		if (stats->ops[i].count > 0) {
			__u64 avg_ns = stats->ops[i].total_time_ns / stats->ops[i].count;
			printf("  %-8s: %10llu ops, %8llu failures, %8llu ns avg\n",
				op_names[i],
				stats->ops[i].count,
				stats->ops[i].failures,
				avg_ns);
		}
	}
	printf("\n");
}

#endif /* !__BPF__ */

/* ========================================================================
 * ITERATION HELPERS
 * ======================================================================== */

/**
 * DS_FOR_EACH - Iterator macro template
 * 
 * Data structures should provide their own for_each macros following
 * this pattern, similar to list_for_each_entry.
 */

/* ========================================================================
 * VERIFICATION HELPERS
 * ======================================================================== */

/**
 * DS_VERIFY_CONDITION - Helper for verification code
 */
#define DS_VERIFY_CONDITION(cond, error) \
	do { if (!(cond)) return error; } while (0)

/* ========================================================================
 * EXAMPLE IMPLEMENTATION TEMPLATE
 * ======================================================================== */

/*
 * To implement a new data structure, follow this template:
 * 
 * 1. Create ds_<name>.h
 * 2. Include this header: #include "ds_api.h"
 * 3. Define your node and head structures:
 *    struct ds_<name>_node {
 *        // Your fields
 *        __u64 key;
 *        __u64 value;
 *    };
 *    struct ds_<name>_head {
 *        struct ds_<name>_node __arena *first;
 *        struct ds_stats stats;
 *        __u64 count;
 *    };
 * 
 * 4. Implement operations using the API macros:
 *    DS_API_IMPL_INIT(name, struct ds_<name>_head, {
 *        // Initialize head
 *        return DS_SUCCESS;
 *    })
 * 
 * 5. Provide metadata:
 *    static const struct ds_metadata <name>_metadata = {
 *        .name = "name",
 *        .description = "Brief description",
 *        .node_size = sizeof(struct ds_<name>_node),
 *        .requires_locking = 0,
 *    };
 * 
 * 6. In skeleton.bpf.c, add to the dispatch switch:
 *    #include "ds_<name>.h"
 *    case OP_<NAME>_INSERT:
 *        result = ds_<name>_insert(&ds_head, op.key, op.value);
 *        break;
 * 
 * 7. In skeleton.c, add userspace operations:
 *    void* userspace_<name>_worker(void *arg) {
 *        struct ds_<name>_head *head = (struct ds_<name>_head*)arg;
 *        // Perform operations on head
 *    }
 */

#endif /* DS_API_H */
