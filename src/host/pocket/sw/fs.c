/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"
#include "fs.h"

#include "core/str/unicode.h"
#include "core/sys/debug_log.h"
#include "core/term/font.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FS_SLOT_FIRST 1
#define FS_OPEN_MAX 8

#define FS_NAME_MAX 256
#define FS_PARAM_FLAGS 256
#define FS_PARAM_SIZE 260

#define FS_DS_CREATE 1u
#define FS_DS_RESIZE 2u

#define FS_SECTOR 512u

static struct
{
    bool used;
    uint8_t flags;
    bool stale;
    uint32_t len;
    uint32_t pos;
    uint32_t cache_at;
    uint32_t cache_len;
    char name[FS_NAME_MAX];
} fs_pool[FS_OPEN_MAX];

/* pocket_file runs one command at a time and reports its status in one
 * register. Each command is issued under a worker id, FS_W(d) or FS_W_SYS,
 * and fs_owner records the id of the command in flight. fs_wait_free can
 * collect the status of a command that was issued under another id, so
 * fs_reap stores each status under the id in fs_owner. A worker that took
 * the other command's status would copy a window that was not filled for it
 * or advance its position by bytes that went to another file. */
#define FS_W_NONE 0u
#define FS_W(d) ((uint32_t)(d) + 1u)
#define FS_W_SYS (FS_OPEN_MAX + 1u)
#define FS_W_MAX (FS_OPEN_MAX + 2u)

static uint8_t fs_owner;
static bool fs_mail[FS_W_MAX];
static uint32_t fs_mail_st[FS_W_MAX];

/* Starting a command is separate from polling for its status because
 * core_bridge_cmd.v waits about 0.9 s for a host that does not reply before
 * it times the command out. fs_std_read, fs_std_write and fs_std_sync poll
 * once per pass of the main loop so that the wait does not stall every
 * other task. */
static void fs_start(uint32_t who, uint32_t op)
{
    fs_owner = (uint8_t)who;
    fs_mail[who] = false;
    FILE_CTL = op;
}

static void fs_reap(void)
{
    if (fs_owner == FS_W_NONE)
        return;
    uint32_t v = FILE_CTL;
    if (v & (FILE_ST_BUSY | FILE_ST_DRAIN))
        return;
    fs_mail[fs_owner] = true;
    fs_mail_st[fs_owner] = v;
    fs_owner = FS_W_NONE;
}

static bool fs_poll(uint32_t who, uint32_t *st)
{
    fs_reap();
    if (!fs_mail[who])
        return false;
    fs_mail[who] = false;
    *st = fs_mail_st[who];
    return true;
}

static bool fs_may(uint32_t who)
{
    return fs_owner == FS_W_NONE || fs_owner == who;
}

/* pocket_file starts a command only while it is idle. A FILE_CTL write made
 * while a command is in flight is lost, and the status that follows is the
 * earlier command's, so a blocking command first waits for the command in
 * flight to finish. */
static void fs_wait_free(void)
{
    while (fs_owner != FS_W_NONE)
        fs_reap();
}

static uint16_t fs_n_tmo, fs_n_err, fs_n_defer;
static uint32_t fs_last_st;

void fs_log(void)
{
    if (!fs_n_tmo && !fs_n_err)
    {
        fs_n_defer = 0;
        return;
    }
    RP6502_LOG(fs, WARN, "tmo=%u err=%u defer=%u last=%02x", (unsigned)fs_n_tmo,
               (unsigned)fs_n_err, (unsigned)fs_n_defer,
               (unsigned)(fs_last_st & 0xFFu));
    fs_n_tmo = fs_n_err = fs_n_defer = 0;
}

/* After a successful restore, SST_RESTORED stays set until wake_task clears
 * it, and wake_task calls fs_restore before it does. api_task runs before
 * wake_task in each pass, so the handler of a syscall that was in progress
 * when the state was saved can call into this driver again before
 * fs_restore has marked the descriptors stale, while a slot may still be
 * bound to a file opened by the session that the restore replaced.
 * Returning STD_PENDING leaves pos where it was, so a syscall handler
 * repeats the whole operation on a later pass. */
static bool fs_adrift(void)
{
    if (!(SST_CTL & SST_RESTORED))
        return false;
    fs_n_defer++;
    return true;
}

#define FS_SAY_MAX 4

static bool fs_note(uint32_t st)
{
    fs_last_st = st;
    if (st & FILE_ST_TIMEOUT)
        fs_n_tmo++;
    else if (st & FILE_ST_ERR)
        fs_n_err++;
    return (unsigned)(fs_n_tmo + fs_n_err) <= FS_SAY_MAX;
}

static uint32_t fs_command(uint32_t who, uint32_t op)
{
    uint32_t st;
    fs_wait_free();
    fs_start(who, op);
    while (!fs_poll(who, &st))
        ;
    return st;
}

static bool fs_grow;

/* fs_std_close waits for a command in flight only when it flushes, so a
 * command started for a descriptor that is closed without a flush can still
 * be in flight after std_stop has closed that descriptor. */
void fs_stop(void)
{
    fs_wait_free();
    for (uint32_t w = 0; w < FS_W_MAX; w++)
        fs_mail[w] = false;
    fs_grow = false;
}

static void fs_win_put(uint32_t off, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4)
    {
        uint32_t w = 0;
        for (uint32_t j = 0; j < 4; j++)
            if (i + j < len)
                w |= (uint32_t)src[i + j] << (24 - 8 * j);
        FILE_WIN[(off + i) >> 2] = w;
    }
}

/* The host reads the path in the parameter struct as a byte stream, most
 * significant byte of each word first, but reads each integer field as a
 * whole bridge word. An integer is therefore written as a word rather than
 * packed like the path, which would turn flags of 3 into 0x03000000. */
static void fs_win_u32(uint32_t off, uint32_t v)
{
    FILE_WIN[off >> 2] = v;
}

#define FS_DT_PAIRS 20

static uint32_t fs_dt(uint32_t word)
{
    FILE_ID = word;
    fs_command(FS_W_SYS, FILE_OP_DT);
    return FILE_RESULT;
}

bool fs_slot_len(uint32_t slot, uint32_t *len)
{
    for (uint32_t i = 0; i < FS_DT_PAIRS; i++)
        if (fs_dt(i * 2) == slot)
        {
            *len = fs_dt(i * 2 + 1);
            return true;
        }
    return false;
}

bool fs_getfile(uint32_t slot, char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
    FILE_ID = slot;
    FILE_BRIDGE = GETFILE_BRIDGE;
    uint32_t st = fs_command(FS_W_SYS, FILE_OP_GETFILE);
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        return false;

    /* Hardware measurements show that the host writes the whole 256-byte
     * response struct on every Get File and leaves a NUL at offset 0 when
     * the slot is bound to nothing, so an empty name means an unbound
     * slot. */
    char utf8[FS_NAME_MAX];
    size_t n = 0;
    while (n < FS_NAME_MAX - 1 && GETFILE_WIN[n])
        utf8[n] = (char)GETFILE_WIN[n], n++;
    utf8[n] = 0;

    uint16_t page = font_get_code_page();
    const char *p = utf8;
    size_t o = 0;
    for (;;)
    {
        unsigned char c = unicode_from_utf8_next(&p, page);
        if (!c)
            break;
        if (o + 1 >= cap)
        {
            out[0] = 0;
            return false;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o != 0;
}

#define FS_SAVES_LEN (sizeof FS_SAVES_PATH - 1)
#define FS_RC_MALFORMED 4u

#define FS_RC_STARTED 0xFFu

static uint32_t fs_try_open_start(uint32_t who, uint32_t slot,
                                   const char *name, uint32_t flags,
                                   uint32_t size, const char *root)
{
    uint8_t pad[FS_NAME_MAX];
    uint16_t page = font_get_code_page();
    size_t n = 0;
    if (*name != '/')
    {
        n = strlen(root);
        memcpy(pad, root, n);
    }
    for (const unsigned char *s = (const unsigned char *)name; *s; s++)
    {
        char enc[4];
        int k = unicode_to_utf8_char(*s, page, enc);
        if (n + (size_t)k >= FS_NAME_MAX)
            return FS_RC_MALFORMED;
        memcpy(pad + n, enc, (size_t)k);
        n += (size_t)k;
    }
    memset(pad + n, 0, FS_NAME_MAX - n);
    fs_win_put(0, pad, FS_NAME_MAX);
    fs_win_u32(FS_PARAM_FLAGS, flags);
    fs_win_u32(FS_PARAM_SIZE, size);
    FILE_ID = slot;
    fs_start(who, FILE_OP_OPEN);
    return FS_RC_STARTED;
}

static bool fs_try_open_poll(uint32_t who, uint32_t *rc)
{
    uint32_t st;
    if (!fs_poll(who, &st))
        return false;
    *rc = (st & FILE_ST_TIMEOUT) ? FS_RC_MALFORMED : ((st & FILE_ST_ERR) >> 1);
    return true;
}

static uint32_t fs_try_open(uint32_t who, uint32_t slot, const char *name,
                             uint32_t flags, uint32_t size,
                             const char *root)
{
    fs_wait_free();
    uint32_t rc = fs_try_open_start(who, slot, name, flags, size, root);
    if (rc == FS_RC_STARTED)
        while (!fs_try_open_poll(who, &rc))
            ;
    return rc;
}

static bool fs_open_slot(uint32_t slot, const char *name, uint32_t flags,
                          uint32_t size)
{
    return fs_try_open(FS_W_SYS, slot, name, flags, size, FS_SAVES_PATH) <= 1;
}

const char *fs_strip_drive(const char *path)
{
    if ((path[0] | 0x20) == 'f' && (path[1] | 0x20) == 's' && path[2] == ':')
        return path + 3;
    return path;
}

static bool fs_still_bound(int d)
{
    char have[FS_NAME_MAX];
    if (!fs_getfile(FS_SLOT_FIRST + (uint32_t)d, have, sizeof have))
        return false;
    const char *want = fs_pool[d].name;
    const char *at = have;
    if (*want != '/')
    {
        size_t n = FS_SAVES_LEN;
        if (strncmp(have, FS_SAVES_PATH, n) != 0)
            return false;
        at += n;
    }
    return strcmp(at, want) == 0;
}

void fs_restore(void)
{
    fs_stop();
    for (int d = 0; d < FS_OPEN_MAX; d++)
    {
        if (!fs_pool[d].used)
            continue;
        fs_pool[d].stale = true;
        /* SLOT_WIN(d) is in the SDRAM staging store, which the savestate
         * blob does not include, so after a restore SLOT_WIN(d) may not
         * hold the bytes that cache_at and cache_len describe. */
        fs_pool[d].cache_len = 0;
    }
}

static void fs_rebind(int d)
{
    if (!fs_pool[d].stale)
        return;
    fs_pool[d].stale = false;
    bool kept = fs_still_bound(d);
    uint32_t rc = 0;
    if (!kept)
        rc = fs_try_open(FS_W(d), FS_SLOT_FIRST + (uint32_t)d,
                          fs_pool[d].name, 0, 0, FS_SAVES_PATH);
    uint32_t len = 0;
    bool got = fs_slot_len(FS_SLOT_FIRST + (uint32_t)d, &len);
    if (!kept && rc > 1)
        return;
    if (got && !(fs_pool[d].flags & FS_WR))
        fs_pool[d].len = len;
    if (fs_pool[d].pos > fs_pool[d].len)
        fs_pool[d].pos = fs_pool[d].len;
}

static int fs_desc(int desc)
{
    if (desc < 0 || desc >= FS_OPEN_MAX || !fs_pool[desc].used)
        return -1;
    return desc;
}

/* core_bridge_cmd.v reports result 7 when a data slot command has not
 * completed within 2^26 clocks at 74.25 MHz, about 0.9 s, whether or not the
 * host has acknowledged it. pocket_file times out after twice that, so a host
 * that does not reply shows up as result 7 rather than as FILE_ST_TIMEOUT. */
#define FS_RC_NO_HOST 7u

static bool fs_unanswered(uint32_t st)
{
    return (st & FILE_ST_TIMEOUT)
           || ((st & FILE_ST_ERR) >> 1) == FS_RC_NO_HOST;
}

/* The host does not reply to Flush, 0x0188, so each flush costs the
 * bridge's 0.9 s deadline and ends in result 7. No flush is sent after the
 * first one that ends in result 7 or FILE_ST_TIMEOUT. */
static enum { FS_FLUSH_UNTRIED, FS_FLUSH_WORKS, FS_FLUSH_NEVER }
    fs_flush_state;

/* The bridge's 0.9 s deadline covers the whole slot operation, and the host
 * writes at about 3.4 MB/s at worst, so fs_rom_pull reads the ROM slot in
 * 512 KB chunks that take about 0.15 s each. */
#define FS_STAGE_CHUNK 0x80000u

static struct
{
    bool used;
    uint32_t len;
    uint32_t pos;
} fs_rom;

uint32_t fs_rom_staged_len(void)
{
    return fs_rom.len;
}

static bool fs_rom_pull(uint32_t len)
{
    for (uint32_t at = 0; at < len; at += FS_STAGE_CHUNK)
    {
        uint32_t n = len - at;
        if (n > FS_STAGE_CHUNK)
            n = FS_STAGE_CHUNK;
        FILE_ID = FS_SLOT_ROM;
        FILE_OFFSET = at;
        FILE_BRIDGE = ROM_BRIDGE + at;
        FILE_LENGTH = n;
        uint32_t st = fs_command(FS_W_SYS, FILE_OP_READ);
        if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
            return false;
    }
    return true;
}

int fs_rom_open(const char *path, uint8_t flags, api_errno *err)
{
    if (flags != FS_RD)
    {
        *err = (flags == (FS_WR | FS_CREAT | FS_EXCL)) ? API_EACCES : API_EINVAL;
        return -1;
    }
    if (path[0] == ':')
    {
        *err = API_ENOENT;
        return -1;
    }
    if (CPU_RESB & 1)
    {
        RP6502_LOG(rom, ERROR, "stage refused, 6502 running");
        *err = API_EBUSY;
        return -1;
    }
    const char *p = fs_strip_drive(path);
    if (!*p)
    {
        *err = API_EINVAL;
        return -1;
    }
    assert(!fs_rom.used);
    if (fs_try_open(FS_W_SYS, FS_SLOT_ROM, p, 0, 0, FS_ASSETS_PATH) > 1)
    {
        *err = API_ENOENT;
        return -1;
    }
    uint32_t len;
    if (!fs_slot_len(FS_SLOT_ROM, &len) || !len || len > ROM_MAX)
    {
        *err = API_EIO;
        return -1;
    }
    if (!fs_rom_pull(len))
    {
        *err = API_EIO;
        return -1;
    }
    fs_rom.used = true;
    fs_rom.len = len;
    fs_rom.pos = 0;
    return FS_DESC_ROM;
}

int fs_rom_adopt(api_errno *err)
{
    assert(!fs_rom.used);
    uint32_t len;
    if (!fs_slot_len(FS_SLOT_ROM, &len) || !len || len > ROM_MAX)
    {
        *err = API_EIO;
        return -1;
    }
    fs_rom.used = true;
    fs_rom.len = len;
    fs_rom.pos = 0;
    return FS_DESC_ROM;
}

bool fs_rom_remove(const char *name, api_errno *err)
{
    (void)name;
    *err = API_ENOENT;
    return false;
}

bool fs_std_handles(const char *path)
{
    (void)path;
    return true;
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    path = fs_strip_drive(path);
    if (path[0] == ':')
    {
        *err = API_ENODEV;
        return -1;
    }
    if (!*path || strlen(path) >= FS_NAME_MAX - FS_SAVES_LEN)
    {
        *err = API_EINVAL;
        return -1;
    }
    int d = -1;
    for (int i = 0; i < FS_OPEN_MAX; i++)
        if (!fs_pool[i].used)
        {
            d = i;
            break;
        }
    if (d < 0)
    {
        *err = API_EMFILE;
        return -1;
    }
    uint32_t slot = FS_SLOT_FIRST + (uint32_t)d;

    /* The host creates a file only when Open File has both FS_DS_CREATE and
     * FS_DS_RESIZE set, and both bits on an existing file resize it to the
     * size given, which is zero here, so the first open sets neither bit to
     * find out whether the file exists. */
    bool exists = fs_open_slot(slot, path, 0, 0);
    if (exists && (flags & (FS_CREAT | FS_EXCL))
                      == (FS_CREAT | FS_EXCL))
    {
        *err = API_EEXIST;
        return -1;
    }
    if (!exists && !(flags & FS_CREAT))
    {
        *err = API_ENOENT;
        return -1;
    }
    bool empty = !exists || ((flags & FS_TRUNC) && (flags & FS_WR));
    if (empty && exists && !fs_open_slot(slot, path, FS_DS_RESIZE, 0))
    {
        *err = API_EIO;
        return -1;
    }
    /* Open File reports success for a create into a missing folder and
     * creates nothing, so a plain open after the create checks that the
     * file exists. */
    if (!exists
        && !(fs_open_slot(slot, path, FS_DS_CREATE | FS_DS_RESIZE, 0)
             && fs_open_slot(slot, path, 0, 0)))
    {
        *err = API_ENOENT;
        return -1;
    }

    uint32_t len = 0;
    if (!fs_slot_len(slot, &len))
    {
        *err = API_EIO;
        return -1;
    }
    fs_pool[d].used = true;
    fs_pool[d].flags = flags;
    fs_pool[d].stale = false;
    fs_pool[d].cache_at = 0;
    fs_pool[d].cache_len = 0;
    fs_pool[d].len = empty ? 0 : len;
    fs_pool[d].pos = (flags & FS_APPEND) ? fs_pool[d].len : 0;
    memcpy(fs_pool[d].name, path, strlen(path) + 1);
    return d;
}

void fs_release(int desc)
{
    if (fs_desc(desc) < 0)
        return;
    fs_pool[desc].used = false;
    fs_pool[desc].cache_len = 0;
}

void fs_std_settle(void)
{
}

bool fs_std_ident(int desc, sst_cursor_t *c)
{
    (void)desc, (void)c;
    return false;
}

int fs_std_reopen(sst_cursor_t *c, api_errno *err)
{
    (void)c;
    *err = API_ENOSYS;
    return -1;
}

std_rw_result fs_std_close(int desc, api_errno *err)
{
    (void)err;
    if (desc == FS_DESC_ROM && fs_rom.used)
    {
        fs_rom.used = false;
        fs_rom.pos = 0;
        return STD_OK;
    }
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if (!(fs_pool[desc].flags & FS_WR) || fs_flush_state == FS_FLUSH_NEVER)
    {
        fs_pool[desc].used = false;
        fs_pool[desc].cache_len = 0;
        return STD_OK;
    }
    if (fs_adrift())
        return STD_PENDING;
    fs_rebind(desc);
    std_rw_result res = STD_OK;
    {
        /* The flush blocks, unlike the one in fs_std_sync, because std_stop
         * calls close once and ignores its result, so a STD_PENDING there
         * would drop the flush. */
        fs_wait_free();
        fs_mail[FS_W(desc)] = false;
        FILE_ID = FS_SLOT_FIRST + (uint32_t)desc;
        uint32_t st = fs_command(FS_W(desc), FILE_OP_FLUSH);
        if (fs_unanswered(st))
            fs_flush_state = FS_FLUSH_NEVER;
        else
        {
            fs_flush_state = FS_FLUSH_WORKS;
            if (st & FILE_ST_ERR)
            {
                *err = API_EIO;
                res = STD_ERROR;
            }
        }
    }
    fs_pool[desc].used = false;
    fs_pool[desc].cache_len = 0;
    return res;
}

std_rw_result fs_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err)
{
    *got = 0;
    if (desc == FS_DESC_ROM && fs_rom.used)
    {
        uint32_t n = fs_rom.pos < fs_rom.len ? fs_rom.len - fs_rom.pos : 0;
        if (n > count)
            n = count;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = (char)ROM_IMG[fs_rom.pos + i];
        fs_rom.pos += n;
        *got = n;
        return STD_OK;
    }
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if ((fs_pool[desc].flags & (FS_RD | FS_WR)) == FS_WR)
    {
        *err = API_EACCES;
        return STD_ERROR;
    }
    if (fs_adrift())
        return STD_PENDING;
    fs_rebind(desc);
    uint32_t pos = fs_pool[desc].pos, len = fs_pool[desc].len;
    uint32_t want = pos < len ? len - pos : 0;
    if (want > count)
        want = count;
    if (want > FILE_XFER_MAX)
        want = FILE_XFER_MAX;
    if (!want)
        return STD_OK;

    uint32_t at = fs_pool[desc].cache_at, have = fs_pool[desc].cache_len;
    if (!(have && pos >= at && pos + want <= at + have))
    {
        uint32_t from, n;
        bool grow = have && pos == at + have
                    && !((at + have) % FS_SECTOR)
                    && have < FILE_XFER_MAX;
        if (count > FS_SECTOR)
        {
            from = pos;
            n = want;
            grow = false;
        }
        else
        {
            from = grow ? pos : pos - (pos % FS_SECTOR);
            n = (pos % FS_SECTOR) + count > FS_SECTOR
                    ? 2 * FS_SECTOR
                    : FS_SECTOR;
        }
        if (from + n > len)
            n = len - from;
        uint32_t into = grow ? have : 0;
        if (into + n > FILE_XFER_MAX)
        {
            grow = false;
            into = 0;
        }
        uint32_t st;
        if (!fs_may(FS_W(desc)))
            return STD_PENDING;
        if (fs_owner == FS_W_NONE)
        {
            FILE_ID = FS_SLOT_FIRST + (uint32_t)desc;
            FILE_OFFSET = from;
            FILE_BRIDGE = SLOT_WIN_BRIDGE(desc) + into;
            FILE_LENGTH = n;
            fs_start(FS_W(desc), FILE_OP_READ);
        }
        if (!fs_poll(FS_W(desc), &st))
            return STD_PENDING;
        if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        {
            if (fs_note(st))
                RP6502_LOG(fs, DEBUG, "read %u off=%u len=%u st=%02x", (unsigned)desc,
                           (unsigned)from, (unsigned)n, (unsigned)(st & 0xFFu));
            fs_pool[desc].cache_len = 0;
            *err = API_EIO;
            return STD_ERROR;
        }
        if (grow)
            fs_pool[desc].cache_len = have + n;
        else
        {
            fs_pool[desc].cache_at = from;
            fs_pool[desc].cache_len = n;
        }
        at = fs_pool[desc].cache_at;
    }
    for (uint32_t i = 0; i < want; i++)
        buf[i] = (char)SLOT_WIN(desc)[pos - at + i];
    /* The soft CPU can be halted inside the copy loop above while a savestate
     * is made, and the blob does not include the staging store, so after a
     * restore the rest of the loop copies from a SLOT_WIN(desc) that the
     * restore did not put back. SST_RESTORED is checked again so that the
     * whole read is repeated rather than committed. */
    if (fs_adrift())
        return STD_PENDING;
    fs_pool[desc].pos = pos + want;
    *got = want;
    return STD_OK;
}

std_rw_result fs_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *wrote, api_errno *err)
{
    *wrote = 0;
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if (fs_adrift())
        return STD_PENDING;
    fs_rebind(desc);
    if (!(fs_pool[desc].flags & FS_WR))
    {
        *err = API_EACCES;
        return STD_ERROR;
    }
    fs_pool[desc].cache_len = 0;
    if (!count)
        return STD_OK;
    uint32_t pos = fs_pool[desc].pos;
    uint32_t want = count > FILE_WIN_SIZE ? FILE_WIN_SIZE : count;
    uint32_t slot = FS_SLOT_FIRST + (uint32_t)desc;
    if (pos + want > fs_pool[desc].len)
    {
        uint32_t rc;
        if (!fs_may(FS_W(desc)))
            return STD_PENDING;
        if (!fs_grow)
        {
            if (fs_try_open_start(FS_W(desc), slot, fs_pool[desc].name,
                                   FS_DS_CREATE | FS_DS_RESIZE, pos + want,
                                   FS_SAVES_PATH)
                != FS_RC_STARTED)
            {
                *err = API_EIO;
                return STD_ERROR;
            }
            fs_grow = true;
        }
        if (!fs_try_open_poll(FS_W(desc), &rc))
            return STD_PENDING;
        fs_grow = false;
        if (rc > 1)
        {
            *err = API_EIO;
            return STD_ERROR;
        }
        fs_pool[desc].len = pos + want;
    }
    uint32_t st;
    if (!fs_may(FS_W(desc)))
        return STD_PENDING;
    if (fs_owner == FS_W_NONE)
    {
        fs_win_put(0, (const uint8_t *)buf, want);
        FILE_ID = slot;
        FILE_OFFSET = pos;
        FILE_BRIDGE = FILE_WIN_BASE;
        FILE_LENGTH = want;
        fs_start(FS_W(desc), FILE_OP_WRITE);
    }
    if (!fs_poll(FS_W(desc), &st))
        return STD_PENDING;
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
    {
        if (fs_note(st))
            RP6502_LOG(fs, DEBUG, "write %u off=%u len=%u st=%02x", (unsigned)desc,
                       (unsigned)pos, (unsigned)want, (unsigned)(st & 0xFFu));
        *err = API_EIO;
        return STD_ERROR;
    }
    if (fs_adrift())
        return STD_PENDING;
    fs_pool[desc].pos = pos + want;
    *wrote = want;
    return want < count ? STD_PENDING : STD_OK;
}

std_rw_result fs_std_sync(int desc, api_errno *err)
{
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    fs_rebind(desc);
    if (fs_adrift())
        return STD_PENDING;
    if (fs_flush_state == FS_FLUSH_NEVER)
        return STD_OK;
    uint32_t st;
    if (!fs_may(FS_W(desc)))
        return STD_PENDING;
    if (fs_owner == FS_W_NONE)
    {
        FILE_ID = FS_SLOT_FIRST + (uint32_t)desc;
        fs_start(FS_W(desc), FILE_OP_FLUSH);
    }
    if (!fs_poll(FS_W(desc), &st))
        return STD_PENDING;
    if (fs_unanswered(st))
    {
        fs_flush_state = FS_FLUSH_NEVER;
        return STD_OK;
    }
    fs_flush_state = FS_FLUSH_WORKS;
    if (st & FILE_ST_ERR)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    return STD_OK;
}

int fs_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err)
{
    if (desc == FS_DESC_ROM && fs_rom.used)
    {
        int32_t from = whence == SEEK_SET   ? 0
                       : whence == SEEK_CUR ? (int32_t)fs_rom.pos
                       : whence == SEEK_END ? (int32_t)fs_rom.len
                                            : -1;
        if (from < 0 || off < -from)
        {
            *err = API_EINVAL;
            return -1;
        }
        fs_rom.pos = (uint32_t)(from + off);
        *pos = (int32_t)fs_rom.pos;
        return 0;
    }
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return -1;
    }
    fs_rebind(desc);
    int32_t from = whence == SEEK_SET   ? 0
                   : whence == SEEK_CUR ? (int32_t)fs_pool[desc].pos
                   : whence == SEEK_END ? (int32_t)fs_pool[desc].len
                                        : -1;
    if (from < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    int64_t target = (int64_t)from + off;
    if (target < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    if (target > 0x7FFFFFFF)
    {
        *err = API_ERANGE;
        return -1;
    }
    if (target > (int64_t)fs_pool[desc].len)
        target = (int64_t)fs_pool[desc].len;
    fs_pool[desc].pos = (uint32_t)target;
    *pos = (int32_t)target;
    return 0;
}
