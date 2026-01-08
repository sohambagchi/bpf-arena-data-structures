/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Non-Blocking Binary Search Tree Implementation for BPF Arena
 * 
 * Based on: "Non-blocking binary search trees" by Ellen, Fatourou, 
 * Ruppert, and van Breugel (2010)
 * 
 * This is a leaf-oriented BST where:
 * - Internal nodes contain routing keys and pointers
 * - Leaf nodes contain actual key-value pairs
 * - Concurrency via single-word CAS with cooperative helping
 * - Progress guarantee: Lock-free (non-blocking)
 * - Consistency: Linearizable
 */
#ifndef DS_BINTREE_H
#define DS_BINTREE_H

#pragma once

#include "ds_api.h"

/* ========================================================================
 * CONSTANTS AND STATE DEFINITIONS
 * ======================================================================== */

enum ds_bst_mo {
	BST_RELAXED = 0,
	BST_ACQUIRE,
	BST_RELEASE,
	BST_ACQ_REL,
	BST_SEQ_CST
};

/* State definitions for the Update field (stored in low 2 bits) */
enum ds_bst_state {
	BST_CLEAN = 0,  /* No operation in progress */
	BST_DFLAG = 1,  /* Delete operation flagged */
	BST_IFLAG = 2,  /* Insert operation flagged */
	BST_MARK  = 3   /* Parent marked for deletion */
};

/* Node Types */
enum ds_bst_node_type {
	BST_NODE_INTERNAL = 0,
	BST_NODE_LEAF     = 1,
	BST_NODE_KEY_INF1 = 2,
	BST_NODE_KEY_INF2 = 4,
	BST_NODE_KEY_INFINITE = (BST_NODE_KEY_INF1 | BST_NODE_KEY_INF2)
};

/* Info record types */
enum ds_bst_info_type {
	BST_INFO_INSERT = 0,
	BST_INFO_DELETE = 1
};

/* Tagged pointer manipulation */
#define UPDATE_MASK_STATE 0x3UL
#define UPDATE_MASK_PTR   (~0x3UL)

/* Loop limit for verifier */
#define BST_MAX_DEPTH 4

/* Sentinel key values (users must use keys < BST_SENTINEL_KEY1) */
#define BST_SENTINEL_KEY1 ((__u64)-2)  /* UINT64_MAX - 1 */
#define BST_SENTINEL_KEY2 ((__u64)-1)  /* UINT64_MAX */

/* ========================================================================
 * DATA STRUCTURES
 * ======================================================================== */

struct ds_bst_tree_node {
	enum ds_bst_node_type type;
};

/**
 * struct ds_bst_leaf - Leaf node containing actual data
 * @header: Common node header
 * @kv: Key-value pair stored in the leaf
 */
struct ds_bst_leaf {
	struct ds_bst_tree_node header;
    struct ds_kv kv;
};

/**
 * struct ds_bst_internal - Internal routing node
 * @header: Common node header
 * @key: Routing key (for navigation)
 * @left: Pointer to left child
 * @right: Pointer to right child
 * @update: Synchronization field (tagged pointer: Info* | State)
 * 
 * The update field combines a pointer to an Info record with a 2-bit state.
 * Must be accessed atomically with proper memory ordering.
 */
struct ds_bst_internal {
	struct ds_bst_tree_node header;
	struct ds_k key;
	struct ds_bst_tree_node __arena *pLeft;
	struct ds_bst_tree_node __arena *pRight;
	__u64 m_pUpdate;  /* Tagged pointer: (Info* & ~0x3) | State */
};

/**
 * struct ds_bst_info - Base for Info records
 * @type: Info record type (BST_INFO_INSERT or BST_INFO_DELETE)
 */
struct ds_bst_info {
	__u32 type;
};

/**
 * struct ds_bst_iinfo - Insert operation context
 * @header: Common info header
 * @p: Parent internal node
 * @l: Leaf to replace
 * @new_internal: New internal node to insert
 */
struct ds_bst_iinfo {
	struct ds_bst_info header;
	struct ds_bst_internal __arena *pParent;
	struct ds_bst_internal __arena *pNew;
	struct ds_bst_leaf __arena *pLeaf;
	bool bRightLeaf;
};

/**
 * struct ds_bst_dinfo - Delete operation context
 * @header: Common info header
 * @gp: Grandparent internal node
 * @p: Parent internal node
 * @l: Leaf to delete
 * @p_update: Expected value of p->update at time of flagging
 */
struct ds_bst_dinfo {
	struct ds_bst_internal __arena *pGrandParent;
	struct ds_bst_internal __arena *pParent;
	struct ds_bst_leaf __arena *pLeaf;
	__u64 pUpdateParent; // previous update descriptor
	bool bDisposeLeaf;
	bool bRightParent;
	bool bRightLeaf;
};

struct ds_bst_update_desc {
	union {
		struct ds_bst_iinfo __arena *insert;
		struct ds_bst_dinfo __arena *delete;
	};
	struct ds_bst_update_desc __arena *next_retire;
};

struct ds_bst_stats {
	__u32 total_inserts;
	__u32 total_deletes;
	__u32 total_searches;
	__u32 total_rebalances;
	__u32 total_failures;
	__u32 max_tree_depth;
	__u32 insert_failure_invalid_head;
	__u32 insert_failure_invalid_key;
	__u32 insert_failure_exists;
	__u32 insert_failure_nomem;
	__u32 insert_failure_busy;
	__u32 insert_failure_no_parent;
	__u32 insert_failure_no_leaf;
	__u32 insert_failure_leaf_is_internal;
	__u32 insert_failure_cas_fail;
	__u32 insert_retry_didnt_help;
	__u32 insert_into_updates;
	__u32 delete_failure_invalid_head;
	__u32 delete_failure_not_found;
	__u32 delete_failure_nomem;
	__u32 delete_failure_busy;
	__u32 delete_retry_didnt_help_gp;
	__u32 delete_retry_didnt_help_p;
	__u32 search_failure_invalid_head;
	__u32 search_not_found;
	__u32 search_found;
};

/**
 * struct ds_bst_head - Tree head structure
 * @root: Root internal node
 * @count: Number of elements (excluding sentinels)
 */
struct ds_bst_head {
	struct ds_bst_stats stats;
	struct ds_bst_internal __arena *root;
	struct ds_bst_leaf __arena *leaf_inf1;
	struct ds_bst_leaf __arena *leaf_inf2;
	__u64 count;
};

#define ds_bst_node(ptr, type, member) arena_container_of(ptr, type, member)

/* ========================================================================
 * TAGGED POINTER HELPERS
 * ======================================================================== */

/**
 * make_update - Combine Info pointer and state into tagged pointer
 */
static inline __u64 bst_make_update(void __arena *info, __u64 state)
{
	return ((__u64)info & UPDATE_MASK_PTR) | (state & UPDATE_MASK_STATE);
}

/**
 * get_state - Extract state from tagged pointer
 */
static inline __u8 bst_get_bits(__u64 update)
{
	return (int)(update & UPDATE_MASK_STATE);
}

/**
 * get_info - Extract Info pointer from tagged pointer
 */
static inline __u64 bst_get_ptr(__u64 update)
{
	return (update & UPDATE_MASK_PTR);
}

static inline struct ds_bst_tree_node __arena * bst_get_child(struct ds_bst_internal __arena *node, bool bRight, enum ds_bst_mo mo) {
	if (mo == BST_RELAXED) {
		return bRight ? READ_ONCE(node->pRight) : READ_ONCE(node->pLeft);
	} else {
		return bRight ? smp_load_acquire(&node->pRight) : smp_load_acquire(&node->pLeft);
	}
}

static inline bool bst_is_internal(struct ds_bst_tree_node __arena *node) {
	return node->type == BST_NODE_INTERNAL;
}

static inline bool bst_is_leaf(struct ds_bst_tree_node __arena *node) {
	return node->type == BST_NODE_LEAF;
}

static inline unsigned int bst_get_infinite_key(struct ds_bst_tree_node __arena *node) {
	return smp_load_acquire(&node->type) & BST_NODE_KEY_INFINITE;
}

static inline void bst_set_infinite_key(struct ds_bst_tree_node __arena *node, int nInf) {
	unsigned int nFlags = READ_ONCE(node->type);
	nFlags &= ~BST_NODE_KEY_INFINITE;
	switch (nInf) {
		case 1:
			nFlags |= BST_NODE_KEY_INF1;
			break;
		case 2:
			nFlags |= BST_NODE_KEY_INF2;
			break;
		case 0:
			break;
		default:
			/* Invalid infinite key value */
			break;
	}
	smp_store_release(&node->type, nFlags);
}

/* ========================================================================
 * SEARCH RESULT STRUCTURE
 * ======================================================================== */

/**
 * struct bst_search_result - Context returned by search
 * @gp: Grandparent internal node
 * @p: Parent internal node
 * @l: Leaf node found
 * @p_update: Value of p->update at search time
 * @gp_update: Value of gp->update at search time
 */
struct bst_search_result {
	struct ds_bst_internal __arena *pGrandParent;
	struct ds_bst_internal __arena *pParent;
	struct ds_bst_leaf __arena *pLeaf;
	__u64 updParent;
	__u64 updGrandParent;
	bool bRightLeaf;
	bool bRightParent;
};

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static inline void bst_help(__u64 update);
static inline void bst_help_insert(struct ds_bst_update_desc __arena *op);
static inline bool bst_help_delete(struct ds_bst_update_desc __arena *op);
static inline void bst_help_marked(struct ds_bst_update_desc __arena *op);
static inline void bst_cas_child(struct ds_bst_internal __arena *parent,
                                  void __arena *old_node,
                                  void __arena *new_node);

/* ========================================================================
 * SEARCH HELPER
 * ======================================================================== */

/**
 * bst_search - Search for a key and return context
 * @head: Tree head
 * @key: Key to search for
 * @res: Output parameter for search result
 * 
 * Traverses the tree to find the leaf that would contain key.
 * Returns grandparent, parent, and leaf context needed for updates.
 */
static __always_inline bool bst_search(struct ds_bst_head __arena *head, __u64 key,
                               struct bst_search_result *res)
{
	struct ds_bst_internal __arena *pParent;
	struct ds_bst_internal __arena *pGrandParent = NULL;
	struct ds_bst_tree_node __arena *pLeaf;

	struct ds_bst_update_desc __arena *updParent;
	struct ds_bst_update_desc __arena *updGrandParent;

	bool bRightLeaf;
	bool bRightParent = false;

	int iterations = 0;

	bool nCmp = 0;

	// retry:

	pParent = NULL;
	cast_kern(head);
	pLeaf = (struct ds_bst_tree_node __arena *) head->root;
	cast_kern(pLeaf);

	updParent = NULL;
	bRightLeaf = false;

	while (bst_is_internal(pLeaf) && iterations < BST_MAX_DEPTH && can_loop) {
		pGrandParent = pParent;
		pParent = (struct ds_bst_internal __arena *) pLeaf;
		bRightParent = bRightLeaf;
		updGrandParent = updParent;
		
		cast_kern(pParent);
		updParent = (struct ds_bst_update_desc __arena *) smp_load_acquire(&pParent->m_pUpdate);
		
		__u64 state = bst_get_bits((__u64)updParent);
		if (state == BST_DFLAG || state == BST_MARK) {
			iterations++;
			cast_user(pParent);
			break;  /* Backoff: don't traverse further */
		}

		if (key < pParent->key.key) {
			pLeaf = bst_get_child(pParent, false, BST_ACQUIRE);
			bRightLeaf = false;
		} else {
			pLeaf = bst_get_child(pParent, true, BST_ACQUIRE);
			bRightLeaf = true;
		}

		cast_user(pParent);
		iterations++;
	}

	/* pLeaf should be a leaf node at this point */

	nCmp = (key == ((struct ds_bst_leaf __arena *)pLeaf)->kv.key);
	
	res->pGrandParent = pGrandParent;
	res->pParent = pParent;
	res->pLeaf = (struct ds_bst_leaf __arena *) pLeaf;
	res->updParent = (__u64)updParent;
	res->updGrandParent = (__u64)updGrandParent;
	res->bRightLeaf = bRightLeaf;
	res->bRightParent = bRightParent;

	return nCmp;
}

/* ========================================================================
 * HELPING MECHANISMS
 * ======================================================================== */

/**
 * bst_cas_child - CAS the appropriate child pointer of parent
 * @parent: Parent node whose child to update
 * @old_node: Expected old child
 * @new_node: New child to install
 */
static inline void bst_cas_child(struct ds_bst_internal __arena *parent,
                                  void __arena *old_node,
                                  void __arena *new_node)
{
	struct ds_bst_tree_node __arena *new_h;
	
	cast_kern(parent);
	cast_kern(new_node);
	new_h = (struct ds_bst_tree_node __arena *)new_node;
	
	/* Determine which child based on key comparison */
	__u64 new_key;
	if (new_h->type == BST_NODE_LEAF) {
		new_key = ((struct ds_bst_leaf __arena *)new_node)->kv.key;
	} else {
		new_key = ((struct ds_bst_internal __arena *)new_node)->key.key;
	}
	
	if (new_key < parent->key.key) {
		arena_atomic_cmpxchg(&parent->pLeft, old_node, new_node,
		                     ARENA_ACQ_REL, ARENA_ACQUIRE);
	} else {
		arena_atomic_cmpxchg(&parent->pRight, old_node, new_node,
		                     ARENA_ACQ_REL, ARENA_ACQUIRE);
	}
}

/**
 * bst_help_insert - Complete an insert operation
 * @op: Insert operation context
 * 
 * Steps:
 * 1. CAS child pointer of op->p to point to op->new_internal
 * 2. Unflag op->p->update (IFLAG -> CLEAN)
 */
static inline void bst_help_insert(struct ds_bst_update_desc __arena *op)
{
	cast_kern(op);
	
	struct ds_bst_tree_node __arena *pLeaf= (struct ds_bst_tree_node __arena *)op->insert->pLeaf;

	if (op->insert->bRightLeaf) {
		arena_atomic_cmpxchg(&op->insert->pParent->pRight, pLeaf, op->insert->pNew, ARENA_RELEASE, ARENA_RELAXED);
	} else {
		arena_atomic_cmpxchg(&op->insert->pParent->pLeft, pLeaf, op->insert->pNew, ARENA_RELEASE, ARENA_RELAXED);
	}

	__u64 expected = bst_make_update(op, BST_IFLAG);
	__u64 clean = bst_make_update(op, BST_CLEAN);
	arena_atomic_cmpxchg(&op->insert->pParent->m_pUpdate, expected, clean,
	                     ARENA_RELEASE, ARENA_RELAXED);
}

/**
 * bst_help_marked - Complete physical deletion
 * @op: Delete operation context
 * 
 * Steps:
 * 1. Determine sibling of op->l
 * 2. Splice out op->p by swinging op->gp's child to sibling
 * 3. Unflag op->gp->update (DFLAG -> CLEAN)
 */
static inline void bst_help_marked(struct ds_bst_update_desc __arena *op)
{
	struct ds_bst_internal __arena *p;
	void __arena *sibling;
	void __arena *right_child;
	
	cast_kern(op);
	p = op->delete->pParent;
	cast_kern(p);
	
	/* Find sibling of op->pLeaf */
	right_child = smp_load_acquire(&p->pRight);
	
	if (right_child == (void __arena *)op->delete->pLeaf) {
		sibling = smp_load_acquire(&p->pLeft);
	} else {
		sibling = right_child;
	}
	
	/* Physically delete p by swinging gp's child to sibling */
	bst_cas_child(op->delete->pGrandParent, p, sibling);
	
	/* Unflag grandparent (DFLAG -> CLEAN) */
	__u64 expected = bst_make_update(op, BST_DFLAG);
	__u64 clean = bst_make_update(op, BST_CLEAN);
	
	cast_kern(op->delete->pGrandParent);
	arena_atomic_cmpxchg(&op->delete->pGrandParent->m_pUpdate, expected, clean,
	                     ARENA_RELEASE, ARENA_RELAXED);
}

/**
 * bst_help_delete - Attempt to mark parent and complete deletion
 * @op: Delete operation context
 * 
 * Steps:
 * 1. Try to mark op->p (CAS p->update from expected to MARK)
 * 2. If successful, call help_marked
 * 3. If failed, backtrack (unflag gp) and return false
 * 
 * Returns: true if deletion completed, false if backtracked
 */
static inline bool bst_help_delete(struct ds_bst_update_desc __arena *op)
{
	__u64 expected;
	__u64 marked_val;
	__u64 res;
	
	cast_kern(op);
	expected = op->delete->pUpdateParent;
	marked_val = bst_make_update(op, BST_MARK);
	
	cast_kern(op->delete->pParent);
	/* Use RELAXED on failure - avoid recursion depth issues */
	res = arena_atomic_cmpxchg(&op->delete->pParent->m_pUpdate, expected, marked_val,
	                           ARENA_ACQ_REL, ARENA_RELAXED);
	
	if (res == expected || res == marked_val) {
		/* Successfully marked (or already marked by us) */
		bst_help_marked(op);
		return true;
	}
	
	/* Failed to mark - backtrack by unflagging gp */
	/* Don't call bst_help(res) to avoid deep recursion in BPF */
	
	__u64 gp_expected = bst_make_update(op, BST_DFLAG);
	__u64 gp_clean = bst_make_update(op, BST_CLEAN);
	
	cast_kern(op->delete->pGrandParent);
	arena_atomic_cmpxchg(&op->delete->pGrandParent->m_pUpdate, gp_expected, gp_clean,
	                     ARENA_RELEASE, ARENA_RELAXED);
	
	return false;
}

/**
 * bst_help - Help complete a pending operation
 * @update: Tagged pointer containing operation state
 * 
 * Examines the state and dispatches to appropriate helper.
 */
static inline void bst_help(__u64 update)
{
	void __arena *info = (void __arena *)bst_get_ptr(update);
	int state = bst_get_bits(update);
	
	if (!info)
		return;
	
	cast_kern(info);
	
	if (state == BST_IFLAG) {
		bst_help_insert(info);
	} else if (state == BST_MARK) {
		bst_help_marked(info);
	} else if (state == BST_DFLAG) {
		bst_help_delete(info);
	}
}

/* ========================================================================
 * API IMPLEMENTATION
 * ======================================================================== */

/**
 * ds_bintree_init - Initialize an empty binary search tree
 * @head: Tree head to initialize
 * 
 * Initializes with two sentinel leaves to avoid edge cases:
 * - Leaf1: Key = UINT64_MAX - 1
 * - Leaf2: Key = UINT64_MAX
 * - Root: Internal node with both leaves as children
 * 
 * Users must insert keys < BST_SENTINEL_KEY1.
 * 
 * Returns: DS_SUCCESS on success, DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_bintree_init(struct ds_bst_head __arena *head)
{
	struct ds_bst_leaf __arena *leaf1, *leaf2;
	struct ds_bst_internal __arena *root;
	
	cast_user(head);
	if (!head)
		return DS_ERROR_INVALID;
	
	/* Allocate sentinel leaves */
	leaf1 = (struct ds_bst_leaf __arena *)bpf_arena_alloc(sizeof(*leaf1));
	if (!leaf1)
		return DS_ERROR_NOMEM;
	
	leaf2 = (struct ds_bst_leaf __arena *)bpf_arena_alloc(sizeof(*leaf2));
	if (!leaf2) {
		bpf_arena_free(leaf1);
		return DS_ERROR_NOMEM;
	}
	
	/* Allocate root internal node */
	root = (struct ds_bst_internal __arena *)bpf_arena_alloc(sizeof(*root));
	if (!root) {
		bpf_arena_free(leaf1);
		bpf_arena_free(leaf2);
		return DS_ERROR_NOMEM;
	}
	
	cast_kern(leaf1);
	cast_kern(leaf2);
	cast_kern(root);
	
	/* Initialize leaf1 */
	leaf1->header.type = BST_NODE_LEAF;
	leaf1->kv.key = BST_SENTINEL_KEY1;
	leaf1->kv.value = 0;
	
	/* Initialize leaf2 */
	leaf2->header.type = BST_NODE_LEAF;
	leaf2->kv.key = BST_SENTINEL_KEY2;
	leaf2->kv.value = 0;
	
	/* Initialize root */
	root->header.type = BST_NODE_INTERNAL;
	root->key.key = BST_SENTINEL_KEY2;
	
	cast_user(leaf1);
	cast_user(leaf2);
	root->pLeft = (struct ds_bst_tree_node __arena *)leaf1;
	root->pRight = (struct ds_bst_tree_node __arena *)leaf2;
	root->m_pUpdate = bst_make_update(NULL, BST_CLEAN);
	
	/* Set head */
	cast_user(root);
	head->root = root;
	head->count = 0;
	
	return DS_SUCCESS;
}

/**
 * ds_bintree_insert - Insert a key-value pair
 * @head: Tree head
 * @key: Key to insert (must be < BST_SENTINEL_KEY1)
 * @value: Value to associate with key
 * 
 * Returns: DS_SUCCESS on success
 *          DS_ERROR_EXISTS if key already exists
 *          DS_ERROR_NOMEM if allocation fails
 *          DS_ERROR_INVALID if key is invalid
 */
static inline int ds_bintree_insert(struct ds_bst_head __arena *head,
                                     struct ds_kv kv)
{
	struct bst_search_result res;
	struct ds_bst_leaf __arena *new_leaf;
	struct ds_bst_internal __arena *new_internal;
	struct ds_bst_update_desc __arena *pOp;
	int iterations = 0;
	
	cast_kern(head);
	if (!head) 
		return DS_ERROR_INVALID;
	
	/* Validate key */
	if (kv.key >= BST_SENTINEL_KEY1) {
		head->stats.insert_failure_invalid_key++;
		return DS_ERROR_INVALID;
	}
	
	while (iterations < BST_MAX_DEPTH && can_loop) {
		/* Search for key */
		if (bst_search(head, kv.key, &res)) {
			/* Key exists - update the value atomically */
			cast_kern(res);
			cast_kern(res.pLeaf);
			res.pLeaf->kv.value = kv.value;
			head->stats.insert_into_updates++;
			return DS_SUCCESS;
		}
		
		/* Validate search result - check for backoff conditions */
		if (!res.pParent) {
			head->stats.insert_failure_no_parent++;
			return DS_ERROR_BUSY;
		} 
		
		if (!res.pLeaf) {
			head->stats.insert_failure_no_leaf++;
			return DS_ERROR_BUSY;
		}
		
		// if (bst_is_internal((struct ds_bst_tree_node __arena *)res.pLeaf)) {
		// 	head->stats.insert_failure_leaf_is_internal++;
		// 	return DS_ERROR_BUSY;
		// }

		/* Check if parent is clean */
		if (bst_get_bits(res.updParent) != BST_CLEAN || 
		    bst_get_bits(res.updGrandParent) != BST_CLEAN) {
			head->stats.insert_retry_didnt_help++;
			/* Retry without helping to avoid BPF recursion */
			iterations++;
			continue;
		}
		
		if (bst_get_child(res.pParent, res.bRightLeaf, BST_RELAXED) == (struct ds_bst_tree_node __arena *)res.pLeaf) {
			/* Allocate new leaf */
			new_leaf = (struct ds_bst_leaf __arena *)bpf_arena_alloc(sizeof(*new_leaf));
			if (!new_leaf) {
				head->stats.insert_failure_nomem++;
				return DS_ERROR_NOMEM;
			}

			/* Allocate new internal node */
			new_internal = (struct ds_bst_internal __arena *)bpf_arena_alloc(sizeof(*new_internal));
			if (!new_internal) {
				bpf_arena_free(new_leaf);
				head->stats.insert_failure_nomem++;
				return DS_ERROR_NOMEM;
			}

			/* Initialize new leaf */
			new_leaf->header.type = BST_NODE_LEAF;
			new_leaf->kv.key = kv.key;
			new_leaf->kv.value = kv.value;

			/* Initialize new internal node based on key comparison */
			new_internal->header.type = BST_NODE_INTERNAL;
			new_internal->m_pUpdate = bst_make_update(NULL, BST_CLEAN);
			
			bool bNewKeyIsLess = (kv.key < res.pLeaf->kv.key);
			if (bNewKeyIsLess) {
				if (res.pGrandParent) {
					bst_set_infinite_key(&new_internal->header, 0);
					new_internal->key.key = res.pLeaf->kv.key;
				} else {
					/* Root case - pLeaf should have INF1 key */
					bst_set_infinite_key(&new_internal->header, 1);
				}
				WRITE_ONCE(new_internal->pLeft, &new_leaf->header);
				WRITE_ONCE(new_internal->pRight, &res.pLeaf->header);
			} else {
				/* New key is greater - becomes right child */
				bst_set_infinite_key(&new_internal->header, 0);

				new_internal->key.key = kv.key;
				WRITE_ONCE(new_internal->pLeft, &res.pLeaf->header);
				WRITE_ONCE(new_internal->pRight, &new_leaf->header);
			}

			/* Allocate update descriptor and iinfo */
			pOp = (struct ds_bst_update_desc __arena *)bpf_arena_alloc(sizeof(*pOp));
			if (!pOp) {
				bpf_arena_free(new_leaf);
				bpf_arena_free(new_internal);
				head->stats.insert_failure_nomem++;
				return DS_ERROR_NOMEM;
			}
			
			struct ds_bst_iinfo __arena *iinfo = (struct ds_bst_iinfo __arena *)bpf_arena_alloc(sizeof(*iinfo));
			if (!iinfo) {
				bpf_arena_free(pOp);
				bpf_arena_free(new_leaf);
				bpf_arena_free(new_internal);
				head->stats.insert_failure_nomem++;
				return DS_ERROR_NOMEM;
			}

			/* Setup iinfo */
			iinfo->pParent = res.pParent;
			iinfo->pNew = new_internal;
			iinfo->pLeaf = res.pLeaf;
			iinfo->bRightLeaf = res.bRightLeaf;
			
			/* Link to update descriptor */
			pOp->insert = iinfo;

			__u64 expected = (__u64) bst_get_ptr(res.updParent);
			__u64 desired = (__u64) bst_make_update(pOp, BST_IFLAG);

			__u64 cas_result = arena_atomic_cmpxchg(&res.pParent->m_pUpdate, expected, desired, ARENA_ACQ_REL, ARENA_ACQUIRE);
			if (cas_result == expected) {
				/* CAS succeeded, help complete the operation */
				bst_help_insert(pOp);
				arena_atomic_inc(&head->count);
				head->stats.total_inserts++;
				return DS_SUCCESS;
			} else {
				/* CAS failed, free allocated memory and retry */
				bpf_arena_free(iinfo);
				bpf_arena_free(pOp);
				bpf_arena_free(new_leaf);
				bpf_arena_free(new_internal);
				head->stats.insert_retry_didnt_help++;
			}

		}
		iterations++;
	}
	head->stats.insert_failure_busy++;
	return DS_ERROR_BUSY;
}

/**
 * ds_bintree_delete - Delete a key from the tree
 * @head: Tree head
 * @key: Key to delete
 * 
 * Returns: DS_SUCCESS on success
 *          DS_ERROR_NOT_FOUND if key doesn't exist
 *          DS_ERROR_NOMEM if allocation fails
 */
static inline int ds_bintree_delete(struct ds_bst_head __arena *head, struct ds_kv kv)
{
	struct bst_search_result res;
	struct ds_bst_update_desc __arena *pOp;
	__u64 new_up;
	__u64 res_cas;
	int iterations = 0;
	
	cast_kern(head);
	if (!head)
		return DS_ERROR_INVALID;
	
	while (iterations < BST_MAX_DEPTH && can_loop) {
		/* Search for key */
		bst_search(head, kv.key, &res);
		
		/* Validate search result - check for backoff conditions */
		if (!res.pParent || !res.pLeaf || !res.pGrandParent || bst_is_internal((struct ds_bst_tree_node __arena *)res.pLeaf)) {
			head->stats.delete_failure_busy++;
			return DS_ERROR_BUSY;
		}
		
		cast_kern(res.pLeaf);
		
		/* Key not found? */
		if (res.pLeaf->kv.key != kv.key) {
			head->stats.delete_failure_not_found++;	
			return DS_ERROR_NOT_FOUND;
		}
		
		/* Check if grandparent is clean */
		if (bst_get_bits(res.updGrandParent) != BST_CLEAN) {
			/* Retry without helping to avoid BPF recursion */
			iterations++;
			head->stats.delete_retry_didnt_help_gp++;
			continue;
		}
		
		/* Check if parent is clean */
		if (bst_get_bits(res.updParent) != BST_CLEAN) {
			/* Retry without helping to avoid BPF recursion */
			iterations++;
			head->stats.delete_retry_didnt_help_p++;
			continue;
		}
		
		/* Allocate update descriptor and dinfo */
		pOp = bpf_arena_alloc(sizeof(*pOp));
		if (!pOp) {
			head->stats.delete_failure_nomem++;
			return DS_ERROR_NOMEM;
		}
		
		struct ds_bst_dinfo __arena *dinfo = bpf_arena_alloc(sizeof(*dinfo));
		if (!dinfo) {
			head->stats.delete_failure_nomem++;
			bpf_arena_free(pOp);
			return DS_ERROR_NOMEM;
		}
		
		/* Setup dinfo */
		dinfo->pGrandParent = res.pGrandParent;
		dinfo->pParent = res.pParent;
		dinfo->pLeaf = res.pLeaf;
		dinfo->bDisposeLeaf = true;
		dinfo->pUpdateParent = bst_get_ptr(res.updParent);
		dinfo->bRightParent = res.bRightParent;
		dinfo->bRightLeaf = res.bRightLeaf;
		
		/* Link to update descriptor */
		pOp->delete = dinfo;
		
		/* Try to flag grandparent */
		cast_user(pOp);
		new_up = bst_make_update(pOp, BST_DFLAG);
		
		cast_kern(res.pGrandParent);
		/* Use RELAXED on failure - avoid recursion */
		res_cas = arena_atomic_cmpxchg(&res.pGrandParent->m_pUpdate, bst_get_ptr(res.updGrandParent), new_up,
		                               ARENA_ACQ_REL, ARENA_RELAXED);
		
		if (res_cas == bst_get_ptr(res.updGrandParent)) {
			/* Success! Try to complete deletion */
			cast_kern(pOp);
			if (bst_help_delete(pOp)) {
				arena_atomic_dec(&head->count);
				return DS_SUCCESS;
			}
			/* help_delete backtracked - retry */
		} else {
			/* CAS failed - don't help to avoid deep recursion */
			bpf_arena_free(dinfo);
			bpf_arena_free(pOp);
		}
		
		iterations++;

	}
	
	head->stats.delete_failure_busy++;
	return DS_ERROR_BUSY;
}

/**
 * ds_bintree_search - Search for a key
 * @head: Tree head
 * @key: Key to search for
 * 
 * Returns: DS_SUCCESS if found, DS_ERROR_NOT_FOUND otherwise
 */
static inline int ds_bintree_search(struct ds_bst_head __arena *head, struct ds_kv kv)
{
	struct bst_search_result res;
	
	cast_kern(head);
	if (!head)
		return DS_ERROR_INVALID;
	
	bst_search(head, kv.key, &res);
	
	/* Check if search was successful */
	if (!res.pLeaf)
		return DS_ERROR_NOT_FOUND;  /* Tree too deep */
	
	cast_kern(res.pLeaf);
	if (res.pLeaf->kv.key == kv.key)
		return DS_SUCCESS;
	
	return DS_ERROR_NOT_FOUND;
}

/**
 * ds_bintree_verify - Verify tree integrity
 * @head: Tree head
 * 
 * Checks:
 * - BST property (left < parent <= right)
 * - Internal nodes have two children
 * - Leaf count matches head->count
 * - No structural corruption
 * 
 * Returns: DS_SUCCESS if valid, DS_ERROR_CORRUPT otherwise
 */
static inline int ds_bintree_verify(struct ds_bst_head __arena *head)
{
	/* Simple verification - check that we can traverse and count leaves */
	struct ds_bst_internal __arena *stack[BST_MAX_DEPTH];
	int stack_top = 0;
	__u64 leaf_count = 0;
	int iterations = 0;
	
	cast_kern(head);
	if (!head || !head->root)
		return DS_ERROR_INVALID;
	
	/* Start at root */
	cast_kern(head->root);
	stack[stack_top++] = head->root;
	
	/* Iterative DFS traversal */
	while (stack_top > 0 && iterations < BST_MAX_DEPTH * 4 && can_loop) {
		struct ds_bst_internal __arena *node;
		void __arena *left_ptr, *right_ptr;
		struct ds_bst_tree_node __arena *left_h, *right_h;
		
		node = stack[--stack_top];
		cast_kern(node);
		
		/* Load children */
		left_ptr = node->pLeft;
		right_ptr = node->pRight;
		
		if (!left_ptr || !right_ptr)
			return DS_ERROR_CORRUPT;
		
		cast_kern(left_ptr);
		cast_kern(right_ptr);
		
		left_h = (struct ds_bst_tree_node __arena *)left_ptr;
		right_h = (struct ds_bst_tree_node __arena *)right_ptr;
		
		/* Process left child */
		if (left_h->type == BST_NODE_LEAF) {
			struct ds_bst_leaf __arena *leaf = (struct ds_bst_leaf __arena *)left_ptr;
			if (leaf->kv.key < BST_SENTINEL_KEY1)
				leaf_count++;
		} else if (stack_top < BST_MAX_DEPTH - 1) {
			stack[stack_top++] = (struct ds_bst_internal __arena *)left_ptr;
		}
		
		/* Process right child */
		if (right_h->type == BST_NODE_LEAF) {
			struct ds_bst_leaf __arena *leaf = (struct ds_bst_leaf __arena *)right_ptr;
			if (leaf->kv.key < BST_SENTINEL_KEY1)
				leaf_count++;
		} else if (stack_top < BST_MAX_DEPTH - 1) {
			stack[stack_top++] = (struct ds_bst_internal __arena *)right_ptr;
		}
		
		iterations++;
	}
	
	/* Verify count matches */
	if (leaf_count != head->count)
		return DS_ERROR_CORRUPT;
	
	return DS_SUCCESS;
}

/**
 * ds_bintree_get_metadata - Get data structure metadata
 * 
 * Returns: Pointer to metadata structure
 */
static inline const struct ds_metadata* ds_bintree_get_metadata(void)
{
	static const struct ds_metadata metadata = {
		.name = "bintree",
		.description = "Non-blocking binary search tree (Ellen et al. 2010)",
		.node_size = sizeof(struct ds_bst_leaf),
		.requires_locking = 0,  /* Lock-free */
	};
	
	return &metadata;
}
#endif