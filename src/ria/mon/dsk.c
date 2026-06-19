/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "mon/dsk.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/mem.h"
#include "sys/ria.h"
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

// RP6502 FF_MIN_GPT hook: definition for the runtime threshold the FatFs edits
// reference (ffconf.h [1/3] macro + [2/3] declaration, ff.c [3/3]). The default reproduces the
// stock threshold; dsk_do_mkfs forces MBR or GPT around f_mkfs(). The <=2^32 doc
// max (which the deleted ff.c #error guarded) holds for every value written to
// dsk_gpt_threshold: the static_assert covers the default, and dsk_do_mkfs only
// ever assigns 0 (force GPT) or (LBA_t)-1 (force MBR; intentionally above the max
// so GPT is never chosen). Only the GPT-capable (exFAT/LBA64) build references it.
#if FF_LBA64
#define DSK_GPT_DEFAULT 0x10000000
static_assert(DSK_GPT_DEFAULT <= 0x100000000ULL, "FF_MIN_GPT default out of range");
static LBA_t dsk_gpt_threshold = DSK_GPT_DEFAULT;
unsigned long long dsk_min_gpt(void) { return dsk_gpt_threshold; }
#endif

// On-disk layout for "disk format". AUTO resolves by device class/size.
enum
{
    DSK_LAYOUT_AUTO,
    DSK_LAYOUT_SFD,
    DSK_LAYOUT_MBR,
    DSK_LAYOUT_GPT,
};

static enum {
    DSK_IDLE,
    DSK_RUN_FORMAT_UNIT,
    DSK_RUN_MKFS,
    DSK_RUN_ERASE,
    DSK_RUN_VERIFY,
} dsk_state;

// Trailing preview lines the generator emits after the shared info block.
enum
{
    DSK_PREVIEW_PLAIN, // info / verify: info block only
    DSK_PREVIEW_FORMAT,
    DSK_PREVIEW_ERASE,
};

// Operation context, valid from preview through completion.
static uint8_t dsk_vol;  // logical volume (MSCn:)
static uint8_t dsk_gen;  // mount generation captured at preview (TOCTOU guard)
static char dsk_path[6]; // "MSCn:" for FatFs calls
static bool dsk_is_floppy;
static uint8_t dsk_fs;   // 0=auto, 1=FAT, 2=exFAT
static bool dsk_full;    // /full low-level format
static uint32_t dsk_au;  // allocation unit bytes (requested, then resolved)
static uint8_t dsk_fsty; // resolved FS_FAT12/16/32/EXFAT for format
static bool dsk_has_label;
static char dsk_label_oem[12];
static uint8_t dsk_layout;     // DSK_LAYOUT_* for format
static uint8_t dsk_preview_op; // DSK_PREVIEW_* trailing lines for the generator
static uint8_t dsk_fmt_track;  // current track in the per-track format loop
static uint8_t dsk_fmt_head;   // current head in the per-track format loop
static uint8_t dsk_fmt_tracks; // track count for the loop
static uint8_t dsk_fmt_heads;  // head count for the loop
static uint64_t dsk_total; // sectors for zero/verify
static uint64_t dsk_lba;   // current sector
static int dsk_last_pct;
static uint32_t dsk_bad;   // verify bad-sector count
static uint32_t dsk_pin_n; // verify: sectors in the failing chunk being pinpointed (0 = none)
static uint32_t dsk_pin_i; // verify: cursor within that chunk

// Cached f_getfree result so the two VOL preview lines share one FAT scan.
static struct
{
    bool valid;
    DWORD csize;
    DWORD n_fatent;
    DWORD nclst;
} dsk_free;

// Label result captured synchronously, emitted by dsk_label_response.
static char dsk_label_old[24]; // previous label (changed case)
static char dsk_label_cur[24]; // current or new label to display
static bool dsk_label_changed; // false: show current; true: old -> new

// Parse an allocation-unit option like "/16k" or "/512". Returns false unless
// it is a power of two from 512 bytes to 16 MiB. Rejects values that would
// overflow before the range check rather than letting them wrap into range.
static bool dsk_parse_alloc(const char *tok, uint32_t *au)
{
    const char *p = tok + 1; // skip '/'
    if (!isdigit((unsigned char)*p))
        return false;
    uint32_t v = 0;
    while (isdigit((unsigned char)*p))
    {
        v = v * 10 + (uint32_t)(*p++ - '0');
        if (v > 0x1000000) // already past the max; no suffix shrinks it
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

static const char *dsk_fs_name(BYTE fs_type)
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

// Scheme word for the current on-disk layout, or NULL if unknown.
static const char *dsk_scheme_word(void)
{
    if (!msc_dsk_read(dsk_vol, mbuf, 0, 1))
        return NULL;
    if (dsk_is_fat_vbr(mbuf))
        return STR_SFD;
    if (mbuf[510] == 0x55 && mbuf[511] == 0xAA)
        return (mbuf[446 + 4] == 0xEE) ? STR_GPT : STR_MBR;
    return NULL;
}

// Build the "<scheme> <filesystem> <cluster> <suffix> (<label>)" descriptor shared
// by the VOL (current) and FMT (target) lines. scheme: word or NULL (omit).
// fsname: the filesystem name (never NULL). au_bytes: cluster size in KB (512 B
// shown in bytes), 0 to omit when there is no filesystem. suffix: extra word
// (Quick/Full) or NULL. label: appended in parens when non-empty (NULL/"" omits).
// The `n >= size` guards stop a truncated segment from underflowing size - n.
static void dsk_fmt_desc(char *out, size_t size, const char *scheme, const char *fsname,
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

// Device/volume preview as a monitor response generator: one line per call (an
// empty fill skips a line). dsk_preview_op adds the warning / target lines.
// Routed through the response queue so the \a alignment markers work.
static int dsk_preview_response(char *buf, size_t size, int state)
{
    if (state < 0)
        return state;
    switch (state)
    {
    case 0: // DEV: device size (matches the status command) + inquiry strings
    {
        char vendor[9], product[17], rev[5];
        if (msc_dsk_inquiry_strings(dsk_vol, vendor, product, rev))
        {
            msc_dsk_info_t info;
            char szbuf[24];
            szbuf[0] = '\0';
            if (msc_dsk_get_info(dsk_vol, &info) && info.block_size)
                str_size((uint64_t)info.block_count * info.block_size, szbuf, sizeof(szbuf));
            snprintf_utf8(buf, size, S(STR_DISK_DEV), szbuf, vendor, product, rev);
        }
        return 1;
    }
    case 1:
    {
        char serial[USB_DESC_STRING_BUF_SIZE];
        if (msc_dsk_serial(dsk_vol, serial, sizeof(serial)))
            snprintf_utf8(buf, size, S(STR_DISK_SERIAL), serial);
        return 2;
    }
    case 2: // VOL: boot scheme, filesystem, cluster size, and volume label
    {
        char desc[64], label[24];
        DWORD vsn;
        const char *scheme = dsk_scheme_word();
        if (f_getlabel(dsk_path, label, &vsn) != FR_OK)
            label[0] = '\0';
        if (dsk_preview_op == DSK_PREVIEW_PLAIN)
        {
            // INFO/VERIFY: a full scan feeds the "used of total" line (case 3).
            DWORD nclst;
            FATFS *fs;
            dsk_free.valid = false;
            if (f_getfree(dsk_path, &nclst, &fs) == FR_OK)
            {
                dsk_free.valid = true;
                dsk_free.csize = fs->csize;
                dsk_free.n_fatent = fs->n_fatent;
                dsk_free.nclst = nclst;
                dsk_fmt_desc(desc, sizeof(desc), scheme, dsk_fs_name(fs->fs_type),
                             fs->csize * 512u, NULL, label);
            }
            else
                dsk_fmt_desc(desc, sizeof(desc), scheme, S(STR_PARENS_NONE), 0, NULL, label);
            snprintf_utf8(buf, size, S(STR_DISK_VOL_FMT), desc);
            return 3;
        }
        // FORMAT/ERASE: skip the usage scan (about to wipe); read FS type and
        // cluster straight from the mounted object.
        uint8_t fsty;
        uint32_t csize;
        if (msc_dsk_fs_geom(dsk_vol, &fsty, &csize))
            dsk_fmt_desc(desc, sizeof(desc), scheme, dsk_fs_name(fsty), csize * 512u, NULL, label);
        else
            dsk_fmt_desc(desc, sizeof(desc), scheme, S(STR_PARENS_NONE), 0, NULL, label);
        snprintf_utf8(buf, size, S(STR_DISK_VOL_FMT), desc);
        return 4; // skip the usage line; go to the warning
    }
    case 3: // VOL: filesystem used of total, percent used (PLAIN only; case 2's scan)
    {
        if (dsk_free.valid)
        {
            uint64_t totb = (uint64_t)(dsk_free.n_fatent - 2) * dsk_free.csize * 512u;
            uint64_t freeb = (uint64_t)dsk_free.nclst * dsk_free.csize * 512u;
            uint64_t usedb = totb - freeb;
            unsigned pct = totb ? (unsigned)(usedb * 100 / totb) : 0;
            char usedbuf[24], totbuf[24];
            str_size(usedb, usedbuf, sizeof(usedbuf));
            str_size(totb, totbuf, sizeof(totbuf));
            snprintf_utf8(buf, size, S(STR_DISK_VOL_USE), usedbuf, totbuf, pct);
        }
        return -1;
    }
    case 4: // confirm warning (format or erase)
        snprintf_utf8(buf, size, S(dsk_preview_op == DSK_PREVIEW_ERASE ? STR_DISK_WARN_ERASE : STR_DISK_WARN_FORMAT));
        return dsk_preview_op == DSK_PREVIEW_FORMAT ? 5 : -1;
    case 5: // FMT: target layout, filesystem, cluster size, quick/full, label
    {
        const char *scheme = dsk_layout == DSK_LAYOUT_SFD   ? STR_SFD
                             : dsk_layout == DSK_LAYOUT_GPT ? STR_GPT
                                                            : STR_MBR;
        char desc[64];
        dsk_fmt_desc(desc, sizeof(desc), scheme, dsk_fs_name(dsk_fsty), dsk_au,
                     dsk_full ? STR_FULL : STR_QUICK,
                     dsk_has_label ? dsk_label_oem : NULL);
        snprintf_utf8(buf, size, S(STR_DISK_FMT), desc);
        return -1;
    }
    default:
        return -1;
    }
}

// Standard FAT cluster-count limits (FAT spec, not FatFs internals).
#define DSK_MAX_FAT12 0xFF5u  // 4085
#define DSK_MAX_FAT16 0xFFF5u // 65525
#define DSK_MAX_FAT32 0x0FFFFFF5u
#define DSK_MIN_FAT 32u // f_mkfs's MIN_FAT12 floor (ff.c), applies to any FAT type

// Partition base f_mkfs reserves ahead of the volume, in 512 B sectors:
// MBR 63 / GPT 2081 (2048 aligned + the 32+1 GPT structures) / SFD 0.
static LBA_t dsk_part_base(uint8_t layout)
{
    return layout == DSK_LAYOUT_GPT ? 2081 : layout == DSK_LAYOUT_MBR ? 63 : 0;
}

// First-pass cluster count, exactly what f_mkfs uses to pick the FAT sub-type
// (ff.c:6369): volume sectors minus the partition base, divided by the cluster
// size — before any reserved/FAT/root overhead.
static LBA_t dsk_fat_first_pass(LBA_t raw, uint8_t layout, UINT au)
{
    LBA_t base = dsk_part_base(layout);
    return raw > base ? (raw - base) / au : 0;
}

// Final data-cluster count a FAT/FAT32 volume holds, matching f_mkfs's own
// accounting: the first-pass count picks the FAT width (and thus the reserved
// area, two FATs, and 512-entry root), then the overhead is removed (ff.c:6397).
// The width here only sizes the overhead; the caller selects the sub-type from
// dsk_fat_first_pass(), as f_mkfs does. (FAT layout is the spec; the 63/2081
// bases are de-facto standard, not FatFs internals.)
static LBA_t dsk_fat_data_clusters(LBA_t raw, uint8_t layout, UINT au)
{
    LBA_t base = dsk_part_base(layout);
    if (raw <= base)
        return 0;
    LBA_t vol = raw - base;
    LBA_t n0 = vol / au; // first-pass count selects the FAT width
    LBA_t rsv, dir, fatb;
    if (n0 > DSK_MAX_FAT16)
    {
        rsv = 32; // FAT32 reserved area
        dir = 0;  // root is a cluster chain, not static
        fatb = n0 * 4 + 8;
    }
    else if (n0 > DSK_MAX_FAT12)
    {
        rsv = 1;
        dir = 32; // 512-entry root directory
        fatb = n0 * 2 + 4;
    }
    else
    {
        rsv = 1;
        dir = 32;
        fatb = (n0 * 3 + 1) / 2 + 3; // 12-bit entries
    }
    LBA_t overhead = rsv + 2 * ((fatb + 511) / 512) + dir; // reserved + 2 FATs + root
    if (vol <= overhead)
        return 0;
    return (vol - overhead) / au;
}

// The disk tool's own, stable FS-selection policy, so the result is OUR contract
// rather than whatever f_mkfs would auto-pick. We hand f_mkfs an explicit type +
// cluster size via its public MKFS_PARM (so its internal auto-selection never
// runs). The sub-type is chosen from the first-pass count (dsk_fat_first_pass),
// exactly as f_mkfs picks it, while dsk_fat_data_clusters() validates the final
// post-overhead count so a forced (type, cluster) can never abort. `raw` is the
// device sector count (512 B sectors); `layout` is the resolved DSK_LAYOUT_*.
// `want`: 0=auto, 1=FAT family, 2=exFAT; a user /Nk (req_au_bytes) overrides the
// cluster size. Returns FS_FAT12/16/32/EXFAT + cluster size in *au_sectors, or 0
// if unsatisfiable.
static BYTE dsk_resolve_fs(LBA_t raw, uint8_t want, uint32_t req_au_bytes, UINT *au_sectors, uint8_t layout)
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
            au = 8; // 4 KB
            if (raw >= 0x80000)
                au = 64; // >= 256 MiB -> 32 KB
            if (raw >= 0x4000000)
                au = 256; // >= 32 GiB  -> 128 KB
        }
        *au_sectors = au;
        return FS_EXFAT;
    }
#endif
#if FF_LBA64
    if (raw >= 0x100000000)
        return 0; // beyond FAT sector addressing; exFAT only
#endif
    // Explicit cluster: the sub-type follows the first-pass count, exactly as
    // f_mkfs picks it, so the preview matches what gets built. f_mkfs clamps the
    // FAT/FAT32 cluster to 128 sectors (ff.c:6176); mirror that first.
    if (req_au_bytes)
    {
        UINT au = req_au_bytes / 512;
        if (au > 128)
            au = 128;
        LBA_t fp = dsk_fat_first_pass(raw, layout, au);
        LBA_t n = dsk_fat_data_clusters(raw, layout, au);
        if (fp == 0 || n < DSK_MIN_FAT)
            return 0;
        if (fp <= DSK_MAX_FAT12) // FAT12: final <= first-pass, always valid
        {
            *au_sectors = au;
            return FS_FAT12;
        }
        if (fp <= DSK_MAX_FAT16) // FAT16: only if the final count stays in range
        {
            if (n <= DSK_MAX_FAT12 || n > DSK_MAX_FAT16)
                return 0; // f_mkfs would FR_MKFS_ABORTED at this fixed cluster
            *au_sectors = au;
            return FS_FAT16;
        }
        if (fp > DSK_MAX_FAT32 || n <= DSK_MAX_FAT16)
            return 0; // beyond FAT32, or too few clusters after overhead
        *au_sectors = au;
        return FS_FAT32;
    }
    // Auto cluster: grow the cluster until the first-pass count fits FAT16 (cap at
    // 32 KB), as f_mkfs would; media within the FAT12 limit becomes FAT12. The
    // final-count tests mirror f_mkfs's validity checks so the chosen (type,
    // cluster) never aborts; the boundary case bumps the cluster once more.
    UINT au = 1;
    while (dsk_fat_first_pass(raw, layout, au) > DSK_MAX_FAT16 && au < 64)
        au <<= 1;
    for (; au <= 64; au <<= 1)
    {
        LBA_t fp = dsk_fat_first_pass(raw, layout, au);
        LBA_t n = dsk_fat_data_clusters(raw, layout, au);
        if (fp == 0 || n < DSK_MIN_FAT)
            return 0;
        if (fp > DSK_MAX_FAT16)
            break; // needs FAT32
        if (fp <= DSK_MAX_FAT12)
        {
            *au_sectors = au;
            return FS_FAT12;
        }
        if (n > DSK_MAX_FAT12 && n <= DSK_MAX_FAT16)
        {
            *au_sectors = au;
            return FS_FAT16;
        }
        // Boundary: overhead pushed the FAT16 count into FAT12 territory; a larger
        // cluster makes the first-pass count FAT12, so try again.
    }
    // Too many clusters for FAT16 -> FAT32, with a >= 4 KB cluster scaled by size
    // so the count stays well clear of the FAT16 ceiling.
    au = 8; // 4 KB
    if (raw >= 0x1000000)
        au = 16; // >= 8 GiB  -> 8 KB
    if (raw >= 0x2000000)
        au = 32; // >= 16 GiB -> 16 KB
    if (raw >= 0x4000000)
        au = 64; // >= 32 GiB -> 32 KB (FAT32 cluster max)
    while (dsk_fat_data_clusters(raw, layout, au) > DSK_MAX_FAT32 && au < 128)
        au <<= 1;
    LBA_t nc = dsk_fat_data_clusters(raw, layout, au);
    if (nc <= DSK_MAX_FAT16 || nc > DSK_MAX_FAT32)
        return 0; // too few (FAT32 floor) or too many; use exFAT
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
    // MBR. Force the chosen scheme; FM_SFD ignores it. (LBA_t)-1 intentionally
    // exceeds the 2^32 doc max so GPT is never chosen.
    dsk_gpt_threshold = (dsk_layout == DSK_LAYOUT_GPT) ? 0 : (LBA_t)-1;
#endif
    FRESULT fr = f_mkfs(dsk_path, &parm, mbuf, MBUF_SIZE);
#if FF_LBA64
    dsk_gpt_threshold = DSK_GPT_DEFAULT;
#endif
    if (fr == FR_OK && dsk_has_label)
    {
        char arg[sizeof(dsk_path) + sizeof(dsk_label_oem)];
        snprintf(arg, sizeof(arg), "%s%s", dsk_path, dsk_label_oem);
        fr = f_setlabel(arg);
    }
    return fr;
}

// Floppy track/head geometry for the per-track FORMAT UNIT loop, by sector count.
// Single-sided 160 KB (320) and 180 KB (360) media have one head; every other
// standard floppy has two. Tracks is 40 up to the 360 KB formats, else 80.
static void dsk_floppy_geometry(uint64_t blocks, uint8_t *tracks, uint8_t *heads)
{
    *heads = (blocks == 320 || blocks == 360) ? 1 : 2;
    *tracks = (blocks <= 720) ? 40 : 80;
}

// One mon_response producer drives a whole run (format/erase/verify), one chunk
// per call, emitting at most one line; progress redraws in place via \r, so
// unchanged-percent ticks emit nothing. dsk_state selects the phase. Ctrl-C
// aborts via main_break(), whose break_() resets the queue and owns dsk IDLE.
static int dsk_run_response(char *buf, size_t size, int state)
{
    if (state < 0)
        return state; // response cancelled (break)
    switch (dsk_state)
    {
    case DSK_RUN_FORMAT_UNIT:
        if (ria_get_sigint()) // Ctrl-C stops the format between tracks
        {
            putchar('\n');
            msc_dsk_reenumerate(dsk_vol); // partial low-level format; drop stale mount
            main_break();
            return -1;
        }
        if (!msc_dsk_format_track(dsk_vol, dsk_fmt_track, dsk_fmt_head))
        {
            mon_add_response_utf8(S(STR_ERR_FORMAT_FAILED));
            msc_dsk_reenumerate(dsk_vol);
            dsk_state = DSK_IDLE;
            if (dsk_last_pct >= 0) // break from the in-place progress line
                snprintf(buf, size, "\n");
            return -1;
        }
        if (++dsk_fmt_head >= dsk_fmt_heads) // this track/head done; advance
        {
            dsk_fmt_head = 0;
            dsk_fmt_track++;
        }
        {
            uint32_t total = (uint32_t)dsk_fmt_tracks * dsk_fmt_heads;
            uint32_t done = (uint32_t)dsk_fmt_track * dsk_fmt_heads + dsk_fmt_head;
            int overall = total ? (int)(done * 100 / total) : 100;
            if (overall != dsk_last_pct)
            {
                dsk_last_pct = overall;
                snprintf_utf8(buf, size, STR_DISK_PROG_FORMAT, overall);
            }
        }
        if (dsk_fmt_track >= dsk_fmt_tracks)
            dsk_state = DSK_RUN_MKFS; // mkfs emits the line break before its result
        return 0;

    case DSK_RUN_MKFS:
    {
        FRESULT fr = dsk_do_mkfs();
        msc_dsk_reenumerate(dsk_vol);
        dsk_state = DSK_IDLE;
        // A per-track pass left the cursor on "\rFormat 100%"; break from it
        // before the result. The quick path printed no progress.
        const char *nl = dsk_last_pct >= 0 ? "\n" : "";
        if (fr == FR_OK)
            snprintf_utf8(buf, size, "%s%s", nl, S(STR_DISK_DONE));
        else
        {
            mon_add_response_fatfs(fr);
            snprintf(buf, size, "%s", nl);
        }
        return -1;
    }

    case DSK_RUN_ERASE:
        if (ria_get_sigint()) // Ctrl-C stops the erase
        {
            putchar('\n');
            msc_dsk_reenumerate(dsk_vol); // sectors were zeroed; drop stale mount
            main_break();
            return -1;
        }
        if (dsk_lba >= dsk_total) // all sectors zeroed; final progress already shown
        {
            msc_dsk_reenumerate(dsk_vol);
            dsk_state = DSK_IDLE;
            snprintf_utf8(buf, size, "\n%s", S(STR_DISK_DONE));
            return -1;
        }
        {
            // Chunk is bounded by mbuf, the only scratch available; disk_write would
            // accept more sectors per transfer, but a larger buffer is not affordable.
            // mbuf was zeroed once in dsk_run_erase and nothing dirties it between
            // ticks, so it stays zero — no per-chunk memset. (512 B sectors only;
            // dsk_validate rejects anything else.)
            uint32_t per = MBUF_SIZE / 512;
            uint64_t remain = dsk_total - dsk_lba;
            uint32_t n = remain < per ? (uint32_t)remain : per;
            if (!msc_dsk_write(dsk_vol, mbuf, dsk_lba, n))
            {
                mon_add_response_fatfs(FR_DISK_ERR);
                msc_dsk_reenumerate(dsk_vol);
                dsk_state = DSK_IDLE;
                snprintf(buf, size, "\n"); // break from the in-place progress line
                return -1;
            }
            dsk_lba += n;
            int pct = (int)(dsk_lba * 100 / dsk_total);
            if (pct != dsk_last_pct)
            {
                dsk_last_pct = pct;
                snprintf_utf8(buf, size, STR_DISK_PROG_ERASE, pct);
            }
        }
        return 0;

    case DSK_RUN_VERIFY:
        if (ria_get_sigint()) // Ctrl-C stops the scan
        {
            putchar('\n');
            main_break();
            return -1;
        }
        if (dsk_pin_n) // re-reading a failed chunk one sector at a time
        {
            while (dsk_pin_i < dsk_pin_n)
            {
                uint64_t lba = dsk_lba + dsk_pin_i++;
                if (!msc_dsk_read(dsk_vol, mbuf, lba, 1))
                {
                    dsk_bad++;
                    buf[0] = '\r'; // overwrite the transient progress line
                    snprintf_utf8(buf + 1, size - 1, S(STR_DISK_BAD_SECTOR), (unsigned long long)lba);
                    return 0;
                }
            }
            dsk_lba += dsk_pin_n; // chunk pinpointed; redraw progress, fall through
            dsk_pin_n = 0;
            dsk_last_pct = -1;
        }
        else if (dsk_lba < dsk_total) // scan the next chunk
        {
            uint32_t per = MBUF_SIZE / 512; // 512 B sectors only (see dsk_validate)
            uint64_t remain = dsk_total - dsk_lba;
            uint32_t n = remain < per ? (uint32_t)remain : per;
            if (!msc_dsk_read(dsk_vol, mbuf, dsk_lba, n))
            {
                dsk_pin_n = n; // pinpoint the bad sector(s) on the next calls
                dsk_pin_i = 0;
                return 0;
            }
            dsk_lba += n;
        }
        {
            int pct = (int)(dsk_lba * 100 / dsk_total);
            if (pct != dsk_last_pct)
            {
                dsk_last_pct = pct;
                snprintf_utf8(buf, size, STR_DISK_PROG_VERIFY, pct);
                return 0;
            }
        }
        if (dsk_lba >= dsk_total)
        {
            dsk_state = DSK_IDLE;
            buf[0] = '\n';
            snprintf_utf8(buf + 1, size - 1, S(STR_DISK_VERIFY_DONE), (int)dsk_bad);
            return -1;
        }
        return 0;

    default:
        dsk_state = DSK_IDLE;
        return -1;
    }
}

// Re-validate the target at the moment of a destructive run, not just at preview.
// A USB hot-swap during the confirm prompt frees the slot and a new device can
// reuse it, so the captured generation must still match; media/write-protect can
// also have changed. Queues the matching error and returns false on any mismatch.
static bool dsk_run_revalidate(bool need_writable)
{
    if (msc_dsk_gen(dsk_vol) != dsk_gen)
    {
        mon_add_response_utf8(S(STR_ERR_DEVICE_CHANGED));
        return false;
    }
    msc_dsk_info_t info;
    if (!msc_dsk_get_info(dsk_vol, &info) || !info.present)
    {
        mon_add_response_utf8(S(STR_ERR_NO_MEDIA));
        return false;
    }
    if (info.block_count == 0 || info.block_size != 512)
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return false;
    }
    if (need_writable && info.write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return false;
    }
    return true;
}

static void dsk_run_format(void)
{
    if (!dsk_run_revalidate(true))
    {
        dsk_state = DSK_IDLE;
        return;
    }
    dsk_last_pct = -1;
    dsk_fmt_track = 0;
    dsk_fmt_head = 0;
    if (dsk_full && dsk_is_floppy)
    {
        dsk_floppy_geometry(dsk_total, &dsk_fmt_tracks, &dsk_fmt_heads);
        mon_add_response_utf8(S(STR_DISK_FORMATTING)); // banner before the per-track pass
        dsk_state = DSK_RUN_FORMAT_UNIT;
    }
    else
        dsk_state = DSK_RUN_MKFS;
    mon_add_response_fn(dsk_run_response);
}

static void dsk_run_erase(void)
{
    if (!dsk_run_revalidate(true))
    {
        dsk_state = DSK_IDLE;
        return;
    }
    dsk_last_pct = -1;
    dsk_lba = 0;
    memset(mbuf, 0, MBUF_SIZE); // zero once; the chunk loop never dirties mbuf
    mon_add_response_utf8(S(STR_DISK_ERASING)); // banner before the zero pass
    dsk_state = DSK_RUN_ERASE;
    mon_add_response_fn(dsk_run_response);
}

// Validate a resolved volume: fill *info and require present[, writable].
// Sets dsk_path and returns true, or queues the error and returns false.
static bool dsk_validate(uint8_t vol, msc_dsk_info_t *info, bool need_writable)
{
    if (!msc_dsk_get_info(vol, info))
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return false;
    }
    if (!info->present)
    {
        mon_add_response_utf8(S(STR_ERR_NO_MEDIA));
        return false;
    }
    // A present volume always has nonzero geometry; 0 sectors means a bogus or
    // overflowed READ CAPACITY. Reject so the progress math never divides by 0.
    if (info->block_count == 0)
    {
        mon_add_response_fatfs(FR_INVALID_DRIVE);
        return false;
    }
    // 512-byte logical sectors only: FatFs is built FF_MAX_SS==512 and mbuf is
    // the sole scratch for the raw erase/verify passes, so a larger sector
    // cannot be buffered.
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
    msc_vol_path(dsk_path, vol); // canonical "MSCn:" so "0:" and "MSC0:" agree
    return true;
}

// Parse a drive-only argument list (info/erase/verify). Returns the volume, or
// -1 after queueing sub's help (no drive) or an argument error (garbage/extra).
static int dsk_parse_drive_only(const char *args, const char *sub)
{
    int vol = -1;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        int v;
        if (vol < 0 && (v = msc_dsk_vol_from_name(t)) >= 0)
            vol = v;
        else
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return -1;
        }
    }
    if (vol < 0)
    {
        hlp_disk_sub_response(sub);
        return -1;
    }
    return vol;
}

static void dsk_format(const char *args)
{
    dsk_fs = 0;
    dsk_full = false;
    dsk_au = 0;
    dsk_has_label = false;
    dsk_layout = DSK_LAYOUT_AUTO;
    // Tokens may appear in any order: a '/' token is a flag, the first token
    // that names a volume is the drive, any other token is the label.
    int vol = -1;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        int v;
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
        else if (vol < 0 && (v = msc_dsk_vol_from_name(t)) >= 0)
            vol = v;
        else if (!dsk_has_label)
        {
            // Volume label (OEM bytes from the terminal, as FatFs expects).
            // An empty "" clears the label.
            snprintf(dsk_label_oem, sizeof(dsk_label_oem), "%s", t);
            dsk_has_label = true;
        }
        else
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
    }
    if (vol < 0)
    {
        hlp_disk_sub_response(STR_FORMAT);
        return;
    }
    msc_dsk_info_t info;
    if (!dsk_validate((uint8_t)vol, &info, true))
        return;
    if (dsk_fs == 2 && !FF_FS_EXFAT)
    {
        mon_add_response_utf8(S(STR_ERR_EXFAT_DISABLED));
        return;
    }
    dsk_vol = (uint8_t)vol;
    dsk_gen = msc_dsk_gen(dsk_vol); // re-checked at YES against a hot-swap
    dsk_is_floppy = info.is_floppy;
    dsk_total = info.block_count; // sector count for the per-track format geometry
    if (dsk_full && !dsk_is_floppy)
    {
        mon_add_response_utf8(S(STR_ERR_NOT_FORMATTABLE));
        return;
    }
    if (dsk_layout == DSK_LAYOUT_GPT && !FF_LBA64)
    {
        mon_add_response_utf8(S(STR_ERR_GPT_DISABLED));
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
    dsk_fsty = dsk_resolve_fs(info.block_count, dsk_fs, dsk_au, &au_sectors, dsk_layout);
    if (dsk_fsty == 0)
    {
        mon_add_response_fatfs(FR_MKFS_ABORTED);
        return;
    }
    dsk_au = au_sectors * 512u; // resolved cluster size in bytes
    dsk_preview_op = DSK_PREVIEW_FORMAT;
    mon_add_response_fn(dsk_preview_response);
    mon_response_confirm(dsk_run_format);
}

static void dsk_erase(const char *args)
{
    int vol = dsk_parse_drive_only(args, STR_ERASE);
    if (vol < 0)
        return;
    msc_dsk_info_t info;
    if (!dsk_validate((uint8_t)vol, &info, true))
        return;
    dsk_vol = (uint8_t)vol;
    dsk_gen = msc_dsk_gen(dsk_vol); // re-checked at YES against a hot-swap
    dsk_total = info.block_count;
    dsk_preview_op = DSK_PREVIEW_ERASE;
    mon_add_response_fn(dsk_preview_response);
    mon_response_confirm(dsk_run_erase);
}

static void dsk_verify(const char *args)
{
    int vol = dsk_parse_drive_only(args, STR_VERIFY);
    if (vol < 0)
        return;
    msc_dsk_info_t info;
    if (!dsk_validate((uint8_t)vol, &info, false))
        return;
    dsk_vol = (uint8_t)vol;
    dsk_total = info.block_count;
    dsk_preview_op = DSK_PREVIEW_PLAIN;
    mon_add_response_fn(dsk_preview_response); // info block first...
    // ...then the read-only scan, queued behind it (no YES needed).
    dsk_lba = 0;
    dsk_last_pct = -1;
    dsk_bad = 0;
    dsk_pin_n = 0;
    dsk_state = DSK_RUN_VERIFY;
    mon_add_response_fn(dsk_run_response);
}

// One-line label result through the response queue (width-aware, paged) instead
// of a bare printf, like the rest of the monitor.
static int dsk_label_response(char *buf, size_t size, int state)
{
    if (state < 0)
        return state;
    if (dsk_label_changed)
        snprintf_utf8(buf, size, S(STR_DISK_LABEL_CHANGED), dsk_label_old, dsk_label_cur);
    else
        snprintf_utf8(buf, size, S(STR_DISK_LABEL_RESPONSE), dsk_label_cur);
    return -1;
}

static void dsk_label(const char *args)
{
    // drive + optional label, in any order.
    int vol = -1;
    const char *newlabel = NULL;
    const char *t;
    while ((t = str_parse_string(&args)) != NULL)
    {
        int v;
        if (vol < 0 && (v = msc_dsk_vol_from_name(t)) >= 0)
            vol = v;
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
        hlp_disk_sub_response(STR_LABEL);
        return;
    }
    msc_dsk_info_t info;
    if (!dsk_validate((uint8_t)vol, &info, false))
        return;
    DWORD vsn;
    if (!newlabel) // show current
    {
        FRESULT fr = f_getlabel(dsk_path, dsk_label_cur, &vsn);
        if (fr != FR_OK)
        {
            mon_add_response_fatfs(fr);
            return;
        }
        dsk_label_changed = false;
        mon_add_response_fn(dsk_label_response);
        return;
    }
    if (info.write_prot)
    {
        mon_add_response_fatfs(FR_WRITE_PROTECTED);
        return;
    }
    if (f_getlabel(dsk_path, dsk_label_old, &vsn) != FR_OK)
        dsk_label_old[0] = '\0';
    // An empty "" label clears it.
    char arg[sizeof(dsk_path) + sizeof(dsk_label_oem)];
    snprintf(arg, sizeof(arg), "%s%s", dsk_path, newlabel);
    FRESULT fr = f_setlabel(arg);
    if (fr != FR_OK)
    {
        mon_add_response_fatfs(fr);
        return;
    }
    if (f_getlabel(dsk_path, dsk_label_cur, &vsn) != FR_OK)
        dsk_label_cur[0] = '\0';
    dsk_label_changed = true;
    mon_add_response_fn(dsk_label_response);
}

static void dsk_info(const char *args)
{
    int vol = dsk_parse_drive_only(args, STR_INFO);
    if (vol < 0)
        return;
    msc_dsk_info_t info;
    if (!dsk_validate((uint8_t)vol, &info, false))
        return;
    dsk_vol = (uint8_t)vol;
    dsk_preview_op = DSK_PREVIEW_PLAIN;
    mon_add_response_fn(dsk_preview_response);
}

bool dsk_active(void)
{
    return dsk_state != DSK_IDLE;
}

void dsk_break(void)
{
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
    else if (!strcasecmp(sub, STR_ERASE))
        dsk_erase(args);
    else if (!strcasecmp(sub, STR_VERIFY))
        dsk_verify(args);
    else if (!strcasecmp(sub, STR_LABEL))
        dsk_label(args);
    else
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
}
