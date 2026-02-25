/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCS_INTERVAL_TREE_H_
#define UCS_INTERVAL_TREE_H_

#include <ucs/debug/assert.h>
#include <ucs/sys/math.h>
#include <ucs/type/status.h>
#include <stddef.h>
#include <stdint.h>


/**
 * Callback for allocating an interval tree node
 *
 * @param [in]  size  Size of the node to allocate
 * @param [in]  arg   User-defined argument passed during tree initialization

 * @return Pointer to allocated node, or NULL on failure
 */
typedef void *(*ucs_interval_tree_alloc_node_func_t)(size_t size, void *arg);


/**
 * Callback for deallocating an interval tree node
 *
 * @param [in]  node  Node to deallocate
 * @param [in]  arg   User-defined argument passed during tree initialization
 */
typedef void (*ucs_interval_tree_free_node_func_t)(void *node, void *arg);


/**
 * Optional callback invoked when a new node is created and inserted (e.g. after
 * merge in insert_slow). Used to track total size of intervals in the tree.
 *
 * @param [in]  node   Newly created node
 * @param [in]  start  Start of the interval stored in the node
 * @param [in]  end    End of the interval stored in the node
 * @param [in]  arg    User-defined argument passed during tree initialization
 */
typedef void (*ucs_interval_tree_node_created_func_t)(void *node, uint64_t start,
                                                     uint64_t end, void *arg);


/**
 * Interval tree node allocation/deallocation operations
 */
typedef struct {
    ucs_interval_tree_alloc_node_func_t     alloc_node;   /* Node allocation callback */
    ucs_interval_tree_free_node_func_t     free_node;    /* Node deallocation callback */
    ucs_interval_tree_node_created_func_t  node_created; /* Optional: called when a node
                                                          * is created (e.g. after merge) */
    void                                   *arg;         /* User-defined argument for
                                                          * callbacks */
} ucs_interval_tree_ops_t;


/**
 * Interval tree node structure
 */
typedef struct ucs_interval_node {
    struct ucs_interval_node *left;  /**< Left child node */
    struct ucs_interval_node *right; /**< Right child node */
    uint64_t                  start; /**< Start of interval */
    uint64_t                  end;   /**< End of interval */
} ucs_interval_node_t;


typedef struct {
    ucs_interval_node_t     *root;  /**< Root node of the tree */
    ucs_interval_tree_ops_t  ops;   /**< Memory management callbacks */
} ucs_interval_tree_t;


/**
 * Initialize an interval tree
 *
 * @param [in]  tree        Interval tree to initialize
 * @param [in]  ops         Memory management operations (alloc, free, arg)
 */
void ucs_interval_tree_init(ucs_interval_tree_t *tree,
                            const ucs_interval_tree_ops_t *ops);


/**
 * Cleanup and free all nodes in the interval tree
 *
 * @param [in]  tree  Interval tree to clean up
 */
void ucs_interval_tree_cleanup(ucs_interval_tree_t *tree);


/**
 * Check if the interval tree is empty
 *
 * @param [in]  tree  Interval tree
 *
 * @return Non-zero if tree has no nodes, 0 otherwise
 */
int ucs_interval_tree_is_empty(const ucs_interval_tree_t *tree);


/**
 * Remove one interval from the tree (the root) and return its range.
 * The node is freed via the tree's free_node callback.
 *
 * @param [in]   tree   Interval tree
 * @param [out]  start  Start of the removed interval
 * @param [out]  end    End of the removed interval
 *
 * @return Non-zero if an interval was removed, 0 if tree was empty
 */
int ucs_interval_tree_pop_any(ucs_interval_tree_t *tree, uint64_t *start,
                              uint64_t *end);


/**
 * Return the number of intervals (nodes) in the tree
 *
 * @param [in]  tree  Interval tree
 *
 * @return Number of nodes
 */
size_t ucs_interval_tree_count(const ucs_interval_tree_t *tree);


/* TODO: remove this forward declaration when file is refactored to minimize 
 * exposing private logic */
ucs_status_t ucs_interval_tree_insert_slow(ucs_interval_tree_t *tree,
                                           uint64_t start, uint64_t end);


/**
 * Check if tree has only a root node (no children)
 */
static UCS_F_ALWAYS_INLINE int
ucs_interval_tree_is_single_node(const ucs_interval_tree_t *tree)
{
    return (tree->root != NULL) && (tree->root->left == NULL) &&
           (tree->root->right == NULL);
}


/**
 * Insert a new interval into the tree
 *
 * @param [in]  tree   Interval tree
 * @param [in]  start  Start of interval 
 * @param [in]  end    End of interval 
 *
 * @return UCS_OK on success, or error code on failure
 */
static UCS_F_ALWAYS_INLINE ucs_status_t ucs_interval_tree_insert(
        ucs_interval_tree_t *tree, uint64_t start, uint64_t end)
{
    ucs_assert(start <= end);

    /* Fast path: if tree has only root and new interval overlaps/touches it, extend it */
    if (ucs_interval_tree_is_single_node(tree) &&
        (start <= (tree->root->end + 1)) && (tree->root->start <= (end + 1))) {
        tree->root->start = ucs_min(tree->root->start, start);
        tree->root->end   = ucs_max(tree->root->end, end);
        return UCS_OK;
    }

    /* Slow path: handle complex merging or empty tree */
    return ucs_interval_tree_insert_slow(tree, start, end);
}

/**
 * Check if tree contains exactly one interval with the given range
 *
 * @param [in]  tree   Interval tree
 * @param [in]  start  Start of range to check
 * @param [in]  end    End of range to check
 *
 * @return Non-zero if tree has exactly one interval [start, end], 0 otherwise
 */
static UCS_F_ALWAYS_INLINE int
ucs_interval_tree_is_equal_range(const ucs_interval_tree_t *tree,
                                  uint64_t start, uint64_t end)
{
    ucs_assert(start <= end);

    return ucs_interval_tree_is_single_node(tree) &&
           (tree->root->start == start) && (tree->root->end == end);
}

#endif
