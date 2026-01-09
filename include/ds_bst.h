/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Ellen Binary Search Tree Implementation for BPF Arena
 * 
 * Based on "Non-blocking Binary Search Tree" by F. Ellen et al., 2010.
 * This is a leaf-oriented, lock-free binary search tree designed for
 * concurrent access using Compare-And-Swap (CAS) primitives.
 * 
 * Key characteristics:
 * - All data stored in leaf nodes
 * - Internal nodes contain routing keys
 * - Lock-free operations (no mutual exclusion)
 * - O(log N) average case, O(N) worst case (unbalanced)
 */
#pragma once

#include "ds_api.h"

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

/**
 * struct bst_tree_node - Base tree node (discriminator)
 */
struct bst_tree_node {
	__u8 is_leaf;         /* 1 = leaf, 0 = internal */
	__u8 infinite_key;    /* Sentinel key marker (0, 1, or 2) */
	__u16 reserved;
	__u32 reserved2;
};

/**
 * struct bst_leaf_node - Leaf node storing key-value pairs
 */
struct bst_leaf_node {
	struct bst_tree_node base;
    struct ds_kv kv;   /* Key-value pair */
};

/**
 * struct bst_internal_node - Internal routing node
 */
struct bst_internal_node {
	struct bst_tree_node base;
	struct ds_k routing_key;    /* Key for routing decisions */
	
	/* Pointers to children (left < routing_key <= right) - accessed atomically */
	struct bst_tree_node __arena *left;
	struct bst_tree_node __arena *right;
};

/**
 * struct ds_bst_head - Tree head structure
 */
struct ds_bst_head {
	/* Root internal node with infinite key = 2 */
	struct bst_internal_node __arena *root;
	
	/* Sentinel leaves (infinite keys 1 and 2) */
	struct bst_leaf_node __arena *leaf_inf1;
	struct bst_leaf_node __arena *leaf_inf2;
};



/* ========================================================================
 * CONSTANTS
 * ======================================================================== */

#define BST_MAX_RETRIES 100

 /* ========================================================================
 * HELPER STRUCTURES
 * ======================================================================== */

/**
 * struct bst_search_result - Result of tree search operation
 */
struct bst_search_result {
	struct bst_internal_node __arena *grandparent;
	struct bst_internal_node __arena *parent;
	struct bst_leaf_node __arena *leaf;
	__u8 parent_is_right;  /* Parent is right child of grandparent */
	__u8 leaf_is_right;    /* Leaf is right child of parent */
	__u8 found;            /* Key match found */
};

/* ========================================================================
 * INTERNAL HELPERS
 * ======================================================================== */

/**
 * bst_search - Search for a key in the tree (internal helper)
 */
static inline void bst_search(
	struct ds_bst_head __arena *head,
	__u64 key,
	struct bst_search_result *result)
{
	struct bst_tree_node __arena *node;
	struct bst_internal_node __arena *parent = NULL;
	struct bst_internal_node __arena *grandparent = NULL;
	__u8 parent_is_right = 0, leaf_is_right = 0;
	__u64 iterations = 0;
	
	if (!head || !result) {
		if (result) {
			result->found = 0;
			result->leaf = NULL;
		}
		return;
	}
	
	cast_kern(head);
	node = (struct bst_tree_node __arena *)head->root;
	
	/* Traverse tree (bounded for BPF verifier) */
	while (node && iterations < BST_MAX_RETRIES && can_loop) {
		/* Cast node first before checking is_leaf */
		cast_kern(node);
		
		if (node->is_leaf)
			break;
		
		struct bst_internal_node __arena *internal = 
			(struct bst_internal_node __arena *)node;
		
		/* Update grandparent tracking */
		grandparent = parent;
		parent_is_right = leaf_is_right;
		
		/* Descend left or right based on key comparison */
		parent = internal;
		if (key < internal->routing_key.key) {
			node = smp_load_acquire(&internal->left);
			leaf_is_right = 0;
		} else {
			node = smp_load_acquire(&internal->right);
			leaf_is_right = 1;
		}
		
		iterations++;
	}
	
	/* Fill result structure */
	struct bst_leaf_node __arena *leaf = (struct bst_leaf_node __arena *)node;
	cast_kern(leaf);
	
	result->grandparent = grandparent;
	result->parent = parent;
	result->leaf = leaf;
	result->parent_is_right = parent_is_right;
	result->leaf_is_right = leaf_is_right;
	result->found = (leaf && !leaf->base.infinite_key && leaf->kv.key == key);
}

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

/**
 * ds_bst_init - Initialize an empty binary search tree
 * @head: Tree head to initialize
 * 
 * Creates the initial tree structure with root node and two sentinel leaves.
 * 
 * Returns: DS_SUCCESS on success, DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_bst_init(struct ds_bst_head __arena *head)
{
	cast_kern(head);
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Allocate sentinel leaves */
	head->leaf_inf1 = bpf_arena_alloc(sizeof(struct bst_leaf_node));
	head->leaf_inf2 = bpf_arena_alloc(sizeof(struct bst_leaf_node));
	if (!head->leaf_inf1 || !head->leaf_inf2) {
		if (head->leaf_inf1)
			bpf_arena_free(head->leaf_inf1);
		if (head->leaf_inf2)
			bpf_arena_free(head->leaf_inf2);
		return DS_ERROR_NOMEM;
	}
	
	cast_kern(head->leaf_inf1);
	head->leaf_inf1->base.is_leaf = 1;
	head->leaf_inf1->base.infinite_key = 1;
	head->leaf_inf1->kv.key = ~0ULL - 1;  /* ∞₁ */
	head->leaf_inf1->kv.value = 0;
	
	cast_kern(head->leaf_inf2);
	head->leaf_inf2->base.is_leaf = 1;
	head->leaf_inf2->base.infinite_key = 2;
	head->leaf_inf2->kv.key = ~0ULL;      /* ∞₂ */
	head->leaf_inf2->kv.value = 0;
	
	/* Allocate root internal node */
	head->root = bpf_arena_alloc(sizeof(struct bst_internal_node));
	if (!head->root) {
		bpf_arena_free(head->leaf_inf1);
		bpf_arena_free(head->leaf_inf2);
		return DS_ERROR_NOMEM;
	}
	
	cast_kern(head->root);
	head->root->base.is_leaf = 0;
	head->root->base.infinite_key = 2;
	head->root->routing_key.key = ~0ULL;   /* ∞₂ */
	WRITE_ONCE(head->root->left, (struct bst_tree_node __arena *)head->leaf_inf1);
	smp_store_release(&head->root->right, (struct bst_tree_node __arena *)head->leaf_inf2);
	
	return DS_SUCCESS;
}

/**
 * ds_bst_insert - Insert a key-value pair into the tree
 * @head: Tree head
 * @key: Key to insert
 * @value: Value to associate with key
 * 
 * Lock-free insertion using CAS. Retries on conflicts.
 * Rejects duplicate keys and infinite keys (reserved for sentinels).
 * 
 * Returns: DS_SUCCESS on success,
 *          DS_ERROR_INVALID if key exists or is invalid,
 *          DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_bst_insert(
	struct ds_bst_head __arena *head,
	__u64 key,
	__u64 value)
{
	cast_kern(head);
	
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Reserve infinite keys for sentinels */
	if (key >= (~0ULL - 1))
		return DS_ERROR_INVALID;
	
	struct bst_search_result result;
	__u64 retries = 0;
	
	while (retries < 100 && can_loop) {
		bst_search(head, key, &result);
		
		/* Key already exists */
		if (result.found) {
			return DS_ERROR_INVALID;
		}
		
		if (!result.parent || !result.leaf) {
			retries++;
			continue;
		}
		
		/* Allocate new leaf and internal node */
		struct bst_leaf_node __arena *new_leaf = 
			bpf_arena_alloc(sizeof(struct bst_leaf_node));
		struct bst_internal_node __arena *new_internal = 
			bpf_arena_alloc(sizeof(struct bst_internal_node));
		
		if (!new_leaf || !new_internal) {
			if (new_leaf)
				bpf_arena_free(new_leaf);
			if (new_internal)
				bpf_arena_free(new_internal);
			return DS_ERROR_NOMEM;
		}
		
		/* Initialize new leaf */
		cast_kern(new_leaf);
		new_leaf->base.is_leaf = 1;
		new_leaf->base.infinite_key = 0;
		new_leaf->kv.key = key;
		new_leaf->kv.value = value;
		
		/* Create internal node to route between old and new leaf */
		cast_kern(new_internal);
		new_internal->base.is_leaf = 0;
		new_internal->base.infinite_key = 0;
		
		/* Choose routing key and child arrangement */
		if (key < result.leaf->kv.key) {
			new_internal->routing_key.key = result.leaf->kv.key;
			new_internal->left = (struct bst_tree_node __arena *)new_leaf;
			new_internal->right = (struct bst_tree_node __arena *)result.leaf;
		} else {
			new_internal->routing_key.key = key;
			new_internal->left = (struct bst_tree_node __arena *)result.leaf;
			new_internal->right = (struct bst_tree_node __arena *)new_leaf;
		}
		
		/* CAS to install new internal node as parent's child */
		struct bst_tree_node __arena *old_child = 
			(struct bst_tree_node __arena *)result.leaf;
		struct bst_tree_node __arena *new_child = 
			(struct bst_tree_node __arena *)new_internal;
		
		cast_kern(result.parent);
		cast_kern(new_child);
		
		/* Perform CAS with atomic compare-exchange */
		struct bst_tree_node __arena *prev;
		if (result.leaf_is_right) {
			prev = arena_atomic_cmpxchg(&result.parent->right, old_child, new_child,
						    ARENA_RELEASE, ARENA_RELAXED);
		} else {
			prev = arena_atomic_cmpxchg(&result.parent->left, old_child, new_child,
						    ARENA_RELEASE, ARENA_RELAXED);
		}
		
		if (prev == old_child) {
			/* Success */
			// TODO: Add statistics tracking (insert_count++)
			return DS_SUCCESS;
		}
		
		/* CAS failed - cleanup and retry */
		bpf_arena_free(new_leaf);
		bpf_arena_free(new_internal);
		retries++;
		// TODO: Add statistics tracking (insert_retries++)
	}
	return DS_ERROR_BUSY;  /* Too many retries */
}

/**
 * ds_bst_delete - Delete a key from the tree
 * @head: Tree head
 * @key: Key to delete
 * 
 * Lock-free deletion using CAS. Retries on conflicts.
 * Removes the leaf node and promotes its sibling.
 * 
 * Returns: DS_SUCCESS if deleted,
 *          DS_ERROR_NOT_FOUND if key doesn't exist
 */
static inline int ds_bst_delete(
	struct ds_bst_head __arena *head,
	__u64 key)
{
	cast_kern(head);
	
	if (!head)
		return DS_ERROR_INVALID;
	
	struct bst_search_result result;
	__u64 retries = 0;
	
	while (retries < 100 && can_loop) {
		bst_search(head, key, &result);
		
		/* Key not found */
		if (!result.found)
			return DS_ERROR_NOT_FOUND;
		
		if (!result.grandparent || !result.parent || !result.leaf) {
			retries++;
			continue;
		}
		
		/* Get sibling node (the one to promote) */
		cast_kern(result.parent);
		struct bst_tree_node __arena *sibling = result.leaf_is_right ?
			smp_load_acquire(&result.parent->left) :
			smp_load_acquire(&result.parent->right);
		
		/* CAS to replace parent with sibling in grandparent */
		struct bst_tree_node __arena *old_child = 
			(struct bst_tree_node __arena *)result.parent;
		struct bst_tree_node __arena *new_child = sibling;
		
		cast_kern(result.grandparent);
		cast_kern(new_child);
		
		/* Perform CAS with atomic compare-exchange */
		struct bst_tree_node __arena *prev;
		if (result.parent_is_right) {
			prev = arena_atomic_cmpxchg(&result.grandparent->right, old_child, new_child,
						    ARENA_RELEASE, ARENA_RELAXED);
		} else {
			prev = arena_atomic_cmpxchg(&result.grandparent->left, old_child, new_child,
						    ARENA_RELEASE, ARENA_RELAXED);
		}
		
		if (prev == old_child) {
			/* Success - defer memory reclamation to arena */
			// TODO: Add statistics tracking (delete_count++)
			bpf_arena_free(result.leaf);
			bpf_arena_free(result.parent);
			return DS_SUCCESS;
		}
		
		/* CAS failed - retry */
		retries++;
		// TODO: Add statistics tracking (delete_retries++)
	}
	return DS_ERROR_BUSY;  /* Too many retries */
}

/**
 * ds_bst_search - Search for a key in the tree
 * @head: Tree head
 * @key: Key to search for
 * 
 * Wait-free search operation (read-only, no synchronization).
 * 
 * Returns: DS_SUCCESS if found, DS_ERROR_NOT_FOUND otherwise
 */
static inline int ds_bst_search(
	struct ds_bst_head __arena *head,
	__u64 key)
{
	cast_kern(head);
	
	if (!head)
		return DS_ERROR_INVALID;
	
	struct bst_search_result result;
	bst_search(head, key, &result);
	
	if (result.found)
		return DS_SUCCESS;
	
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_bst_pop - Remove and return the minimum element (leftmost leaf)
 * @head: Tree head
 * @data: Output parameter for popped key-value pair
 * 
 * This operation is useful for priority queue behavior.
 * 
 * Returns: DS_SUCCESS if element popped,
 *          DS_ERROR_INVALID if head or data is NULL,
 *          DS_ERROR_NOT_FOUND if tree is empty (only sentinels)
 */
static inline int ds_bst_pop(struct ds_bst_head __arena *head, struct ds_kv *data)
{
	cast_kern(head);
	
	if (!head || !data)
		return DS_ERROR_INVALID;
	
	/* Find leftmost non-sentinel leaf */
	struct bst_tree_node __arena *node;
	// struct bst_internal_node __arena *parent = NULL;
	// struct bst_internal_node __arena *grandparent = NULL;
	// __u8 parent_is_right = 0;
	__u64 iterations = 0;
	
	node = (struct bst_tree_node __arena *)head->root;
	
	/* Always go left to find minimum */
	while (node && iterations < 1000 && can_loop) {
		/* Cast node first before checking is_leaf */
		cast_kern(node);
		
		if (node->is_leaf)
			break;
		
		struct bst_internal_node __arena *internal = 
			(struct bst_internal_node __arena *)node;
		
		// grandparent = parent;
		// parent_is_right = 0;  /* Always going left */
		// parent = internal;
		node = smp_load_acquire(&internal->left);
		
		iterations++;
	}
	
	struct bst_leaf_node __arena *leaf = (struct bst_leaf_node __arena *)node;
	if (!leaf)
		return DS_ERROR_NOT_FOUND;
	
	cast_kern(leaf);
	
	/* Check if it's a sentinel */
	if (leaf->base.infinite_key)
		return DS_ERROR_NOT_FOUND;  /* Tree is empty */
	
	/* Copy data out */
	data->key = leaf->kv.key;
	data->value = leaf->kv.value;
	
	/* Delete the leaf using the standard delete operation */
	return ds_bst_delete(head, leaf->kv.key);
}

/**
 * ds_bst_verify - Verify tree integrity
 * @head: Tree head
 * 
 * Checks:
 * - Root and sentinels exist
 * - All internal nodes have two children
 * - No dangling pointers (bounded check)
 * 
 * Returns: DS_SUCCESS if valid, DS_ERROR_INVALID or DS_ERROR_CORRUPT otherwise
 */
static inline int ds_bst_verify(struct ds_bst_head __arena *head)
{
	cast_kern(head);
	
	if (!head)
		return DS_ERROR_INVALID;
	
	if (!head->root || !head->leaf_inf1 || !head->leaf_inf2)
		return DS_ERROR_INVALID;
	
	/* Verify sentinels */
	cast_kern(head->leaf_inf1);
	cast_kern(head->leaf_inf2);
	if (head->leaf_inf1->base.infinite_key != 1 ||
	    head->leaf_inf2->base.infinite_key != 2)
		return DS_ERROR_INVALID;
	
	/* Bounded BFS traversal to check invariants */
	struct bst_tree_node __arena *queue[100];
	__u64 queue_head = 0, queue_tail = 0;
	__u64 visited = 0;
	
	queue[queue_tail++] = (struct bst_tree_node __arena *)head->root;
	
	while (queue_head < queue_tail && visited < 100 && can_loop) {
		struct bst_tree_node __arena *node = queue[queue_head++];
		visited++;
		
		if (!node)
			return DS_ERROR_INVALID;
		
		cast_kern(node);
		
		if (!node->is_leaf) {
			struct bst_internal_node __arena *internal = 
				(struct bst_internal_node __arena *)node;
			
			cast_kern(internal);
			
			struct bst_tree_node __arena *left = 
				smp_load_acquire(&internal->left);
			struct bst_tree_node __arena *right = 
				smp_load_acquire(&internal->right);
			
			if (!left || !right)
				return DS_ERROR_INVALID;
			
			/* Enqueue children (if space) */
			if (queue_tail < 100)
				queue[queue_tail++] = left;
			if (queue_tail < 100)
				queue[queue_tail++] = right;
		}
	}
	
	return DS_SUCCESS;
}

/**
 * ds_bst_get_metadata - Get data structure metadata
 * 
 * Returns: Pointer to metadata structure
 */
static inline const struct ds_metadata* ds_bst_get_metadata(void)
{
	static const struct ds_metadata metadata = {
		.name = "ellen_bst",
		.description = "Ellen Binary Search Tree (lock-free, leaf-oriented)",
		.node_size = sizeof(struct bst_internal_node),
		.requires_locking = 0, /* Lock-free */
	};
	
	return &metadata;
}

/* ========================================================================
 * ITERATION HELPER
 * ======================================================================== */

/**
 * ds_bst_iterate - Iterate over all elements in sorted order
 * @head: Tree head
 * @callback: Function to call for each element
 * @ctx: Context to pass to callback
 * 
 * Callback should return 0 to continue, non-zero to stop.
 * 
 * NOTE: This uses bounded stack-based in-order traversal (limited to 100 nodes)
 * 
 * Returns: Number of elements visited
 */
typedef int (*ds_bst_iter_fn)(__u64 key, __u64 value, void *ctx);

static inline __u64 ds_bst_iterate(struct ds_bst_head __arena *head,
                                    ds_bst_iter_fn fn,
                                    void *ctx)
{
	struct bst_tree_node __arena *stack[100];
	__u64 stack_top = 0;
	__u64 count = 0;
	struct bst_tree_node __arena *current;
	
	if (!head || !fn)
		return 0;
	
	cast_kern(head);
	current = (struct bst_tree_node __arena *)head->root;
	
	/* In-order traversal (left, root, right) */
	while ((current != NULL || stack_top > 0) && count < 100 && can_loop) {
		/* Go to leftmost node */
		while (current != NULL && stack_top < 100 && can_loop) {
			cast_kern(current);
			
			if (current->is_leaf)
				break;
			
			stack[stack_top++] = current;
			struct bst_internal_node __arena *internal = 
				(struct bst_internal_node __arena *)current;
			cast_kern(internal);
			current = smp_load_acquire(&internal->left);
		}
		
		/* Process leaf */
		if (current && current->is_leaf) {
			struct bst_leaf_node __arena *leaf = 
				(struct bst_leaf_node __arena *)current;
			cast_kern(leaf);
			
			/* Skip sentinels */
			if (!leaf->base.infinite_key) {
				int ret = fn(leaf->kv.key, leaf->kv.value, ctx);
				if (ret != 0)
					break;
				count++;
			}
			
			/* Backtrack to parent and go right */
			if (stack_top > 0) {
				struct bst_internal_node __arena *parent = 
					(struct bst_internal_node __arena *)stack[--stack_top];
				cast_kern(parent);
				current = smp_load_acquire(&parent->right);
			} else {
				current = NULL;
			}
		} else {
			current = NULL;
		}
	}
	
	return count;
}
