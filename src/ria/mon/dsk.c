/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/dsk.h"
#include "mon/mon.h"
#include "str/str.h"
#include "str/rln.h"
#include "sys/mem.h"
#include "usb/msc.h"
#include "usb/usb.h"
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_DSK)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Runtime FF_MIN_GPT hook. The default reproduces the stock threshold; the disk
// tool forces MBR or GPT around f_mkfs()/f_fdisk(). Only the GPT-capable
// (exFAT/LBA64) build references it.
#if FF_LBA64
#define DSK_GPT_DEFAULT 0x10000000
static LBA_t dsk_gpt_threshold = DSK_GPT_DEFAULT;
LBA_t dsk_min_gpt(void) { return dsk_gpt_threshold; }
#endif

enum
{
    DSK_SCHEME_MBR,
    DSK_SCHEME_GPT,
    DSK_SCHEME_NONE,
};

static enum
{
    DSK_IDLE,
    DSK_CONFIRM,
    DSK_RUN_FORMAT_UNIT,
    DSK_RUN_MKFS,
    DSK_RUN_ZERO,
    DSK_RUN_VERIFY,
    DSK_RUN_PART,
} dsk_state;

// Operation context, valid from confirm through completion.
static int dsk_after_confirm;     // state to enter when the user types YES
static uint8_t dsk_vol;           // logical volume (MSCn:)
static uint8_t dsk_pdrv;          // physical drive backing dsk_vol
static char dsk_path[6];          // "MSCn:" for FatFs calls
static bool dsk_is_floppy;
static uint8_t dsk_fs;            // 0=auto, 1=FAT, 2=exFAT
static bool dsk_full;             // /full low-level format
static uint32_t dsk_au;           // allocation unit bytes (0 = auto)
static bool dsk_has_label;
static char dsk_label_oem[12];
static uint8_t dsk_scheme;        // DSK_SCHEME_* for part
static bool dsk_fmt_started;      // FORMAT UNIT issued (poll phase)
static uint32_t dsk_block_size;
static uint64_t dsk_total;        // sectors for zero/verify
static uint64_t dsk_lba;          // current sector
static int dsk_last_pct;
static uint32_t dsk_bad;          // verify bad-sector count

// Copy a drive token into dsk_path as "MSCn:".
static void dsk_set_path(const char *tok)
{
    size_t n = 0;
    while (tok[n] && tok[n] != ':' && n < sizeof(dsk_path) - 2)
    {
        dsk_path[n] = tok[n];
        n++;
    }
    dsk_path[n++] = ':';
    dsk_path[n] = '\0';
}

// Parse an allocation-unit option like "/16k" or "/512". Returns false unless
// it is a power of two from 512 bytes to 16 MiB.
static bool dsk_parse_alloc(const char *tok, uint32_t *au)
{
    const char *p = tok + 1; // skip '/'
    if (!isdigit((unsigned char)*p))
        return false;
    uint32_t v = 0;
    while (isdigit((unsigned char)*p))
        v = v * 10 + (uint32_t)(*p++ - '0');
    if (*p == 'k' || *p == 'K')
    {
        v *= 1024;
        p++;
    }
    else if (*p == 'm' || *p == 'M')
    {
        v *= 1024 * 1024;
        p++;
    }
    if (*p != '\0' || v < 512 || v > 0x1000000 || (v & (v - 1)) != 0)
        return false;
    *au = v;
    return true;
}

// Determine the on-disk partitioning scheme via LBA 0.
static uint8_t dsk_read_scheme(void)
{
    if (!msc_dsk_read(dsk_vol, mbuf, 0, 1))
        return DSK_SCHEME_NONE;
    if (mbuf[510] != 0x55 || mbuf[511] != 0xAA)
        return DSK_SCHEME_NONE;
    if (mbuf[446 + 4] == 0xEE)
        return DSK_SCHEME_GPT;
    return DSK_SCHEME_MBR;
}

// Print the device identity, size, and partition list (the confirm preview).
static void dsk_preview(void)
{
    char vendor[9], product[17], rev[5];
    if (msc_dsk_inquiry_strings(dsk_vol, vendor, product, rev))
        printf_utf8(S(STR_DISK_DEV), vendor, product, rev);
    char serial[USB_DESC_STRING_BUF_SIZE];
    if (msc_dsk_serial(dsk_vol, serial, sizeof(serial)))
        printf_utf8(S(STR_DISK_SERIAL), serial);
    msc_dsk_info_t info;
    if (msc_dsk_get_info(dsk_vol, &info) && info.block_size)
        printf_utf8(S(STR_DISK_SIZE),
                    (unsigned long)(info.block_count / (1048576 / info.block_size)));
    if (msc_dsk_read(dsk_vol, mbuf, 0, 1) &&
        mbuf[510] == 0x55 && mbuf[511] == 0xAA && mbuf[446 + 4] != 0xEE)
    {
        bool any = false;
        for (int i = 0; i < 4; i++)
        {
            const uint8_t *e = mbuf + 446 + i * 16;
            uint8_t type = e[4];
            uint32_t size = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
            if (type == 0 || size == 0)
                continue;
            printf_utf8(S(STR_DISK_PART_LINE), i + 1, type, (unsigned long)(size / 2048));
            any = true;
        }
        if (!any)
            printf_utf8(S(STR_DISK_PART_NONE));
    }
    else
        printf_utf8(S(STR_DISK_PART_NONE));
}

static void dsk_preview_make_format(void)
{
#if FF_FS_EXFAT
    printf_utf8(S(dsk_fs == 2 ? STR_DISK_MAKE_EXFAT : STR_DISK_MAKE_FAT));
#else
    printf_utf8(S(STR_DISK_MAKE_FAT));
#endif
    if (dsk_au)
        printf_utf8(S(STR_DISK_ALLOC), (unsigned)dsk_au);
    if (dsk_full)
        printf_utf8(S(STR_DISK_MODE_FULL));
}

// Build the filesystem (and apply the label). Returns the FatFs result.
static FRESULT dsk_do_mkfs(void)
{
    MKFS_PARM parm;
    memset(&parm, 0, sizeof(parm));
    parm.n_fat = 2;
    parm.au_size = dsk_au;
    if (dsk_is_floppy)
        parm.fmt = FM_FAT | FM_SFD;
#if FF_FS_EXFAT
    else if (dsk_fs == 2)
        parm.fmt = FM_EXFAT;
#endif
    else
        parm.fmt = FM_FAT | FM_FAT32;
#if FF_LBA64
    dsk_gpt_threshold = (LBA_t)-1; // any new table f_mkfs writes is MBR
#endif
    FRESULT fr = f_mkfs(dsk_path, &parm, mbuf, MBUF_SIZE);
#if FF_LBA64
    dsk_gpt_threshold = DSK_GPT_DEFAULT;
#endif
    if (fr == FR_OK && dsk_has_label)
    {
        char arg[sizeof(dsk_path) + sizeof(dsk_label_oem)];
        size_t n = 0;
        while (dsk_path[n])
        {
            arg[n] = dsk_path[n];
            n++;
        }
        for (size_t m = 0; dsk_label_oem[m] && n < sizeof(arg) - 1; m++)
            arg[n++] = dsk_label_oem[m];
        arg[n] = '\0';
        fr = f_setlabel(arg);
    }
    return fr;
}

static void dsk_confirm_cb(bool timeout, const char *buf)
{
    if (dsk_state != DSK_CONFIRM) // cancelled by break
        return;
    const char *tok = str_parse_string(&buf);
    if (timeout || !tok || strcasecmp(tok, STR_YES) != 0 || !str_parse_end(buf))
    {
        mon_add_response_utf8(S(STR_DISK_ABORTED));
        dsk_state = DSK_IDLE;
        return;
    }
    dsk_last_pct = -1;
    dsk_fmt_started = false;
    dsk_state = dsk_after_confirm;
}

// Resolve and validate a drive token. Returns the volume index or -1 (after
// queueing the appropriate error). Fills *info.
static int dsk_open(const char *tok, msc_dsk_info_t *info, bool need_writable)
{
    if (!tok)
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return -1;
    }
    int vol = msc_dsk_vol_from_name(tok);
    if (vol < 0)
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return -1;
    }
    if (!msc_dsk_get_info((uint8_t)vol, info))
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return -1;
    }
    if (!info->present)
    {
        mon_add_response_utf8(S(STR_ERR_NO_MEDIA));
        return -1;
    }
    if (need_writable && info->write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return -1;
    }
    dsk_set_path(tok);
    return vol;
}

static void dsk_format(const char *args)
{
    msc_dsk_info_t info;
    int vol = dsk_open(str_parse_string(&args), &info, true);
    if (vol < 0)
        return;
    dsk_fs = 0;
    dsk_full = false;
    dsk_au = 0;
    dsk_has_label = false;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        if (!strcasecmp(t, STR_OPT_FAT))
            dsk_fs = 1;
        else if (!strcasecmp(t, STR_OPT_EXFAT))
            dsk_fs = 2;
        else if (!strcasecmp(t, STR_OPT_QUICK))
            dsk_full = false;
        else if (!strcasecmp(t, STR_OPT_FULL))
            dsk_full = true;
        else if (t[0] == '/')
        {
            if (!dsk_parse_alloc(t, &dsk_au))
            {
                mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
                return;
            }
        }
        else
        {
            // Volume label (OEM bytes from the terminal, as FatFs expects).
            size_t n = 0;
            while (t[n] && n < sizeof(dsk_label_oem) - 1)
            {
                dsk_label_oem[n] = t[n];
                n++;
            }
            dsk_label_oem[n] = '\0';
            if (dsk_label_oem[0] == '-' && dsk_label_oem[1] == '\0')
                dsk_label_oem[0] = '\0';
            dsk_has_label = true;
        }
    }
    if (dsk_fs == 2 && !FF_FS_EXFAT)
    {
        mon_add_response_utf8(S(STR_ERR_EXFAT_DISABLED));
        return;
    }
    dsk_vol = (uint8_t)vol;
    msc_dsk_pdrv_of_vol(dsk_vol, &dsk_pdrv);
    dsk_is_floppy = info.is_floppy;
    if (dsk_full && !dsk_is_floppy)
    {
        mon_add_response_utf8(S(STR_ERR_NOT_FORMATTABLE));
        return;
    }
    dsk_preview();
    dsk_preview_make_format();
    printf_utf8(S(STR_DISK_CONFIRM_PROMPT));
    dsk_after_confirm = (dsk_full && dsk_is_floppy) ? DSK_RUN_FORMAT_UNIT : DSK_RUN_MKFS;
    dsk_state = DSK_CONFIRM;
    rln_read_line(dsk_confirm_cb);
}

static void dsk_zero(const char *args)
{
    msc_dsk_info_t info;
    int vol = dsk_open(str_parse_string(&args), &info, true);
    if (vol < 0)
        return;
    if (!str_parse_end(args))
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    dsk_vol = (uint8_t)vol;
    msc_dsk_pdrv_of_vol(dsk_vol, &dsk_pdrv);
    dsk_block_size = info.block_size;
    dsk_total = info.block_count;
    dsk_preview();
    printf_utf8(S(STR_DISK_CONFIRM_PROMPT));
    dsk_after_confirm = DSK_RUN_ZERO;
    dsk_state = DSK_CONFIRM;
    rln_read_line(dsk_confirm_cb);
}

static void dsk_verify(const char *args)
{
    msc_dsk_info_t info;
    int vol = dsk_open(str_parse_string(&args), &info, false);
    if (vol < 0)
        return;
    if (!str_parse_end(args))
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    dsk_vol = (uint8_t)vol;
    msc_dsk_pdrv_of_vol(dsk_vol, &dsk_pdrv);
    dsk_block_size = info.block_size;
    dsk_total = info.block_count;
    dsk_lba = 0;
    dsk_last_pct = -1;
    dsk_bad = 0;
    dsk_state = DSK_RUN_VERIFY; // read-only, no confirmation
}

static void dsk_part(const char *args)
{
    msc_dsk_info_t info;
    int vol = dsk_open(str_parse_string(&args), &info, true);
    if (vol < 0)
        return;
    dsk_scheme = DSK_SCHEME_MBR;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        if (!strcasecmp(t, STR_OPT_MBR))
            dsk_scheme = DSK_SCHEME_MBR;
        else if (!strcasecmp(t, STR_OPT_GPT))
            dsk_scheme = DSK_SCHEME_GPT;
        else
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
    }
    if (info.is_floppy)
    {
        // Floppies are superfloppies; partitioning is not supported.
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    if (dsk_scheme == DSK_SCHEME_GPT && !FF_LBA64)
    {
        mon_add_response_utf8(S(STR_ERR_EXFAT_DISABLED));
        return;
    }
    dsk_vol = (uint8_t)vol;
    msc_dsk_pdrv_of_vol(dsk_vol, &dsk_pdrv);
    if (dsk_read_scheme() == dsk_scheme)
    {
        mon_add_response_utf8(S(STR_DISK_NO_CHANGE));
        return;
    }
    dsk_preview();
    printf_utf8(S(dsk_scheme == DSK_SCHEME_GPT ? STR_DISK_MAKE_GPT : STR_DISK_MAKE_MBR));
    printf_utf8(S(STR_DISK_CONFIRM_PROMPT));
    dsk_after_confirm = DSK_RUN_PART;
    dsk_state = DSK_CONFIRM;
    rln_read_line(dsk_confirm_cb);
}

static void dsk_label(const char *args)
{
    msc_dsk_info_t info;
    int vol = dsk_open(str_parse_string(&args), &info, false);
    if (vol < 0)
        return;
    char label[24];
    DWORD vsn;
    const char *t = str_parse_string(&args);
    if (!t)
    {
        FRESULT fr = f_getlabel(dsk_path, label, &vsn);
        if (fr == FR_OK)
            printf_utf8(S(STR_DISK_LABEL_RESPONSE), label);
        else
            mon_add_response_fatfs(fr);
        return;
    }
    if (info.write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return;
    }
    char oldlabel[24];
    if (f_getlabel(dsk_path, oldlabel, &vsn) != FR_OK)
        oldlabel[0] = '\0';
    char arg[sizeof(dsk_path) + sizeof(dsk_label_oem)];
    size_t n = 0;
    while (dsk_path[n])
    {
        arg[n] = dsk_path[n];
        n++;
    }
    if (!(t[0] == '-' && t[1] == '\0'))
        for (size_t m = 0; t[m] && n < sizeof(arg) - 1; m++)
            arg[n++] = t[m];
    arg[n] = '\0';
    FRESULT fr = f_setlabel(arg);
    if (fr == FR_OK)
    {
        if (f_getlabel(dsk_path, label, &vsn) != FR_OK)
            label[0] = '\0';
        printf_utf8(S(STR_DISK_LABEL_CHANGED), oldlabel, label);
    }
    else
        mon_add_response_fatfs(fr);
}

void dsk_task(void)
{
    switch (dsk_state)
    {
    case DSK_RUN_FORMAT_UNIT:
        if (!dsk_fmt_started)
        {
            int r = msc_dsk_format_start(dsk_vol);
            if (r == 0)
                dsk_fmt_started = true;
            else if (r == -2)
            {
                printf_utf8(S(STR_DISK_FORMATTING));
                msc_dsk_format_sync(dsk_vol); // blocking, uninterruptible
                dsk_state = DSK_RUN_MKFS;
            }
            else // low-level format unavailable; fall through to quick
                dsk_state = DSK_RUN_MKFS;
            return;
        }
        {
            int pct = msc_dsk_format_poll(dsk_vol);
            if (pct < 0)
            {
                putchar('\n');
                mon_add_response_fatfs(FR_DISK_ERR);
                msc_dsk_reenumerate(dsk_pdrv);
                dsk_state = DSK_IDLE;
                return;
            }
            if (pct != dsk_last_pct)
            {
                dsk_last_pct = pct;
                printf_utf8(STR_DISK_PROG_FORMAT, pct);
            }
            if (pct >= 100)
            {
                putchar('\n');
                dsk_state = DSK_RUN_MKFS;
            }
        }
        return;

    case DSK_RUN_MKFS:
    {
        FRESULT fr = dsk_do_mkfs();
        msc_dsk_reenumerate(dsk_pdrv);
        if (fr == FR_OK)
            mon_add_response_utf8(S(STR_DISK_DONE));
        else
            mon_add_response_fatfs(fr);
        dsk_state = DSK_IDLE;
        return;
    }

    case DSK_RUN_ZERO:
    {
        uint32_t per = dsk_block_size ? (uint32_t)(MBUF_SIZE / dsk_block_size) : 1;
        if (per == 0)
            per = 1;
        uint64_t remain = dsk_total - dsk_lba;
        uint32_t n = remain < per ? (uint32_t)remain : per;
        memset(mbuf, 0, (size_t)n * dsk_block_size);
        if (!msc_dsk_write(dsk_vol, mbuf, dsk_lba, n))
        {
            putchar('\n');
            mon_add_response_fatfs(FR_DISK_ERR);
            msc_dsk_reenumerate(dsk_pdrv);
            dsk_state = DSK_IDLE;
            return;
        }
        dsk_lba += n;
        int pct = (int)(dsk_lba * 100 / dsk_total);
        if (pct != dsk_last_pct)
        {
            dsk_last_pct = pct;
            printf_utf8(STR_DISK_PROG_ZERO, pct);
        }
        if (dsk_lba >= dsk_total)
        {
            putchar('\n');
            msc_dsk_reenumerate(dsk_pdrv);
            mon_add_response_utf8(S(STR_DISK_DONE));
            dsk_state = DSK_IDLE;
        }
        return;
    }

    case DSK_RUN_VERIFY:
    {
        uint32_t per = dsk_block_size ? (uint32_t)(MBUF_SIZE / dsk_block_size) : 1;
        if (per == 0)
            per = 1;
        uint64_t remain = dsk_total - dsk_lba;
        uint32_t n = remain < per ? (uint32_t)remain : per;
        if (!msc_dsk_read(dsk_vol, mbuf, dsk_lba, n))
        {
            // Pinpoint the failing sector(s) within the chunk.
            for (uint32_t i = 0; i < n; i++)
                if (!msc_dsk_read(dsk_vol, mbuf, dsk_lba + i, 1))
                {
                    putchar('\n');
                    printf_utf8(S(STR_DISK_BAD_SECTOR), (unsigned long)(dsk_lba + i));
                    dsk_bad++;
                }
            dsk_last_pct = -1;
        }
        dsk_lba += n;
        int pct = (int)(dsk_lba * 100 / dsk_total);
        if (pct != dsk_last_pct)
        {
            dsk_last_pct = pct;
            printf_utf8(STR_DISK_PROG_VERIFY, pct);
        }
        if (dsk_lba >= dsk_total)
        {
            putchar('\n');
            printf_utf8(S(STR_DISK_VERIFY_DONE), (int)dsk_bad);
            dsk_state = DSK_IDLE;
        }
        return;
    }

    case DSK_RUN_PART:
    {
        LBA_t ptbl[2] = {100, 0}; // one partition spanning the whole drive
#if FF_LBA64
        dsk_gpt_threshold = (dsk_scheme == DSK_SCHEME_GPT) ? 0 : (LBA_t)-1;
#endif
        FRESULT fr = f_fdisk(dsk_pdrv, ptbl, mbuf);
#if FF_LBA64
        dsk_gpt_threshold = DSK_GPT_DEFAULT;
#endif
        msc_dsk_reenumerate(dsk_pdrv);
        if (fr == FR_OK)
            mon_add_response_utf8(S(STR_DISK_DONE));
        else
            mon_add_response_fatfs(fr);
        dsk_state = DSK_IDLE;
        return;
    }

    default:
        return;
    }
}

bool dsk_active(void)
{
    return dsk_state != DSK_IDLE;
}

void dsk_break(void)
{
    // A FORMAT UNIT cannot be cancelled; let it finish and keep control.
    if (dsk_state == DSK_RUN_FORMAT_UNIT)
        return;
    dsk_state = DSK_IDLE;
}

void dsk_mon_disk(const char *args)
{
    const char *sub = str_parse_string(&args);
    if (!sub)
    {
        mon_add_response_utf8(S(STR_HELP_DISK));
        return;
    }
    if (!strcasecmp(sub, STR_FORMAT))
        dsk_format(args);
    else if (!strcasecmp(sub, STR_ZERO))
        dsk_zero(args);
    else if (!strcasecmp(sub, STR_VERIFY))
        dsk_verify(args);
    else if (!strcasecmp(sub, STR_PART))
        dsk_part(args);
    else if (!strcasecmp(sub, STR_LABEL))
        dsk_label(args);
    else
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
}
