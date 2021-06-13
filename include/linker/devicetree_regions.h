/*
 * Copyright (c) 2021, Commonwealth Scientific and Industrial Research
 * Organisation (CSIRO) ABN 41 687 119 230.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generate memory regions from devicetree nodes.
 */

/* Conditions under which a region is not automatically generated.
 * Currently:
 *     * Do not generate regions for nodes that overlap `reserved-memory`
 */
#define _REGION_SKIP_GEN(node) \
	DT_NODE_OVERLAPS_RESERVED_MEMORY(node)

/* Declare a memory region from a devicetree node */
#define _REGION_DECLARE(node, attr) DT_LABEL(node)(attr) : \
	ORIGIN = DT_REG_ADDR(node),			   \
	LENGTH = DT_REG_SIZE(node)

/* Read-Write memory region from a devicetree node that shouldn't be skipped */
#define _RW_SAFE_MEMORY_REGION_DECLARE(node) \
	COND_CODE_1(_REGION_SKIP_GEN(node),  \
		    (), (_REGION_DECLARE(node, rw)))

/**
 * @brief Generate a linker memory region from a devicetree node
 *
 * @param node devicetree node with a \<reg\> property defining region location
 *             and size and a \<label\> property defining the region name
 * @param attr region attributes to use (rx, rw, ...)
 */
#define DT_REGION_FROM_NODE_STATUS_OKAY(node, attr) \
	COND_CODE_1(DT_NODE_HAS_STATUS(node, okay), \
		    (_REGION_DECLARE(node, attr)),  \
		    ())

/**
 * @brief Generate a linker memory region for each valid child under a node
 *
 * @param node Node which has children to generate memory regions for
 */
#define DT_REGIONS_FROM_CHILDREN(node) \
	DT_FOREACH_CHILD(node, _RW_SAFE_MEMORY_REGION_DECLARE)

/**
 * @brief Generate a linker memory region for each valid node in compatible
 *
 * @param compat compatible to generate memory regions for
 */
#define DT_REGIONS_FROM_COMPAT(compat) \
	DT_COMPAT_FOREACH_NODE_STATUS_OKAY(compat, _RW_SAFE_MEMORY_REGION_DECLARE)
