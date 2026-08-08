/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's drive. Eight data slots stand in for eight open files:
 * Open File binds one to a name, Slot Read and Slot Write move bytes
 * between it and memory the bridge can reach, and the data table says
 * how long the file is. pocket_file does the asking; this decides what
 * to ask.
 *
 * Where a name resolves was measured on hardware: the host has no
 * working directory and does not resolve a relative name at all, so the
 * drive spells one out. There is no single root to spell, either — a
 * program opening a file means its own saves folder, and a program being
 * exec'd is browsed for where the menu browses, so each side of the API
 * pins its own. A leading slash names the card's root and travels
 * untouched, which is why MSC0:/ is the card and argv[0] keeps its
 * prefix.
 *
 * A program's path is code page bytes and the host's is UTF-8, so the
 * name is converted on its way out. Three bytes per character is the
 * worst case, and a name that will not fit the struct that way is
 * refused rather than truncated into a different file.
 *
 * A slot's file has a length, not a high-water mark, so a write past the
 * end reopens the slot with the resize flag and the new length before
 * sending the bytes. That costs a round trip per extension.
 *
 * The two integers in Open File's struct are words, not bytes of the
 * stream the path rides in. See msc_win_u32.
 */

#include "font.h"
#include "mmio.h"
#include "msc.h"

#include "ria/api/uni.h"

#include <stdio.h>
#include <string.h>

/* Slots 1-8 are the open files, one per descriptor, above the ROM's
 * slot 0 the way their windows sit above its ceiling. The fonts and
 * the code page tables close the list, staged whole. */
#define MSC_SLOT_FIRST 1
#define MSC_OPEN_MAX 8

#define MSC_NAME_MAX 256
#define MSC_PARAM_FLAGS 256
#define MSC_PARAM_SIZE 260

#define MSC_DS_CREATE 1u
#define MSC_DS_RESIZE 2u

#define MSC_O_READ 0x01
#define MSC_O_WRITE 0x02
#define MSC_O_CREAT 0x10
#define MSC_O_TRUNC 0x20
#define MSC_O_APPEND 0x40
#define MSC_O_EXCL 0x80

static struct
{
    bool used;
    bool writable;
    uint32_t len;
    uint32_t pos;
    /* Kept because growing the file means opening it again, and the
     * window the name went out through cannot be read back. */
    char name[MSC_NAME_MAX];
} msc_pool[MSC_OPEN_MAX];

/* A command in two halves, because the task loop does not wait for
 * anything. The bridge gives a silent host about 0.9 seconds
 * before it retires the command and pocket_file's own backstop is twice
 * that, and spinning either out stops every other task: the 6502's
 * console output, the video frames, and the next file operation with
 * them. Start it, and poll it once per pass. */
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

/* The blocking form, for open and for boot-time staging, and it stays
 * blocking: open happens once per file and the 6502 is parked in its
 * syscall for it either way. Everything the machine does at volume —
 * read, write, sync — goes the other way and is re-entered. */
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

/* std_stop's closes drain only a descriptor they are going to flush, so
 * a read-only one arrives here still in flight. Left there, the bridge
 * keeps holding the command and the next one stacks a toggle on top of
 * it. Nothing is owed the bytes: the program that asked is gone. */
void msc_stop(void)
{
    if (msc_busy || msc_grow)
    {
        uint32_t st;
        while (!msc_poll(&st))
            ;
    }
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
 * but the host takes an integer field as the bridge word stands: written
 * low byte first, flags of 3 arrive as 0x03000000 — every documented bit
 * clear, every reserved bit set — and the host opens the file without
 * creating or resizing it. Which looks like success on a file that
 * exists and "not found" on one that does not. */
static void msc_win_u32(uint32_t off, uint32_t v)
{
    FILE_WIN[off >> 2] = v;
}

/* The table is id/size pairs and the host decides where each one lands —
 * Analogue's side is a RAM the host writes, and defines no layout — so a
 * slot's size is looked up by its id. */
#define MSC_DT_PAIRS 20

uint32_t msc_dt(uint32_t word)
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

/* The one command the host answers with something other than a result
 * code, and the one direction the outbound window cannot carry: the
 * bridge writes the store, so the name lands in the scratch between the
 * assets and the ROM.
 *
 * Analogue documents the command and not the shape of what comes back.
 * This reads a NUL-terminated name at offset 0, which is where Open
 * File's parameter struct carries one. */
bool msc_getfile(uint32_t slot, char *out, size_t cap)
{
    FILE_ID = slot;
    FILE_BRIDGE = GETFILE_BRIDGE;
    uint32_t st = msc_command(FILE_OP_GETFILE);
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
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
            return false;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o != 0;
}

/* Measured on hardware, both of them. The host does not create folders
 * and does not say so — a create into a missing folder answers with a
 * descriptor and leaves nothing on the card — so the package ships the
 * one folder the drive needs. And it does not resolve a relative name,
 * so the drive spells the path out. A name that arrives absolute is the
 * program reaching for the card's root and travels untouched. */
#define MSC_SAVES_LEN (sizeof MSC_SAVES_PATH - 1)
#define MSC_RC_MALFORMED 4u

/* Bind a slot to a name. Open File answers 0 when the file was there
 * and 1 when it had to make it, and both of those are yes — the rest
 * are 2 slot not defined, 3 not found, 4 malformed path, 5 general. */
/* Not a host answer: the command is on its way and the caller must come
 * back for it. */
#define MSC_RC_STARTED 0xFFu

/* Builds the name into the window and starts the command. Answers
 * MSC_RC_MALFORMED without starting one when the name will not fit. */
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

/* A bridge that stopped answering reads as MSC_RC_MALFORMED, which
 * every caller already treats as a refusal. A host that never picked the
 * command up is not that: it comes back as an ordinary result 7. */
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

/* Only the drive prefix is stripped; the slash after it, or its
 * absence, is what decides where the name lands. "foo.txt" and
 * "MSC0:foo.txt" are the same saved game in the drive's folder, the way
 * open("save.dat") should land anywhere; "MSC0:/foo.txt" starts at the
 * card's root. A named drive that is not 0 is refused, not aliased. */
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

static int msc_desc(int desc)
{
    if (desc < 0 || desc >= MSC_OPEN_MAX || !msc_pool[desc].used)
        return -1;
    return desc;
}

/* The bridge's deadline expires before pocket_file's and retires the
 * command itself, so a host that never picked it up arrives as result 7
 * rather than as our own timeout. Either way nobody ran it. */
#define MSC_RC_NO_HOST 7u

static bool msc_unanswered(uint32_t st)
{
    return (st & FILE_ST_TIMEOUT)
           || ((st & FILE_ST_ERR) >> 1) == MSC_RC_NO_HOST;
}

/* Flush, 0x0188: documented by Analogue and absent from its own
 * core_bridge_cmd.v, which was the warning. Asking costs one deadline
 * rather than the session, so the first ask decides and the answer is
 * remembered. */
static enum { MSC_FLUSH_UNTRIED, MSC_FLUSH_WORKS, MSC_FLUSH_NEVER }
    msc_flush_state;

/* Exec's staging: bind the ROM slot to the next image and pull the whole
 * of it to where the host stages one, so the loader and the ROM: drive
 * read it the same way however it arrived.
 *
 * Chunked because the bridge's ~0.9 s deadline times the whole slot
 * operation — the host's ack does not reset it — and the host writes at
 * worst one word per ~1185 ns, about 3.4 MB/s. Half a megabyte leaves
 * the card read and four fifths of the deadline in hand. */
#define MSC_STAGE_CHUNK 0x80000u

bool msc_stage_rom(const char *path, uint32_t *len)
{
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

    /* Creating takes both bits: the create bit alone answers with a
     * descriptor and makes nothing, and resize is what puts the file
     * there. So a create must first know whether the file exists —
     * both bits against one that does would cut it to nothing — which
     * is the same question exclusive creation asks. */
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
    /* Nothing to keep, either because it is new or because the program
     * asked for it gone. The host holds a file at no length, so this is
     * a resize to nothing and a length of nothing on both sides.
     *
     * Truncating one that is already there cannot be a missing folder —
     * the probe above just found it — so the conjure below has nothing
     * to offer and a failure here is the host's, and final. */
    bool empty = !exists || (flags & MSC_O_TRUNC);
    if (empty && exists && !msc_open_slot(slot, path, MSC_DS_RESIZE, 0))
    {
        *err = API_EIO;
        return -1;
    }
    /* A create is answered the same way whether or not one happened: ask
     * for a file in a folder the card does not have and the host returns
     * a descriptor, having written nothing. Nothing in the result tells
     * the two apart, so ask again plainly and take that answer. */
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
    msc_pool[d].len = empty ? 0 : len;
    msc_pool[d].pos = (flags & MSC_O_APPEND) ? msc_pool[d].len : 0;
    memcpy(msc_pool[d].name, path, strlen(path) + 1);
    return d;
}

/* A close flushes. There is no close command to send the host, so this
 * is the only thing that puts a write on the card: a program that writes
 * and closes without syncing has asked for durability the only way the
 * API offers, and getting it is not optional.
 *
 * It blocks, unlike sync, because std_stop discards what close returns.
 * A flush that answered STD_PENDING there would be dropped on the path
 * every exiting program takes. */
std_rw_result msc_std_close(int desc, api_errno *err)
{
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
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
    uint32_t pos = msc_pool[desc].pos, len = msc_pool[desc].len;
    uint32_t want = pos < len ? len - pos : 0;
    if (want > count)
        want = count;
    if (want > FILE_XFER_MAX)
        want = FILE_XFER_MAX;
    if (!want)
        return STD_OK; /* short or zero at the end, which is EOF */
    uint32_t st;
    if (!msc_busy)
    {
        FILE_ID = MSC_SLOT_FIRST + (uint32_t)desc;
        FILE_OFFSET = pos;
        FILE_BRIDGE = SLOT_WIN_BRIDGE(desc);
        FILE_LENGTH = want;
        msc_busy = true;
        msc_start(FILE_OP_READ);
    }
    if (!msc_poll(&st))
        return STD_PENDING;
    msc_busy = false;
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    for (uint32_t i = 0; i < want; i++)
        buf[i] = (char)SLOT_WIN(desc)[i];
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
    if (!msc_pool[desc].writable)
    {
        *err = API_EACCES;
        return STD_ERROR;
    }
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
        *err = API_EIO;
        return STD_ERROR;
    }
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

/* Synthetic: the host cannot be asked, but it was measured and it cannot
 * move. Spelled from the drive so appending a name to it opens the same
 * file the bare name does. chdir fails whatever it names — even the
 * directory getcwd answered — so nothing concludes that directories
 * work. */
bool msc_api_getcwd(void)
{
    static const char cwd[] = "MSC0:/Saves/rp6502/common/";
    uint16_t len = sizeof cwd - 1;
    xstack_ptr = XSTACK_SIZE - len;
    memcpy(&xstack[xstack_ptr], cwd, len);
    return api_return_ax(len + 1);
}

bool msc_api_chdir(void)
{
    xstack_ptr = XSTACK_SIZE;
    return api_return_errno(API_ENOSYS);
}

bool msc_api_chdrive(void)
{
    const char *name = (const char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    const char *rest = msc_strip_drive(name);
    if (!rest || *rest)
        return api_return_errno(API_ENODEV);
    return api_return_ax(0);
}
