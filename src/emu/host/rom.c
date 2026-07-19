/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/host/msc.h"
#include "emu/host/rom.h"
#include "emu/plat.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* ROM: drive — read-only assets, windows into the loaded .rp6502      */
/* ------------------------------------------------------------------ */

#define ROM_OPEN_MAX 16 /* concurrent ROM: window opens (cf. the std fd pool) */

/* The loaded program's backing .rp6502 and the file offset where its asset
 * directory begins (0 = the image carries no assets). Assets are NOT indexed in
 * memory: a "ROM:name" open scans the directory in the file for the entry, like
 * the firmware's rom_find_asset, then reads it on demand. A new program replaces
 * these; the MSC0: drive beside them is untouched. */
static char g_rom_src[MSC_MAX_PATH];
static size_t g_rom_assets_start;

static long fgets_line(FILE *f, char *line, size_t cap);
static bool parse_u32(const char **pp, uint32_t *out);

/* Name the backing .rp6502 for the ROM: drive; the loader also records where its
 * asset directory begins (g_rom_assets_start). */
static void rom_set_src(const char *hostpath)
{
    snprintf(g_rom_src, sizeof(g_rom_src), "%s", hostpath);
}

/* Forget the loaded ROM's assets when a new program loads (exec/boot). Nothing is
 * held but the directory's location, so this just clears it — the MSC0: drive is
 * untouched, and open windows are closed separately by the machine reset. */
void rom_assets_reset(void)
{
    g_rom_src[0] = 0;
    g_rom_assets_start = 0;
}

/* Scan the asset directory in g_rom_src for the entry named `name` (the text
 * after "ROM:"). On success *base is the file offset of its data and *len its
 * length. Mirrors the firmware rom_find_asset: walk the "#>len crc name" headers
 * from the directory start, skipping each body, until the name matches or the
 * list ends — no in-memory index, so the program may carry any number of assets. */
static bool rom_find_asset(const char *name, size_t *base, size_t *len)
{
    if (!g_rom_assets_start || !g_rom_src[0])
        return false;
    FILE *f = fs_fopen_rd(g_rom_src);
    if (!f)
        return false;
    bool found = false;
    if (fseek(f, (long)g_rom_assets_start, SEEK_SET) == 0)
    {
        char line[512];
        while (fgets_line(f, line, sizeof(line)) > 0 && line[0] == '#' && line[1] == '>')
        {
            const char *p = line + 2;
            uint32_t alen, acrc;
            if (!parse_u32(&p, &alen) || !parse_u32(&p, &acrc))
                break;
            while (*p == ' ' || *p == '\t')
                p++;
            long data = ftell(f); /* the asset's data starts just after its header */
            if (strcasecmp(p, name) == 0)
            {
                *base = (size_t)data;
                *len = (size_t)alen;
                found = true;
                break;
            }
            if (fseek(f, data + (long)alen, SEEK_SET) != 0)
                break; /* past EOF: no more assets */
        }
    }
    fclose(f);
    return found;
}

/* If path names the ROM drive, return true and the asset name after "ROM:". */
static bool path_is_rom(const char *path, const char **rest)
{
    if (strncasecmp(path, "ROM:", 4) == 0)
    {
        *rest = path + 4;
        return true;
    }
    return false;
}

/* ---- Read-only file windows [base, base+len) into a host file, opened by the ROM:
 * asset driver. The window's fd is kept positioned at base + pos, and reads go through
 * the non-blocking fs seam. Descriptors index the pool. ---- */
struct rom_window
{
    bool used;
    int fd;
    size_t base;
    size_t len;
    size_t pos;
};
static struct rom_window windows[ROM_OPEN_MAX];

static struct rom_window *rom_win(int desc)
{
    if (desc < 0 || desc >= ROM_OPEN_MAX || !windows[desc].used)
        return NULL;
    return &windows[desc];
}

/* Open a read-only [base, base+len) window on hostpath. desc >= 0, or -1 + *err. */
static int rom_window_open(const char *hostpath, size_t base, size_t len, api_errno *err)
{
    int fd = fs_open(hostpath, O_RDONLY, 0);
    if (fd < 0)
    {
        *err = msc_errno_to_api_errno(errno);
        return -1;
    }
    int des = 0;
    for (; des < ROM_OPEN_MAX; des++)
        if (!windows[des].used)
            break;
    if (des == ROM_OPEN_MAX)
    {
        fs_close(fd);
        *err = API_EMFILE;
        return -1;
    }
    windows[des] = (struct rom_window){.used = true, .fd = fd, .base = base, .len = len};
    fs_lseek(fd, (int64_t)base, SEEK_SET); /* window reads start at base; keep fd == base + pos */
    return des;
}

std_rw_result rom_std_close(int desc, api_errno *err)
{
    struct rom_window *w = rom_win(desc);
    if (!w)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if (w->fd >= 0)
        fs_close(w->fd);
    w->used = false;
    return STD_OK;
}

std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    struct rom_window *w = rom_win(desc);
    if (!w)
    {
        *got = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    size_t avail = (w->pos < w->len) ? w->len - w->pos : 0;
    uint32_t want = count < avail ? count : (uint32_t)avail;
    if (want == 0)
    {
        *got = 0;
        return STD_OK; /* at the window's end (EOF): nothing to read */
    }
    std_rw_result r = fs_read(w->fd, buf, want, got);
    if (r == STD_OK)
        w->pos += *got;
    else if (r == STD_ERROR)
        *err = msc_errno_to_api_errno(errno);
    return r;
}

int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    struct rom_window *w = rom_win(desc);
    if (!w)
    {
        *err = API_EBADF;
        return -1;
    }
    long base = (whence == SEEK_SET) ? 0
                : (whence == SEEK_CUR) ? (long)w->pos
                : (whence == SEEK_END) ? (long)w->len
                                       : -1;
    if (base < 0 || base + off < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    long np = base + off;
    if ((size_t)np > w->len) /* clamp to the asset extent (firmware rom_std_lseek) */
        np = (long)w->len;
    w->pos = (size_t)np;
    fs_lseek(w->fd, (int64_t)(w->base + w->pos), SEEK_SET); /* keep fd == base + pos for fs_read */
    *pos = (int32_t)w->pos;
    return 0;
}

/* ---- The ROM: driver (read-only asset windows), registered in std.c's table. ---- */

bool rom_std_handles(const char *path)
{
    const char *rest;
    return path_is_rom(path, &rest);
}

int rom_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const char *rest;
    path_is_rom(path, &rest);
    if (flags & 0x02) /* write requested on a read-only asset */
    {
        *err = API_EACCES;
        return -1;
    }
    size_t base, len;
    if (!rom_find_asset(rest, &base, &len))
    {
        *err = API_ENOENT;
        return -1;
    }
    return rom_window_open(g_rom_src, base, len, err);
}

/* A standalone CRC-32/ISO-HDLC (zlib). The firmware's CRC is littlefs's lfs_crc,
 * wrapped as ria_buf_crc32 over the fixed mbuf — neither a general (buf,len)
 * function nor part of a module the emulator compiles, so the emulator keeps its
 * own. Same polynomial, so the values match the .rp6502 headers and golden CRC. */
uint32_t rom_crc32(uint32_t crc, const void *buf, size_t len)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    crc ^= 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static bool parse_u32(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    uint32_t v = 0;
    int n = 0;
    bool hex = false;
    if (*p == '$')
    {
        hex = true;
        p++;
    }
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        hex = true;
        p += 2;
    }
    if (hex)
        while (isxdigit((unsigned char)*p))
        {
            char c = *p++;
            int d = (c <= '9') ? c - '0' : (toupper((unsigned char)c) - 'A' + 10);
            v = v * 16 + (uint32_t)d;
            n++;
        }
    else
        while (isdigit((unsigned char)*p))
        {
            v = v * 10 + (uint32_t)(*p++ - '0');
            n++;
        }
    if (!n)
        return false;
    *pp = p;
    *out = v;
    return true;
}

static bool parse_end(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == 0;
}

/* Read one text line from f into line[] (NUL-terminated, CR/LF stripped, capped).
 * Returns its length, or -1 at EOF with nothing read. The file position is left
 * at the first byte after the line's newline — i.e. the start of a record's raw
 * data, or the next header. */
static long fgets_line(FILE *f, char *line, size_t cap)
{
    size_t i = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n')
        if (i + 1 < cap)
            line[i++] = (char)c;
    if (c == EOF && i == 0)
    {
        line[0] = 0;
        return -1;
    }
    if (i && line[i - 1] == '\r')
        i--;
    line[i] = 0;
    return (long)i;
}

/* ------------------------------------------------------------------ */
/* Install: map a boot/exec ":name" to its backing host .rp6502        */
/* ------------------------------------------------------------------ */

#define INSTALL_MAX 16
#define INSTALL_NAME_MAX 64

typedef struct
{
    bool used;
    char name[INSTALL_NAME_MAX]; /* basename, e.g. "adventure.rp6502" (the text after ":") */
    char host[MSC_MAX_PATH]; /* the backing file */
    size_t size;
} install_t;
static install_t installs[INSTALL_MAX];

static const char *host_basename(const char *hostpath)
{
    const char *base = hostpath;
    for (const char *p = hostpath; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool rom_install(const char *hostpath)
{
    const char *base = host_basename(hostpath);
    if (!*base || strlen(base) >= INSTALL_NAME_MAX || strlen(hostpath) >= MSC_MAX_PATH)
        return false;
    struct fs_meta meta;
    if (!fs_stat(hostpath, &meta)) /* must exist; size for the whole-file window */
        return false;
    for (int i = 0; i < INSTALL_MAX; i++)
        if (!installs[i].used)
        {
            installs[i].used = true;
            strcpy(installs[i].name, base);
            strcpy(installs[i].host, hostpath);
            installs[i].size = (size_t)meta.size;
            return true;
        }
    return false;
}

/* Find an installed ROM by name (the text after ":"), case-insensitively to match
 * the firmware's installed-name handling and the sibling ROM: asset driver. */
static install_t *install_find(const char *name)
{
    for (int i = 0; i < INSTALL_MAX; i++)
        if (installs[i].used && strcasecmp(installs[i].name, name) == 0)
            return &installs[i];
    return NULL;
}

/* Resolve a boot/exec ROM path to the host file to open: an installed ":name" ->
 * its backing file, a drive path -> msc_to_host, else the bare path (the native
 * CLI / tests, against the process cwd). The loader then opens it. */
bool rom_resolve(const char *path, char *out, size_t outsz)
{
    if (path[0] == ':') /* null drive: an installed ROM, or nothing */
    {
        install_t *in = install_find(path + 1);
        if (!in)
        {
            errno = ENOENT;
            return false;
        }
        if (strlen(in->host) >= outsz)
            return false;
        strcpy(out, in->host);
        return true;
    }
    if (msc_has_drive_prefix(path))
        return msc_to_host(path, out, outsz);
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}

bool rom_load(const char *path)
{
    char host[MSC_MAX_PATH];
    if (!rom_resolve(path, host, sizeof(host)))
    {
        fprintf(stderr, "rp6502-emu: cannot resolve ROM '%s'\n", path);
        return false;
    }
    FILE *f = fs_fopen_rd(host);
    if (!f)
    {
        fprintf(stderr, "rp6502-emu: cannot open ROM '%s'\n", path);
        return false;
    }

    char line[512];
    if (fgets_line(f, line, sizeof(line)) < 0 ||
        strncasecmp(line, "#!RP6502", 8) != 0)
    {
        fprintf(stderr, "rp6502-emu: not a .rp6502 file (bad magic)\n");
        fclose(f);
        return false;
    }

    /* Optional "#>$chunks_len $crc" header gives the program-section byte length;
     * named assets follow it. Classic format (no header) is all program records,
     * no assets. */
    long after_magic = ftell(f);
    long prog_end = -1; /* -1 = run to EOF (classic) */
    long n = fgets_line(f, line, sizeof(line));
    if (n >= 2 && line[0] == '#' && line[1] == '>')
    {
        const char *p = line + 2;
        uint32_t chunks_len, image_crc;
        if (!parse_u32(&p, &chunks_len) || !parse_u32(&p, &image_crc))
        {
            fprintf(stderr, "rp6502-emu: bad header line\n");
            fclose(f);
            return false;
        }
        prog_end = ftell(f) + (long)chunks_len; /* records end here; assets follow */
    }
    else
        fseek(f, after_magic, SEEK_SET); /* classic: reprocess from line 2 */

    rom_assets_reset();   /* forget the previous ROM's assets (the MSC0: drive persists) */
    rom_set_src(host); /* ROM: reads seek into this file */
    /* The asset directory (if any) begins where the program chunks end; a ROM:
     * open scans it from there on demand. Classic images carry no assets. */
    g_rom_assets_start = (prog_end >= 0) ? (size_t)prog_end : 0;

    /* Program memory-chunk records: stream each straight into ram[]/xram[]. */
    bool reset_lo = false, reset_hi = false;
    while (prog_end < 0 || ftell(f) < prog_end)
    {
        n = fgets_line(f, line, sizeof(line));
        if (n < 0)
            break; /* EOF (classic) */
        if (n == 0 || line[0] == '#')
            continue; /* blank or comment */
        const char *p = line;
        uint32_t addr, len, crc;
        if (!parse_u32(&p, &addr) || !parse_u32(&p, &len) ||
            !parse_u32(&p, &crc) || !parse_end(p))
        {
            fprintf(stderr, "rp6502-emu: malformed data record: %s\n", line);
            fclose(f);
            return false;
        }
        if (addr > 0x1FFFF || len == 0 || len > 0x20000 - addr ||
            (addr < 0x10000 && len > 0x10000 - addr))
        {
            fprintf(stderr, "rp6502-emu: data record out of range (addr=$%X len=$%X)\n",
                    addr, len);
            fclose(f);
            return false;
        }
        uint8_t *dst = (addr > 0xFFFF) ? &xram[addr - 0x10000] : &ram[addr];
        /* A ROM load must not write the RIA register window. The firmware's
         * ria_write_buf skips $FF00-$FFF9 (only the $FFFA-$FFFF vectors land in
         * that page); mirror it by snapshotting those cells and restoring them
         * after the record streams in (the CRC still covers the file bytes). */
        uint8_t guard_save[0xFFFA - 0xFF00];
        bool guard = addr < 0x10000 && addr < 0xFFFA && addr + len > 0xFF00;
        if (guard)
            memcpy(guard_save, &ram[0xFF00], sizeof guard_save);
        if (fread(dst, 1, len, f) != len)
        {
            fprintf(stderr, "rp6502-emu: truncated data record at $%X\n", addr);
            fclose(f);
            return false;
        }
        if (rom_crc32(0, dst, len) != crc)
        {
            fprintf(stderr, "rp6502-emu: CRC mismatch in record at $%X\n", addr);
            fclose(f);
            return false;
        }
        if (guard)
            memcpy(&ram[0xFF00], guard_save, sizeof guard_save);
        if (addr <= 0xFFFC && addr + len > 0xFFFC)
            reset_lo = true;
        if (addr <= 0xFFFD && addr + len > 0xFFFD)
            reset_hi = true;
    }

    /* Named assets follow the program chunks; they are not parsed here — a ROM:
     * open scans the directory (from g_rom_assets_start) for the named entry and
     * reads it on demand, so the bytes never enter RAM. */
    fclose(f);
    if (!reset_lo || !reset_hi)
    {
        fprintf(stderr, "rp6502-emu: ROM has no reset vector ($FFFC/$FFFD)\n");
        return false;
    }
    return true;
}
