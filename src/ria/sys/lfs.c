/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/lfs.h"
#include "pico/printf.h"

// 1MB for ROM storage, 512K for Pico W
// TODO Pico W now using some of this flash for bluetooth
//      see PICO_FLASH_BANK_STORAGE_OFFSET and github issue#19
#ifdef RASPBERRYPI_PICO_W
#define LFS_DISK_BLOCKS 128
#else
#define LFS_DISK_BLOCKS 256
#endif
static_assert(!(LFS_DISK_BLOCKS % 8));

static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size);
static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, const void *buffer, lfs_size_t size);
static int lfs_erase(const struct lfs_config *c, lfs_block_t block);
static int lfs_sync(const struct lfs_config *c);

#define LFS_DISK_SIZE (LFS_DISK_BLOCKS * FLASH_SECTOR_SIZE)
#define LFS_LOOKAHEAD_SIZE LFS_DISK_BLOCKS / 8

lfs_t lfs_volume;
static char lfs_read_buffer[FLASH_PAGE_SIZE];
static char lfs_prog_buffer[FLASH_PAGE_SIZE];
static char lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE];
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

static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)(c);
    memcpy(buffer,
           (void *)XIP_NOCACHE_NOALLOC_BASE +
               (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) +
               (block * FLASH_SECTOR_SIZE) +
               off,
           size);
    return LFS_ERR_OK;
}

static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)(c);
    uint32_t flash_offs = (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) +
                          (block * FLASH_SECTOR_SIZE) +
                          off;
    flash_range_program(flash_offs, buffer, size);
    return LFS_ERR_OK;
}

static int lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)(c);
    uint32_t flash_offs = (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) +
                          (block * FLASH_SECTOR_SIZE);
    flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    return LFS_ERR_OK;
}

static int lfs_sync(const struct lfs_config *c)
{
    (void)(c);
    return LFS_ERR_OK;
}

void lfs_init(void)
{
    // mount the filesystem
    int err = lfs_mount(&lfs_volume, &cfg);
    if (err)
    {
        // Maybe first boot. Attempt format.
        // lfs_format returns -84 here, but still succeeds
        lfs_format(&lfs_volume, &cfg);
        err = lfs_mount(&lfs_volume, &cfg);
        if (err)
            printf("?Unable to format lfs (%d)", err);
    }
}

int lfs_eof(lfs_file_t *file)
{
    return file->pos >= file->ctz.size;
}

struct lfs_printf_ctx
{
    lfs_t *lfs;
    lfs_file_t *file;
    int result;
};

static void lfs_printf_cb(char character, void *arg)
{
    struct lfs_printf_ctx *ctx = arg;
    if (ctx->result < 0)
        return;
    ctx->result = lfs_file_write(ctx->lfs, ctx->file, &character, 1);
}

// Returns number of characters written or a lfs_error.
int lfs_printf(lfs_t *lfs, lfs_file_t *file, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    struct lfs_printf_ctx ctx = {
        .lfs = lfs,
        .file = file,
        .result = 0};
    // vfctprintf is Marco Paland's "Tiny printf" from the Pi Pico SDK
    int result = vfctprintf(lfs_printf_cb, &ctx, format, va);
    if (ctx.result < 0)
        return ctx.result;
    return result;
}

char *lfs_gets(char *str, int n, lfs_t *lfs, lfs_file_t *file)
{
    int len = 0;
    for (len = 0; len < n - 1; len++)
    {
        lfs_ssize_t result = lfs_file_read(lfs, file, &str[len], 1);
        if (result != 1)
        {
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
    if (!len && lfs_eof(file))
        return NULL;
    return str;
}
