/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The .rp6502 loader: the record pump every machine drives, and this
 * machine's deposit into ram[]/xram[]. The pump reads through the fs seam's
 * ROM descriptor, one record per step, so a machine that must not stall its
 * walks steps it once per pass and a machine that can block loops it. The
 * bytes land wherever the machine puts them; the format lands here.
 */

#include "core/log.h"
#include "core/api/fs.h"
#include "core/str/path.h"
#include "core/rom/rom.h"
#include "core/rom/rom_rec.h"
#include "host/os.h"
#include "core/mem/mem.h"
#include "core/str/str.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* The record pump                                                     */
/* ------------------------------------------------------------------ */

/* Reads on the ROM descriptor may report STD_PENDING on a host whose file
 * driver is asynchronous. The pump spins them out: it runs only after the
 * machine stopped, when std_stop has closed every guest descriptor and
 * reaped whatever was in flight, so the transfer it waits on is its own. */
static std_rw_result pump_read(int fd, void *buf, uint32_t count, uint32_t *got,
                               api_errno *err)
{
    std_rw_result r;
    do
        r = fs_std_read(fd, buf, count, got, err);
    while (r == STD_PENDING);
    return r;
}

/* One text line into line[] (NUL-terminated, CR/LF stripped, capped). Returns
 * its length, or -1 at EOF with nothing read. The pump's position is left at
 * the first byte after the newline -- the start of a record's raw data, or
 * the next header. Reads a block and seeks back, because the seam has no
 * byte-at-a-time worth using. */
static long pump_gets(rom_pump_t *p, char *line, size_t cap, api_errno *err)
{
    int32_t landed;
    if (fs_std_lseek(p->fd, SEEK_SET, (int32_t)p->pos, &landed, err) < 0)
        return -1;
    uint32_t got = 0;
    if (pump_read(p->fd, line, (uint32_t)cap - 1, &got, err) != STD_OK)
        return -1;
    if (got == 0)
    {
        line[0] = 0;
        return -1; /* EOF */
    }
    size_t i = 0;
    while (i < got && line[i] != '\n')
        i++;
    p->pos += (uint32_t)(i < got ? i + 1 : got);
    if (i && line[i - 1] == '\r')
        i--;
    line[i] = 0;
    return (long)i;
}

bool rom_pump_open(rom_pump_t *p, const char *path, api_errno *err)
{
    memset(p, 0, sizeof *p);
    p->fd = fs_rom_open(path, FS_RD, err);
    if (p->fd < 0)
        return false;
    char line[ROM_REC_MAX];
    if (pump_gets(p, line, sizeof line, err) < 0 ||
        strncasecmp(line, "#!RP6502", 8) != 0)
    {
        rom_pump_close(p);
        *err = API_ENOEXEC;
        return false;
    }
    /* Optional "#>$chunks_len $crc" bounds the program records; named assets
     * follow. The directory starts at the header line itself -- it parses as
     * an asset with no name, so a walker skips it like any other entry.
     * Classic format runs records to EOF and carries no assets. */
    uint32_t after_shebang = p->pos;
    long n = pump_gets(p, line, sizeof line, err);
    if (n >= 2 && line[0] == '#' && line[1] == '>')
    {
        const char *scan = line + 2;
        uint32_t chunks_len, image_crc;
        if (!str_parse_uint32(&scan, &chunks_len) ||
            !str_parse_uint32(&scan, &image_crc))
        {
            rom_pump_close(p);
            *err = API_ENOEXEC;
            return false;
        }
        p->prog_end = p->pos + chunks_len;
        p->assets_start = after_shebang;
    }
    else
        p->pos = after_shebang; /* classic: reprocess from line 2 */
    return true;
}

rom_pump_result rom_pump_next(rom_pump_t *p, uint8_t *buf, rom_rec_t *rec,
                              api_errno *err)
{
    if (p->prog_end && p->pos >= p->prog_end)
        return ROM_PUMP_EOF;
    char line[ROM_REC_MAX];
    long n = pump_gets(p, line, sizeof line, err);
    if (n < 0)
        return p->prog_end ? ROM_PUMP_ERROR : ROM_PUMP_EOF; /* classic ends at EOF */
    rom_rec_result r = rom_rec_parse(line, ROM_REC_MAX, rec);
    if (r == ROM_REC_SKIP)
        return ROM_PUMP_SKIP;
    if (r != ROM_REC_OK)
    {
        *err = API_ENOEXEC;
        return ROM_PUMP_ERROR;
    }
    int32_t landed;
    if (fs_std_lseek(p->fd, SEEK_SET, (int32_t)p->pos, &landed, err) < 0)
        return ROM_PUMP_ERROR;
    uint32_t got = 0;
    if (pump_read(p->fd, buf, rec->len, &got, err) != STD_OK || got != rec->len)
    {
        *err = API_ENOEXEC;
        return ROM_PUMP_ERROR;
    }
    p->pos += rec->len;
    if (mem_crc32(0, buf, rec->len) != rec->crc)
    {
        *err = API_ENOEXEC;
        return ROM_PUMP_ERROR;
    }
    rom_rec_note(&p->vectors, rec);
    return ROM_PUMP_RECORD;
}

bool rom_pump_complete(const rom_pump_t *p)
{
    return rom_rec_complete(&p->vectors);
}

void rom_pump_close(rom_pump_t *p)
{
    if (p->fd >= 0)
    {
        api_errno ignored;
        fs_std_close(p->fd, &ignored);
        p->fd = -1;
    }
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
    char host[HOST_MAX_PATH];    /* the backing file */
} install_t;
static install_t installs[INSTALL_MAX];

/* Install a .rp6502 on the null drive, keyed by its host-path basename. */
bool rom_install(const char *hostpath)
{
    const char *base = path_basename(hostpath);
    if (!*base || strlen(base) >= INSTALL_NAME_MAX || strlen(hostpath) >= HOST_MAX_PATH)
        return false;
    /* Must exist. Asked through the driver, because that is the machine's
     * answer for what a file is. */
    api_errno err;
    int fd = fs_std_open(hostpath, FS_RD, &err);
    if (fd < 0)
        return false;
    fs_std_close(fd, &err);
    for (int i = 0; i < INSTALL_MAX; i++)
        if (!installs[i].used)
        {
            installs[i].used = true;
            strcpy(installs[i].name, base);
            strcpy(installs[i].host, hostpath);
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

/* Resolve a boot/exec ROM path to the file to open: an installed ":name" ->
 * its backing file, anything else verbatim. A drive path and a bare host path
 * are both spellings the filesystem seam accepts, so neither is rewritten
 * here. The loader then opens it. */
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
    if (strlen(path) >= outsz)
        return false;
    strcpy(out, path);
    return true;
}

/* ------------------------------------------------------------------ */
/* This machine's loader: pump the records into ram[]/xram[]           */
/* ------------------------------------------------------------------ */

/* Deposit one record's bytes. A load never writes the RIA register window:
 * $FF00-$FFF9 is skipped (the firmware's ria_write_buf does the same over
 * the bus), and the $FFFA-$FFFF vectors land in the register cells too --
 * a load bypasses the bus, so ram[] keeps the shadow every reader uses and
 * regs[] gets the copy the RIA would have taken. */
static void rom_deposit(const rom_rec_t *rec, const uint8_t *buf)
{
    if (rec->addr > 0xFFFF)
    {
        for (uint32_t i = 0; i < rec->len; i++)
            xram[rec->addr - 0x10000 + i] = buf[i]; /* volatile: no memcpy */
        return;
    }
    for (uint32_t i = 0; i < rec->len; i++)
    {
        uint32_t a = rec->addr + i;
        if (a < 0xFF00 || a >= 0xFFFA)
            ram[a] = buf[i];
        if (a >= 0xFFFA && a <= 0xFFFF)
            regs[a & 0x1F] = buf[i];
    }
}

bool rom_load(const char *path)
{
    rom_assets_reset(); /* forget the previous ROM (the MSC0: drive persists) */
    api_errno err;
    rom_pump_t pump;
    if (!rom_pump_open(&pump, path, &err))
    {
        log_error("cannot load ROM '%s'", path);
        return false;
    }
    static uint8_t buf[ROM_REC_MAX];
    rom_rec_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&pump, buf, &rec, &err)) != ROM_PUMP_EOF)
    {
        if (r == ROM_PUMP_SKIP)
            continue;
        if (r == ROM_PUMP_ERROR)
        {
            log_error("bad ROM record in '%s'", path);
            rom_pump_close(&pump);
            return false;
        }
        rom_deposit(&rec, buf);
    }
    if (!rom_pump_complete(&pump))
    {
        log_error("ROM has no reset vector ($FFFC/$FFFD)");
        rom_pump_close(&pump);
        return false;
    }
    /* The descriptor and the directory offset become the ROM: drive's: an
     * asset open scans the file for the entry and reads it on demand, so the
     * bytes never enter RAM. */
    rom_asset_adopt(pump.fd, pump.assets_start);
    return true;
}
