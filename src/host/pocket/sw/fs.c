/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mmio.h"
#include "fs.h"

#include "core/str/path.h"
#include "core/str/unicode.h"
#include "core/sys/debug_log.h"
#include "core/sys/sst.h"
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
 * it times the command out. fs_std_read and fs_std_write poll once per pass
 * of the main loop so that the wait does not stall every other task. */
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

static uint32_t fs_failed;

uint32_t fs_failed_count(void)
{
    return fs_failed;
}

static bool fs_poll(uint32_t who, uint32_t *st)
{
    fs_reap();
    if (!fs_mail[who])
        return false;
    fs_mail[who] = false;
    *st = fs_mail_st[who];
    if (*st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        fs_failed++;
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

bool fs_idle(void)
{
    return fs_owner == FS_W_NONE;
}

/* Only the first few failed transfers are logged. */
#define FS_SAY_MAX 4
static uint8_t fs_said;

static bool fs_note(void)
{
    return fs_said < FS_SAY_MAX && ++fs_said;
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

/* A program stopped partway through a call can leave its command in flight.
 * fs_stop collects it and drops its status, so the next program's command
 * on that descriptor does not read it as its own. */
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

/* Copies out of the staging store. A word read there costs what a byte
 * read does, so the copy reads whole words once the source is aligned.
 * The destination is written a byte at a time, since it can start at
 * any address. This is no memcpy, which also copies the byte-wide
 * windows, where a word read is one byte four times. */
static void fs_stage_copy(char *dst, volatile const uint8_t *src, uint32_t n)
{
    for (; n && ((uintptr_t)src & 3u); n--)
        *dst++ = (char)*src++;
    for (; n >= 4; n -= 4, src += 4)
    {
        uint32_t w = *(volatile const uint32_t *)src;
        *dst++ = (char)w;
        *dst++ = (char)(w >> 8);
        *dst++ = (char)(w >> 16);
        *dst++ = (char)(w >> 24);
    }
    while (n--)
        *dst++ = (char)*src++;
}

bool fs_getfile(uint32_t slot, char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
    /* Hardware measurements show that the host writes the whole 256-byte
     * response struct on every Get File and leaves a NUL at offset 0 when
     * the slot is bound to nothing, so an empty name means an unbound
     * slot. */
    char utf8[FS_NAME_MAX];
    FILE_ID = slot;
    FILE_BRIDGE = GETFILE_BRIDGE;
    uint32_t st = fs_command(FS_W_SYS, FILE_OP_GETFILE);
    fs_stage_copy(utf8, GETFILE_WIN, FS_NAME_MAX - 1);
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        return false;
    utf8[FS_NAME_MAX - 1] = 0;

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

#define FS_RC_MALFORMED 4u

#define FS_RC_STARTED 0xFFu

static uint32_t fs_try_open_start(uint32_t who, uint32_t slot,
                                   const char *card, uint32_t flags,
                                   uint32_t size)
{
    uint8_t pad[FS_NAME_MAX];
    uint16_t page = font_get_code_page();
    size_t n = 0;
    for (const unsigned char *s = (const unsigned char *)card; *s; s++)
    {
        char enc[4];
        int k = unicode_to_utf8_char(*s, page, enc);
        if (n + (size_t)k >= FS_NAME_MAX)
            return FS_RC_MALFORMED;
        memcpy(pad + n, enc, (size_t)k);
        n += (size_t)k;
    }
    memset(pad + n, 0, FS_NAME_MAX - n);
    FILE_ID = slot;
    fs_win_put(0, pad, FS_NAME_MAX);
    fs_win_u32(FS_PARAM_FLAGS, flags);
    fs_win_u32(FS_PARAM_SIZE, size);
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

static uint32_t fs_try_open(uint32_t who, uint32_t slot, const char *card,
                             uint32_t flags, uint32_t size)
{
    fs_wait_free();
    uint32_t rc = fs_try_open_start(who, slot, card, flags, size);
    if (rc == FS_RC_STARTED)
        while (!fs_try_open_poll(who, &rc))
            ;
    return rc;
}

static bool fs_open_slot(uint32_t slot, const char *card, uint32_t flags,
                          uint32_t size)
{
    return fs_try_open(FS_W_SYS, slot, card, flags, size) <= 1;
}

const char *fs_strip_drive(const char *path)
{
    if ((path[0] | 0x20) == 'f' && (path[1] | 0x20) == 's' && path[2] == ':')
        return path + 3;
    return path;
}

/* The host resolves no relative name and no . or .., so every name sent to
 * it is made absolute and resolved here. A .. at the card root stays at the
 * root. */
static bool fs_walk(const char *s, char *out, size_t *at, size_t cap)
{
    size_t n = *at;
    while (*s)
    {
        size_t len = strcspn(s, "/\\");
        if (len == 2 && s[0] == '.' && s[1] == '.')
            while (n && out[--n] != '/')
                ;
        else if (len && !(len == 1 && s[0] == '.'))
        {
            if (n + 1 + len >= cap)
                return false;
            out[n++] = '/';
            memcpy(out + n, s, len);
            n += len;
        }
        s += len;
        if (*s)
            s++;
    }
    *at = n;
    return true;
}

bool fs_card_path(const char *path, const char *root, char *out, size_t cap)
{
    size_t n = 0;
    if ((!path_is_sep(*path) && !fs_walk(root, out, &n, cap))
        || !fs_walk(path, out, &n, cap))
        return false;
    if (!n)
        out[n++] = '/';
    out[n] = 0;
    return true;
}

/* The host has no stat, so FS_ASSETS_PATH, FS_SAVES_PATH and the folders
 * above them are the only paths an open refuses as directories. */
static bool fs_leads_to(const char *card, const char *root)
{
    size_t n = strlen(card);
    return !strncmp(card, root, n) && (!root[n] || root[n] == '/');
}

static bool fs_known_dir(const char *card)
{
    return !card[1] || fs_leads_to(card, FS_ASSETS_PATH)
           || fs_leads_to(card, FS_SAVES_PATH);
}

static int fs_desc(int desc)
{
    if (desc < 0 || desc >= FS_OPEN_MAX || !fs_pool[desc].used)
        return -1;
    return desc;
}

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

/* The bridge's commands are waited out before a save, and the files are
 * opened again by the rows of the drivers that hold them. */
#define FS_POCKET_SST(X)                                                \
    X(SKIP, fs_pool, "opened again by the STD row's reopen")            \
    X(SKIP, fs_owner, "a bridge command in flight, which a save waits out") \
    X(SKIP, fs_mail)                                                    \
    X(SKIP, fs_mail_st)                                                 \
    X(SKIP, fs_grow)                                                    \
    X(SKIP, fs_failed, "a count of failed transfers, for the log")      \
    X(SKIP, fs_said)                                                    \
    X(SKIP, fs_rom, "opened again by the ASSET row's reopen")
SST_TABLE(fs_pocket, FS_POCKET_SST)

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

static bool fs_rom_fetch(const char *card, uint32_t *len, api_errno *err)
{
    if (fs_try_open(FS_W_SYS, FS_SLOT_ROM, card, 0, 0) > 1)
    {
        *err = API_ENOENT;
        return false;
    }
    if (!fs_slot_len(FS_SLOT_ROM, len) || !*len || *len > ROM_MAX
        || !fs_rom_pull(*len))
    {
        *err = API_EIO;
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
    const char *rest = fs_strip_drive(path);
    if (!path_fat_ok(rest, rest != path, err))
        return -1;
    char card[FS_NAME_MAX];
    if (!*rest || !fs_card_path(rest, FS_SAVES_PATH, card, sizeof card))
    {
        *err = API_EINVAL;
        return -1;
    }
    /* The image already in the ROM slot is taken as it is when the slot
     * holds this file, as it does after a wake, where the host has loaded
     * the slot again. Fetching it would copy the whole image off the card
     * a second time. */
    char bound[FS_NAME_MAX];
    if (fs_getfile(FS_SLOT_ROM, bound, sizeof bound) && !strcmp(card, bound))
        return fs_rom_adopt(err);
    assert(!fs_rom.used);
    uint32_t len;
    if (!fs_rom_fetch(card, &len, err))
        return -1;
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

static int fs_open_slot_as(int d, uint8_t flags, api_errno *err)
{
    const char *card = fs_pool[d].name;
    uint32_t slot = FS_SLOT_FIRST + (uint32_t)d;

    /* The host creates a file only when Open File has both FS_DS_CREATE and
     * FS_DS_RESIZE set, and both bits on an existing file resize it to the
     * size given, which is zero here, so the first open sets neither bit to
     * find out whether the file exists. */
    bool exists = fs_open_slot(slot, card, 0, 0);
    if (exists && (flags & (FS_CREAT | FS_EXCL)) == (FS_CREAT | FS_EXCL))
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
    if (empty && exists && !fs_open_slot(slot, card, FS_DS_RESIZE, 0))
    {
        *err = API_EIO;
        return -1;
    }
    /* Open File reports success for a create into a missing folder and
     * creates nothing, so a plain open after the create checks that the
     * file exists. */
    if (!exists
        && !(fs_open_slot(slot, card, FS_DS_CREATE | FS_DS_RESIZE, 0)
             && fs_open_slot(slot, card, 0, 0)))
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
    fs_pool[d].cache_at = 0;
    fs_pool[d].cache_len = 0;
    fs_pool[d].len = empty ? 0 : len;
    fs_pool[d].pos = (flags & FS_APPEND) ? fs_pool[d].len : 0;
    return d;
}

static int fs_free_desc(api_errno *err)
{
    for (int d = 0; d < FS_OPEN_MAX; d++)
        if (!fs_pool[d].used)
            return d;
    *err = API_EMFILE;
    return -1;
}

static int fs_open_card(const char *path, const char *root, uint8_t flags,
                        api_errno *err)
{
    int d = fs_free_desc(err);
    if (d < 0)
        return -1;
    char *card = fs_pool[d].name;
    if (!fs_card_path(path, root, card, FS_NAME_MAX))
    {
        *err = API_EINVAL;
        return -1;
    }
    if (fs_known_dir(card))
    {
        *err = API_EACCES;
        return -1;
    }
    return fs_open_slot_as(d, flags, err);
}

int fs_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const char *rest = fs_strip_drive(path);
    if (!path_fat_ok(rest, rest != path, err))
        return -1;
    return fs_open_card(rest, FS_SAVES_PATH, flags, err);
}

int fs_save_open(const char *name, uint8_t flags, api_errno *err)
{
    return fs_open_card(name, FS_SAVES_PATH, flags, err);
}

/* SAVE: is FS_SAVES_PATH for every ROM, so there is nothing to set when a
 * ROM starts. */
void fs_save_start(void)
{
}

void fs_std_settle(void)
{
}

std_rw_result fs_std_close(int desc, api_errno *err)
{
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
    fs_pool[desc].used = false;
    fs_pool[desc].cache_len = 0;
    return STD_OK;
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
        fs_stage_copy(buf, ROM_IMG + fs_rom.pos, n);
        fs_rom.pos += n;
        *got = n;
        return STD_OK;
    }
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
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
            if (fs_note())
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
    fs_stage_copy(buf, SLOT_WIN(desc) + (pos - at), want);
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
                                   FS_DS_CREATE | FS_DS_RESIZE, pos + want)
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
        FILE_ID = slot;
        fs_win_put(0, (const uint8_t *)buf, want);
        FILE_OFFSET = pos;
        FILE_BRIDGE = FILE_WIN_BASE;
        FILE_LENGTH = want;
        fs_start(FS_W(desc), FILE_OP_WRITE);
    }
    if (!fs_poll(FS_W(desc), &st))
        return STD_PENDING;
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
    {
        if (fs_note())
            RP6502_LOG(fs, DEBUG, "write %u off=%u len=%u st=%02x", (unsigned)desc,
                       (unsigned)pos, (unsigned)want, (unsigned)(st & 0xFFu));
        *err = API_EIO;
        return STD_ERROR;
    }
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
    return STD_OK;
}

/* What a resize puts in the bytes it adds is unrecorded, so the gap that a
 * seek past the end opens is written with zeros. When a write fails, the file
 * is resized back so that the failed seek changes nothing. */
static bool fs_extend(int d, uint32_t to)
{
    uint32_t slot = FS_SLOT_FIRST + (uint32_t)d;
    uint32_t rc = fs_try_open(FS_W(d), slot, fs_pool[d].name,
                              FS_DS_CREATE | FS_DS_RESIZE, to);
    if (rc > 1)
        return false;
    for (uint32_t i = 0; i < FILE_WIN_SIZE / 4; i++)
        FILE_WIN[i] = 0;
    for (uint32_t at = fs_pool[d].len; at < to; at += FILE_WIN_SIZE)
    {
        FILE_ID = slot;
        FILE_OFFSET = at;
        FILE_BRIDGE = FILE_WIN_BASE;
        FILE_LENGTH = to - at < FILE_WIN_SIZE ? to - at : FILE_WIN_SIZE;
        if (fs_command(FS_W(d), FILE_OP_WRITE) & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        {
            fs_try_open(FS_W(d), slot, fs_pool[d].name, FS_DS_RESIZE,
                        fs_pool[d].len);
            return false;
        }
    }
    fs_pool[d].len = to;
    return true;
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
    {
        if (!(fs_pool[desc].flags & FS_WR))
            target = (int64_t)fs_pool[desc].len;
        else if (!fs_extend(desc, (uint32_t)target))
        {
            *err = API_EIO;
            return -1;
        }
    }
    fs_pool[desc].pos = (uint32_t)target;
    *pos = (int32_t)target;
    return 0;
}

/* A savestate is made only while no command is in flight, so a
 * descriptor is its card path and its position. The staging store is not
 * in the savestate, so a reopen starts with nothing in its window. */
bool fs_std_ident(int desc, sst_cursor_t *c)
{
    if (fs_desc(desc) < 0)
        return false;
    sst_put_str(c, fs_pool[desc].name, FS_PATH_SLOT);
    sst_put_u32(c, fs_pool[desc].pos);
    return sst_ok(c);
}

int fs_std_reopen(sst_cursor_t *c, uint8_t mode, api_errno *err)
{
    const char *card = sst_view_str(c, FS_PATH_SLOT);
    uint32_t pos = sst_get_u32(c);
    int d = sst_ok(c) && strlen(card) < FS_NAME_MAX ? fs_free_desc(err) : -1;
    if (d < 0)
        return -1;
    strcpy(fs_pool[d].name, card);
    if (fs_open_slot_as(d, mode, err) < 0)
        return -1;
    if (pos > fs_pool[d].len)
    {
        fs_pool[d].used = false;
        *err = API_EIO;
        return -1;
    }
    fs_pool[d].pos = pos;
    return d;
}
