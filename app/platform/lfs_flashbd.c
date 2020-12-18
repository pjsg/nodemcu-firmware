/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs_flashbd.h"

#define LFS_RAMBD_TRACE(...)

int lfs_flashbd_createcfg(const struct lfs_config *cfg,
        const struct lfs_flashbd_config *bdcfg) {
    LFS_RAMBD_TRACE("lfs_flashbd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "%p {.erase_value=%"PRId32", .buffer=%p})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            (void*)bdcfg, bdcfg->erase_value, bdcfg->buffer);
    lfs_flashbd_t *bd = cfg->context;
    bd->cfg = bdcfg;

#if 0
    // allocate buffer?
    if (bd->cfg->buffer) {
        bd->buffer = bd->cfg->buffer;
    } else {
        bd->buffer = lfs_malloc(cfg->block_size * cfg->block_count);
        if (!bd->buffer) {
            LFS_RAMBD_TRACE("lfs_flashbd_createcfg -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
    }

    // zero for reproducability?
    if (bd->cfg->erase_value != -1) {
        memset(bd->buffer, bd->cfg->erase_value,
                cfg->block_size * cfg->block_count);
    }
#endif

    LFS_RAMBD_TRACE("lfs_flashbd_createcfg -> %d", 0);
    return 0;
}

int lfs_flashbd_create(const struct lfs_config *cfg) {
    LFS_RAMBD_TRACE("lfs_flashbd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count);
    static const struct lfs_flashbd_config defaults = {};
    int err = lfs_flashbd_createcfg(cfg, &defaults);
    LFS_RAMBD_TRACE("lfs_flashbd_create -> %d", err);
    return err;
}

int lfs_flashbd_destroy(const struct lfs_config *cfg) {
    LFS_RAMBD_TRACE("lfs_flashbd_destroy(%p)", (void*)cfg);
    // clean up memory
    lfs_flashbd_t *bd = cfg->context;
#if 0
    if (!bd->cfg->buffer) {
        lfs_free(bd->buffer);
    }
#endif
    LFS_RAMBD_TRACE("lfs_flashbd_destroy -> %d", 0);
    return 0;
}

int lfs_flashbd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_RAMBD_TRACE("lfs_flashbd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_flashbd_t *bd = cfg->context;

    // check if read is valid
    LFS_ASSERT(off  % cfg->read_size == 0);
    LFS_ASSERT(size % cfg->read_size == 0);
    LFS_ASSERT(block < cfg->block_count);

#if 0
    // read data
    memcpy(buffer, &bd->buffer[block*cfg->block_size + off], size);
#endif

    LFS_RAMBD_TRACE("lfs_flashbd_read -> %d", 0);
    return 0;
}

int lfs_flashbd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_RAMBD_TRACE("lfs_flashbd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_flashbd_t *bd = cfg->context;

    // check if write is valid
    LFS_ASSERT(off  % cfg->prog_size == 0);
    LFS_ASSERT(size % cfg->prog_size == 0);
    LFS_ASSERT(block < cfg->block_count);

#if 0
    // check that data was erased? only needed for testing
    if (bd->cfg->erase_value != -1) {
        for (lfs_off_t i = 0; i < size; i++) {
            LFS_ASSERT(bd->buffer[block*cfg->block_size + off + i] ==
                    bd->cfg->erase_value);
        }
    }

    // progflash data
    memcpy(&bd->buffer[block*cfg->block_size + off], buffer, size);
#endif

    LFS_RAMBD_TRACE("lfs_flashbd_prog -> %d", 0);
    return 0;
}

int lfs_flashbd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_RAMBD_TRACE("lfs_flashbd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs_flashbd_t *bd = cfg->context;

    // check if erase is valid
    LFS_ASSERT(block < cfg->block_count);

#if 0
    // erase, only needed for testing
    if (bd->cfg->erase_value != -1) {
        memset(&bd->buffer[block*cfg->block_size],
                bd->cfg->erase_value, cfg->block_size);
    }
#endif

    LFS_RAMBD_TRACE("lfs_flashbd_erase -> %d", 0);
    return 0;
}

int lfs_flashbd_sync(const struct lfs_config *cfg) {
    LFS_RAMBD_TRACE("lfs_flashbd_sync(%p)", (void*)cfg);
    // sync does nothing because we aren't backed by anything real
    (void)cfg;
    LFS_RAMBD_TRACE("lfs_flashbd_sync -> %d", 0);
    return 0;
}
