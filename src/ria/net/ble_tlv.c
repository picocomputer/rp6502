/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/mon.h"
#include "sys/lfs.h"
#include <hal_flash_bank.h>
#include <hardware/flash.h>
#include <string.h>

/* This replaces btstack_flash_bank.c from Pi Pico SDK.
 * By using LittleFS instead of the same two flash sectors,
 * writes will be spread among the entire LittleFS allocation.
 * Erased state is 0xFF, matching NOR flash semantics.
 */

// Place strings in flash
static const char __in_flash("ble_tlv_db_path") BankPath0[] = "BLETLVDB0.SYS";
static const char __in_flash("ble_tlv_db_path") BankPath1[] = "BLETLVDB1.SYS";
static const char __in_flash("ble_tlv_db_paths") * bank_path[] = {BankPath0, BankPath1};

// Keep files open to avoid expensive open/close operations
static lfs_file_t bank_files[2];
static uint8_t file_buffers[2][FLASH_PAGE_SIZE];
static struct lfs_file_config file_configs[2] = {
    {.buffer = file_buffers[0]},
    {.buffer = file_buffers[1]}};

static bool init_attempted = false;
static bool files_open = false;
static bool error_reported = false;

static void report_lfs_error_once(int result)
{
    if (result < 0 && !error_reported)
    {
        mon_add_response_lfs(result);
        error_reported = true;
    }
}

static void pico_flash_bank_init(void)
{
    if (init_attempted)
        return;
    init_attempted = true;

    for (int i = 0; i < 2; i++)
    {
        int result = lfs_file_opencfg(&lfs_volume, &bank_files[i], bank_path[i],
                                      LFS_O_RDWR | LFS_O_CREAT, &file_configs[i]);
        if (result < 0)
        {
            report_lfs_error_once(result);
            return;
        }
    }
    files_open = true;
}

static uint32_t pico_flash_bank_get_size(void *context)
{
    (void)context;
    return FLASH_SECTOR_SIZE;
}

static uint32_t pico_flash_bank_get_alignment(void *context)
{
    (void)context;
    return 1;
}

static void pico_flash_bank_erase(void *context, int bank)
{
    (void)context;
    if (bank > 1)
        return;

    pico_flash_bank_init();

    if (!files_open)
        return;

    // Close and remove the file
    int result = lfs_file_close(&lfs_volume, &bank_files[bank]);
    if (result < 0)
    {
        report_lfs_error_once(result);
        return;
    }

    result = lfs_remove(&lfs_volume, bank_path[bank]);
    if (result < 0 && result != LFS_ERR_NOENT)
        report_lfs_error_once(result);

    // Reopen the file
    result = lfs_file_opencfg(&lfs_volume, &bank_files[bank], bank_path[bank],
                              LFS_O_RDWR | LFS_O_CREAT, &file_configs[bank]);
    if (result < 0)
    {
        report_lfs_error_once(result);
        files_open = false;
    }
}

static void pico_flash_bank_read(void *context, int bank,
                                 uint32_t offset, uint8_t *buffer, uint32_t size)
{
    (void)context;
    if (bank > 1 || offset >= FLASH_SECTOR_SIZE || (offset + size) > FLASH_SECTOR_SIZE)
        return;

    pico_flash_bank_init();

    // Default to erased state (0xFF represents erased NOR flash)
    memset(buffer, 0xFF, size);

    if (!files_open)
        return;

    lfs_soff_t file_size = lfs_file_size(&lfs_volume, &bank_files[bank]);
    if (file_size < 0)
    {
        report_lfs_error_once(file_size);
        return;
    }

    // If reading beyond end of file, buffer remains 0xFF (erased state)
    if ((lfs_soff_t)offset >= file_size)
        return;

    lfs_soff_t seek_result = lfs_file_seek(&lfs_volume, &bank_files[bank], offset, LFS_SEEK_SET);
    if (seek_result < 0)
    {
        report_lfs_error_once(seek_result);
        return;
    }

    lfs_ssize_t avail = file_size - offset;
    lfs_ssize_t to_read = (lfs_ssize_t)size < avail ? (lfs_ssize_t)size : avail;
    lfs_ssize_t bytes_read = lfs_file_read(&lfs_volume, &bank_files[bank], buffer, to_read);
    report_lfs_error_once(bytes_read);
}

static void pico_flash_bank_write(void *context, int bank,
                                  uint32_t offset, const uint8_t *data, uint32_t size)
{
    (void)context;
    if (bank > 1 || offset >= FLASH_SECTOR_SIZE ||
        (offset + size) > FLASH_SECTOR_SIZE || size == 0)
        return;

    pico_flash_bank_init();

    if (!files_open)
        return;

    lfs_soff_t file_size = lfs_file_size(&lfs_volume, &bank_files[bank]);
    if (file_size < 0)
    {
        report_lfs_error_once(file_size);
        return;
    }

    // Fill gap between end of file and start of write with 0xFF (erased state)
    if (file_size < (lfs_soff_t)offset)
    {
        uint8_t ff[64];
        memset(ff, 0xFF, sizeof(ff));

        lfs_soff_t seek_result = lfs_file_seek(&lfs_volume, &bank_files[bank], file_size, LFS_SEEK_SET);
        if (seek_result < 0)
        {
            report_lfs_error_once(seek_result);
            return;
        }

        lfs_ssize_t gap = (lfs_ssize_t)offset - file_size;
        while (gap > 0)
        {
            lfs_ssize_t chunk = gap < (lfs_ssize_t)sizeof(ff) ? gap : (lfs_ssize_t)sizeof(ff);
            lfs_ssize_t written = lfs_file_write(&lfs_volume, &bank_files[bank], ff, chunk);
            if (written < 0)
            {
                report_lfs_error_once(written);
                return;
            }
            gap -= written;
        }
    }
    else
    {
        lfs_soff_t seek_result = lfs_file_seek(&lfs_volume, &bank_files[bank], offset, LFS_SEEK_SET);
        if (seek_result < 0)
        {
            report_lfs_error_once(seek_result);
            return;
        }
    }

    lfs_ssize_t written = lfs_file_write(&lfs_volume, &bank_files[bank], data, size);
    if (written < 0)
        report_lfs_error_once(written);
    else if (written != (lfs_ssize_t)size)
        report_lfs_error_once(LFS_ERR_NOSPC);

    // Sync to ensure data is written
    int sync_result = lfs_file_sync(&lfs_volume, &bank_files[bank]);
    report_lfs_error_once(sync_result);
}

static const hal_flash_bank_t pico_flash_bank_instance_obj = {
    .get_size = &pico_flash_bank_get_size,
    .get_alignment = &pico_flash_bank_get_alignment,
    .erase = &pico_flash_bank_erase,
    .read = &pico_flash_bank_read,
    .write = &pico_flash_bank_write,
};

const hal_flash_bank_t *pico_flash_bank_instance(void)
{
    return &pico_flash_bank_instance_obj;
}
