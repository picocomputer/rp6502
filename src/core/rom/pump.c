/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The .rp6502 record pump, the piece of the loader every machine drives:
 * the stream read through the fs seam's ROM descriptor, one record per
 * step into the caller's buffer, CRC checked there. A machine that must
 * not stall its walks steps it once per pass; one that can block loops
 * it. The bytes land wherever the machine's own deposit puts them.
 */

#include "osal/fs.h"
#include "host/host.h"
#include "core/rom/rom.h"
#include "core/str/str.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* The record pump                                                     */
/* ------------------------------------------------------------------ */

typedef enum
{
    RECORD_OK,
    RECORD_SKIP,      /* blank line or comment: not a record, not an error */
    RECORD_MALFORMED, /* not three numbers and nothing else */
    RECORD_RANGE,     /* would land somewhere no record may */
} record_result;

/* Read one header line. RAM is below 0x10000 and XRAM above, and a record
 * may not straddle them, run off the end of either, or exceed the format's
 * own record cap. */
static record_result record_parse(const char *line, rom_record_t *rec)
{
    if (!line[0] || line[0] == '#')
        return RECORD_SKIP;
    const char *p = line;
    if (!str_parse_uint32(&p, &rec->addr) ||
        !str_parse_uint32(&p, &rec->len) ||
        !str_parse_uint32(&p, &rec->crc) ||
        !str_parse_end(p))
        return RECORD_MALFORMED;
    if (rec->addr > 0x1FFFF || rec->len == 0 ||
        rec->len > 0x20000 - rec->addr ||
        (rec->addr < 0x10000 && rec->len > 0x10000 - rec->addr) ||
        rec->len > ROM_RECORD_MAX)
        return RECORD_RANGE;
    return RECORD_OK;
}

/* The reset vector is what makes an image loadable, and it arrives as two
 * ordinary bytes inside whichever record happens to cover $FFFC and $FFFD.
 * Noted only once a record has landed, so a truncated or corrupt one cannot
 * vouch for the vector it was carrying. */
static void record_note(rom_pump_t *p, const rom_record_t *rec)
{
    if (rec->addr <= 0xFFFC && rec->addr + rec->len > 0xFFFC)
        p->vec_lo = true;
    if (rec->addr <= 0xFFFD && rec->addr + rec->len > 0xFFFD)
        p->vec_hi = true;
}

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

bool rom_pump_open(rom_pump_t *p, const char *path, uint8_t *buf, api_errno *err)
{
    int fd = fs_rom_open(path, FS_RD, err);
    if (fd < 0)
        return false;
    return rom_pump_open_fd(p, fd, buf, err);
}

/* From a descriptor the machine already holds -- the Pocket boots from an
 * image its host staged before anything ran, so the open was the host's.
 * The header lines borrow the caller's record buffer, as rom_pump_next's do,
 * and for the same reason: nothing parsed out of them outlives this call. */
bool rom_pump_open_fd(rom_pump_t *p, int fd, uint8_t *buf, api_errno *err)
{
    memset(p, 0, sizeof *p);
    p->fd = fd;
    char *line = (char *)buf;
    if (pump_gets(p, line, ROM_RECORD_MAX, err) < 0 ||
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
    long n = pump_gets(p, line, ROM_RECORD_MAX, err);
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

rom_pump_result rom_pump_next(rom_pump_t *p, uint8_t *buf, rom_record_t *rec,
                              api_errno *err)
{
    if (p->prog_end && p->pos >= p->prog_end)
        return ROM_PUMP_EOF;
    /* The header line borrows the caller's record buffer: the parse is done
     * with it before the payload read below writes over it, and a task-pump
     * machine keeps a kilobyte off its stack. */
    long n = pump_gets(p, (char *)buf, ROM_RECORD_MAX, err);
    if (n < 0)
        return p->prog_end ? ROM_PUMP_ERROR : ROM_PUMP_EOF; /* classic ends at EOF */
    record_result r = record_parse((char *)buf, rec);
    if (r == RECORD_SKIP)
        return ROM_PUMP_SKIP;
    if (r != RECORD_OK)
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
    if (host_crc32(0, buf, rec->len) != rec->crc)
    {
        *err = API_ENOEXEC;
        return ROM_PUMP_ERROR;
    }
    record_note(p, rec);
    return ROM_PUMP_RECORD;
}

bool rom_pump_complete(const rom_pump_t *p)
{
    return p->vec_lo && p->vec_hi;
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

