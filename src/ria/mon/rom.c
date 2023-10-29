/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "str.h"
#include "api/api.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "sys/cfg.h"
#include "sys/lfs.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "fatfs/ff.h"

static enum {
    ROM_IDLE,
    ROM_LOADING,
    ROM_XRAM_WRITING,
    ROM_RIA_WRITING,
    ROM_RIA_VERIFYING,
} rom_state;
static uint32_t rom_addr;
static uint32_t rom_len;
static bool rom_FFFC;
static bool rom_FFFD;
static bool is_reading_fat;
static bool lfs_file_open;
static lfs_file_t lfs_file;
static LFS_FILE_CONFIG(lfs_file_config);
static FIL fat_fil;

static size_t rom_gets(void)
{
    size_t len;
    if (is_reading_fat)
    {
        if (!f_gets((char *)mbuf, MBUF_SIZE, &fat_fil))
            return 0;
    }
    else
    {
        if (!lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file))
            return 0;
    }
    len = strlen((char *)mbuf);
    if (len && mbuf[len - 1] == '\n')
        len--;
    if (len && mbuf[len - 1] == '\r')
        len--;
    mbuf[len] = 0;
    return len;
}

static bool rom_open(const char *name, bool is_fat)
{
    is_reading_fat = is_fat;
    if (is_fat)
    {
        FRESULT result = f_open(&fat_fil, name, FA_READ);
        if (result != FR_OK)
        {
            printf("?Unable to open file (%d)\n", result);
            return false;
        }
    }
    else
    {
        int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, name,
                                         LFS_O_RDONLY, &lfs_file_config);
        if (lfsresult < 0)
        {
            printf("?Unable to lfs_file_opencfg (%d)\n", lfsresult);
            return false;
        }
        lfs_file_open = true;
    }
    if (rom_gets() != 8 || strnicmp("#!RP6502", (char *)mbuf, 8))
    {
        printf("?Missing RP6502 ROM header\n");
        rom_state = ROM_IDLE;
        return false;
    }
    rom_FFFC = false;
    rom_FFFD = false;
    return true;
}

static bool rom_eof(void)
{
    if (is_reading_fat)
        return !!f_eof(&fat_fil);
    else
        return !!lfs_eof(&lfs_file);
}

static bool rom_read(uint32_t len, uint32_t crc)
{
    if (is_reading_fat)
    {
        FRESULT result = f_read(&fat_fil, mbuf, len, &mbuf_len);
        if (result != FR_OK)
        {
            printf("?Unable to read file (%d)\n", result);
            return false;
        }
    }
    else
    {
        lfs_ssize_t lfsresult = lfs_file_read(&lfs_volume, &lfs_file, mbuf, len);
        if (lfsresult < 0)
        {
            printf("?Unable to lfs_file_read (%ld)\n", lfsresult);
            return false;
        }
        mbuf_len = lfsresult;
    }
    if (len != mbuf_len)
    {
        printf("?Unable to read binary data\n");
        return false;
    }
    if (ria_buf_crc32() != crc)
    {
        printf("?CRC failed\n");
        return false;
    }
    return true;
}

static bool rom_next_chunk(void)
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
    uint32_t rom_crc;
    const char *args = (char *)mbuf;
    if (parse_uint32(&args, &len, &rom_addr) &&
        parse_uint32(&args, &len, &rom_len) &&
        parse_uint32(&args, &len, &rom_crc) &&
        parse_end(args, len))
    {
        if (rom_addr > 0x1FFFF)
        {
            printf("?invalid address\n");
            return false;
        }
        if (!rom_len || rom_len > MBUF_SIZE ||
            (rom_addr < 0x10000 && rom_addr + rom_len > 0x10000) ||
            (rom_addr + rom_len > 0x20000))
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

static void rom_loading(void)
{
    if (rom_eof())
    {
        rom_state = ROM_IDLE;
        if (rom_FFFC && rom_FFFD)
            main_run();
        else
            printf("Loaded. No reset vector.\n");
        return;
    }
    if (!rom_next_chunk())
    {
        rom_state = ROM_IDLE;
        return;
    }
    if (mbuf_len)
    {
        if (rom_addr > 0xFFFF)
            rom_state = ROM_XRAM_WRITING;
        else
        {
            rom_state = ROM_RIA_WRITING;
            ria_write_buf(rom_addr);
        }
    }
}

void rom_mon_install(const char *args, size_t len)
{
    // Strip special extension, validate and upcase name
    char lfs_name[LFS_NAME_MAX + 1];
    size_t lfs_name_len = len;
    while (lfs_name_len && args[lfs_name_len - 1] == ' ')
        lfs_name_len--;
    if (lfs_name_len > 7)
        if (!strnicmp(".RP6502", args + lfs_name_len - 7, 7))
            lfs_name_len -= 7;
    if (lfs_name_len > LFS_NAME_MAX)
        lfs_name_len = 0;
    lfs_name[lfs_name_len] = 0;
    memcpy(lfs_name, args, lfs_name_len);
    for (size_t i = 0; i < lfs_name_len; i++)
    {
        if (lfs_name[i] >= 'a' && lfs_name[i] <= 'z')
            lfs_name[i] -= 32;
        if (lfs_name[i] >= 'A' && lfs_name[i] <= 'Z')
            continue;
        if (i && lfs_name[i] >= '0' && lfs_name[i] <= '9')
            continue;
        lfs_name_len = 0;
    }
    // Test for system conflicts
    if (!lfs_name_len ||
        mon_command_exists(lfs_name, lfs_name_len) ||
        help_text_lookup(lfs_name, lfs_name_len))
    {
        printf("?Invalid ROM name.\n");
        return;
    }

    // Test contents of file
    if (!rom_open(args, true))
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
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, lfs_name,
                                     LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        if (lfsresult == LFS_ERR_EXIST)
            printf("?ROM already exists.\n");
        else
            printf("?Unable to lfs_file_opencfg (%d)\n", lfsresult);
        return;
    }
    lfs_file_open = true;
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
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    lfs_file_open = false;
    if (lfscloseresult < 0)
        printf("?Unable to lfs_file_close (%d)\n", lfscloseresult);
    if (lfsresult >= 0)
        lfsresult = lfscloseresult;
    fresult = f_close(&fat_fil);
    if (fresult != FR_OK)
        printf("?Unable to f_close file (%d)\n", fresult);
    if (fresult == FR_OK && lfsresult >= 0)
        printf("Installed %s.\n", lfs_name);
    else
        lfs_remove(&lfs_volume, lfs_name);
}

void rom_mon_remove(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (parse_rom_name(&args, &len, lfs_name) &&
        parse_end(args, len))
    {
        const char *boot = cfg_get_boot();
        if (!strcmp(lfs_name, boot))
        {
            printf("?Unable to remove boot ROM\n");
            return;
        }
        int lfsresult = lfs_remove(&lfs_volume, lfs_name);
        if (lfsresult < 0)

            printf("?Unable to lfs_remove (%d)\n", lfsresult);
        else
            printf("Removed %s.\n", lfs_name);
        return;
    }
    printf("?Invalid ROM name\n");
}

void rom_mon_load(const char *args, size_t len)
{
    (void)(len);
    if (rom_open(args, true))
        rom_state = ROM_LOADING;
}

bool rom_load(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (parse_rom_name(&args, &len, lfs_name) &&
        parse_end(args, len))
    {
        struct lfs_info info;
        if (lfs_stat(&lfs_volume, lfs_name, &info) < 0)
            return false;
        if (rom_open(lfs_name, false))
            rom_state = ROM_LOADING;
        return true;
    }
    return false;
}

void rom_mon_info(const char *args, size_t len)
{
    (void)(len);
    if (!rom_open(args, true))
        return;
    bool found = false;
    while (rom_gets() && mbuf[0] == '#' && mbuf[1] == ' ')
    {
        puts((char *)mbuf + 2);
        found = true;
    }
    if (!found)
        puts("?No help found in file.");
}

// Returns false and prints nothing if ROM not found.
// Something will always print before returning true.
bool rom_help(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (parse_rom_name(&args, &len, lfs_name) &&
        parse_end(args, len))
    {
        struct lfs_info info;
        if (lfs_stat(&lfs_volume, lfs_name, &info) < 0)
            return false;
        bool found = false;
        if (rom_open(lfs_name, false))
            while (rom_gets() && mbuf[0] == '#' && mbuf[1] == ' ')
            {
                puts((char *)mbuf + 2);
                found = true;
            }
        if (!found)
            puts("?No help found in ROM.");
        return true; // even when !found
    }
    return false;
}

static bool rom_action_is_finished(void)
{
    if (ria_active())
        return false;
    if (ria_print_error_message())
    {
        rom_state = ROM_IDLE;
        return false;
    }
    return true;
}

static bool rom_xram_writing(void)
{
    while (rom_len && pix_ready())
    {
        uint32_t addr = rom_addr + --rom_len - 0x10000;
        xram[addr] = mbuf[rom_len];
        PIX_SEND_XRAM(addr, xram[addr]);
    }
    return !!rom_len;
}

void rom_init(void)
{
    // Try booting the set boot ROM
    char *boot = cfg_get_boot();
    size_t boot_len = strlen(boot);
    rom_load((char *)boot, boot_len);
}

void rom_task(void)
{
    switch (rom_state)
    {
    case ROM_IDLE:
        break;
    case ROM_LOADING:
        rom_loading();
        break;
    case ROM_XRAM_WRITING:
        if (!rom_xram_writing())
            rom_state = ROM_LOADING;
        break;
    case ROM_RIA_WRITING:
        if (rom_action_is_finished())
        {
            rom_state = ROM_RIA_VERIFYING;
            ria_verify_buf(rom_addr);
        }
        break;
    case ROM_RIA_VERIFYING:
        if (rom_action_is_finished())
            rom_state = ROM_LOADING;
        break;
    }

    if (rom_state == ROM_IDLE && lfs_file_open)
    {
        int lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
        lfs_file_open = false;
        if (lfsresult < 0)
            printf("?Unable to lfs_file_close (%d)\n", lfsresult);
    }

    if (rom_state == ROM_IDLE && fat_fil.obj.fs)
    {
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
}

bool rom_active(void)
{
    return rom_state != ROM_IDLE;
}

void rom_reset(void)
{
    rom_state = ROM_IDLE;
}
