/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rom.h"
#include "str.h"
#include "mem/mbuf.h"
#include "ria/ria.h"
#include "ria/act.h"
#include "mon.h"
#include "lfs.h"
#include "fatfs/ff.h"
#include <stdio.h>
#include <string.h>

static enum {
    ROM_IDLE,
    ROM_LOADING,
    ROM_RIA_WRITING,
    ROM_RIA_VERIFYING,
} rom_state;
static FIL fat_fil;
static uint32_t rom_addr;
static bool rom_FFFC;
static bool rom_FFFD;

static size_t rom_gets()
{
    if (!f_gets((char *)mbuf, MBUF_SIZE, &fat_fil))
        return 0;
    size_t len = strlen((char *)mbuf);
    if (mbuf[len - 1] == '\n')
        len--;
    if (mbuf[len - 1] == '\r')
        len--;
    return len;
}

static bool rom_open(const char *name)
{
    FRESULT result = f_open(&fat_fil, name, FA_READ | FA_WRITE);
    if (result != FR_OK)
    {
        printf("?Unable to open file (%d)\n", result);
        return false;
    }
    if (rom_gets() != 8 || strnicmp("#!RP6502", (char *)mbuf, 8))
    {
        printf("?Missing RP6502 ROM header\n");
        rom_reset();
        return false;
    }
    rom_FFFC = false;
    rom_FFFD = false;
    return true;
}

static bool rom_eof()
{
    return !!f_eof(&fat_fil);
}

static bool rom_read(uint32_t len, uint32_t crc)
{

    FRESULT result = f_read(&fat_fil, mbuf, len, &mbuf_len);
    if (result != FR_OK)
    {
        printf("?Unable to read file (%d)\n", result);
        return false;
    }
    if (len != mbuf_len)
    {
        printf("?Unable to read binary data\n");
        return false;
    }
    if (mbuf_crc32() != crc)
    {
        printf("?CRC failed\n");
        return false;
    }
    return true;
}

static bool rom_next_chunk()
{
    mbuf_len = 0;
    size_t len = rom_gets();
    for (size_t i = 0; i < len; i++)
        switch (mbuf[i])
        {
        case ' ':
            continue;
        case '#':
            return true;
        default:
            break;
        }

    uint32_t rom_len;
    uint32_t rom_crc;
    const char *args = (char *)mbuf;
    if (parse_uint32(&args, &len, &rom_addr) &&
        parse_uint32(&args, &len, &rom_len) &&
        parse_uint32(&args, &len, &rom_crc) &&
        parse_end(args, len))
    {
        if (rom_addr > 0xFFFF)
        {
            printf("?invalid address\n");
            return false;
        }
        if (!rom_len || rom_len > MBUF_SIZE || rom_addr + rom_len > 0x10000)
        {
            printf("?invalid length\n");
            return false;
        }
        if (rom_addr <= 0xFFFC && rom_addr + rom_len > 0xFFFC)
            rom_FFFC = true;
        if (rom_addr <= 0xFFFD && rom_addr + rom_len > 0xFFFD)
            rom_FFFD = true;
        return rom_read(rom_len, rom_crc);
    }
    printf("?Corrupt ROM file\n");
    return false;
}

static void rom_loading()
{
    if (rom_eof())
    {
        rom_reset();
        if (rom_FFFC && rom_FFFD)
            ria_reset();
        else
            printf("Loaded. No reset vector.\n");
        return;
    }
    if (!rom_next_chunk())
    {
        rom_reset();
        return;
    }
    if (mbuf_len)
    {
        rom_state = ROM_RIA_WRITING;
        act_ram_write(rom_addr);
    }
}

void rom_install(const char *args, size_t len)
{
    // Strip special extension, validate and upcase name
    char lfs_name[LFS_NAME_MAX + 1];
    size_t lfs_name_len = strlen(args);
    if (lfs_name_len > 7)
        if (!strnicmp(".RP6502", args + lfs_name_len - 7, 7))
            lfs_name_len -= 7;
    if (lfs_name_len > LFS_NAME_MAX)
        lfs_name_len = 0;
    while (lfs_name_len && args[lfs_name_len - 1] == ' ')
        lfs_name_len--;
    lfs_name[lfs_name_len] = 0; // strncpy is garbage
    strncpy(lfs_name, args, lfs_name_len);
    for (size_t i = 0; i < lfs_name_len; i++)
    {
        if (lfs_name[i] >= 'a' && lfs_name[i] <= 'z')
            lfs_name[i] -= 32;
        if (lfs_name[i] < 'A' || lfs_name[i] > 'Z')
            lfs_name_len = 0;
    }
    if (!lfs_name_len || mon_command_exists(args, len))
    {
        printf("?Invalid ROM name.\n");
        return;
    }

    // Test contents of file
    if (!rom_open(args))
        return;
    while (!rom_eof())
        if (!rom_next_chunk())
            return;
    if (!rom_FFFC || !rom_FFFD)
    {
        printf("?No reset vector.\n");
        return;
    }
    FRESULT fresult = f_rewind(&fat_fil);
    if (fresult != FR_OK)
    {
        printf("?Unable to rewind file (%d)\n", fresult);
        return;
    }

    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);

    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, lfs_name,
                                     LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_opencfg (%d)\n", lfsresult);
        return;
    }
    while (true)
    {
        fresult = f_read(&fat_fil, mbuf, MBUF_SIZE, &mbuf_len);
        if (fresult != FR_OK)
        {
            printf("?Unable to read file (%d)\n", fresult);
            break;
        }
        lfsresult = lfs_file_write(&lfs_volume, &lfs_file, mbuf, mbuf_len);
        if (lfsresult < 0)
        {
            printf("?Unable to lfs_file_write (%d)\n", lfsresult);
            break;
        }
        if (mbuf_len < MBUF_SIZE)
            break;
    }

    lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file,
                                  lfs_file_tell(&lfs_volume, &lfs_file));
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_truncate (%d)\n", lfsresult);
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_close (%d)\n", lfsresult);
    }
    fresult = f_close(&fat_fil);
    if (fresult != FR_OK)
    {
        printf("?Unable to close file (%d)\n", fresult);
    }
    if (fresult == FR_OK && lfsresult >= 0)
        printf("Installed %s.\n", lfs_name);
    else
        lfs_remove(&lfs_volume, lfs_name);
}

void rom_remove(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (parse_rom_name(&args, &len, lfs_name) &&
        parse_end(args, len))
    {
        int lfsresult = lfs_remove(&lfs_volume, lfs_name);
        if (lfsresult < 0)
        {
            printf("?Unable to lfs_remove (%d)\n", lfsresult);
        }
        return;
    }
    printf("?Invalid ROM name\n");
}

void rom_load(const char *args, size_t len)
{
    (void)(len);
    if (rom_open(args))
        rom_state = ROM_LOADING;
}

static bool rom_action_is_finished()
{
    if (ria_is_active())
        return false;
    int32_t result = act_result();
    switch (result)
    {
    case -1:
        return true;
        break;
    case -2:
        printf("?action watchdog timeout\n");
        break;
    default:
        printf("?verify error at $%04lX\n", result);
        break;
    }
    rom_reset();
    return false;
}

void rom_task()
{
    switch (rom_state)
    {
    case ROM_IDLE:
        break;
    case ROM_LOADING:
        rom_loading();
        break;
    case ROM_RIA_WRITING:
        if (rom_action_is_finished())
        {
            rom_state = ROM_RIA_VERIFYING;
            act_ram_verify(rom_addr);
        }
        break;
    case ROM_RIA_VERIFYING:
        if (rom_action_is_finished())
            rom_state = ROM_LOADING;
        break;
    }

    if (rom_state == ROM_IDLE && fat_fil.obj.fs)
    {
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
}

bool rom_is_active()
{
    return rom_state != ROM_IDLE;
}

void rom_reset()
{
    rom_state = ROM_IDLE;
}