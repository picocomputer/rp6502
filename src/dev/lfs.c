/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lfs.h"
#include "hardware/flash.h"

// 1MB for ROM storage, 512K for Pico W
#ifdef RASPBERRYPI_PICO_W
#define ROM_DISK_BLOCKS 128
#else
#define ROM_DISK_BLOCKS 256
#endif
static_assert(!(ROM_DISK_BLOCKS % 8));

// Our implementaion is limited to a single volume and one file handle.
// We use LFS_NO_MALLOC and have no need to pause interrupts.
static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size);
static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, const void *buffer, lfs_size_t size);
static int lfs_erase(const struct lfs_config *c, lfs_block_t block);
static int lfs_sync(const struct lfs_config *c);

#define LFS_ROMDISK_SIZE (ROM_DISK_BLOCKS * FLASH_SECTOR_SIZE)
#define LFS_LOOKAHEAD_SIZE ROM_DISK_BLOCKS / 8

static char lfs_read_buffer[FLASH_PAGE_SIZE];
static char lfs_prog_buffer[FLASH_PAGE_SIZE];
static char lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE];

lfs_t lfs_volume;
static const struct lfs_config cfg = {
    .read = lfs_read,
    .prog = lfs_prog,
    .erase = lfs_erase,
    .sync = lfs_sync,
    .read_size = 1,
    .prog_size = FLASH_PAGE_SIZE,
    .block_size = FLASH_SECTOR_SIZE,
    .block_count = LFS_ROMDISK_SIZE / FLASH_SECTOR_SIZE,
    .cache_size = FLASH_PAGE_SIZE,
    .lookahead_size = LFS_LOOKAHEAD_SIZE,
    .block_cycles = 500,
    .read_buffer = lfs_read_buffer,
    .prog_buffer = lfs_prog_buffer,
    .lookahead_buffer = lfs_lookahead_buffer,
};

static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)(c);
    assert(block < cfg.block_count);
    assert(off + size <= FLASH_SECTOR_SIZE);
    memcpy(buffer,
           (void *)XIP_NOCACHE_NOALLOC_BASE +
               (PICO_FLASH_SIZE_BYTES - LFS_ROMDISK_SIZE) +
               (block * FLASH_SECTOR_SIZE) +
               off,
           size);
    return LFS_ERR_OK;
}

static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)(c);
    assert(block < cfg.block_count);
    uint32_t flash_offs = (PICO_FLASH_SIZE_BYTES - LFS_ROMDISK_SIZE) +
                          (block * FLASH_SECTOR_SIZE) +
                          off;
    flash_range_program(flash_offs, buffer, size);
    // TODO verify for LFS_ERR_CORRUPT?
    return LFS_ERR_OK;
}

static int lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)(c);
    assert(block < cfg.block_count);
    uint32_t flash_offs = (PICO_FLASH_SIZE_BYTES - LFS_ROMDISK_SIZE) +
                          (block * FLASH_SECTOR_SIZE);
    flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    // TODO verify for LFS_ERR_CORRUPT?
    return LFS_ERR_OK;
}

static int lfs_sync(const struct lfs_config *c)
{
    (void)(c);
    return LFS_ERR_OK;
}

void lfs_init()
{
    // TODO remove this littlefs example from the README
    // mount the filesystem
    int err = lfs_mount(&lfs_volume, &cfg);

    // reformat if we can't mount the filesystem
    // this should only happen on the first boot
    if (err)
    {
        lfs_format(&lfs_volume, &cfg);
        lfs_mount(&lfs_volume, &cfg);
    }

    // read current count
    lfs_file_t lfs_file;
    uint8_t lfs_file_config_buffer[FLASH_PAGE_SIZE];
    struct lfs_file_config lfs_file_config = {
        .buffer = lfs_file_config_buffer,
    };

    uint32_t boot_count = 0;
    lfs_file_opencfg(&lfs_volume, &lfs_file, "boot_count", LFS_O_RDWR | LFS_O_CREAT, &lfs_file_config);
    lfs_file_read(&lfs_volume, &lfs_file, &boot_count, sizeof(boot_count));

    // update boot count
    boot_count += 1;
    lfs_file_rewind(&lfs_volume, &lfs_file);
    lfs_file_write(&lfs_volume, &lfs_file, &boot_count, sizeof(boot_count));

    // remember the storage is not updated until the file is closed successfully
    lfs_file_close(&lfs_volume, &lfs_file);

    // release any resources we were using
    // lfs_unmount(&lfs);

    // print the boot count
    printf("lfs test boot_count: %ld\n", boot_count);
};
