/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's drive. Eight data slots stand in for eight open files.
 *
 * The host has no working directory and will not resolve a relative
 * name, so the drive spells one out. There is no single root to spell:
 * an open means the program's own saves folder and an exec means where
 * the menu browses, so each side of the API pins its own. A leading
 * slash names the card's root and travels untouched.
 *
 * Paths are code page bytes going out and UTF-8 at the host, worst case
 * three bytes per character. A name that will not fit the struct is
 * refused rather than truncated into a different file.
 *
 * A slot's file has a length, not a high-water mark, so a write past the
 * end costs a resize-open round trip before the bytes.
 */

#include "font.h"
#include "log.h"
#include "mmio.h"
#include "msc.h"

#include "core/api/uni.h"

#include <stdio.h>
#include <string.h>

/* Slots 1-8 are the open files, above the ROM's slot 0. */
#define MSC_SLOT_FIRST 1
#define MSC_OPEN_MAX 8

#define MSC_NAME_MAX 256
#define MSC_PARAM_FLAGS 256
#define MSC_PARAM_SIZE 260

#define MSC_DS_CREATE 1u
#define MSC_DS_RESIZE 2u

/* The card's sector. A short read that straddles two of them costs two
 * whatever this side does, so the fetch is aligned to one and never
 * shorter than one. */
#define MSC_SECTOR 512u

static struct
{
    bool used;
    bool writable;
    /* A restore leaves the drive's own idea of a descriptor intact and
     * the host's binding for it gone. Rebinding all eight there costs
     * eight round trips for files the session may never touch again, so
     * it is deferred to whatever asks first. */
    bool stale;
    uint32_t len;
    uint32_t pos;
    /* What range of the file is in SLOT_WIN(d) right now. Zero length is
     * empty. Reads inside it cost nothing; a read off the end appends
     * the next sector beside what is there rather than starting over,
     * which is what lets this reach the window's full size on a
     * sequential walk. Writes do not cache and clear it. */
    uint32_t cache_at;
    uint32_t cache_len;
    /* Kept because growing the file means opening it again, and the
     * window the name went out through cannot be read back. */
    char name[MSC_NAME_MAX];
} msc_pool[MSC_OPEN_MAX];

/* Two halves, because the task loop does not wait: a silent host holds
 * the bridge for about 0.9 seconds and spinning that out stops every
 * other task. Start it, and poll once per pass. */
static void msc_start(uint32_t op)
{
    FILE_CTL = op;
}

static bool msc_poll(uint32_t *st)
{
    uint32_t v = FILE_CTL;
    if (v & (FILE_ST_BUSY | FILE_ST_DRAIN))
        return false;
    *st = v;
    return true;
}

/* Counted rather than printed at the point of failure: a stream that
 * goes wrong goes wrong every frame, and a console full of it would be
 * the second thing the user could not read. Reported once. */
static uint16_t msc_n_tmo, msc_n_err, msc_n_defer;
static uint32_t msc_last_st;

void msc_log(void)
{
    /* Deferrals are the restore's own one-pass stall and not a
     * failure, so they are reported beside the errors but never on
     * their own -- a clean restore says nothing. */
    if (!msc_n_tmo && !msc_n_err)
    {
        msc_n_defer = 0;
        return;
    }
    printf("msc: tmo=%u err=%u defer=%u last=%02x\n", (unsigned)msc_n_tmo,
           (unsigned)msc_n_err, (unsigned)msc_n_defer,
           (unsigned)(msc_last_st & 0xFFu));
    msc_n_tmo = msc_n_err = msc_n_defer = 0;
}

/* A restore has landed and the fixups have not finished. Between those
 * two moments this driver's word is worth nothing: the descriptor came
 * out of the blob, the slot it names belongs to whatever the wake booted
 * into, and pocket_file was reconfigured under a command the sleeping
 * session issued. The main loop makes the window unavoidable -- api_task
 * runs before sst_task, so a syscall carried across the sleep is
 * re-dispatched a whole pass before msc_restore rebinds anything -- so
 * the guard belongs here, at the driver, where it holds whatever order
 * the tasks run in.
 *
 * Answering STD_PENDING is lossless: pos is not advanced, msc_restore
 * clears msc_busy, and the next pass re-issues the same operation
 * against a slot that is its own again. The stall is one pass, because
 * sst_task clears the bit at the end of it. */
static bool msc_adrift(void)
{
    if (!(SST_CTL & SST_RESTORED))
        return false;
    msc_n_defer++;
    return true;
}

/* A stream that fails, fails every frame. The first few say what
 * happened and the rest are counted, because a console nobody can read
 * is how this started. */
#define MSC_SAY_MAX 4

static bool msc_note(uint32_t st)
{
    msc_last_st = st;
    if (st & FILE_ST_TIMEOUT)
        msc_n_tmo++;
    else if (st & FILE_ST_ERR)
        msc_n_err++;
    return (unsigned)(msc_n_tmo + msc_n_err) <= MSC_SAY_MAX;
}

/* The blocking form, for open and boot-time staging: once per file, and
 * the 6502 is parked in its syscall either way. */
static uint32_t msc_command(uint32_t op)
{
    uint32_t st;
    msc_start(op);
    while (!msc_poll(&st))
        ;
    return st;
}

/* One record is enough: the 6502 is parked in a single syscall, so only
 * one worker is ever mid-op. */
static bool msc_busy;

/* A second command the write worker can have in flight: the resize-open
 * that makes room before the WRITE that fills it. */
static bool msc_grow;

/* std_stop drains only descriptors it will flush, so a read-only one
 * arrives still in flight. Left there, the next command stacks a toggle
 * on top of it. */
void msc_stop(void)
{
    /* Unconditional, because the two flags below say what the session
     * that wrote them was doing and the command in the fabric may
     * belong to another one: a restore brings back a machine that
     * thought it was idle over a pocket_file that is mid-command, and
     * skipping the drain there stacks the next command's toggle onto a
     * live one, where the bridge drops it outright and answers the
     * previous command instead. Idle costs one read. */
    uint32_t st;
    bool waited = false;
    while (!msc_poll(&st))
        waited = true;
    if (waited || msc_busy || msc_grow)
        LOG_SAY("msc: drain waited=%u busy=%u grow=%u st=%02x\n",
               (unsigned)waited, (unsigned)msc_busy, (unsigned)msc_grow,
               (unsigned)(st & 0xFFu));
    msc_busy = false;
    msc_grow = false;
}

static void msc_win_put(uint32_t off, const uint8_t *src, uint32_t len)
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

/* Words, not bytes. The path rides the byte stream and arrives intact,
 * but an integer field is taken as the bridge word stands: written low
 * byte first, flags of 3 arrive as 0x03000000. */
static void msc_win_u32(uint32_t off, uint32_t v)
{
    FILE_WIN[off >> 2] = v;
}

/* The table is id/size pairs with no defined layout, so a slot's size is
 * looked up by its id. */
#define MSC_DT_PAIRS 20

static uint32_t msc_dt(uint32_t word)
{
    FILE_ID = word;
    msc_command(FILE_OP_DT);
    return FILE_RESULT;
}

bool msc_slot_len(uint32_t slot, uint32_t *len)
{
    for (uint32_t i = 0; i < MSC_DT_PAIRS; i++)
        if (msc_dt(i * 2) == slot)
        {
            *len = msc_dt(i * 2 + 1);
            return true;
        }
    return false;
}

/* The shape of the reply is undocumented. This reads a NUL-terminated
 * name at offset 0, where Open File's parameter struct carries one. */
bool msc_getfile(uint32_t slot, char *out, size_t cap)
{
    FILE_ID = slot;
    FILE_BRIDGE = GETFILE_BRIDGE;
    uint32_t st = msc_command(FILE_OP_GETFILE);
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        return false;
    /* An answer of ok with nothing written is legal -- Get File has no
     * result code for a slot that is defined but has nothing bound --
     * and the window is a read-only view of the staging store, so it
     * cannot be blanked beforehand to tell the difference. The fabric
     * watches for the write instead. Without this the caller is handed
     * the previous Get File's name for a slot that has none, which
     * msc_still_bound would read as a binding that was kept. */
    if (!(st & FILE_ST_WROTE))
        return false;

    char utf8[MSC_NAME_MAX];
    size_t n = 0;
    while (n < MSC_NAME_MAX - 1 && GETFILE_WIN[n])
        utf8[n] = (char)GETFILE_WIN[n], n++;
    utf8[n] = 0;

    uint16_t page = font_get_code_page();
    const char *p = utf8;
    size_t o = 0;
    for (;;)
    {
        unsigned char c = uni_from_utf8_next(&p, page);
        if (!c)
            break;
        if (o + 1 >= cap)
        {
            /* Terminated before giving up, because the caller's buffer
             * has already been written into and a refusal must not hand
             * back an unterminated one. */
            out[0] = 0;
            return false;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o != 0;
}

/* The host does not create folders and does not say so, so the package
 * ships the one folder the drive needs. */
#define MSC_SAVES_LEN (sizeof MSC_SAVES_PATH - 1)
#define MSC_RC_MALFORMED 4u

/* Not a host answer: the command is on its way and the caller must come
 * back for it. */
#define MSC_RC_STARTED 0xFFu

/* Answers MSC_RC_MALFORMED without starting a command when the name will
 * not fit. */
static uint32_t msc_try_open_start(uint32_t slot, const char *name,
                                   uint32_t flags, uint32_t size,
                                   const char *root)
{
    uint8_t pad[MSC_NAME_MAX];
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
        int k = uni_to_utf8_char(*s, page, enc);
        if (n + (size_t)k >= MSC_NAME_MAX)
            return MSC_RC_MALFORMED;
        memcpy(pad + n, enc, (size_t)k);
        n += (size_t)k;
    }
    memset(pad + n, 0, MSC_NAME_MAX - n);
    msc_win_put(0, pad, MSC_NAME_MAX);
    msc_win_u32(MSC_PARAM_FLAGS, flags);
    msc_win_u32(MSC_PARAM_SIZE, size);
    FILE_ID = slot;
    msc_start(FILE_OP_OPEN);
    return MSC_RC_STARTED;
}

/* A bridge that stopped answering reads as MSC_RC_MALFORMED, which every
 * caller already treats as a refusal. */
static bool msc_try_open_poll(uint32_t *rc)
{
    uint32_t st;
    if (!msc_poll(&st))
        return false;
    *rc = (st & FILE_ST_TIMEOUT) ? MSC_RC_MALFORMED : ((st & FILE_ST_ERR) >> 1);
    return true;
}

/* The blocking form, for open and for exec's staging. */
static uint32_t msc_try_open(uint32_t slot, const char *name,
                             uint32_t flags, uint32_t size,
                             const char *root)
{
    uint32_t rc = msc_try_open_start(slot, name, flags, size, root);
    if (rc == MSC_RC_STARTED)
        while (!msc_try_open_poll(&rc))
            ;
    return rc;
}

static bool msc_open_slot(uint32_t slot, const char *name, uint32_t flags,
                          uint32_t size)
{
    return msc_try_open(slot, name, flags, size, MSC_SAVES_PATH) <= 1;
}

/* Only the prefix is stripped; the slash after it, or its absence, is
 * what decides where the name lands. A drive that is not 0 is refused,
 * not aliased. */
static const char *msc_strip_drive(const char *path)
{
    const char *p = path;
    if ((p[0] | 0x20) == 'm' && (p[1] | 0x20) == 's' && (p[2] | 0x20) == 'c')
        p += 3;
    if (*p >= '0' && *p <= '9' && p[1] == ':')
    {
        if (*p != '0')
            return NULL;
        return p + 2;
    }
    return path;
}

/* What the host currently has this slot bound to, in the code page,
 * against what this descriptor was opened as. The name kept beside the
 * descriptor is the program's -- relative or absolute -- and the host
 * answers with the whole path, so the comparison rebuilds the prefix
 * the same way an open would have added it. */
static bool msc_still_bound(int d)
{
    char have[MSC_NAME_MAX];
    if (!msc_getfile(MSC_SLOT_FIRST + (uint32_t)d, have, sizeof have))
        return false;
    const char *want = msc_pool[d].name;
    const char *at = have;
    if (*want != '/')
    {
        size_t n = MSC_SAVES_LEN;
        if (strncmp(have, MSC_SAVES_PATH, n) != 0)
            return false;
        at += n;
    }
    return strcmp(at, want) == 0;
}

/* Every open file was a data slot the host had bound to a path. Whether
 * a wake keeps that binding is not written down anywhere -- Analogue's
 * boot process says the host loads the slots data.json describes, and
 * these eight are deferload with no filename in it, so there is nothing
 * for it to bind them to; but the docs never say what becomes of a
 * binding made at runtime with 0x0192, and this file asserted for a
 * while that they were simply gone.
 *
 * So it asks. 0x0190 answers with the path a slot is bound to, and a
 * slot that still holds the right file is left alone. That is right
 * whichever way the host behaves, and it is the difference between one
 * round trip per open file and none: a wake that kept its bindings pays
 * nothing, and one that did not is put back exactly as before.
 *
 * The name is kept beside the descriptor because the window it went out
 * through cannot be read back. The position is the
 * caller's own count and never went near the host.
 *
 * A slot that will not open again is left open here, so a program that
 * goes on using it is told the read failed rather than that its file
 * was never open. */
void msc_restore(void)
{
    msc_stop();
    for (int d = 0; d < MSC_OPEN_MAX; d++)
    {
        if (!msc_pool[d].used)
            continue;
        msc_pool[d].stale = true;
        /* The window is the board's and the blob does not carry it, so
         * whatever it held belongs to the session that went away. */
        msc_pool[d].cache_len = 0;
    }
}

/* The deferred half of the above, run for a descriptor the first time
 * anything asks it for something after a restore.
 *
 * The whole of what this side is unsure about, per descriptor and once
 * per wake: whether the host still had the slot, what it said when asked
 * to bind it again, and whether its idea of the file's length is the one
 * the sleeping session had. Every one of those is a question Analogue
 * documents nowhere and that only the device can answer.
 *
 * A slot that will not open again is left open, so a program that goes
 * on using it is told the read failed rather than that its file was
 * never open. */
static void msc_rebind(int d)
{
    if (!msc_pool[d].stale)
        return;
    msc_pool[d].stale = false;
    bool kept = msc_still_bound(d);
    uint32_t rc = 0;
    if (!kept)
        rc = msc_try_open(MSC_SLOT_FIRST + (uint32_t)d,
                          msc_pool[d].name, 0, 0, MSC_SAVES_PATH);
    uint32_t len = 0;
    bool got = msc_slot_len(MSC_SLOT_FIRST + (uint32_t)d, &len);
    LOG_SAY("msc: %u '%s' kept=%u rc=%u blob=%u host=%u/%u pos=%u w=%u\n",
           (unsigned)d, msc_pool[d].name, (unsigned)kept, (unsigned)rc,
           (unsigned)msc_pool[d].len, (unsigned)got, (unsigned)len,
           (unsigned)msc_pool[d].pos, (unsigned)msc_pool[d].writable);
    if (!kept && rc > 1)
        return;
    /* The length is the host's to say for a file this side did not
     * write: the card outlived the session and the file may not be
     * the size the blob remembers. A writable descriptor keeps its
     * own, because it is what resized the file and the host's table
     * is only as fresh as the last open. */
    if (got && !msc_pool[d].writable)
        msc_pool[d].len = len;
    if (msc_pool[d].pos > msc_pool[d].len)
        msc_pool[d].pos = msc_pool[d].len;
}

static int msc_desc(int desc)
{
    if (desc < 0 || desc >= MSC_OPEN_MAX || !msc_pool[desc].used)
        return -1;
    return desc;
}

/* The bridge retires the command before pocket_file's deadline, so a host
 * that never picked it up arrives as result 7, not as our own timeout. */
#define MSC_RC_NO_HOST 7u

static bool msc_unanswered(uint32_t st)
{
    return (st & FILE_ST_TIMEOUT)
           || ((st & FILE_ST_ERR) >> 1) == MSC_RC_NO_HOST;
}

/* Flush, 0x0188, is documented but absent from core_bridge_cmd.v. Asking
 * costs one deadline, so the first ask decides and is remembered. */
static enum { MSC_FLUSH_UNTRIED, MSC_FLUSH_WORKS, MSC_FLUSH_NEVER }
    msc_flush_state;

/* Chunked because the bridge's ~0.9 s deadline times the whole slot
 * operation, and the host writes at worst ~3.4 MB/s. */
#define MSC_STAGE_CHUNK 0x80000u

bool msc_stage_rom(const char *path, uint32_t *len)
{
    /* Never under a running 6502. The store is what its ROM: assets are
     * read out of, and rewriting it beneath a program is both a pause it
     * did not ask for and a file changing under an open descriptor.
     * Every caller stops the machine first; this is that rule made
     * enforceable rather than remembered. */
    if (CPU_RESB & 1)
    {
        printf("rom: stage refused, 6502 running\n");
        return false;
    }
    const char *p = msc_strip_drive(path);
    if (!p || !*p)
        return false;
    if (msc_try_open(MSC_SLOT_ROM, p, 0, 0, MSC_ASSETS_PATH) > 1)
        return false;
    if (!msc_slot_len(MSC_SLOT_ROM, len) || !*len || *len > ROM_MAX)
        return false;
    for (uint32_t at = 0; at < *len; at += MSC_STAGE_CHUNK)
    {
        uint32_t n = *len - at;
        if (n > MSC_STAGE_CHUNK)
            n = MSC_STAGE_CHUNK;
        FILE_ID = MSC_SLOT_ROM;
        FILE_OFFSET = at;
        FILE_BRIDGE = ROM_BRIDGE + at;
        FILE_LENGTH = n;
        uint32_t st = msc_command(FILE_OP_READ);
        if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
            return false;
    }
    return true;
}

bool msc_std_handles(const char *path)
{
    (void)path;
    return true;
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    path = msc_strip_drive(path);
    if (!path)
    {
        *err = API_ENODEV;
        return -1;
    }
    if (!*path || strlen(path) >= MSC_NAME_MAX - MSC_SAVES_LEN)
    {
        *err = API_EINVAL;
        return -1;
    }
    int d = -1;
    for (int i = 0; i < MSC_OPEN_MAX; i++)
        if (!msc_pool[i].used)
        {
            d = i;
            break;
        }
    if (d < 0)
    {
        *err = API_EMFILE;
        return -1;
    }
    uint32_t slot = MSC_SLOT_FIRST + (uint32_t)d;

    /* Creating takes both bits: create alone makes nothing, and both bits
     * against a file that exists would cut it to nothing. So probe first. */
    bool exists = msc_open_slot(slot, path, 0, 0);
    if (exists && (flags & (MSC_O_CREAT | MSC_O_EXCL))
                      == (MSC_O_CREAT | MSC_O_EXCL))
    {
        *err = API_EEXIST;
        return -1;
    }
    if (!exists && !(flags & MSC_O_CREAT))
    {
        *err = API_ENOENT;
        return -1;
    }
    /* The probe above found the file, so a failure here is the host's and
     * final; the retry below has nothing to offer. */
    bool empty = !exists || (flags & MSC_O_TRUNC);
    if (empty && exists && !msc_open_slot(slot, path, MSC_DS_RESIZE, 0))
    {
        *err = API_EIO;
        return -1;
    }
    /* A create into a missing folder returns a descriptor having written
     * nothing, and the result cannot tell the two apart. Ask again plainly. */
    if (!exists
        && !(msc_open_slot(slot, path, MSC_DS_CREATE | MSC_DS_RESIZE, 0)
             && msc_open_slot(slot, path, 0, 0)))
    {
        *err = API_ENOENT;
        return -1;
    }

    uint32_t len = 0;
    if (!msc_slot_len(slot, &len))
    {
        *err = API_EIO;
        return -1;
    }
    msc_pool[d].used = true;
    msc_pool[d].writable = (flags & MSC_O_WRITE) != 0;
    /* A fresh binding, so nothing is owed and the window holds nothing
     * of this file. */
    msc_pool[d].stale = false;
    msc_pool[d].cache_at = 0;
    msc_pool[d].cache_len = 0;
    msc_pool[d].len = empty ? 0 : len;
    msc_pool[d].pos = (flags & MSC_O_APPEND) ? msc_pool[d].len : 0;
    memcpy(msc_pool[d].name, path, strlen(path) + 1);
    return d;
}

/* A close flushes: there is no close command, so this is the only thing
 * that puts a write on the card. It blocks, unlike sync, because
 * std_stop discards what close returns and would drop a STD_PENDING. */
std_rw_result msc_std_close(int desc, api_errno *err)
{
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    msc_rebind(desc);
    std_rw_result res = STD_OK;
    if (msc_pool[desc].writable && msc_flush_state != MSC_FLUSH_NEVER)
    {
        uint32_t st;
        /* A read or write left in flight has to land before the flush
         * can start; the descriptor is going away either way. */
        if (msc_busy)
        {
            while (!msc_poll(&st))
                ;
            msc_busy = false;
        }
        FILE_ID = MSC_SLOT_FIRST + (uint32_t)desc;
        st = msc_command(FILE_OP_FLUSH);
        if (msc_unanswered(st))
            msc_flush_state = MSC_FLUSH_NEVER;
        else
        {
            msc_flush_state = MSC_FLUSH_WORKS;
            if (st & FILE_ST_ERR)
            {
                *err = API_EIO;
                res = STD_ERROR;
            }
        }
    }
    /* Released even when the flush failed, the way close always does. */
    msc_pool[desc].used = false;
    msc_pool[desc].cache_len = 0;
    return res;
}

std_rw_result msc_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err)
{
    *got = 0;
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if (msc_adrift())
        return STD_PENDING;
    msc_rebind(desc);
    uint32_t pos = msc_pool[desc].pos, len = msc_pool[desc].len;
    uint32_t want = pos < len ? len - pos : 0;
    if (want > count)
        want = count;
    if (want > FILE_XFER_MAX)
        want = FILE_XFER_MAX;
    if (!want)
        return STD_OK; /* short or zero at the end, which is EOF */

    /* Already here, so nothing goes to the card at all. */
    uint32_t at = msc_pool[desc].cache_at, have = msc_pool[desc].cache_len;
    if (!(have && pos >= at && pos + want <= at + have))
    {
        /* What to fetch is the card's business, not the caller's: the
         * sector is 512 and a short read straddling two costs both
         * whichever way this is written. So a small ask is rounded out
         * to the sector it lands in, and to the pair when it spans them.
         * A caller asking for more than a sector is not second-guessed
         * and gets exactly what it asked for. */
        uint32_t from, n;
        bool grow = have && pos == at + have
                    && !((at + have) % MSC_SECTOR)
                    && have < FILE_XFER_MAX;
        if (count > MSC_SECTOR)
        {
            from = pos;
            n = want;
            grow = false;
        }
        else
        {
            from = grow ? pos : pos - (pos % MSC_SECTOR);
            n = (pos % MSC_SECTOR) + count > MSC_SECTOR
                    ? 2 * MSC_SECTOR
                    : MSC_SECTOR;
        }
        if (from + n > len)
            n = len - from;
        /* Beside what is already there rather than over it, so a walk
         * through a file fills the window instead of replacing one
         * sector with the next. */
        uint32_t into = grow ? have : 0;
        if (into + n > FILE_XFER_MAX)
        {
            grow = false;
            into = 0;
        }
        uint32_t st;
        if (!msc_busy)
        {
            FILE_ID = MSC_SLOT_FIRST + (uint32_t)desc;
            FILE_OFFSET = from;
            FILE_BRIDGE = SLOT_WIN_BRIDGE(desc) + into;
            FILE_LENGTH = n;
            msc_busy = true;
            msc_start(FILE_OP_READ);
        }
        if (!msc_poll(&st))
            return STD_PENDING;
        msc_busy = false;
        if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        {
            if (msc_note(st))
                printf("msc: read %u off=%u len=%u st=%02x\n", (unsigned)desc,
                       (unsigned)from, (unsigned)n, (unsigned)(st & 0xFFu));
            msc_pool[desc].cache_len = 0;
            *err = API_EIO;
            return STD_ERROR;
        }
        if (grow)
            msc_pool[desc].cache_len = have + n;
        else
        {
            msc_pool[desc].cache_at = from;
            msc_pool[desc].cache_len = n;
        }
        at = msc_pool[desc].cache_at;
    }
    for (uint32_t i = 0; i < want; i++)
        buf[i] = (char)SLOT_WIN(desc)[pos - at + i];
    /* Again after the copy, not only before it: the window the bytes
     * came out of is the board's and no blob carries it, so a freeze
     * that landed anywhere in that loop lifted the tail of this buffer
     * out of a store the wake rebuilt. Asked once more here, the whole
     * read is re-taken instead of half-committed. */
    if (msc_adrift())
        return STD_PENDING;
    msc_pool[desc].pos = pos + want;
    *got = want;
    return STD_OK;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count,
                            uint32_t *wrote, api_errno *err)
{
    *wrote = 0;
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    if (msc_adrift())
        return STD_PENDING;
    msc_rebind(desc);
    if (!msc_pool[desc].writable)
    {
        *err = API_EACCES;
        return STD_ERROR;
    }
    /* Writes do not cache, and they make whatever the window holds a
     * different file. */
    msc_pool[desc].cache_len = 0;
    if (!count)
        return STD_OK;
    uint32_t pos = msc_pool[desc].pos;
    uint32_t want = count > FILE_WIN_SIZE ? FILE_WIN_SIZE : count;
    uint32_t slot = MSC_SLOT_FIRST + (uint32_t)desc;
    if (pos + want > msc_pool[desc].len)
    {
        uint32_t rc;
        if (!msc_grow)
        {
            if (msc_try_open_start(slot, msc_pool[desc].name,
                                   MSC_DS_CREATE | MSC_DS_RESIZE, pos + want,
                                   MSC_SAVES_PATH)
                != MSC_RC_STARTED)
            {
                *err = API_ENOSPC;
                return STD_ERROR;
            }
            msc_grow = true;
        }
        if (!msc_try_open_poll(&rc))
            return STD_PENDING;
        msc_grow = false;
        if (rc > 1)
        {
            *err = API_ENOSPC;
            return STD_ERROR;
        }
        msc_pool[desc].len = pos + want;
    }
    uint32_t st;
    if (!msc_busy)
    {
        msc_win_put(0, (const uint8_t *)buf, want);
        FILE_ID = slot;
        FILE_OFFSET = pos;
        FILE_BRIDGE = FILE_WIN_BASE;
        FILE_LENGTH = want;
        msc_busy = true;
        msc_start(FILE_OP_WRITE);
    }
    if (!msc_poll(&st))
        return STD_PENDING;
    msc_busy = false;
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
    {
        if (msc_note(st))
            printf("msc: write %u off=%u len=%u st=%02x\n", (unsigned)desc,
                   (unsigned)pos, (unsigned)want, (unsigned)(st & 0xFFu));
        *err = API_EIO;
        return STD_ERROR;
    }
    if (msc_adrift())
        return STD_PENDING;
    msc_pool[desc].pos = pos + want;
    *wrote = want;
    return want < count ? STD_PENDING : STD_OK;
}

std_rw_result msc_std_sync(int desc, api_errno *err)
{
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    msc_rebind(desc);
    /* msc_flush_state is sticky, so a verdict reached against a
     * reconfigured bridge would condemn every flush for the rest of
     * the session. */
    if (msc_adrift())
        return STD_PENDING;
    if (msc_flush_state == MSC_FLUSH_NEVER)
        return STD_OK;
    uint32_t st;
    if (!msc_busy)
    {
        FILE_ID = MSC_SLOT_FIRST + (uint32_t)desc;
        msc_busy = true;
        msc_start(FILE_OP_FLUSH);
    }
    if (!msc_poll(&st))
        return STD_PENDING;
    msc_busy = false;
    if (msc_unanswered(st))
    {
        msc_flush_state = MSC_FLUSH_NEVER;
        return STD_OK;
    }
    msc_flush_state = MSC_FLUSH_WORKS;
    if (st & FILE_ST_ERR)
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    return STD_OK;
}

int msc_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err)
{
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return -1;
    }
    msc_rebind(desc);
    int32_t from = whence == SEEK_SET   ? 0
                   : whence == SEEK_CUR ? (int32_t)msc_pool[desc].pos
                   : whence == SEEK_END ? (int32_t)msc_pool[desc].len
                                        : -1;
    if (from < 0 || from + off < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    msc_pool[desc].pos = (uint32_t)(from + off);
    *pos = from + off;
    return 0;
}

/* ---- The drive, as core/api/dir.c asks for it ---------------------------- */

/* Synthetic: the host cannot be asked. Spelled from the drive so
 * appending a name opens the same file the bare name does. */
static bool msc_dir_getcwd(char *buf, size_t size, api_errno *err)
{
    static const char cwd[] = "MSC0:/Saves/rp6502/common/";
    if (size < sizeof cwd)
    {
        *err = API_ENOMEM;
        return false;
    }
    memcpy(buf, cwd, sizeof cwd);
    return true;
}

static bool msc_dir_chdrive(const char *drive, api_errno *err)
{
    const char *rest = msc_strip_drive(drive);
    if (!rest || *rest)
    {
        *err = API_ENODEV;
        return false;
    }
    return true;
}

/* One folder, no directories to walk and no metadata to read, so all this
 * drive answers is where it is and that it is the only one. The rest of the
 * slots stay empty and the syscalls above them say ENOSYS. */
const dir_backend_t msc_dir_backend = {
    .chdrive = msc_dir_chdrive,
    .getcwd = msc_dir_getcwd,
};
