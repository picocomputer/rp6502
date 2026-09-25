/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "core/api/api.h"
#include "osal/pico/errmap.h"
#include "core/str/oem.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "ria/mon/help.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria-w/net/cyw.h"
#include "core/sys/xram.h"
#include "ria/sys/mbuf.h"
#include "core/str/path.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "sys/path.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "osal/pico/lfs.h"
#include "osal/fs.h"
#include "core/rom/rom.h"
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/usb/usb.h"
#include <fatfs/ff.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

/* rom_pump_open and rom_pump_next take no buffer size and read up to
 * ROM_RECORD_MAX bytes into mbuf. */
_Static_assert(MBUF_SIZE >= ROM_RECORD_MAX, "mbuf is the RIA's record buffer");

static enum {
    ROM_IDLE,
    ROM_HELPING,
    ROM_LOADING,
    ROM_XRAM_WRITING,
    ROM_RIA_WRITING,
    ROM_RUNNING,
} rom_state;
static uint32_t rom_addr;
static uint32_t rom_len;
static rom_pump_t rom_pump = {.fd = -1};
static uint32_t help_pos;
static uint32_t help_end;


static void rom_written(bool ok)
{
    rom_state = ok ? ROM_LOADING : ROM_IDLE;
}

static void rom_loading(void)
{
    rom_record_t rec;
    api_errno err;
    switch (rom_pump_next(&rom_pump, mbuf, &rec, &err))
    {
    case ROM_PUMP_SKIP:
        return;
    case ROM_PUMP_ERROR:
        rom_state = ROM_IDLE;
        mon_add_response_errno(err);
        return;
    case ROM_PUMP_EOF:
        if (rom_pump_complete(&rom_pump))
        {
            if (usb_boot_enumerating())
                return;
            rom_asset_adopt(rom_pump.fd, rom_pump.assets_start);
            rom_pump.fd = -1;
            rom_state = ROM_RUNNING;
            fs_save_start();
            sys_run();
        }
        else
        {
            rom_state = ROM_IDLE;
            mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
        }
        return;
    case ROM_PUMP_RECORD:
        rom_addr = rec.addr;
        rom_len = rec.len;
        mbuf_len = rec.len;
        if (rom_addr > 0xFFFF)
            rom_state = ROM_XRAM_WRITING;
        else
        {
            rom_state = ROM_RIA_WRITING;
            ria_write_buf(rom_addr, rom_written);
        }
        return;
    }
}

static bool rom_copy_install_name(char *dst, const char *src, size_t len)
{
    size_t i;
    for (i = 0; src[i] && (!len || i < len); i++)
    {
        if (i >= LFS_NAME_MAX)
            return false;
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x80)
            return false;
        if (!isalpha(c) && !(i && isdigit(c)))
            return false;
        if (dst)
            dst[i] = (char)toupper(c);
    }
    if (dst)
        dst[i] = 0;
    return i > 0;
}

void rom_mon_install(const char *args)
{
    const char *args_start = args;
    const char *tok = str_parse_string(&args);
    if (!tok || *tok == ':')
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    char lfs_name[LFS_NAME_MAX + 1];
    const char *lfs_tok = str_parse_string(&args);
    if (lfs_tok)
    {
        // Optional second arg: explicit LFS ROM name
        if (!str_parse_end(args))
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
        if (!rom_copy_install_name(lfs_name, lfs_tok, 0))
        {
            mon_add_response_utf8(S(STR_ERR_ROM_NAME_INVALID));
            return;
        }
    }
    else
    {
        // Derive LFS ROM name from FAT filename
        const char *base = tok;
        for (const char *p = tok; *p; p++)
            if (*p == ':' || path_is_sep(*p))
                base = p + 1;
        size_t lfs_name_len = strlen(base);
        if (lfs_name_len > 7 && !strncasecmp(".RP6502", base + lfs_name_len - 7, 7))
            lfs_name_len -= 7;
        if (!rom_copy_install_name(lfs_name, base, lfs_name_len))
        {
            mon_add_response_utf8(S(STR_ERR_ROM_NAME_INVALID));
            return;
        }
    }
    // Test for system conflicts
    if (mon_command_exists(lfs_name) ||
        help_topic_exists(lfs_name))
    {
        mon_add_response_utf8(S(STR_ERR_ROM_NAME_INVALID));
        return;
    }
    // mon_command_exists and help_topic_exists nuke our string
    tok = str_parse_string(&args_start);
    rom_assets_reset();
    rom_pump_close(&rom_pump);
    api_errno err;
    if (!rom_pump_open(&rom_pump, tok, mbuf, &err))
    {
        mon_add_response_errno(err);
        return;
    }
    rom_record_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&rom_pump, mbuf, &rec, &err)) != ROM_PUMP_EOF)
        if (r == ROM_PUMP_ERROR)
        {
            mon_add_response_errno(err);
            rom_pump_close(&rom_pump);
            return;
        }
    if (!rom_pump_complete(&rom_pump))
    {
        mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
        rom_pump_close(&rom_pump);
        return;
    }
    int32_t landed;
    if (fs_std_lseek(rom_pump.fd, SEEK_SET, 0, &landed, &err) < 0)
    {
        mon_add_response_errno(err);
        rom_pump_close(&rom_pump);
        return;
    }
    char dest[1 + LFS_NAME_MAX + 1];
    snprintf(dest, sizeof dest, ":%s", lfs_name);
    int wr = fs_rom_open(dest, FS_WR | FS_CREAT | FS_EXCL, &err);
    if (wr < 0)
    {
        mon_add_response_errno(err);
        rom_pump_close(&rom_pump);
        return;
    }
    bool ok = true;
    for (;;)
    {
        uint32_t got = 0;
        if (fs_std_read(rom_pump.fd, (char *)mbuf, MBUF_SIZE, &got, &err) != STD_OK)
        {
            ok = false;
            break;
        }
        if (!got)
            break;
        uint32_t put = 0;
        if (fs_std_write(wr, (const char *)mbuf, got, &put, &err) != STD_OK ||
            put != got)
        {
            ok = false;
            break;
        }
    }
    if (fs_std_close(wr, &err) != STD_OK)
        ok = false;
    rom_pump_close(&rom_pump);
    if (!ok)
    {
        mon_add_response_errno(err);
        api_errno ignored;
        fs_rom_remove(dest, &ignored);
    }
}

void rom_mon_remove(const char *args)
{
    const char *tok = str_parse_string(&args);
    if (!tok || !str_parse_end(args))
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    char name[LFS_NAME_MAX + 1];
    if (!rom_copy_install_name(name, tok, 0))
    {
        mon_add_response_utf8(S(STR_ERR_ROM_NAME_INVALID));
        return;
    }
    const char *boot = rom_get_boot();
    boot = str_parse_string(&boot);
    if (boot && !strcasecmp(name, boot))
        rom_set_boot("");
    api_errno err;
    if (!fs_rom_remove(name, &err))
        mon_add_response_errno(err);
}

void rom_exec(void)
{
    const char *argv0 = arg_index(0);
    assert(argv0);
    if (*argv0 == ':')
    {
        if (!rom_copy_install_name(NULL, argv0 + 1, 0))
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }
    const char *filepath = path_abs(argv0);
    if (!filepath)
    {
        if (!strchr(argv0, ':'))
        {
            char cwd[256];
            FRESULT fr = f_getcwd(cwd, sizeof(cwd));
            if (fr != FR_OK)
            {
                mon_add_response_fatfs(fr);
                return;
            }
        }
        return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }
    char path[256];
    size_t flen = strlen(filepath);
    if (flen >= sizeof path)
        return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    memcpy(path, filepath, flen + 1);
    // Skip case correction for installed ROMs (live in flash, not on disk).
    if (*argv0 != ':' && !path_correct_basename(path, sizeof path))
        return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    if (!arg_replace(0, path))
        return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    /* An exec can run from a stop hook while sys_commit stops the previous
     * program, or after sys_stop has set sys_state to stopping and before
     * sys_commit runs the stop hooks, so that program's ROM descriptor can
     * still be open. There is only one ROM read descriptor, so it is closed
     * here before the open. */
    rom_assets_reset();
    rom_pump_close(&rom_pump);
    api_errno err;
    if (!rom_pump_open(&rom_pump, path, mbuf, &err))
    {
        mon_add_response_errno(err);
        return;
    }
    rom_state = ROM_LOADING;
}

void rom_load_argv(const char *argv0, const char *args)
{
    arg_clear();
    if (!arg_append(argv0))
        return mon_add_response_utf8(S(STR_ERR_ROM_ARGV_OVERFLOW));
    const char *arg;
    while ((arg = str_parse_string(&args)) != NULL)
        if (!arg_append(arg))
        {
            arg_clear();
            mon_add_response_utf8(S(STR_ERR_ROM_ARGV_OVERFLOW));
            return;
        }
    if (!str_parse_end(args))
    {
        arg_clear();
        mon_add_response_utf8(S(STR_ERR_ROM_ARGV_INVALID));
        return;
    }
    rom_exec();
}

void rom_mon_load(const char *args)
{
    const char *filename = str_parse_string(&args);
    if (!filename || *filename == ':')
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    rom_load_argv(filename, args);
}

bool rom_installed(const char *name, char *argv0)
{
    if (!rom_copy_install_name(argv0 + 1, name, 0))
        return false;
    argv0[0] = ':';
    struct lfs_info info;
    return lfs_stat(&lfs_volume, argv0 + 1, &info) >= 0 &&
           info.type == LFS_TYPE_REG;
}

bool rom_load_installed(const char *args)
{
    const char *tok = str_parse_string(&args);
    char argv0[1 + LFS_NAME_MAX + 1];
    if (!tok || !rom_installed(tok, argv0))
        return false;
    rom_load_argv(argv0, args);
    return true;
}

static int rom_utf8_seq_len(unsigned char b0)
{
    if (b0 < 0x80)
        return 1;
    if ((b0 & 0xE0) == 0xC0)
        return 2;
    if ((b0 & 0xF0) == 0xE0)
        return 3;
    if ((b0 & 0xF8) == 0xF0)
        return 4;
    return 1;
}

/* fs_std_read on this machine never returns STD_PENDING, because littlefs and
 * FatFs both read synchronously, so one call is enough. */
static bool help_read(uint32_t want, uint32_t *got)
{
    api_errno err;
    int32_t landed;
    if (fs_std_lseek(rom_asset_fd(), SEEK_SET, (int32_t)help_pos, &landed, &err) < 0)
        return false;
    return fs_std_read(rom_asset_fd(), (char *)mbuf, want, got, &err) == STD_OK;
}

static long help_gets(void)
{
    uint32_t got = 0;
    if (!help_read(MBUF_SIZE - 1, &got) || got == 0)
        return -1;
    size_t i = 0;
    while (i < got && mbuf[i] != '\n')
        i++;
    help_pos += (uint32_t)(i < got ? i + 1 : got);
    if (i && mbuf[i - 1] == '\r')
        i--;
    mbuf[i] = 0;
    return (long)i;
}

static int rom_help_response(char *buf, size_t buf_size, int state, unsigned)
{
    if (state < 0)
    {
        rom_state = ROM_IDLE;
        return state;
    }
    // Help asset format
    if (rom_asset_dir())
    {
        if (!state)
        {
            uint32_t base, asset_len;
            if (!rom_asset_find("help", &base, &asset_len))
            {
                mon_add_response_utf8(S(STR_ERR_NO_HELP_FOUND));
                rom_state = ROM_IDLE;
                return -1;
            }
            help_pos = base;
            help_end = base + asset_len;
            state = 1;
        }
        uint32_t remaining = help_end - help_pos;
        if (!remaining)
        {
            rom_state = ROM_IDLE;
            if (state == 1)
            {
                buf[0] = '\n';
                buf[1] = 0;
            }
            return -1;
        }
        uint32_t want = buf_size - 1;
        if (want > remaining)
            want = remaining;
        uint32_t got = 0;
        if (!help_read(want, &got))
        {
            mon_add_response_errno(API_EIO);
            rom_state = ROM_IDLE;
            return -1;
        }
        if (!got)
        {
            rom_state = ROM_IDLE;
            if (state == 1)
            {
                buf[0] = '\n';
                buf[1] = 0;
            }
            return -1;
        }
        // The zero after the data is not a UTF-8 continuation byte, so a
        // sequence cut off at the end of the help asset decodes as 0x7F and
        // leaves p at p_end.
        mbuf[got] = 0;
        size_t out = 0;
        const char *p = (const char *)mbuf;
        const char *p_end = (const char *)mbuf + got;
        while (out + 1 < buf_size && p < p_end)
        {
            int seq = rom_utf8_seq_len((unsigned char)*p);
            if (p + seq > p_end && help_pos + got < help_end)
                break;
            buf[out++] = (char)oem_from_utf8_next(&p);
        }
        help_pos += got - (uint32_t)(p_end - p);
        buf[out] = 0;
        return (out && buf[out - 1] == '\n') ? 2 : 1;
    }
    // Classic format: look for "# " comment lines
    long n = help_gets();
    if (n < 0)
    {
        if (!state)
            mon_add_response_utf8(S(STR_ERR_NO_HELP_FOUND));
        rom_state = ROM_IDLE;
        buf[0] = 0;
        return -1;
    }
    if (n && mbuf[0] == '#' && mbuf[1] == ' ')
    {
        snprintf(buf, buf_size, "%s\n", (char *)mbuf + 2);
        state = 1;
    }
    else
    {
        if (!state)
            mon_add_response_utf8(S(STR_ERR_NO_HELP_FOUND));
        rom_state = ROM_IDLE;
        return -1;
    }
    return state;
}

static bool rom_help_open(const char *path)
{
    rom_assets_reset();
    rom_pump_close(&rom_pump);
    api_errno err;
    if (!rom_pump_open(&rom_pump, path, mbuf, &err))
    {
        mon_add_response_errno(err);
        return false;
    }
    rom_asset_adopt(rom_pump.fd, rom_pump.assets_start);
    help_pos = rom_pump.pos;
    rom_pump.fd = -1;
    return true;
}

void rom_mon_info(const char *args)
{
    const char *tok = str_parse_string(&args);
    if (!tok || *tok == ':' || !str_parse_end(args))
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    if (rom_help_open(tok))
    {
        rom_state = ROM_HELPING;
        mon_add_response_fn(rom_help_response);
    }
}

void rom_mon_help(const char *args)
{
    const char *tok = str_parse_string(&args);
    char name[1 + LFS_NAME_MAX + 1];
    name[0] = ':';
    if (!tok || !rom_copy_install_name(name + 1, tok, 0) || !str_parse_end(args))
    {
        mon_add_response_utf8(S(STR_ERR_NO_HELP_FOUND));
        return;
    }
    if (rom_help_open(name))
    {
        rom_state = ROM_HELPING;
        mon_add_response_fn(rom_help_response);
        return;
    }
}

static bool rom_xram_done(void)
{
    while (rom_len && pix_ready())
    {
        uint32_t addr = rom_addr + --rom_len - 0x10000;
        xram[addr] = mbuf[rom_len];
        PIX_SEND_XRAM(addr, xram[addr]);
    }
    return !rom_len;
}

void __in_flash("rom_init") rom_init(void)
{
    // Try booting the set boot ROM
    const char *boot = rom_get_boot();
    rom_load_installed(boot);
}

void rom_task(void)
{
    switch (rom_state)
    {
    case ROM_IDLE:
        rom_pump_close(&rom_pump);
        rom_assets_reset();
        break;
    case ROM_HELPING:
    case ROM_RUNNING:
    case ROM_RIA_WRITING:
        break; // NOP
    case ROM_LOADING:
        rom_loading();
        break;
    case ROM_XRAM_WRITING:
        if (rom_xram_done())
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

void rom_stop(void)
{
    if (rom_state == ROM_RUNNING)
    {
        rom_state = ROM_IDLE;
    }
}

int rom_installed_response(char *buf, size_t buf_size, int state, unsigned)
{
    if (state < 0)
        return state;
    lfs_dir_t lfs_dir;
    struct lfs_info lfs_info;
    int lfsresult = lfs_dir_open(&lfs_volume, &lfs_dir, "/");
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        return -1;
    unsigned count = 0;
    unsigned valid_idx = 0;
    char found_name[LFS_NAME_MAX + 1] = {0};
    bool found = false;
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
        if (!is_ok)
            continue;
        if (state == 0)
        {
            count++;
        }
        else if (valid_idx == (unsigned)(state - 1))
        {
            size_t n = len;
            if (n > sizeof(found_name) - 1)
                n = sizeof(found_name) - 1;
            memcpy(found_name, lfs_info.name, n);
            found_name[n] = 0;
            found = true;
            break;
        }
        valid_idx++;
    }
    lfsresult = lfs_dir_close(&lfs_volume, &lfs_dir);
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        count = 0;
    if (state == 0)
    {
        if (count)
        {
            oem_snprintf(buf, buf_size,
                              count == 1 ? S(STR_ROM_INSTALLED_SINGULAR)
                                         : S(STR_ROM_INSTALLED_PLURAL),
                              count);
            return 1;
        }
        oem_snprintf(buf, buf_size, S(STR_ROM_INSTALLED_NONE));
        return -1;
    }
    if (found)
    {
        if (state == 1)
            snprintf(buf, buf_size, "%s", found_name);
        else
            snprintf(buf, buf_size, ", %s", found_name);
        return state + 1;
    }
    snprintf(buf, buf_size, ".\n");
    return -1;
}

bool rom_set_boot(const char *args)
{
    const char *p = args;
    const char *argv0 = str_parse_string(&p);
    if (!argv0)
    {
        if (!str_parse_end(args))
            return false;
        cfg_save_boot("");
        return true;
    }
    char installed[1 + LFS_NAME_MAX + 1];
    if (!rom_installed(argv0, installed))
        return false;
    while (!str_parse_end(p))
        if (!str_parse_string(&p))
            return false;
    cfg_save_boot(args);
    return true;
}

const char *rom_get_boot(void)
{
    return cfg_load_boot();
}
