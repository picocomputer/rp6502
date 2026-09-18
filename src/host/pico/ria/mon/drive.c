/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "core/sys/ria.h"
#include "ria/mon/drive.h"
#include "ria/mon/help.h"
#include "ria/mon/mon.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/ria.h"
#include "ria/usb/msc.h"
#include "ria/usb/usb.h"
#include <fatfs/ff.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// ffconf.h defines FF_MIN_GPT as a call to drive_min_gpt(). When f_mkfs
// partitions a drive, it uses a GPT if the drive has at least
// drive_gpt_threshold sectors and an MBR otherwise. DRIVE_GPT_DEFAULT is the
// FatFs default for FF_MIN_GPT.
#if FF_LBA64
#define DRIVE_GPT_DEFAULT 0x10000000
static_assert(DRIVE_GPT_DEFAULT <= 0x100000000ULL, "FF_MIN_GPT default out of range");
static LBA_t drive_gpt_threshold = DRIVE_GPT_DEFAULT;
unsigned long long drive_min_gpt(void) { return drive_gpt_threshold; }
#endif

enum
{
    DRIVE_LAYOUT_AUTO,
    DRIVE_LAYOUT_SFD,
    DRIVE_LAYOUT_MBR,
    DRIVE_LAYOUT_GPT,
};

static enum {
    DRIVE_IDLE,
    DRIVE_RUN_FORMAT_UNIT,
    DRIVE_RUN_MKFS,
    DRIVE_RUN_ERASE,
    DRIVE_RUN_VERIFY,
} drive_state;

enum
{
    DRIVE_PREVIEW_PLAIN,
    DRIVE_PREVIEW_FORMAT,
    DRIVE_PREVIEW_ERASE,
};

static uint8_t drive_vol;
static uint8_t drive_gen;
static char drive_path[6];
static bool drive_is_floppy;
static uint8_t drive_fs_req;
static bool drive_full;
static uint32_t drive_au;
static uint8_t drive_fs_resolved;
static bool drive_has_label;
static char drive_label_oem[12];
static uint8_t drive_layout;
static uint8_t drive_preview_op;
static uint8_t drive_fmt_track;
static uint8_t drive_fmt_head;
static uint8_t drive_fmt_tracks;
static uint8_t drive_fmt_heads;
static uint64_t drive_total;
static uint64_t drive_lba;
static int drive_last_pct;
static uint32_t drive_bad;
static uint32_t drive_pin_n;
static uint32_t drive_pin_i;

static struct
{
    bool valid;
    DWORD csize;
    DWORD n_fatent;
    DWORD nclst;
} drive_free;

static char drive_label_old[24];
static char drive_label_cur[24];
static bool drive_label_changed;

static bool drive_parse_alloc(const char *tok, uint32_t *au)
{
    const char *p = tok + 1;
    if (!isdigit((unsigned char)*p))
        return false;
    uint32_t v = 0;
    while (isdigit((unsigned char)*p))
    {
        v = v * 10 + (uint32_t)(*p++ - '0');
        if (v > 0x1000000)
            return false;
    }
    if (*p == 'k' || *p == 'K')
    {
        if (v > 0x1000000 / 1024)
            return false;
        v *= 1024;
        p++;
    }
    else if (*p == 'm' || *p == 'M')
    {
        if (v > 0x1000000 / (1024 * 1024))
            return false;
        v *= 1024 * 1024;
        p++;
    }
    if (*p != '\0' || v < 512 || v > 0x1000000 || (v & (v - 1)) != 0)
        return false;
    *au = v;
    return true;
}

static const char *drive_fs_name(BYTE fs_type)
{
    switch (fs_type)
    {
    case FS_FAT12:
        return STR_FAT12;
    case FS_FAT16:
        return STR_FAT16;
    case FS_FAT32:
        return STR_FAT32;
    case FS_EXFAT:
        return STR_EXFAT;
    default:
        return S(STR_PARENS_NONE);
    }
}

// drive_is_fat_vbr repeats the tests that FatFs check_fs uses to recognize a
// FAT or exFAT volume boot record. The 0x55AA signature alone does not identify
// a volume boot record, because an MBR ends with the same signature.
static bool drive_is_fat_vbr(const uint8_t *w)
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

static const char *drive_scheme_word(void)
{
    if (!msc_drive_read(drive_vol, mbuf, 0, 1))
        return NULL;
    if (drive_is_fat_vbr(mbuf))
        return STR_SFD;
    if (mbuf[510] == 0x55 && mbuf[511] == 0xAA)
        return (mbuf[446 + 4] == 0xEE) ? STR_GPT : STR_MBR;
    return NULL;
}

static void drive_fmt_desc(char *out, size_t size, const char *scheme, const char *fsname,
                           uint32_t au_bytes, const char *suffix, const char *label)
{
    size_t n = 0;
    if (scheme)
        n += snprintf(out + n, size - n, "%s ", scheme);
    if (n >= size)
        return;
    n += snprintf(out + n, size - n, "%s", fsname);
    if (n >= size)
        return;
    if (au_bytes == 512)
        n += snprintf(out + n, size - n, " 512 B");
    else if (au_bytes)
        n += snprintf(out + n, size - n, " %u KB", (unsigned)(au_bytes / 1024));
    if (n >= size)
        return;
    if (suffix && suffix[0])
    {
        n += snprintf(out + n, size - n, " %s", suffix);
        if (n >= size)
            return;
    }
    if (label && label[0])
        snprintf(out + n, size - n, " (%s)", label);
}

static int drive_preview_response(char *buf, size_t size, int state, unsigned)
{
    if (state < 0)
        return state;
    switch (state)
    {
    case 0:
    {
        char vendor[9], product[17], rev[5];
        if (msc_drive_inquiry_strings(drive_vol, vendor, product, rev))
        {
            msc_drive_info_t info;
            char szbuf[24];
            szbuf[0] = '\0';
            if (msc_drive_get_info(drive_vol, &info) && info.block_size)
                str_size((uint64_t)info.block_count * info.block_size, szbuf, sizeof(szbuf));
            oem_snprintf(buf, size, S(STR_DISK_DEV), szbuf, vendor, product, rev);
        }
        return 1;
    }
    case 1:
    {
        char serial[USB_DESC_STRING_BUF_SIZE];
        if (msc_drive_serial(drive_vol, serial, sizeof(serial)))
            oem_snprintf(buf, size, S(STR_DISK_SERIAL), serial);
        return 2;
    }
    case 2:
    {
        char desc[64], label[24];
        DWORD vsn;
        const char *scheme = drive_scheme_word();
        if (f_getlabel(drive_path, label, &vsn) != FR_OK)
            label[0] = '\0';
        DWORD nclst;
        FATFS *fs;
        drive_free.valid = false;
        if (f_getfree(drive_path, &nclst, &fs) == FR_OK)
        {
            drive_free.valid = true;
            drive_free.csize = fs->csize;
            drive_free.n_fatent = fs->n_fatent;
            drive_free.nclst = nclst;
            drive_fmt_desc(desc, sizeof(desc), scheme, drive_fs_name(fs->fs_type),
                         fs->csize * 512u, NULL, label);
        }
        else
            drive_fmt_desc(desc, sizeof(desc), scheme, S(STR_PARENS_NONE), 0, NULL, label);
        oem_snprintf(buf, size, S(STR_DISK_VOL_FMT), desc);
        return 3;
    }
    case 3:
    {
        if (drive_free.valid)
        {
            uint64_t totb = (uint64_t)(drive_free.n_fatent - 2) * drive_free.csize * 512u;
            uint64_t freeb = (uint64_t)drive_free.nclst * drive_free.csize * 512u;
            uint64_t usedb = totb - freeb;
            unsigned pct = totb ? (unsigned)(usedb * 100 / totb) : 0;
            char usedbuf[24], totbuf[24];
            str_size(usedb, usedbuf, sizeof(usedbuf));
            str_size(totb, totbuf, sizeof(totbuf));
            oem_snprintf(buf, size, S(STR_DISK_VOL_USE), usedbuf, totbuf, pct);
        }
        return drive_preview_op == DRIVE_PREVIEW_PLAIN ? -1 : 4;
    }
    case 4:
        oem_snprintf(buf, size, S(drive_preview_op == DRIVE_PREVIEW_ERASE ? STR_DISK_WARN_ERASE : STR_DISK_WARN_FORMAT));
        return drive_preview_op == DRIVE_PREVIEW_FORMAT ? 5 : -1;
    case 5:
    {
        const char *scheme = drive_layout == DRIVE_LAYOUT_SFD   ? STR_SFD
                             : drive_layout == DRIVE_LAYOUT_GPT ? STR_GPT
                                                            : STR_MBR;
        char desc[64];
        drive_fmt_desc(desc, sizeof(desc), scheme, drive_fs_name(drive_fs_resolved), drive_au,
                     drive_full ? STR_FULL : STR_QUICK,
                     drive_has_label ? drive_label_oem : NULL);
        oem_snprintf(buf, size, S(STR_DISK_FMT), desc);
        return -1;
    }
    default:
        return -1;
    }
}

// f_mkfs in the vendored ff.c calls drive_mkfs_capture after it has chosen the
// filesystem type and cluster size and before it trims or writes the drive. A
// nonzero return makes f_mkfs stop there and return FR_OK.
static bool drive_previewing;

int drive_mkfs_capture(BYTE fsty, DWORD au_sectors)
{
    if (!drive_previewing)
        return 0;
    drive_fs_resolved = fsty;
    drive_au = au_sectors * 512u;
    return 1;
}

// The layout is forced exactly as drive_do_mkfs forces it, because f_mkfs
// chooses the filesystem type and cluster size from the volume size, and the
// volume size depends on the layout.
static FRESULT drive_preview_mkfs(void)
{
    MKFS_PARM parm;
    memset(&parm, 0, sizeof(parm));
    parm.n_fat = 2;
    parm.au_size = drive_au;
    parm.fmt = drive_fs_req == 2   ? FM_EXFAT
               : drive_fs_req == 1 ? (FM_FAT | FM_FAT32)
                                 : FM_ANY;
    if (drive_layout == DRIVE_LAYOUT_SFD)
        parm.fmt |= FM_SFD;
#if FF_LBA64
    drive_gpt_threshold = (drive_layout == DRIVE_LAYOUT_GPT) ? 0 : (LBA_t)-1;
#endif
    drive_previewing = true;
    FRESULT fr = f_mkfs(drive_path, &parm, mbuf, MBUF_SIZE);
    drive_previewing = false;
#if FF_LBA64
    drive_gpt_threshold = DRIVE_GPT_DEFAULT;
#endif
    return fr;
}

static FRESULT drive_set_label(const char *label)
{
    char arg[sizeof(drive_path) + sizeof(drive_label_oem)];
    snprintf(arg, sizeof(arg), "%s%s", drive_path, label);
    return f_setlabel(arg);
}

static FRESULT drive_do_mkfs(void)
{
    MKFS_PARM parm;
    memset(&parm, 0, sizeof(parm));
    parm.n_fat = 2;
    parm.au_size = drive_au;
    switch (drive_fs_resolved)
    {
    case FS_EXFAT:
        parm.fmt = FM_EXFAT;
        break;
    case FS_FAT32:
        parm.fmt = FM_FAT32;
        break;
    default:
        parm.fmt = FM_FAT;
        break;
    }
    if (drive_layout == DRIVE_LAYOUT_SFD)
        parm.fmt |= FM_SFD;
#if FF_LBA64
    drive_gpt_threshold = (drive_layout == DRIVE_LAYOUT_GPT) ? 0 : (LBA_t)-1;
#endif
    FRESULT fr = f_mkfs(drive_path, &parm, mbuf, MBUF_SIZE);
#if FF_LBA64
    drive_gpt_threshold = DRIVE_GPT_DEFAULT;
#endif
    if (fr == FR_OK && drive_has_label)
        fr = drive_set_label(drive_label_oem);
    return fr;
}

// The sector counts are those of the 5.25-inch floppy formats with 512-byte
// sectors. The single-sided 160 KB (320) and 180 KB (360) formats have one
// head, the double-sided 320 KB (640) and 360 KB (720) formats have two, and
// all four have 40 tracks. The 1.2 MB 5.25-inch format and the 3.5-inch
// formats of 720 KB and larger have two heads and 80 tracks.
static void drive_floppy_geometry(uint64_t blocks, uint8_t *tracks, uint8_t *heads)
{
    *heads = (blocks == 320 || blocks == 360) ? 1 : 2;
    *tracks = (blocks <= 720) ? 40 : 80;
}

// The Ctrl-C branches leave drive_state alone because, after sys_break(),
// sys_commit() calls every driver's break handler and drive_break() sets
// DRIVE_IDLE.
static int drive_run_response(char *buf, size_t size, int state, unsigned)
{
    if (state < 0)
        return state;
    switch (drive_state)
    {
    case DRIVE_RUN_FORMAT_UNIT:
        if (ria_get_sigint())
        {
            putchar('\n');
            msc_drive_reenumerate(drive_vol);
            sys_break();
            return -1;
        }
        if (!msc_drive_format_track(drive_vol, drive_fmt_track, drive_fmt_head))
        {
            mon_add_response_utf8(S(STR_ERR_FORMAT_FAILED));
            msc_drive_reenumerate(drive_vol);
            drive_state = DRIVE_IDLE;
            if (drive_last_pct >= 0)
                snprintf(buf, size, "\n");
            return -1;
        }
        if (++drive_fmt_head >= drive_fmt_heads)
        {
            drive_fmt_head = 0;
            drive_fmt_track++;
        }
        {
            uint32_t total = (uint32_t)drive_fmt_tracks * drive_fmt_heads;
            uint32_t done = (uint32_t)drive_fmt_track * drive_fmt_heads + drive_fmt_head;
            int overall = total ? (int)(done * 100 / total) : 100;
            if (overall != drive_last_pct)
            {
                drive_last_pct = overall;
                oem_snprintf(buf, size, STR_DISK_PROG_FORMAT, overall);
            }
        }
        if (drive_fmt_track >= drive_fmt_tracks)
            drive_state = DRIVE_RUN_MKFS;
        return 0;

    case DRIVE_RUN_MKFS:
    {
        FRESULT fr = drive_do_mkfs();
        msc_drive_reenumerate(drive_vol);
        drive_state = DRIVE_IDLE;
        const char *nl = drive_last_pct >= 0 ? "\n" : "";
        if (fr == FR_OK)
            oem_snprintf(buf, size, "%s%s", nl, S(STR_DISK_DONE));
        else
        {
            mon_add_response_fatfs(fr);
            snprintf(buf, size, "%s", nl);
        }
        return -1;
    }

    case DRIVE_RUN_ERASE:
        if (ria_get_sigint())
        {
            putchar('\n');
            msc_drive_reenumerate(drive_vol);
            sys_break();
            return -1;
        }
        if (drive_lba >= drive_total)
        {
            msc_drive_reenumerate(drive_vol);
            drive_state = DRIVE_IDLE;
            oem_snprintf(buf, size, "\n%s", S(STR_DISK_DONE));
            return -1;
        }
        {
            uint32_t per = MBUF_SIZE / 512;
            uint64_t remain = drive_total - drive_lba;
            uint32_t n = remain < per ? (uint32_t)remain : per;
            if (!msc_drive_write(drive_vol, mbuf, drive_lba, n))
            {
                mon_add_response_fatfs(FR_DISK_ERR);
                msc_drive_reenumerate(drive_vol);
                drive_state = DRIVE_IDLE;
                snprintf(buf, size, "\n");
                return -1;
            }
            drive_lba += n;
            int pct = (int)(drive_lba * 100 / drive_total);
            if (pct != drive_last_pct)
            {
                drive_last_pct = pct;
                oem_snprintf(buf, size, STR_DISK_PROG_ERASE, pct);
            }
        }
        return 0;

    case DRIVE_RUN_VERIFY:
        if (ria_get_sigint())
        {
            putchar('\n');
            sys_break();
            return -1;
        }
        if (drive_pin_n)
        {
            while (drive_pin_i < drive_pin_n)
            {
                uint64_t lba = drive_lba + drive_pin_i++;
                if (!msc_drive_read(drive_vol, mbuf, lba, 1))
                {
                    drive_bad++;
                    buf[0] = '\r';
                    oem_snprintf(buf + 1, size - 1, S(STR_DISK_BAD_SECTOR), (unsigned long long)lba);
                    return 0;
                }
            }
            drive_lba += drive_pin_n;
            drive_pin_n = 0;
            drive_last_pct = -1;
        }
        else if (drive_lba < drive_total)
        {
            uint32_t per = MBUF_SIZE / 512;
            uint64_t remain = drive_total - drive_lba;
            uint32_t n = remain < per ? (uint32_t)remain : per;
            if (!msc_drive_read(drive_vol, mbuf, drive_lba, n))
            {
                drive_pin_n = n;
                drive_pin_i = 0;
                return 0;
            }
            drive_lba += n;
        }
        {
            int pct = (int)(drive_lba * 100 / drive_total);
            if (pct != drive_last_pct)
            {
                drive_last_pct = pct;
                oem_snprintf(buf, size, STR_DISK_PROG_VERIFY, pct);
                return 0;
            }
        }
        if (drive_lba >= drive_total)
        {
            drive_state = DRIVE_IDLE;
            buf[0] = '\n';
            oem_snprintf(buf + 1, size - 1, S(STR_DISK_VERIFY_DONE), (int)drive_bad);
            return -1;
        }
        return 0;

    default:
        drive_state = DRIVE_IDLE;
        return -1;
    }
}

// A USB hot-swap during the confirmation prompt frees the slot, and a new
// device can take the same slot, so the mount generation captured at preview
// must still match. The media and the write protection can also change during
// the prompt.
static bool drive_run_revalidate(void)
{
    msc_drive_info_t info;
    if (!msc_drive_get_info(drive_vol, &info) || info.gen != drive_gen)
    {
        mon_add_response_utf8(S(STR_ERR_DEVICE_CHANGED));
        return false;
    }
    if (!info.present)
    {
        mon_add_response_utf8(S(STR_ERR_NO_MEDIA));
        return false;
    }
    if (info.block_count == 0)
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return false;
    }
    if (info.block_size != 512)
    {
        mon_add_response_utf8(S(STR_ERR_SECTOR_SIZE));
        return false;
    }
    if (info.write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return false;
    }
    return true;
}

static void drive_run_format(void)
{
    if (!drive_run_revalidate())
    {
        drive_state = DRIVE_IDLE;
        return;
    }
    drive_last_pct = -1;
    drive_fmt_track = 0;
    drive_fmt_head = 0;
    mon_add_response_utf8(S(STR_DISK_FORMATTING));
    if (drive_full && drive_is_floppy)
    {
        drive_floppy_geometry(drive_total, &drive_fmt_tracks, &drive_fmt_heads);
        drive_state = DRIVE_RUN_FORMAT_UNIT;
    }
    else
        drive_state = DRIVE_RUN_MKFS;
    mon_add_response_fn(drive_run_response);
}

static void drive_run_erase(void)
{
    if (!drive_run_revalidate())
    {
        drive_state = DRIVE_IDLE;
        return;
    }
    drive_last_pct = -1;
    drive_lba = 0;
    memset(mbuf, 0, MBUF_SIZE);
    mon_add_response_utf8(S(STR_DISK_ERASING));
    drive_state = DRIVE_RUN_ERASE;
    mon_add_response_fn(drive_run_response);
}

static bool drive_validate(uint8_t vol, msc_drive_info_t *info, bool need_writable)
{
    if (!msc_drive_get_info(vol, info))
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return false;
    }
    if (!info->present)
    {
        mon_add_response_utf8(S(STR_ERR_NO_MEDIA));
        return false;
    }
    // A block count of 0 is rejected because the verify progress calculation
    // divides by it.
    if (info->block_count == 0)
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return false;
    }
    // Only 512-byte sectors are accepted because FatFs is built with FF_MIN_SS
    // and FF_MAX_SS set to 512 and never queries the drive for its sector size.
    // The erase and verify passes also transfer MBUF_SIZE / 512 sectors at a
    // time through mbuf, so a larger sector would run past the end of mbuf.
    if (info->block_size != 512)
    {
        mon_add_response_utf8(S(STR_ERR_SECTOR_SIZE));
        return false;
    }
    if (need_writable && info->write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return false;
    }
    memcpy(drive_path, info->path, sizeof(drive_path));
    return true;
}

static bool drive_match(const char *t, int *vol)
{
    int v;
    if (*vol < 0 && (v = msc_drive_vol_from_name(t)) >= 0)
    {
        *vol = v;
        return true;
    }
    return false;
}

static void drive_sub_help(const char *sub)
{
    const char *prose = help_lookup(STR_DISK, sub, NULL);
    if (prose)
        mon_add_response_utf8(prose);
}

static int drive_parse_only(const char *args, const char *sub)
{
    int vol = -1;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        if (!drive_match(t, &vol))
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return -1;
        }
    }
    if (vol < 0)
    {
        drive_sub_help(sub);
        return -1;
    }
    return vol;
}

static void drive_format(const char *args)
{
    drive_fs_req = 0;
    drive_full = false;
    drive_au = 0;
    drive_has_label = false;
    drive_layout = DRIVE_LAYOUT_AUTO;
    int vol = -1;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        if (!strcasecmp(t, STR_OPT_FAT))
            drive_fs_req = 1;
#if RP6502_EXFAT
        else if (!strcasecmp(t, STR_OPT_EXFAT))
            drive_fs_req = 2;
#endif
        else if (!strcasecmp(t, STR_OPT_QUICK))
            drive_full = false;
        else if (!strcasecmp(t, STR_OPT_FULL))
            drive_full = true;
        else if (!strcasecmp(t, STR_OPT_SFD) ||
                 !strcasecmp(t, STR_OPT_MBR) ||
                 !strcasecmp(t, STR_OPT_GPT))
        {
            if (drive_layout != DRIVE_LAYOUT_AUTO)
            {
                mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
                return;
            }
            drive_layout = !strcasecmp(t, STR_OPT_SFD)   ? DRIVE_LAYOUT_SFD
                         : !strcasecmp(t, STR_OPT_MBR) ? DRIVE_LAYOUT_MBR
                                                       : DRIVE_LAYOUT_GPT;
        }
        else if (t[0] == '/')
        {
            if (!drive_parse_alloc(t, &drive_au))
            {
                mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
                return;
            }
        }
        else if (drive_match(t, &vol))
        {
        }
        else if (!drive_has_label)
        {
            snprintf(drive_label_oem, sizeof(drive_label_oem), "%s", t);
            drive_has_label = true;
        }
        else
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
    }
    if (vol < 0)
    {
        drive_sub_help(STR_FORMAT);
        return;
    }
    msc_drive_info_t info;
    if (!drive_validate((uint8_t)vol, &info, true))
        return;
    drive_vol = (uint8_t)vol;
    drive_gen = info.gen;
    drive_is_floppy = info.is_floppy;
    drive_total = info.block_count;
    if (drive_full && !drive_is_floppy)
    {
        mon_add_response_utf8(S(STR_ERR_NOT_FORMATTABLE));
        return;
    }
    if (drive_layout == DRIVE_LAYOUT_GPT && !FF_LBA64)
    {
        mon_add_response_utf8(S(STR_ERR_GPT_DISABLED));
        return;
    }
    if (drive_layout == DRIVE_LAYOUT_GPT && drive_is_floppy)
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    if (drive_layout == DRIVE_LAYOUT_AUTO)
    {
        if (drive_is_floppy)
            drive_layout = DRIVE_LAYOUT_SFD;
#if FF_LBA64
        else if (info.block_count >= DRIVE_GPT_DEFAULT)
            drive_layout = DRIVE_LAYOUT_GPT;
#endif
        else
            drive_layout = DRIVE_LAYOUT_MBR;
    }
    FRESULT fr = drive_preview_mkfs();
    msc_drive_reenumerate(drive_vol);
    if (fr != FR_OK)
    {
        mon_add_response_fatfs(fr);
        return;
    }
    drive_preview_op = DRIVE_PREVIEW_FORMAT;
    mon_add_response_fn(drive_preview_response);
    mon_response_confirm(drive_run_format);
}

static void drive_erase(const char *args)
{
    int vol = drive_parse_only(args, STR_ERASE);
    if (vol < 0)
        return;
    msc_drive_info_t info;
    if (!drive_validate((uint8_t)vol, &info, true))
        return;
    drive_vol = (uint8_t)vol;
    drive_gen = info.gen;
    drive_total = info.block_count;
    drive_preview_op = DRIVE_PREVIEW_ERASE;
    mon_add_response_fn(drive_preview_response);
    mon_response_confirm(drive_run_erase);
}

static void drive_verify(const char *args)
{
    int vol = drive_parse_only(args, STR_VERIFY);
    if (vol < 0)
        return;
    msc_drive_info_t info;
    if (!drive_validate((uint8_t)vol, &info, false))
        return;
    drive_vol = (uint8_t)vol;
    drive_total = info.block_count;
    drive_preview_op = DRIVE_PREVIEW_PLAIN;
    mon_add_response_fn(drive_preview_response);
    drive_lba = 0;
    drive_last_pct = -1;
    drive_bad = 0;
    drive_pin_n = 0;
    drive_state = DRIVE_RUN_VERIFY;
    mon_add_response_fn(drive_run_response);
}

static int drive_label_response(char *buf, size_t size, int state, unsigned)
{
    if (state < 0)
        return state;
    if (drive_label_changed)
        oem_snprintf(buf, size, S(STR_DISK_LABEL_CHANGED), drive_label_old, drive_label_cur);
    else
        oem_snprintf(buf, size, S(STR_DISK_LABEL_RESPONSE), drive_label_cur);
    return -1;
}

static void drive_label(const char *args)
{
    int vol = -1;
    const char *newlabel = NULL;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        if (drive_match(t, &vol))
        {
        }
        else if (!newlabel)
            newlabel = t;
        else
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
    }
    if (vol < 0)
    {
        drive_sub_help(STR_LABEL);
        return;
    }
    msc_drive_info_t info;
    if (!drive_validate((uint8_t)vol, &info, false))
        return;
    DWORD vsn;
    if (!newlabel)
    {
        FRESULT fr = f_getlabel(drive_path, drive_label_cur, &vsn);
        if (fr != FR_OK)
        {
            mon_add_response_fatfs(fr);
            return;
        }
        drive_label_changed = false;
        mon_add_response_fn(drive_label_response);
        return;
    }
    if (info.write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return;
    }
    if (f_getlabel(drive_path, drive_label_old, &vsn) != FR_OK)
        drive_label_old[0] = '\0';
    FRESULT fr = drive_set_label(newlabel);
    if (fr != FR_OK)
    {
        mon_add_response_fatfs(fr);
        return;
    }
    if (f_getlabel(drive_path, drive_label_cur, &vsn) != FR_OK)
        drive_label_cur[0] = '\0';
    drive_label_changed = true;
    mon_add_response_fn(drive_label_response);
}

static void drive_info(const char *args)
{
    int vol = drive_parse_only(args, STR_INFO);
    if (vol < 0)
        return;
    msc_drive_info_t info;
    if (!drive_validate((uint8_t)vol, &info, false))
        return;
    drive_vol = (uint8_t)vol;
    drive_preview_op = DRIVE_PREVIEW_PLAIN;
    mon_add_response_fn(drive_preview_response);
}

bool drive_active(void)
{
    return drive_state != DRIVE_IDLE;
}

void drive_break(void)
{
    drive_state = DRIVE_IDLE;
}

void drive_mon_disk(const char *args)
{
    const char *sub = str_parse_string(&args);
    if (!sub)
    {
        mon_add_response_utf8(S(STR_HELP_DISK));
        return;
    }
    if (!strcasecmp(sub, STR_INFO))
        drive_info(args);
    else if (!strcasecmp(sub, STR_FORMAT))
        drive_format(args);
    else if (!strcasecmp(sub, STR_ERASE))
        drive_erase(args);
    else if (!strcasecmp(sub, STR_VERIFY))
        drive_verify(args);
    else if (!strcasecmp(sub, STR_LABEL))
        drive_label(args);
    else
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
}
