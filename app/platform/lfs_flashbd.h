/*
 * Block device in flash
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_FLASHBD_H
#define LFS_FLASHBD_H

#include "lfs.h"
#include "lfs_util.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct lfs_flashbd_config {
	int a;
} lfs_flashbd_config_t;

// flashbd state
typedef struct lfs_flashbd {
    uint32_t base_addr;
    uint32_t flash_base;
    const lfs_flashbd_config_t *cfg;
} lfs_flashbd_t;


// Create a RAM block device using the geometry in lfs_config
int lfs_flashbd_create(const struct lfs_config *cfg);
int lfs_flashbd_createcfg(const struct lfs_config *cfg,
        const struct lfs_flashbd_config *bdcfg);

// Clean up memory associated with block device
int lfs_flashbd_destroy(const struct lfs_config *cfg);

// Read a block
int lfs_flashbd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs_flashbd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_flashbd_erase(const struct lfs_config *cfg, lfs_block_t block);

// Sync the block device
int lfs_flashbd_sync(const struct lfs_config *cfg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
