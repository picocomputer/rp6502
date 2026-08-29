/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys.h"
#include "core/ria.h"
#include "ria/main.h"
#include "core/api/api.h"
#include "api/errmap.h"
#include "core/str/oem.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/api/std.h"
#include "ria/mon/help.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria/net/cyw.h"
#include "ria/sys/mem.h"
#include "core/str/rln.h"
#include "core/str/str.h"
#include "core/rom/rom_rec.h"
#include "core/rom/rom_win.h"
#include "sys/path.h"
#include "ria/sys/com.h"
#include "ria/sys/cfg.h"
#include "ria/sys/lfs.h"
#include "core/api/fs.h"
#include "core/rom/rom.h" /* the pump: the loader half of this file now reads through the seam */
#include "ria/sys/pix.h"
#include "ria/sys/ria.h"
#include "ria/usb/usb.h"
#include <fatfs/ff.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#if defined(DEBUG_MON) || defined(DEBUG_MON_ROM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
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
    ROM_RUNNING,
} rom_state;
static uint32_t rom_addr;
static uint32_t rom_len;
static rom_rec_vectors_t rom_vectors;
/* The loader's pump over the seam's ROM descriptor. fd -1 when no load is in
 * flight; the old handles below serve HELPING and INSTALL until they retire. */
static rom_pump_t rom_pump = {.fd = -1};
/* HELPING's own cursor over the adopted descriptor. */
static uint32_t help_pos;
static uint32_t help_end;
static bool lfs_file_open;
static lfs_file_t lfs_file;
LFS_FILE_CONFIG(lfs_file_config, static);
static FIL fat_fil;
static uint32_t rom_end_pos;
static uint32_t rom_assets_start;

// Read one line into mbuf (CR/LF stripped). Returns the line length, 0 at EOF
// (or a blank line); *err gets the I/O error (>0 FRESULT, <0 lfs) or 0.
static size_t rom_gets(int *err)
{
    *err = 0;
    mbuf[0] = 0;
    if (fat_fil.obj.fs)
    {
        if (!f_gets((char *)mbuf, MBUF_SIZE, &fat_fil))
        {
            *err = (int)f_error(&fat_fil);
            return 0;
        }
    }
    else
    {
        if (!lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file, err))
            return 0;
    }
    size_t len = strlen((char *)mbuf);
    if (len && mbuf[len - 1] == '\n')
        len--;
    if (len && mbuf[len - 1] == '\r')
        len--;
    mbuf[len] = 0;
    return len;
}

static uint32_t rom_ftell(void)
{
    if (fat_fil.obj.fs)
        return (uint32_t)f_tell(&fat_fil);
    lfs_soff_t p = lfs_file_tell(&lfs_volume, &lfs_file);
    return p >= 0 ? (uint32_t)p : 0;
}

static void rom_close(void)
{
    if (lfs_file_open)
    {
        lfs_file_close(&lfs_volume, &lfs_file);
        lfs_file_open = false;
    }
    if (fat_fil.obj.fs)
    {
        f_close(&fat_fil);
        fat_fil.obj.fs = NULL;
    }
}

// Seek the open ROM file. *err (NULL-tolerant) gets the I/O error on failure.
static bool rom_fseek_to(uint32_t pos, int *err)
{
    if (err)
        *err = 0;
    if (fat_fil.obj.fs)
    {
        FRESULT fr = f_lseek(&fat_fil, (FSIZE_t)pos);
        if (fr != FR_OK)
        {
            if (err)
                *err = (int)fr;
            return false;
        }
        return true;
    }
    lfs_soff_t r = lfs_file_seek(&lfs_volume, &lfs_file, (lfs_soff_t)pos, LFS_SEEK_SET);
    if (r < 0)
    {
        if (err)
            *err = (int)r;
        return false;
    }
    return true;
}


static void rom_report_io(int err)
{
    mon_add_response_fatfs(err);
    mon_add_response_lfs(err);
}

static bool rom_open(const char *path)
{
    if (*path != ':')
    {
        FRESULT fresult = f_open(&fat_fil, path, FA_READ);
        mon_add_response_fatfs(fresult);
        if (fresult != FR_OK)
            return false;
    }
    else
    {
        const char *lfs_path = path + 1;
        int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, lfs_path,
                                         LFS_O_RDONLY, &lfs_file_config);
        mon_add_response_lfs(lfsresult);
        if (lfsresult < 0)
            return false;
        lfs_file_open = true;
    }
    int err;
    size_t n = rom_gets(&err);
    if (err)
    {
        rom_report_io(err);
        return false;
    }
    if (n != 8 || strncasecmp("#!RP6502", (char *)mbuf, 8))
        goto invalid;
    uint32_t after_shebang = rom_ftell();
    size_t line2_len = rom_gets(&err);
    if (err)
    {
        rom_report_io(err);
        return false;
    }
    if (line2_len >= 2 && mbuf[0] == '#' && mbuf[1] == '>')
    {
        // New format: parse null-asset header "#>len crc"
        const char *p = (const char *)mbuf + 2;
        uint32_t chunks_len, unused_image_crc;
        if (!str_parse_uint32(&p, &chunks_len) ||
            !str_parse_uint32(&p, &unused_image_crc))
            goto invalid;
        (void)unused_image_crc;
        rom_end_pos = rom_ftell() + chunks_len;
        rom_assets_start = after_shebang;
    }
    else
    {
        rom_end_pos = 0;
        rom_assets_start = 0;
        // Seek back so classic parsing starts from line 2
        if (!rom_fseek_to(after_shebang, &err))
        {
            rom_report_io(err);
            return false;
        }
    }
    rom_vectors = (rom_rec_vectors_t){0};
    return true;
invalid:
    mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
    return false;
}

static bool rom_at_eof(void)
{
    if (rom_end_pos)
        return rom_ftell() >= rom_end_pos;
    return fat_fil.obj.fs ? f_eof(&fat_fil)
                          : lfs_eof(&lfs_volume, &lfs_file);
}

static bool rom_read(uint32_t len, uint32_t crc)
{
    if (fat_fil.obj.fs)
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
        mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
        return false;
    }
    if (mem_crc32(0, mbuf, mbuf_len) != crc)
    {
        mon_add_response_utf8(S(STR_ERR_CRC));
        return false;
    }
    return true;
}

static bool rom_next_chunk(void)
{
    mbuf_len = 0;
    int err;
    rom_gets(&err);
    if (err)
    {
        rom_report_io(err);
        return false;
    }
    /* A record must fit mbuf whole: this machine stages it there before the
     * bus carries it across. */
    rom_rec_t rec;
    rom_rec_result r = rom_rec_parse((char *)mbuf, MBUF_SIZE, &rec);
    if (r == ROM_REC_SKIP)
        return true; /* a blank line or a comment */
    if (r != ROM_REC_OK)
    {
        mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
        return false;
    }
    rom_addr = rec.addr;
    rom_len = rec.len;
    if (!rom_read(rom_len, rec.crc))
        return false;
    /* After the read, not before: a record that arrives truncated or fails its
     * CRC cannot vouch for the vector it was carrying. */
    rom_rec_note(&rom_vectors, &rec);
    return true;
}

static void rom_loading(void)
{
    rom_rec_t rec;
    api_errno err;
    switch (rom_pump_next(&rom_pump, mbuf, &rec, &err))
    {
    case ROM_PUMP_SKIP:
        return; /* a blank line or a comment: one per pass, like a record */
    case ROM_PUMP_ERROR:
        rom_state = ROM_IDLE;
        mon_add_response_errno(err);
        return;
    case ROM_PUMP_EOF:
        if (rom_pump_complete(&rom_pump))
        {
            if (usb_boot_enumerating())
                return;
            /* The ROM: driver reads the running program's assets through
             * the descriptor the load leaves behind. */
            rom_asset_adopt(rom_pump.fd, rom_pump.assets_start);
            rom_pump.fd = -1;
            rom_state = ROM_RUNNING;
            sys_run();
        }
        else
        {
            rom_state = ROM_IDLE;
            mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
        }
        return;
    case ROM_PUMP_RECORD:
        /* The record is staged in mbuf; the bus carries it from there. */
        rom_addr = rec.addr;
        rom_len = rec.len;
        mbuf_len = rec.len;
        if (rom_addr > 0xFFFF)
            rom_state = ROM_XRAM_WRITING;
        else
        {
            rom_state = ROM_RIA_WRITING;
            ria_write_buf(rom_addr);
        }
        return;
    }
}

// Copy, uppercase, and validate an installed ROM name. len=0 means no length cap.
// Pass dst=NULL to validate without copying.
// ASCII letters only (digits allowed after the first char); rejects any byte >= 0x80
// so installed names are always portable across code pages.
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
        size_t lfs_name_len = strlen(tok);
        if (lfs_name_len > 7 && !strncasecmp(".RP6502", tok + lfs_name_len - 7, 7))
            lfs_name_len -= 7;
        if (!rom_copy_install_name(lfs_name, tok, lfs_name_len))
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
    if (!rom_open(tok))
        return;
    while (!rom_at_eof())
        if (!rom_next_chunk())
            return;
    if (!rom_rec_complete(&rom_vectors))
    {
        mon_add_response_utf8(S(STR_ERR_ROM_DATA_INVALID));
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
    int lfsresult = lfs_remove(&lfs_volume, name);
    mon_add_response_lfs(lfsresult);
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
    rom_close();
    rom_pump_close(&rom_pump);
    api_errno err;
    if (!rom_pump_open(&rom_pump, path, &err))
    {
        mon_add_response_errno(err);
        return;
    }
    rom_state = ROM_LOADING;
}

static void rom_load_argv(const char *argv0, const char *args)
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

bool rom_load_installed(const char *args)
{
    const char *tok = str_parse_string(&args);
    if (!tok)
        return false;
    char name[LFS_NAME_MAX + 1];
    if (!rom_copy_install_name(name, tok, 0))
        return false;
    struct lfs_info info;
    if (lfs_stat(&lfs_volume, name, &info) < 0)
        return false;
    char rom_argv0[1 + LFS_NAME_MAX + 1];
    snprintf(rom_argv0, sizeof(rom_argv0), ":%s", name);
    rom_load_argv(rom_argv0, args);
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
    return 1; // invalid lead — oem_from_utf8_next returns 0x7F
}

/* A read at help_pos on the adopted descriptor, clamped to what remains;
 * both stores answer synchronously here. */
static bool help_read(uint32_t want, uint32_t *got)
{
    api_errno err;
    int32_t landed;
    if (fs_std_lseek(rom_asset_fd(), SEEK_SET, (int32_t)help_pos, &landed, &err) < 0)
        return false;
    return fs_std_read(rom_asset_fd(), (char *)mbuf, want, got, &err) == STD_OK;
}

/* One text line at help_pos (CR/LF stripped) into mbuf; length, or -1 at
 * EOF. The classic format's comment scan. */
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

// state encoding: 0 = initial, 1 = streaming (last OEM byte != '\n'),
// 2 = streaming (last OEM byte == '\n', no trailing newline needed at EOF).
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
        // Sentinel: if oem_from_utf8_next reads past p_end into this 0, it sees a
        // non-continuation byte and returns 0x7F without UB.
        mbuf[got] = 0;
        size_t out = 0;
        const char *p = (const char *)mbuf;
        const char *p_end = (const char *)mbuf + got;
        while (out + 1 < buf_size && p < p_end)
        {
            int seq = rom_utf8_seq_len((unsigned char)*p);
            if (p + seq > p_end && help_pos + got < help_end)
                break; // partial glyph — re-read on next call
            buf[out++] = (char)oem_from_utf8_next(&p);
        }
        help_pos += got - (uint32_t)(p_end - p); /* leftover re-reads next call */
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

/* Open path for INFO and HELP <rom>: the pump validates the image and the
 * asset driver adopts the descriptor, exactly as a load does -- HELPING is a
 * load that stops after the directory. */
static bool rom_help_open(const char *path)
{
    rom_assets_reset();
    rom_pump_close(&rom_pump);
    api_errno err;
    if (!rom_pump_open(&rom_pump, path, &err))
    {
        mon_add_response_errno(err);
        return false;
    }
    rom_asset_adopt(rom_pump.fd, rom_pump.assets_start);
    help_pos = rom_pump.pos; /* classic: the line after the shebang */
    rom_pump.fd = -1; /* the driver owns it now */
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

static bool rom_action_can_proceed(void)
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
        rom_close();
        rom_pump_close(&rom_pump);
        rom_assets_reset();
        break;
    case ROM_HELPING:
    case ROM_RUNNING:
        break; // NOP
    case ROM_LOADING:
        rom_loading();
        break;
    case ROM_XRAM_WRITING:
        if (rom_xram_done())
            rom_state = ROM_LOADING;
        break;
    case ROM_RIA_WRITING:
        if (rom_action_can_proceed())
        {
            rom_state = ROM_RIA_VERIFYING;
            ria_verify_buf(rom_addr);
        }
        break;
    case ROM_RIA_VERIFYING:
        if (rom_action_can_proceed())
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
            com_snprintf_utf8(buf, buf_size,
                              count == 1 ? S(STR_ROM_INSTALLED_SINGULAR)
                                         : S(STR_ROM_INSTALLED_PLURAL),
                              count);
            return 1;
        }
        com_snprintf_utf8(buf, buf_size, S(STR_ROM_INSTALLED_NONE));
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
    char buf[LFS_NAME_MAX + 1];
    if (!rom_copy_install_name(buf, argv0, 0))
        return false;
    struct lfs_info info;
    if (lfs_stat(&lfs_volume, buf, &info) < 0)
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
