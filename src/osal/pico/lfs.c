/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "osal/pico/lfs.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(DEBUG_SYS) || defined(DEBUG_SYS_LFS)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// 1MB LFS volume on the tail of flash
#define LFS_DISK_BLOCKS 256

static_assert(!(LFS_DISK_BLOCKS % 8));
#define LFS_LOOKAHEAD_SIZE (LFS_DISK_BLOCKS / 8)
#define LFS_DISK_SIZE (LFS_DISK_BLOCKS * FLASH_SECTOR_SIZE)

lfs_t lfs_volume;
int lfs_mount_error;
static char lfs_read_buffer[FLASH_PAGE_SIZE];
static char lfs_prog_buffer[FLASH_PAGE_SIZE];
static char lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE];

static inline uint32_t lfs_flash_offs(lfs_block_t block)
{
    return (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) + (block * FLASH_SECTOR_SIZE);
}

static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)(c);
    memcpy(buffer,
           (const uint8_t *)XIP_NOCACHE_NOALLOC_BASE + lfs_flash_offs(block) + off,
           size);
    return LFS_ERR_OK;
}

static int __no_inline_not_in_flash_func(lfs_prog)(const struct lfs_config *c, lfs_block_t block,
                                                   lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)(c);
    flash_range_program(lfs_flash_offs(block) + off, buffer, size);
    return LFS_ERR_OK;
}

static int __no_inline_not_in_flash_func(lfs_erase)(const struct lfs_config *c, lfs_block_t block)
{
    (void)(c);
    flash_range_erase(lfs_flash_offs(block), FLASH_SECTOR_SIZE);
    return LFS_ERR_OK;
}

static int lfs_sync(const struct lfs_config *c)
{
    (void)(c);
    return LFS_ERR_OK;
}

static const struct lfs_config cfg = {
    .read = lfs_read,
    .prog = lfs_prog,
    .erase = lfs_erase,
    .sync = lfs_sync,
    .read_size = 1,
    .prog_size = FLASH_PAGE_SIZE,
    .block_size = FLASH_SECTOR_SIZE,
    .block_count = LFS_DISK_SIZE / FLASH_SECTOR_SIZE,
    .cache_size = FLASH_PAGE_SIZE,
    .lookahead_size = LFS_LOOKAHEAD_SIZE,
    .block_cycles = 100,
    .read_buffer = lfs_read_buffer,
    .prog_buffer = lfs_prog_buffer,
    .lookahead_buffer = lfs_lookahead_buffer,
};

void __in_flash("lfs_init") lfs_init(void)
{
    // Check we're not overlapping the LFS region in flash
    extern char __flash_binary_end;
    (void)__flash_binary_end;
    assert(((uintptr_t)&__flash_binary_end - XIP_BASE <= PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE));
    // mount the filesystem
    int err = lfs_mount(&lfs_volume, &cfg);
    if (err)
    {
        // Maybe first boot. Attempt format.
        err = lfs_format(&lfs_volume, &cfg);
        if (!err)
            err = lfs_mount(&lfs_volume, &cfg);
    }
    lfs_mount_error = err;
}

int lfs_eof(lfs_t *lfs, lfs_file_t *file)
{
    lfs_soff_t pos = lfs_file_tell(lfs, file);
    lfs_soff_t size = lfs_file_size(lfs, file);
    if (pos < 0 || size < 0)
        return -1;
    return pos >= size;
}

// Returns number of characters written or a lfs_error.
int lfs_printf(lfs_t *lfs, lfs_file_t *file, const char *format, ...)
{
    char buf[LFS_PRINTF_MAX];
    va_list va;
    va_start(va, format);
    int len = vsnprintf(buf, sizeof(buf), format, va);
    va_end(va);
    if (len < 0)
        return len;
    if (len > (int)sizeof(buf) - 1)
        len = (int)sizeof(buf) - 1;
    lfs_ssize_t r = lfs_file_write(lfs, file, buf, (lfs_size_t)len);
    if (r < 0)
        return (int)r;
    return len;
}

char *lfs_gets(char *str, size_t n, lfs_t *lfs, lfs_file_t *file, int *err)
{
    if (err)
        *err = 0;
    size_t len;
    for (len = 0; len < n - 1; len++)
    {
        lfs_ssize_t result = lfs_file_read(lfs, file, &str[len], 1);
        if (result != 1)
        {
            if (result < 0 && err)
                *err = (int)result;
            str[len] = 0;
            return NULL;
        }
        if (str[len] == '\n')
        {
            len++;
            break;
        }
    }
    str[len] = 0;
    if (!len && lfs_eof(lfs, file))
        return NULL;
    return str;
}
