/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs_flashbd.h"
#include "platform.h"

#ifdef LFS_YES_FLASHBD_TRACE
#define LFS_TRACE_(fmt, ...) \
    printf("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_FLASHBD_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")
#else
#define LFS_FLASHBD_TRACE(...)
#endif

int lfs_flashbd_createcfg(const struct lfs_config *cfg,
        const struct lfs_flashbd_config *bdcfg) {
    LFS_FLASHBD_TRACE("lfs_flashbd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "%p )",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            (void*)bdcfg);
    lfs_flashbd_t *bd = cfg->context;
    bd->cfg = bdcfg;

#if 0
    // allocate buffer?
    if (bd->cfg->buffer) {
        bd->buffer = bd->cfg->buffer;
    } else {
        bd->buffer = lfs_malloc(cfg->block_size * cfg->block_count);
        if (!bd->buffer) {
            LFS_FLASHBD_TRACE("lfs_flashbd_createcfg -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
    }

    // zero for reproducability?
    if (bd->cfg->erase_value != -1) {
        memset(bd->buffer, bd->cfg->erase_value,
                cfg->block_size * cfg->block_count);
    }
#endif

    LFS_FLASHBD_TRACE("lfs_flashbd_createcfg -> %d", 0);
    return 0;
}

int lfs_flashbd_create(const struct lfs_config *cfg) {
    LFS_FLASHBD_TRACE("lfs_flashbd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count);
    static const struct lfs_flashbd_config defaults = {};
    int err = lfs_flashbd_createcfg(cfg, &defaults);
    LFS_FLASHBD_TRACE("lfs_flashbd_create -> %d", err);
    return err;
}

int lfs_flashbd_destroy(const struct lfs_config *cfg) {
    LFS_FLASHBD_TRACE("lfs_flashbd_destroy(%p)", (void*)cfg);
    // clean up memory
    lfs_flashbd_t *bd = cfg->context;
#if 0
    if (!bd->cfg->buffer) {
        lfs_free(bd->buffer);
    }
#endif
    LFS_FLASHBD_TRACE("lfs_flashbd_destroy -> %d", 0);
    return 0;
}

int lfs_flashbd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_FLASHBD_TRACE("lfs_flashbd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_flashbd_t *bd = cfg->context;
    LFS_FLASHBD_TRACE("lfs_flashbd_read: bd = %p", bd);

    // check if read is valid
    LFS_ASSERT(off  % cfg->read_size == 0);
    LFS_ASSERT(size % cfg->read_size == 0);
    LFS_ASSERT(block < cfg->block_count);

    LFS_FLASHBD_TRACE("lfs_flashbd_read: about to read %d bytes from %p", size, bd->phys_addr + block * cfg->block_size + off);
    platform_flash_read(buffer, bd->phys_addr + block * cfg->block_size + off, size);

    LFS_FLASHBD_TRACE("lfs_flashbd_read -> %d", 0);
    return 0;
}

int lfs_flashbd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_FLASHBD_TRACE("lfs_flashbd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_flashbd_t *bd = cfg->context;

    // check if write is valid
    LFS_ASSERT(off  % cfg->prog_size == 0);
    LFS_ASSERT(size % cfg->prog_size == 0);
    LFS_ASSERT(block < cfg->block_count);

    LFS_FLASHBD_TRACE("lfs_flashbd_write: about to write %d bytes to %p", size, bd->phys_addr + block * cfg->block_size + off);
    platform_flash_write(buffer, bd->phys_addr + block * cfg->block_size + off, size);

    LFS_FLASHBD_TRACE("lfs_flashbd_prog -> %d", 0);
    return 0;
}

int lfs_flashbd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_FLASHBD_TRACE("lfs_flashbd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs_flashbd_t *bd = cfg->context;

    // check if erase is valid
    LFS_ASSERT(block < cfg->block_count);

    uint32_t addr = bd->phys_addr + block * cfg->block_size;

    u32_t sect_first = platform_flash_get_sector_of_address(addr);

    u32_t sect_last =  platform_flash_get_sector_of_address(addr + cfg->block_size) - 1;
    while( sect_first <= sect_last ) {
        if( platform_flash_erase_sector( sect_first ++ ) == PLATFORM_ERR ) {
           return -1;
        }
    }
    return 0;
}

int lfs_flashbd_sync(const struct lfs_config *cfg) {
    LFS_FLASHBD_TRACE("lfs_flashbd_sync(%p)", (void*)cfg);
    (void)cfg;
    LFS_FLASHBD_TRACE("lfs_flashbd_sync -> %d", 0);
    return 0;
}
