/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "mon/rom.h"
#include "net/cyw.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/lfs.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include <fatfs/ff.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_ROM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static enum {
    ROM_IDLE,
    ROM_HELPING,
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
LFS_FILE_CONFIG(lfs_file_config, static);
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
        FRESULT fresult = f_open(&fat_fil, name, FA_READ);
        mon_add_response_fatfs(fresult);
        if (fresult != FR_OK)
            return false;
    }
    else
    {
        int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, name,
                                         LFS_O_RDONLY, &lfs_file_config);
        mon_add_response_lfs(lfsresult);
        if (lfsresult < 0)
            return false;
        lfs_file_open = true;
    }
    if (rom_gets() != 8 || strncasecmp("#!RP6502", (char *)mbuf, 8))
    {
        mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
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
        FRESULT fresult = f_read(&fat_fil, mbuf, len, &mbuf_len);
        mon_add_response_fatfs(fresult);
        if (fresult != FR_OK)
            return false;
    }
    else
    {
        lfs_ssize_t lfsresult = lfs_file_read(&lfs_volume, &lfs_file, mbuf, len);
        mon_add_response_lfs(lfsresult);
        if (lfsresult < 0)
            return false;
        mbuf_len = lfsresult;
    }
    if (len != mbuf_len)
    {
        mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
        return false;
    }
    if (ria_buf_crc32() != crc)
    {
        mon_add_response_str(STR_ERR_CRC);
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
    if (str_parse_uint32(&args, &len, &rom_addr) &&
        str_parse_uint32(&args, &len, &rom_len) &&
        str_parse_uint32(&args, &len, &rom_crc) &&
        str_parse_end(args, len))
    {
        if (rom_addr > 0x1FFFF)
        {
            mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
            return false;
        }
        if (!rom_len || rom_len > MBUF_SIZE ||
            (rom_addr < 0x10000 && rom_addr + rom_len > 0x10000) ||
            (rom_addr + rom_len > 0x20000))
        {
            mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
            return false;
        }
        if (rom_addr <= 0xFFFC && rom_addr + rom_len > 0xFFFC)
            rom_FFFC = true;
        if (rom_addr <= 0xFFFD && rom_addr + rom_len > 0xFFFD)
            rom_FFFD = true;
        return rom_read(rom_len, rom_crc);
    }
    mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
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
            mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
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
    if (!lfs_name_len)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    if (lfs_name_len > 7)
        if (!strncasecmp(".RP6502", args + lfs_name_len - 7, 7))
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
        hlp_topic_exists(lfs_name, lfs_name_len))
    {
        mon_add_response_str(STR_ERR_ROM_NAME_INVALID);
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
        mon_add_response_str(STR_ERR_ROM_DATA_INVALID);
        return;
    }
    FRESULT fresult = f_rewind(&fat_fil);
    mon_add_response_fatfs(fresult);
    if (fresult != FR_OK)
        return;
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, lfs_name,
                                     LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL,
                                     &lfs_file_config);
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        return;
    lfs_file_open = true;
    while (true)
    {
        fresult = f_read(&fat_fil, mbuf, MBUF_SIZE, &mbuf_len);
        mon_add_response_fatfs(fresult);
        if (fresult != FR_OK)
            break;
        lfsresult = lfs_file_write(&lfs_volume, &lfs_file, mbuf, mbuf_len);
        mon_add_response_lfs(lfsresult);
        if (lfsresult < 0)
            break;
        if (mbuf_len < MBUF_SIZE)
            break;
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    mon_add_response_lfs(lfscloseresult);
    lfs_file_open = false;
    if (lfsresult >= 0)
        lfsresult = lfscloseresult;
    fresult = f_close(&fat_fil);
    fat_fil.obj.fs = NULL;
    mon_add_response_fatfs(fresult);
    if (fresult != FR_OK || lfsresult < 0)
        lfs_remove(&lfs_volume, lfs_name);
}

void rom_mon_remove(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (str_parse_rom_name(&args, &len, lfs_name) &&
        str_parse_end(args, len))
    {
        const char *boot = rom_get_boot();
        if (!strcmp(lfs_name, boot))
            rom_set_boot("");
        int lfsresult = lfs_remove(&lfs_volume, lfs_name);
        mon_add_response_lfs(lfsresult);
        return;
    }
    mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
}

void rom_mon_load(const char *args, size_t len)
{
    (void)(len);
    if (rom_open(args, true))
        rom_state = ROM_LOADING;
}

static bool rom_is_installed(const char *name)
{
    struct lfs_info info;
    return lfs_stat(&lfs_volume, name, &info) >= 0;
}

bool rom_load_installed(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (!str_parse_rom_name(&args, &len, lfs_name) ||
        !str_parse_end(args, len) ||
        !rom_is_installed(lfs_name) ||
        !rom_open(lfs_name, false))
        return false;
    rom_state = ROM_LOADING;
    return true;
}

static int rom_help_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
    {
        rom_state = ROM_IDLE;
        return state;
    }
    if (rom_gets() && mbuf[0] == '#' && mbuf[1] == ' ')
    {
        snprintf(buf, buf_size, "%s\n", (char *)mbuf + 2);
        state = 1;
    }
    else
    {
        if (!state)
            mon_add_response_str(STR_ERR_NO_HELP_FOUND);
        rom_state = ROM_IDLE;
        return -1;
    }
    return state;
}

void rom_mon_info(const char *args, size_t len)
{
    (void)(len);
    if (rom_open(args, true))
    {
        rom_state = ROM_HELPING;
        mon_add_response_fn(rom_help_response);
    }
}

void rom_mon_help(const char *args, size_t len)
{
    struct lfs_info info;
    char lfs_name[LFS_NAME_MAX + 1];
    if (str_parse_rom_name(&args, &len, lfs_name) &&
        str_parse_end(args, len) &&
        lfs_stat(&lfs_volume, lfs_name, &info) >= 0 &&
        rom_open(lfs_name, false))
    {
        rom_state = ROM_HELPING;
        mon_add_response_fn(rom_help_response);
    }
    else
        mon_add_response_str(STR_ERR_NO_HELP_FOUND);
}

static bool rom_action_is_finished(void)
{
    if (ria_active())
        return false;
    if (ria_handle_error())
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
    const char *boot = rom_get_boot();
    rom_load_installed(boot, strlen(boot));
}

void rom_task(void)
{
    switch (rom_state)
    {
    case ROM_IDLE:
        if (lfs_file_open)
        {
            int lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
            mon_add_response_lfs(lfsresult);
            lfs_file_open = false;
        }
        if (fat_fil.obj.fs)
        {
            FRESULT fresult = f_close(&fat_fil);
            fat_fil.obj.fs = NULL;
            mon_add_response_fatfs(fresult);
        }
        break;
    case ROM_HELPING:
        break; // NOP
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
}

bool rom_active(void)
{
    return rom_state != ROM_IDLE;
}

void rom_break(void)
{
    rom_state = ROM_IDLE;
}

int rom_installed_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
        return state;
    const uint32_t WIDTH = 79; // some terms wrap at 80
    uint32_t count = 0;
    int line = 1;
    uint32_t col = 0;
    lfs_dir_t lfs_dir;
    struct lfs_info lfs_info;
    int lfsresult = lfs_dir_open(&lfs_volume, &lfs_dir, "/");
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        return -1;
    while (true)
    {
        lfsresult = lfs_dir_read(&lfs_volume, &lfs_dir, &lfs_info);
        mon_add_response_lfs(lfsresult);
        if (!lfsresult)
            break;
        if (lfsresult < 0)
        {
            count = 0;
            break;
        }
        bool is_ok = true;
        size_t len = strlen(lfs_info.name);
        for (size_t i = 0; i < len; i++)
        {
            char ch = lfs_info.name[i];
            if (!(i && isdigit(ch)) && !isupper(ch))
                is_ok = false;
        }
        if (is_ok && state)
        {
            if (count)
            {
                if (state == line)
                    buf[col] = ',';
                col += 1;
            }
            if (col + len > WIDTH - 2)
            {
                if (state == line)
                {
                    buf[col] = '\n';
                    buf[++col] = 0;
                }
                line += 1;
                if (state == line)
                    sprintf(buf, "%s", lfs_info.name);
                col = len;
            }
            else
            {
                if (col)
                {
                    if (state == line)
                        buf[col] = ' ';
                    col += 1;
                }
                if (state == line)
                    sprintf(buf + col, "%s", lfs_info.name);
                col += len;
            }
        }
        if (is_ok)
            count++;
    }
    if (state == line)
    {
        if (count)
            buf[col++] = '.';
        buf[col] = '\n';
        buf[++col] = 0;
        state = -2;
    }
    lfsresult = lfs_dir_close(&lfs_volume, &lfs_dir);
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        count = 0;
    if (!state)
    {
        if (count)
        {
            snprintf(buf, buf_size,
                     count == 1 ? STR_ROM_INSTALLED_SINGULAR
                                : STR_ROM_INSTALLED_PLURAL,
                     count);
        }
        else
        {
            snprintf(buf, buf_size, STR_ROM_INSTALLED_NONE);
            state = -2;
        }
    }
    return state + 1;
}

bool rom_set_boot(char *str)
{
    if (str[0] && !rom_is_installed(str))
        return false;
    cfg_save_boot(str);
    return true;
}

const char *rom_get_boot(void)
{
    return cfg_load_boot();
}
