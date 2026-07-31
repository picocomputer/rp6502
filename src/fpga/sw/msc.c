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
 * Where a name resolves is the open question — see msc_open_slot. The
 * machine asks the way that works rather than the way one document
 * says, because the two available documents disagree.
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
 * stream the path rides in — see msc_win_u32, which had them the other
 * way round for long enough that create looked broken and the whole
 * flags word was arriving as zero.
 */

#include "font.h"
#include "mmio.h"
#include "msc.h"

#include "ria/api/uni.h"

#include <stdio.h>
#include <string.h>

/* Slot 0 is the ROM and slot 1 the fonts; the eight above them are
 * ours, and data.json declares them. */
#define MSC_SLOT_FIRST 2
#define MSC_OPEN_MAX 8

/* Open File's parameter struct: 256 bytes of name, then the flags and
 * the size. */
#define MSC_NAME_MAX 256
#define MSC_PARAM_FLAGS 256
#define MSC_PARAM_SIZE 260

#define MSC_DS_CREATE 1u
#define MSC_DS_RESIZE 2u

/* The public open() flags, as fat.c spells them. */
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

/* A command, start to finish. The soft CPU has nothing else to do while
 * the host works — the 6502 is stopped inside its syscall — and
 * pocket_file times out rather than wait forever. */
static uint32_t msc_command(uint32_t op)
{
    FILE_CTL = op;
    uint32_t st;
    do
        st = FILE_CTL;
    while (st & (FILE_ST_BUSY | FILE_ST_DRAIN));
    return st;
}

/* The window is one port of a block RAM and the bridge owns the other,
 * so it is written whole words at a time and never read back. The host
 * takes byte zero of each word from the top eight bits, which is what
 * bridge_endian_little being clear means. */
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

/* The struct's integer fields are words, not bytes. The path above rides
 * the byte stream and arrives intact, so it was reasonable to write these
 * into the same stream — and wrong. The host takes the bridge word as it
 * stands, which means a value written low byte first arrives reversed:
 * flags of 3 land as 0x03000000, every documented bit clear and every
 * reserved bit set, and the host does what that asks. It opens the file
 * and neither creates nor resizes.
 *
 * That reads as a working open on a file already there and as "not
 * found" on one that is not, which is exactly how it looked for a day.
 * Measured in the end by a shrink, the one operation a write cannot
 * counterfeit: a resize to one byte returned success and left an 18 KB
 * file at 18 KB. */
static void msc_win_u32(uint32_t off, uint32_t v)
{
    FILE_WIN[off >> 2] = v;
}

/* The data table is pairs of slot id and slot size, which is how the
 * loader reads slot 0's length. Scanned rather than indexed, so the
 * order the host writes them in does not have to be guessed at. */
#define MSC_DT_PAIRS 20

static uint32_t msc_dt(uint32_t word)
{
    FILE_ID = word;
    msc_command(FILE_OP_DT);
    return FILE_RESULT;
}

static bool msc_slot_len(uint32_t slot, uint32_t *len)
{
    for (uint32_t i = 0; i < MSC_DT_PAIRS; i++)
        if (msc_dt(i * 2) == slot)
        {
            *len = msc_dt(i * 2 + 1);
            return true;
        }
    return false;
}

/* Every name is rooted. The host turns away anything else — "File
 * requested outside /Assets and /Saves, or malformed" — and it does so
 * even when the bare name names a file that exists, which is how we
 * know it is the form and not the lookup. Saves rather than Assets
 * because the machine writes these. */
#define MSC_PATH "/Saves/rp6502/common/"
#define MSC_PATH_LEN (sizeof MSC_PATH - 1)
#define MSC_RC_MALFORMED 4u

/* Bind a slot to a name. Open File answers 0 when the file was there
 * and 1 when it had to make it, and both of those are yes — the rest
 * are 2 slot not defined, 3 not found, 4 malformed path, 5 general. */
static uint32_t msc_try_open(uint32_t slot, const char *name,
                             uint32_t flags, uint32_t size)
{
    uint8_t pad[MSC_NAME_MAX];
    uint16_t page = font_get_code_page();
    memcpy(pad, MSC_PATH, MSC_PATH_LEN);
    size_t n = MSC_PATH_LEN;
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
    uint32_t st = msc_command(FILE_OP_OPEN);
    if (st & FILE_ST_TIMEOUT)
    {
        printf("msc: open timed out\n");
        return MSC_RC_MALFORMED;
    }
    return (st & FILE_ST_ERR) >> 1;
}

static bool msc_open_slot(uint32_t slot, const char *name, uint32_t flags,
                          uint32_t size)
{
    uint32_t rc = msc_try_open(slot, name, flags, size);
    /* 0 opened, 1 created and opened; 2 slot not defined, 3 not found,
     * 4 malformed. open() has one errno for all of them and the console
     * is the only place a program can learn which it got. */
    if (rc > 1)
        printf("msc: open rc=%u\n", (unsigned)rc);
    return rc <= 1;
}

/* The machine names its drive MSC0: and takes 0: as a shortcut, and
 * programs written for it say so. There is one drive here and no
 * working directory, so a prefix is stripped along with the separator
 * that followed it and what remains resolves the same way a bare name
 * does. A path that begins with a slash and no prefix is left alone:
 * that is the host's own root, and it is the only way to reach it. */
static const char *msc_strip_drive(const char *path)
{
    const char *p = path;
    if ((p[0] | 0x20) == 'm' && (p[1] | 0x20) == 's' && (p[2] | 0x20) == 'c')
        p += 3;
    if (*p >= '0' && *p <= '9' && p[1] == ':')
        p += 2;
    else
        return path;
    while (*p == '/' || *p == '\\')
        p++;
    return p;
}

static int msc_desc(int desc)
{
    if (desc < 0 || desc >= MSC_OPEN_MAX || !msc_pool[desc].used)
        return -1;
    return desc;
}

bool msc_std_handles(const char *path)
{
    (void)path;
    return true;
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    path = msc_strip_drive(path);
    if (!*path || strlen(path) >= MSC_NAME_MAX - MSC_PATH_LEN)
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

    /* Exclusive creation has no flag of its own: the host is asked for
     * the file as it stands, and an answer means it is already there. */
    if ((flags & (MSC_O_CREAT | MSC_O_EXCL)) == (MSC_O_CREAT | MSC_O_EXCL)
        && msc_open_slot(slot, path, 0, 0))
    {
        *err = API_EEXIST;
        return -1;
    }

    /* Create and resize are independent, and the size is read only when
     * resize is asked for. So O_CREAT alone opens a file already there
     * without touching it, and only O_TRUNC asks for one back at
     * nothing. Whether the host will hold a file at nothing is the
     * question this shape puts to it. */
    uint32_t dsf = (flags & MSC_O_CREAT) ? MSC_DS_CREATE : 0;
    if (flags & MSC_O_TRUNC)
        dsf |= MSC_DS_RESIZE;
    if (!msc_open_slot(slot, path, dsf, 0))
    {
        *err = API_ENOENT;
        return -1;
    }
    /* A create is answered the same way whether or not one happened. Ask
     * for a file in a folder the card does not have and the host returns
     * a descriptor, having written nothing — a program would be told its
     * save succeeded and find nothing there later. Measured: the create
     * handed back a handle and the next open of that name answered 3.
     *
     * Nothing in the result tells the two apart, so the only honest test
     * is to ask again with no flags and see whether the name is there
     * now. One round trip, and only on the path that creates. */
    if ((dsf & MSC_DS_CREATE) && !msc_open_slot(slot, path, 0, 0))
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
    msc_pool[d].len = (flags & MSC_O_TRUNC) ? 0 : len;
    msc_pool[d].pos = (flags & MSC_O_APPEND) ? msc_pool[d].len : 0;
    memcpy(msc_pool[d].name, path, strlen(path) + 1);
    return d;
}

std_rw_result msc_std_close(int desc, api_errno *err)
{
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    msc_pool[desc].used = false;
    return STD_OK;
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
    FILE_ID = MSC_SLOT_FIRST + (uint32_t)desc;
    FILE_OFFSET = pos;
    FILE_BRIDGE = FILE_STAGE_BRIDGE;
    FILE_LENGTH = want;
    if (msc_command(FILE_OP_READ) & (FILE_ST_ERR | FILE_ST_TIMEOUT))
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    for (uint32_t i = 0; i < want; i++)
        buf[i] = (char)FILE_STAGE[i];
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
        if (!msc_open_slot(slot, msc_pool[desc].name,
                           MSC_DS_CREATE | MSC_DS_RESIZE, pos + want))
        {
            *err = API_ENOSPC;
            return STD_ERROR;
        }
        msc_pool[desc].len = pos + want;
    }
    msc_win_put(0, (const uint8_t *)buf, want);
    FILE_ID = slot;
    FILE_OFFSET = pos;
    FILE_BRIDGE = FILE_WIN_BASE;
    FILE_LENGTH = want;
    if (msc_command(FILE_OP_WRITE) & (FILE_ST_ERR | FILE_ST_TIMEOUT))
    {
        *err = API_EIO;
        return STD_ERROR;
    }
    msc_pool[desc].pos = pos + want;
    *wrote = want;
    return want < count ? STD_PENDING : STD_OK;
}

/* This host does not answer 0x0188. Analogue documents the command and
 * left it out of its own core_bridge_cmd.v, which was the warning; the
 * machine issued one and waited out its timeout, and then every command
 * after it timed out too, because the bridge puts no deadline on a data
 * slot operation and stays in it forever. So one flush takes the drive
 * down for the rest of the session.
 *
 * The plumbing stays — pocket_file carries the op and the override
 * carries the command — because a later Pocket firmware may answer it
 * and a wire costs a fit to put back. Nothing issues it until something
 * proves it is answered. Until then a write is durable when the host
 * says it took it, which is all we can observe. */
std_rw_result msc_std_sync(int desc, api_errno *err)
{
    if (msc_desc(desc) < 0)
    {
        *err = API_EBADF;
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
