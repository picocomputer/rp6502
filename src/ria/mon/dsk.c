/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/dsk.h"
#include "mon/mon.h"
#include "str/str.h"
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

// Runtime FF_MIN_GPT hook. The default reproduces the stock threshold; disk
// format forces MBR or GPT around f_mkfs(). Only the GPT-capable (exFAT/LBA64)
// build references it.
#if FF_LBA64
#define DSK_GPT_DEFAULT 0x10000000
static LBA_t dsk_gpt_threshold = DSK_GPT_DEFAULT;
LBA_t dsk_min_gpt(void) { return dsk_gpt_threshold; }
#endif

// On-disk layout for "disk format". AUTO resolves by device class/size.
enum
{
    DSK_LAYOUT_AUTO,
    DSK_LAYOUT_SFD,
    DSK_LAYOUT_MBR,
    DSK_LAYOUT_GPT,
};

static enum
{
    DSK_IDLE,
    DSK_RUN_FORMAT_UNIT,
    DSK_RUN_MKFS,
    DSK_RUN_ZERO,
    DSK_RUN_VERIFY,
} dsk_state;

// Operation context, valid from preview through completion.
static uint8_t dsk_vol;           // logical volume (MSCn:)
static uint8_t dsk_pdrv;          // physical drive backing dsk_vol
static char dsk_path[6];          // "MSCn:" for FatFs calls
static bool dsk_is_floppy;
static uint8_t dsk_fs;            // 0=auto, 1=FAT, 2=exFAT
static bool dsk_full;             // /full low-level format
static uint32_t dsk_au;           // allocation unit bytes (requested, then resolved)
static uint8_t dsk_fsty;          // resolved FS_FAT12/16/32/EXFAT for format
static bool dsk_has_label;
static char dsk_label_oem[12];
static uint8_t dsk_layout;        // DSK_LAYOUT_* for format
static bool dsk_show_make;        // preview generator: include the format lines
static bool dsk_fmt_started;      // FORMAT UNIT issued (poll phase)
static uint32_t dsk_block_size;
static uint64_t dsk_total;        // sectors for zero/verify
static uint64_t dsk_lba;          // current sector
static int dsk_last_pct;
static uint32_t dsk_bad;          // verify bad-sector count

// Build the canonical "MSCn:" path for a resolved volume (so "0:" and "MSC0:"
// produce the same FatFs path).
static void dsk_set_path(uint8_t vol)
{
    static_assert(FF_VOLUMES <= 10);
    memcpy(dsk_path, "MSC0:", 6);
    dsk_path[3] = (char)('0' + vol);
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

static const char *dsk_fs_name(BYTE fs_type)
{
    switch (fs_type)
    {
    case FS_FAT12:
        return STR_FS_FAT12;
    case FS_FAT16:
        return STR_FS_FAT16;
    case FS_FAT32:
        return STR_FS_FAT32;
    case FS_EXFAT:
        return STR_FS_EXFAT;
    default:
        return STR_FS_NONE;
    }
}

// True if the sector looks like a FAT/exFAT volume boot record (mirrors FatFs
// check_fs). A bare 0x55AA is not enough — an MBR carries it too.
static bool dsk_is_fat_vbr(const uint8_t *w)
{
    uint16_t sign = (uint16_t)(w[510] | (w[511] << 8));
#if FF_FS_EXFAT
    static const uint8_t EXFAT_SIG[11] = {0xEB, 0x76, 0x90, 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
    if (sign == 0xAA55 && memcmp(w, EXFAT_SIG, sizeof(EXFAT_SIG)) == 0)
        return true;
#endif
    uint8_t jmp = w[0];
    if (jmp != 0xEB && jmp != 0xE9 && jmp != 0xE8)
        return false;
    static const uint8_t FAT32_SIG[8] = {'F', 'A', 'T', '3', '2', ' ', ' ', ' '};
    if (sign == 0xAA55 && memcmp(w + 82, FAT32_SIG, sizeof(FAT32_SIG)) == 0)
        return true;
    uint16_t bps = (uint16_t)(w[11] | (w[12] << 8));
    uint8_t spc = w[13];
    uint16_t rsvd = (uint16_t)(w[14] | (w[15] << 8));
    uint8_t nfat = w[16];
    uint16_t root = (uint16_t)(w[17] | (w[18] << 8));
    uint16_t tot16 = (uint16_t)(w[19] | (w[20] << 8));
    uint32_t tot32 = (uint32_t)(w[32] | (w[33] << 8) | (w[34] << 16) | ((uint32_t)w[35] << 24));
    uint16_t fsz16 = (uint16_t)(w[22] | (w[23] << 8));
    return (bps & (bps - 1)) == 0 && bps >= FF_MIN_SS && bps <= FF_MAX_SS &&
           spc != 0 && (spc & (spc - 1)) == 0 && rsvd != 0 &&
           (unsigned)(nfat - 1) <= 1 && root != 0 &&
           (tot16 >= 64 || tot32 >= 0x10000) && fsz16 != 0;
}

// Localized "Part:" line id for the current on-disk layout, or -1 if unknown.
static int dsk_scheme_str(void)
{
    if (!msc_dsk_read(dsk_vol, mbuf, 0, 1))
        return -1;
    if (dsk_is_fat_vbr(mbuf))
        return STR_DISK_PART_SFD;
    if (mbuf[510] == 0x55 && mbuf[511] == 0xAA)
        return (mbuf[446 + 4] == 0xEE) ? STR_DISK_PART_GPT : STR_DISK_PART_MBR;
    return -1;
}

// Device/size/format preview as a monitor response generator: one line per
// call (an empty fill skips a line). dsk_show_make adds the format-only lines.
// Routed through the response queue so the \a alignment markers work.
static int dsk_preview_response(char *buf, size_t size, int state)
{
    if (state < 0)
        return state;
    switch (state)
    {
    case 0:
    {
        char vendor[9], product[17], rev[5];
        if (msc_dsk_inquiry_strings(dsk_vol, vendor, product, rev))
            snprintf_utf8(buf, size, S(STR_DISK_DEV), vendor, product, rev);
        return 1;
    }
    case 1:
    {
        char serial[USB_DESC_STRING_BUF_SIZE];
        if (msc_dsk_serial(dsk_vol, serial, sizeof(serial)))
            snprintf_utf8(buf, size, S(STR_DISK_SERIAL), serial);
        return 2;
    }
    case 2:
    {
        msc_dsk_info_t info;
        if (msc_dsk_get_info(dsk_vol, &info) && info.block_size)
        {
            char szbuf[24];
            msc_dsk_size_str(info.block_count, info.block_size, szbuf, sizeof(szbuf));
            snprintf_utf8(buf, size, S(STR_DISK_SIZE), szbuf);
        }
        return 3;
    }
    case 3:
    {
        char label[24];
        DWORD vsn;
        if (f_getlabel(dsk_path, label, &vsn) == FR_OK && label[0])
            snprintf_utf8(buf, size, S(STR_DISK_INFO_LABEL), label);
        return 4;
    }
    case 4:
    {
        int s = dsk_scheme_str();
        if (s >= 0)
            snprintf_utf8(buf, size, S(s));
        return 5;
    }
    case 5:
    {
        DWORD nclst;
        FATFS *fs;
        if (f_getfree(dsk_path, &nclst, &fs) == FR_OK)
            snprintf_utf8(buf, size, S(STR_DISK_INFO_FORMAT), dsk_fs_name(fs->fs_type));
        else
            snprintf_utf8(buf, size, S(STR_DISK_INFO_FORMAT), STR_FS_NONE);
        return 6;
    }
    case 6:
    {
        DWORD nclst;
        FATFS *fs;
        if (f_getfree(dsk_path, &nclst, &fs) == FR_OK)
            snprintf_utf8(buf, size, S(STR_DISK_ALLOC), (unsigned)(fs->csize * 512u));
        return 7;
    }
    case 7:
    {
        DWORD nclst;
        FATFS *fs;
        if (f_getfree(dsk_path, &nclst, &fs) == FR_OK)
            snprintf_utf8(buf, size, S(STR_DISK_INFO_FREE),
                          (unsigned long)nclst * fs->csize,
                          (unsigned long)(fs->n_fatent - 2) * fs->csize);
        return dsk_show_make ? 8 : -1;
    }
    case 8:
        snprintf_utf8(buf, size,
                      S(dsk_layout == DSK_LAYOUT_SFD   ? STR_DISK_MAKE_SFD
                        : dsk_layout == DSK_LAYOUT_GPT ? STR_DISK_MAKE_GPT
                                                       : STR_DISK_MAKE_MBR));
        return 9;
    case 9:
        snprintf_utf8(buf, size, S(STR_DISK_MAKE_FS), dsk_fs_name(dsk_fsty));
        return 10;
    case 10:
        snprintf_utf8(buf, size, S(STR_DISK_ALLOC), (unsigned)dsk_au);
        return 11;
    case 11:
        if (dsk_full)
            snprintf_utf8(buf, size, S(STR_DISK_MODE_FULL));
        return -1;
    default:
        return -1;
    }
}

// Standard FAT cluster-count limits (FAT spec, not FatFs internals).
#define DSK_MAX_FAT12 0xFF5u  // 4085
#define DSK_MAX_FAT16 0xFFF5u // 65525
#define DSK_MAX_FAT32 0x0FFFFFF5u

// The disk tool's own, stable FS-selection policy, so the result is OUR contract
// rather than whatever f_mkfs would auto-pick. We hand f_mkfs an explicit type +
// cluster size via its public MKFS_PARM (so its internal auto-selection never
// runs); the only thing relied on is the standard FAT cluster-count limits
// above. `raw` is the device sector count (512 B sectors); the small partition /
// metadata overhead is ignored — the auto cluster sizes keep counts well clear
// of the limits, and f_mkfs re-validates as a safety net. `want`: 0=auto, 1=FAT
// family, 2=exFAT; a user /Nk (req_au_bytes) overrides the cluster size. Returns
// FS_FAT12/16/32/EXFAT + cluster size in *au_sectors, or 0 if unsatisfiable.
static BYTE dsk_resolve_fs(LBA_t raw, uint8_t want, uint32_t req_au_bytes, UINT *au_sectors)
{
#if !FF_FS_EXFAT
    (void)want; // only consulted for exFAT selection
#endif
#if FF_FS_EXFAT
    // exFAT when asked, or (auto) for media past ~32 GiB (the SDXC / desktop
    // convention). Cluster size scales with capacity.
    if (want == 2 || (want == 0 && raw >= 0x4000000))
    {
        UINT au = req_au_bytes ? req_au_bytes / 512 : 0;
        if (au == 0)
        {
            au = 8;                         // 4 KB
            if (raw >= 0x80000) au = 64;    // >= 512 MiB -> 32 KB
            if (raw >= 0x4000000) au = 256; // >= 32 GiB  -> 128 KB
        }
        *au_sectors = au;
        return FS_EXFAT;
    }
#endif
#if FF_LBA64
    if (raw >= 0x100000000)
        return 0; // beyond FAT sector addressing; exFAT only
#endif
    // Explicit cluster: the FAT sub-type follows from the count (f_mkfs validates).
    if (req_au_bytes)
    {
        UINT au = req_au_bytes / 512;
        DWORD n = (DWORD)(raw / au);
        if (n > DSK_MAX_FAT32)
            return 0;
        *au_sectors = au;
        return n <= DSK_MAX_FAT12 ? FS_FAT12 : n <= DSK_MAX_FAT16 ? FS_FAT16 : FS_FAT32;
    }
    // Auto cluster: grow until the count fits FAT16 (cap 32 KB clusters); the
    // small media that stays under the FAT12 limit becomes FAT12.
    UINT au = 1;
    while (raw / au > DSK_MAX_FAT16 && au < 64)
        au <<= 1;
    if (raw / au <= DSK_MAX_FAT16)
    {
        *au_sectors = au;
        return (raw / au) <= DSK_MAX_FAT12 ? FS_FAT12 : FS_FAT16;
    }
    // Too many clusters for FAT16 -> FAT32, with a >= 4 KB cluster scaled by size
    // so the count stays well clear of the FAT16 ceiling.
    au = 8;                        // 4 KB
    if (raw >= 0x1000000) au = 16; // >= 8 GiB  -> 8 KB
    if (raw >= 0x2000000) au = 32; // >= 16 GiB -> 16 KB
    while (raw / au > DSK_MAX_FAT32 && au < 128)
        au <<= 1;
    if (raw / au > DSK_MAX_FAT32)
        return 0; // too large for FAT32; use exFAT
    *au_sectors = au;
    return FS_FAT32;
}

// Build the filesystem (and apply the label). Returns the FatFs result.
static FRESULT dsk_do_mkfs(void)
{
    MKFS_PARM parm;
    memset(&parm, 0, sizeof(parm));
    parm.n_fat = 2;
    parm.au_size = dsk_au; // resolved cluster size (bytes); always explicit
    switch (dsk_fsty)
    {
    case FS_EXFAT:
        parm.fmt = FM_EXFAT;
        break;
    case FS_FAT32:
        parm.fmt = FM_FAT32;
        break;
    default: // FAT12 / FAT16
        parm.fmt = FM_FAT;
        break;
    }
    if (dsk_layout == DSK_LAYOUT_SFD)
        parm.fmt |= FM_SFD; // superfloppy: no partition table
#if FF_LBA64
    // f_mkfs writes GPT when the drive is >= FF_MIN_GPT (dsk_min_gpt()), else
    // MBR. Force the chosen scheme; FM_SFD ignores it.
    dsk_gpt_threshold = (dsk_layout == DSK_LAYOUT_GPT) ? 0 : (LBA_t)-1;
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

// Confirm actions: invoked by the monitor only when the user types YES.
static void dsk_run_format(void)
{
    dsk_last_pct = -1;
    dsk_fmt_started = false;
    dsk_state = (dsk_full && dsk_is_floppy) ? DSK_RUN_FORMAT_UNIT : DSK_RUN_MKFS;
}

static void dsk_run_zero(void)
{
    dsk_last_pct = -1;
    dsk_state = DSK_RUN_ZERO;
}

// Continuation run after the info block drains (verify is read-only, no YES).
static void dsk_run_verify(void)
{
    dsk_lba = 0;
    dsk_last_pct = -1;
    dsk_bad = 0;
    dsk_state = DSK_RUN_VERIFY;
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
    dsk_set_path((uint8_t)vol);
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
    dsk_layout = DSK_LAYOUT_AUTO;
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
        else if (!strcasecmp(t, STR_OPT_SFD) ||
                 !strcasecmp(t, STR_OPT_MBR) ||
                 !strcasecmp(t, STR_OPT_GPT))
        {
            if (dsk_layout != DSK_LAYOUT_AUTO) // at most one layout flag
            {
                mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
                return;
            }
            dsk_layout = !strcasecmp(t, STR_OPT_SFD)   ? DSK_LAYOUT_SFD
                         : !strcasecmp(t, STR_OPT_MBR) ? DSK_LAYOUT_MBR
                                                       : DSK_LAYOUT_GPT;
        }
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
            // An empty "" clears the label.
            size_t n = 0;
            while (t[n] && n < sizeof(dsk_label_oem) - 1)
            {
                dsk_label_oem[n] = t[n];
                n++;
            }
            dsk_label_oem[n] = '\0';
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
    if (dsk_layout == DSK_LAYOUT_GPT && !FF_LBA64)
    {
        mon_add_response_utf8(S(STR_ERR_EXFAT_DISABLED));
        return;
    }
    if (dsk_layout == DSK_LAYOUT_GPT && dsk_is_floppy)
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    if (dsk_layout == DSK_LAYOUT_AUTO) // resolve by device class/size
    {
        if (dsk_is_floppy)
            dsk_layout = DSK_LAYOUT_SFD;
#if FF_LBA64
        else if (info.block_count >= DSK_GPT_DEFAULT)
            dsk_layout = DSK_LAYOUT_GPT;
#endif
        else
            dsk_layout = DSK_LAYOUT_MBR;
    }
    // Own the FS type + cluster size (no f_mkfs auto-select).
    UINT au_sectors;
    dsk_fsty = dsk_resolve_fs(info.block_count, dsk_fs, dsk_au, &au_sectors);
    if (dsk_fsty == 0)
    {
        mon_add_response_fatfs(FR_MKFS_ABORTED);
        return;
    }
    dsk_au = au_sectors * 512u; // resolved cluster size in bytes
    dsk_show_make = true;
    mon_add_response_fn(dsk_preview_response);
    mon_response_confirm(dsk_run_format);
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
    dsk_show_make = false;
    mon_add_response_fn(dsk_preview_response);
    mon_response_confirm(dsk_run_zero);
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
    dsk_show_make = false;
    mon_add_response_fn(dsk_preview_response);
    mon_response_then(dsk_run_verify); // lead with info, then scan (read-only)
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
    // An empty "" label clears it.
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

static void dsk_info(const char *args)
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
    dsk_show_make = false;
    mon_add_response_fn(dsk_preview_response);
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
    if (!strcasecmp(sub, STR_INFO))
        dsk_info(args);
    else if (!strcasecmp(sub, STR_FORMAT))
        dsk_format(args);
    else if (!strcasecmp(sub, STR_ZERO))
        dsk_zero(args);
    else if (!strcasecmp(sub, STR_VERIFY))
        dsk_verify(args);
    else if (!strcasecmp(sub, STR_LABEL))
        dsk_label(args);
    else
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
}
