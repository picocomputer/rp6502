/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Standalone .rp6502 loader. Mirrors the format parsed by ria/mon/rom.c:
 *
 *   #!RP6502\r\n                 magic
 *   #>$<len> $<crc> [name]\r\n   optional "new format" header / named assets
 *   $<addr> $<len> $<crc>\r\n    data record header, followed by <len> raw bytes
 *
 * Addresses < $10000 load into 6502 RAM; $10000..$1FFFF load into XRAM.
 * A valid image must supply both reset-vector bytes ($FFFC/$FFFD). Each
 * record's CRC is verified (CRC-32/ISO-HDLC, i.e. zlib).
 */

#include "emu/api/api.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef EMU_HAVE_AIO
#include <aio.h>
#endif

/* ------------------------------------------------------------------ */
/* ROM: drive — read-only assets, windows into the loaded .rp6502      */
/* ------------------------------------------------------------------ */

#define ROM_OPEN_MAX 16 /* concurrent ROM: window opens (cf. the std fd pool) */

/* The loaded program's backing .rp6502 and the file offset where its asset
 * directory begins (0 = the image carries no assets). Assets are NOT indexed in
 * memory: a "ROM:name" open scans the directory in the file for the entry, like
 * the firmware's rom_find_asset, then reads it on demand. A new program replaces
 * these; the MSC0: drive beside them is untouched. */
static char g_rom_src[FS_HOST_MAX_PATH];
static size_t g_rom_assets_start;

/* the loader's text-line reader + hex-field parser, defined below */
static long fgets_line(FILE *f, char *line, size_t cap);
static bool parse_u32(const char **pp, uint32_t *out);

/* Name the backing .rp6502 for the ROM: drive; the loader also records where its
 * asset directory begins (g_rom_assets_start). */
void fs_set_rom_src(const char *hostpath)
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
    FILE *f = fopen(g_rom_src, "rb");
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

/* Read a named ROM asset's bytes host-side (main.c reads the "emulator" args
 * asset; the 6502 reads assets via the ROM: drive). -1 if no such asset. */
long fs_read_rom_asset(const char *name, void *buf, size_t max)
{
    size_t base, len;
    if (!rom_find_asset(name, &base, &len))
        return -1;
    FILE *f = fopen(g_rom_src, "rb");
    if (!f)
        return -1;
    size_t want = len < max ? len : max;
    size_t got = 0;
    if (fseek(f, (long)base, SEEK_SET) == 0)
        got = fread(buf, 1, want, f);
    fclose(f);
    return (long)got;
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

/* Reads run as POSIX AIO under the real-time window so the 6502 keeps clocking
 * while they complete (mirrors the MSC0: driver); synchronous otherwise — the
 * headless/test paths stay deterministic and the web build (no aio) reads from
 * the in-RAM MEMFS. */
static bool g_rom_async;
void rom_set_async(bool on) { g_rom_async = on; }

/* ---- Read-only file windows [base, base+len) into a host file, opened by the
 * ROM: asset driver. Read on demand: a positioned AIO read polled to completion
 * under the window, or pread when synchronous. ---- */
struct rom_window
{
    bool used;
    int fd;
    size_t base;
    size_t len;
    size_t pos;
#ifdef EMU_HAVE_AIO
    bool aio_active;
    struct aiocb cb; /* the single in-flight read, polled by the RIA pump */
#endif
};
static struct rom_window windows[ROM_OPEN_MAX];

void *rom_window_open(const char *hostpath, size_t base, size_t len)
{
    int fd = open(hostpath, O_RDONLY);
    if (fd < 0)
        return NULL; /* errno set by open() */
    struct rom_window *w = NULL;
    for (int i = 0; i < ROM_OPEN_MAX; i++)
        if (!windows[i].used)
        {
            w = &windows[i];
            break;
        }
    if (!w)
    {
        close(fd);
        errno = EMFILE;
        return NULL;
    }
    w->used = true;
    w->fd = fd;
    w->base = base;
    w->len = len;
    w->pos = 0;
#ifdef EMU_HAVE_AIO
    w->aio_active = false;
#endif
    return w;
}

void rom_window_close(void *desc)
{
    struct rom_window *w = desc;
    if (!w || !w->used)
        return;
#ifdef EMU_HAVE_AIO
    if (w->aio_active) /* reap an in-flight read (a reset can close mid-op) */
    {
        const struct aiocb *l = &w->cb;
        aio_cancel(w->fd, &w->cb);
        while (aio_error(&w->cb) == EINPROGRESS)
            aio_suspend(&l, 1, NULL);
        aio_return(&w->cb);
        w->aio_active = false;
    }
#endif
    if (w->fd >= 0)
        close(w->fd);
    w->used = false;
    w->fd = -1;
}

io_result rom_window_read(void *desc, void *buf, size_t n, size_t *got)
{
    struct rom_window *w = desc;
    *got = 0;
    size_t avail = (w->pos < w->len) ? w->len - w->pos : 0;
    size_t want = n < avail ? n : avail;
#ifdef EMU_HAVE_AIO
    if (g_rom_async)
    {
        if (!w->aio_active)
        {
            if (want == 0)
                return IO_OK; /* at the window's end: nothing to submit */
            memset(&w->cb, 0, sizeof(w->cb));
            w->cb.aio_fildes = w->fd;
            w->cb.aio_offset = (off_t)(w->base + w->pos);
            w->cb.aio_buf = buf;
            w->cb.aio_nbytes = want;
            w->cb.aio_sigevent.sigev_notify = SIGEV_NONE;
            if (aio_read(&w->cb) != 0)
                return IO_ERROR; /* errno set by aio_read */
            w->aio_active = true;
            return IO_PENDING;
        }
        int e = aio_error(&w->cb);
        if (e == EINPROGRESS)
            return IO_PENDING;
        w->aio_active = false;
        ssize_t r = aio_return(&w->cb);
        if (r < 0)
        {
            errno = e;
            return IO_ERROR;
        }
        w->pos += (size_t)r;
        *got = (size_t)r;
        return IO_OK;
    }
#endif
    ssize_t r = pread(w->fd, buf, want, (off_t)(w->base + w->pos));
    if (r < 0)
        return IO_ERROR;
    w->pos += (size_t)r;
    *got = (size_t)r;
    return IO_OK;
}

long rom_window_lseek(void *desc, long offset, int whence)
{
    struct rom_window *w = desc;
    long base = (whence == SEEK_SET) ? 0
                : (whence == SEEK_CUR) ? (long)w->pos
                : (whence == SEEK_END) ? (long)w->len
                                       : -1;
    if (base < 0 || base + offset < 0)
    {
        errno = EINVAL;
        return -1;
    }
    w->pos = (size_t)(base + offset);
    return (long)w->pos;
}

/* ---- The ROM: driver (read-only asset windows), registered in std.c's table. ---- */

bool rom_std_handles(const char *path)
{
    const char *rest;
    return path_is_rom(path, &rest);
}

void *rom_std_open(const char *path, uint8_t flags)
{
    const char *rest;
    path_is_rom(path, &rest);
    if (flags & 0x02) /* write requested on a read-only asset */
    {
        errno = EACCES;
        return NULL;
    }
    size_t base, len;
    if (!rom_find_asset(rest, &base, &len))
    {
        errno = ENOENT;
        return NULL;
    }
    return rom_window_open(g_rom_src, base, len);
}

/* A standalone CRC-32/ISO-HDLC (zlib). The firmware's CRC is littlefs's lfs_crc,
 * wrapped as ria_buf_crc32 over the fixed mbuf — neither a general (buf,len)
 * function nor part of a module the emulator compiles, so the emulator keeps its
 * own. Same polynomial, so the values match the .rp6502 headers and golden CRC. */
uint32_t emu_crc32(uint32_t crc, const void *buf, size_t len)
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
    if (*p == '$')
    {
        p++;
        while (isxdigit((unsigned char)*p))
        {
            char c = *p++;
            int d = (c <= '9') ? c - '0' : (toupper((unsigned char)c) - 'A' + 10);
            v = v * 16 + (uint32_t)d;
            n++;
        }
    }
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        while (isxdigit((unsigned char)*p))
        {
            char c = *p++;
            int d = (c <= '9') ? c - '0' : (toupper((unsigned char)c) - 'A' + 10);
            v = v * 16 + (uint32_t)d;
            n++;
        }
    }
    else
    {
        while (isdigit((unsigned char)*p))
        {
            v = v * 10 + (uint32_t)(*p++ - '0');
            n++;
        }
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

bool emu_rom_load(const char *path)
{
    char host[FS_HOST_MAX_PATH];
    if (!fs_resolve_rom(path, host, sizeof(host)))
    {
        fprintf(stderr, "rp6502-emu: cannot resolve ROM '%s'\n", path);
        return false;
    }
    FILE *f = fopen(host, "rb");
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
    fs_set_rom_src(host); /* ROM: reads seek into this file */
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
        if (emu_crc32(0, dst, len) != crc)
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
