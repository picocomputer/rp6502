/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Files on the APF host, as osal/fs.h asks for them: the std driver, over
 * the data-slot bridge and the slot pool it stands on.
 *
 * There is no filesystem down there to speak of. A name is handed to the host
 * and it binds a data slot to it; the eight descriptors are slots 1..8, and
 * slot 0 is the ROM, which data.json puts first because a hot reload writes
 * the new image through the first slot record. The directories such as they
 * are live next door in dir.c.
 *
 * The host has no working directory and will not resolve a relative name, so
 * the drive spells one out. There is no single root to spell: an open means
 * the program's own saves folder and an exec means where the menu browses, so
 * each side of the API pins its own. A leading slash names the card's root
 * and travels untouched.
 *
 * Paths are code page bytes going out and UTF-8 at the host, worst case three
 * bytes per character. A name that will not fit the struct is refused rather
 * than truncated into a different file.
 *
 * A slot's file has a length, not a high-water mark, so a write past the end
 * costs a resize-open round trip before the bytes. A transfer crosses through
 * a 512-byte window, so a longer write lands in pieces and says STD_PENDING
 * until the last one -- the dispatcher comes back for the rest. Sleep and
 * wake can take the host away underneath an open file, so every entry point
 * checks and rebinds.
 */

#include "mmio.h"
#include "fs.h"

#include "core/str/unicode.h"
#include "core/term/font.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Slots 1-8 are the open files, above the ROM's slot 0. */
#define FS_SLOT_FIRST 1
#define FS_OPEN_MAX 8

#define FS_NAME_MAX 256
#define FS_PARAM_FLAGS 256
#define FS_PARAM_SIZE 260

#define FS_DS_CREATE 1u
#define FS_DS_RESIZE 2u

/* The card's sector. A short read that straddles two of them costs two
 * whatever this side does, so the fetch is aligned to one and never
 * shorter than one. */
#define FS_SECTOR 512u

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
    char name[FS_NAME_MAX];
} fs_pool[FS_OPEN_MAX];

/* The fabric carries one command and answers it in one register, so
 * which worker issued it has to be written down. The 6502 is parked in
 * a single syscall and only one of its operations is ever in flight --
 * but the console's own writer is not the 6502 and runs in the same
 * pass. A worker that retires a command it did not issue reads a window
 * nothing filled and moves a file position by bytes that went to
 * another file.
 *
 * An answer found by the wrong worker is kept for the right one instead
 * of being dropped. That is what lets the blocking form wait out a
 * record another worker is holding rather than deadlock against it. */
#define FS_W_NONE 0u
#define FS_W(d) ((uint32_t)(d) + 1u)
/* Staging and binding, which are the machine's own and hold no
 * descriptor. */
#define FS_W_SYS (FS_OPEN_MAX + 1u)
#define FS_W_MAX (FS_OPEN_MAX + 2u)

static uint8_t fs_owner;
static bool fs_mail[FS_W_MAX];
static uint32_t fs_mail_st[FS_W_MAX];

/* Two halves, because the task loop does not wait: a silent host holds
 * the bridge for about 0.9 seconds and spinning that out stops every
 * other task. Start it, and poll once per pass. */
static void fs_start(uint32_t who, uint32_t op)
{
    fs_owner = (uint8_t)who;
    fs_mail[who] = false;
    FILE_CTL = op;
}

/* Reads the fabric at most once and files what it finds under whoever
 * asked for it. */
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

/* Free to take, or already ours.
 *
 * A worker that finds the record free re-issues even when its own
 * answer is sitting in the mail. The mail is there so a blocking spin
 * cannot destroy an answer, not so a later pass can pick one up: every
 * caller here recomputes what to ask for on the way in, and fs_rebind
 * can move the file's length and position between the command going
 * out and the answer being read. Taking that answer for this pass's
 * question records a window under an offset it was never filled at,
 * and the stream comes back a byte short. fs_start drops the stale
 * one. */
static bool fs_may(uint32_t who)
{
    return fs_owner == FS_W_NONE || fs_owner == who;
}

/* For the blocking form: a command cannot go out over one that has not
 * retired, so wait for it -- keeping its answer for its own worker. */
static void fs_wait_free(void)
{
    while (fs_owner != FS_W_NONE)
        fs_reap();
}

/* Counted rather than printed at the point of failure: a stream that
 * goes wrong goes wrong every frame, and a console full of it would be
 * the second thing the user could not read. Reported once. */
static uint16_t fs_n_tmo, fs_n_err, fs_n_defer;
static uint32_t fs_last_st;

void fs_log(void)
{
    /* Deferrals are the restore's own one-pass stall and not a
     * failure, so they are reported beside the errors but never on
     * their own -- a clean restore says nothing. */
    if (!fs_n_tmo && !fs_n_err)
    {
        fs_n_defer = 0;
        return;
    }
    printf("fs: tmo=%u err=%u defer=%u last=%02x\n", (unsigned)fs_n_tmo,
           (unsigned)fs_n_err, (unsigned)fs_n_defer,
           (unsigned)(fs_last_st & 0xFFu));
    fs_n_tmo = fs_n_err = fs_n_defer = 0;
}

/* A restore has landed and the fixups have not finished. Between those
 * two moments this driver's word is worth nothing: the descriptor came
 * out of the blob, the slot it names belongs to whatever the wake booted
 * into, and pocket_file was reconfigured under a command the sleeping
 * session issued. The main loop makes the window unavoidable -- api_task
 * runs before sst_task, so a syscall carried across the sleep is
 * re-dispatched a whole pass before fs_restore rebinds anything -- so
 * the guard belongs here, at the driver, where it holds whatever order
 * the tasks run in.
 *
 * Answering STD_PENDING is lossless: pos is not advanced, fs_restore
 * frees the record, and the next pass re-issues the same operation
 * against a slot that is its own again. The stall is one pass, because
 * sst_task clears the bit at the end of it. */
static bool fs_adrift(void)
{
    if (!(SST_CTL & SST_RESTORED))
        return false;
    fs_n_defer++;
    return true;
}

/* A stream that fails, fails every frame. The first few say what
 * happened and the rest are counted, because a console nobody can read
 * is how this started. */
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

/* The blocking form, for open and boot-time staging: once per file, and
 * the 6502 is parked in its syscall either way. */
static uint32_t fs_command(uint32_t who, uint32_t op)
{
    uint32_t st;
    fs_wait_free();
    fs_start(who, op);
    while (!fs_poll(who, &st))
        ;
    return st;
}

/* The write worker's other command: the resize-open that makes room
 * before the WRITE that fills it. */
static bool fs_grow;

/* std_stop drains only descriptors it will flush, so a read-only one
 * arrives still in flight. Left there, the next command stacks a toggle
 * on top of it. */
void fs_stop(void)
{
    /* Unconditional, because the two flags below say what the session
     * that wrote them was doing and the command in the fabric may
     * belong to another one: a restore brings back a machine that
     * thought it was idle over a pocket_file that is mid-command, and
     * skipping the drain there stacks the next command's toggle onto a
     * live one, where the bridge drops it outright and answers the
     * previous command instead. Idle costs one read. */
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

/* Words, not bytes. The path rides the byte stream and arrives intact,
 * but an integer field is taken as the bridge word stands: written low
 * byte first, flags of 3 arrive as 0x03000000. */
static void fs_win_u32(uint32_t off, uint32_t v)
{
    FILE_WIN[off >> 2] = v;
}

/* The table is id/size pairs with no defined layout, so a slot's size is
 * looked up by its id. */
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

/* The shape of the reply is undocumented. This reads a NUL-terminated
 * name at offset 0, where Open File's parameter struct carries one. */
bool fs_getfile(uint32_t slot, char *out, size_t cap)
{
    /* Cleared first: a refusal must not leave the caller holding the
     * previous ask's answer. proc_restage keeps its name in a static,
     * and a Get File that failed would otherwise stage the program
     * before it. */
    if (cap)
        out[0] = 0;
    FILE_ID = slot;
    FILE_BRIDGE = GETFILE_BRIDGE;
    uint32_t st = fs_command(FS_W_SYS, FILE_OP_GETFILE);
    if (st & (FILE_ST_ERR | FILE_ST_TIMEOUT))
        return false;

    /* The window is the answer, and it says on its own whether there is
     * one. Get File has no result code for a slot that is defined but
     * bound to nothing, so this used to ask the fabric whether the host
     * had written anything -- on the belief that a host with nothing to
     * say writes nothing, leaving the previous ask's name in place.
     *
     * The device says otherwise. It writes the whole 256-byte struct
     * every time, NUL at offset 0 when the slot is bound to nothing:
     * measured on hardware, nine unbound slots in a row answering
     * win[0]=(empty) while the bound one answered with its full path.
     * The host blanks the window itself, so there is nothing to tell
     * apart and nothing to watch for.
     *
     * Watching for it was worse than unnecessary. The flag is armed a
     * state after the request goes out, and a host that answers inside
     * that window is not counted -- so the same call that returned the
     * right name in the window reported wrote=0 nine times out of ten,
     * and every one of those names was thrown away here. That is what
     * made a wake restage the ROM it was already running: the compare
     * in fs_restore could never match a name it was never given. */
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
#define FS_SAVES_LEN (sizeof FS_SAVES_PATH - 1)
#define FS_RC_MALFORMED 4u

/* Not a host answer: the command is on its way and the caller must come
 * back for it. */
#define FS_RC_STARTED 0xFFu

/* Answers FS_RC_MALFORMED without starting a command when the name will
 * not fit. */
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

/* A bridge that stopped answering reads as FS_RC_MALFORMED, which every
 * caller already treats as a refusal. */
static bool fs_try_open_poll(uint32_t who, uint32_t *rc)
{
    uint32_t st;
    if (!fs_poll(who, &st))
        return false;
    *rc = (st & FILE_ST_TIMEOUT) ? FS_RC_MALFORMED : ((st & FILE_ST_ERR) >> 1);
    return true;
}

/* The blocking form, for open and for exec's staging. */
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

/* Only the prefix is stripped; the slash after it, or its absence, is
 * what decides where the name lands. A drive that is not 0 is refused,
 * not aliased. */
const char *fs_strip_drive(const char *path)
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
void fs_restore(void)
{
    fs_stop();
    for (int d = 0; d < FS_OPEN_MAX; d++)
    {
        if (!fs_pool[d].used)
            continue;
        fs_pool[d].stale = true;
        /* The window is the board's and the blob does not carry it, so
         * whatever it held belongs to the session that went away. */
        fs_pool[d].cache_len = 0;
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
    /* The length is the host's to say for a file this side did not
     * write: the card outlived the session and the file may not be
     * the size the blob remembers. A writable descriptor keeps its
     * own, because it is what resized the file and the host's table
     * is only as fresh as the last open. */
    if (got && !fs_pool[d].writable)
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

/* The bridge retires the command before pocket_file's deadline, so a host
 * that never picked it up arrives as result 7, not as our own timeout. */
#define FS_RC_NO_HOST 7u

static bool fs_unanswered(uint32_t st)
{
    return (st & FILE_ST_TIMEOUT)
           || ((st & FILE_ST_ERR) >> 1) == FS_RC_NO_HOST;
}

/* Flush, 0x0188, is documented but absent from core_bridge_cmd.v. Asking
 * costs one deadline, so the first ask decides and is remembered. */
static enum { FS_FLUSH_UNTRIED, FS_FLUSH_WORKS, FS_FLUSH_NEVER }
    fs_flush_state;

/* Chunked because the bridge's ~0.9 s deadline times the whole slot
 * operation, and the host writes at worst ~3.4 MB/s. */
#define FS_STAGE_CHUNK 0x80000u

/* The single ROM descriptor's record, the same shape a pool entry keeps and
 * as restorable: a wake's blob brings these statics back, which is what lets
 * the restore treat the ROM like any file that was open when the machine
 * slept. ROM_IMG holds the bytes, so reads never go near the bridge. */
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

/* Pull the bound slot-0 file into the staging store, chunked because the
 * bridge's ~0.9 s deadline times the whole slot operation. */
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

/* fs_std_open's twin over slot 0: bind, record, hand back the one descriptor
 * the 6502 can never be given. The extra work is the pull into ROM_IMG --
 * except when the host already did it, which is what a boot is: slot 0 bound
 * and written before anything ran. The still-bound check makes that open a
 * no-op rebind, and any binding this side changes marks the store fresh
 * itself, so the skip never serves stale bytes. */
int fs_rom_open(const char *path, uint8_t flags, api_errno *err)
{
    if (flags != FS_RD)
    {
        *err = (flags == (FS_WR | FS_CREAT | FS_EXCL)) ? API_EACCES : API_EINVAL;
        return -1;
    }
    if (path[0] == ':')
    {
        *err = API_ENOENT; /* no null drive: nothing installs here */
        return -1;
    }
    /* Never under a running 6502. The store is what ROM: assets are read out
     * of, and rewriting it beneath a program is a file changing under an
     * open descriptor. Every caller stops the machine first; this is that
     * rule made enforceable rather than remembered. */
    if (CPU_RESB & 1)
    {
        printf("rom: stage refused, 6502 running\n");
        *err = API_EBUSY;
        return -1;
    }
    const char *p = fs_strip_drive(path);
    if (!p || !*p)
    {
        *err = API_EINVAL;
        return -1;
    }
    assert(!fs_rom.used); /* the caller closes before opening again */
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

/* The boot's open, which was the host's: slot 0 arrives bound and written
 * before anything runs, and a Get File this early burns a bridge deadline
 * against the staging still in flight. So the boot adopts the descriptor
 * over what is there and asks only its length; the name comes later, from
 * proc_restage, once the host is free to answer. */
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
    *err = API_ENOENT; /* no null drive */
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
    if (!path)
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

    /* Creating takes both bits: create alone makes nothing, and both bits
     * against a file that exists would cut it to nothing. So probe first. */
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
    /* The probe above found the file, so a failure here is the host's and
     * final; the retry below has nothing to offer. */
    bool empty = !exists || (flags & FS_TRUNC);
    if (empty && exists && !fs_open_slot(slot, path, FS_DS_RESIZE, 0))
    {
        *err = API_EIO;
        return -1;
    }
    /* A create into a missing folder returns a descriptor having written
     * nothing, and the result cannot tell the two apart. Ask again plainly. */
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
    fs_pool[d].writable = (flags & FS_WR) != 0;
    /* A fresh binding, so nothing is owed and the window holds nothing
     * of this file. */
    fs_pool[d].stale = false;
    fs_pool[d].cache_at = 0;
    fs_pool[d].cache_len = 0;
    fs_pool[d].len = empty ? 0 : len;
    fs_pool[d].pos = (flags & FS_APPEND) ? fs_pool[d].len : 0;
    memcpy(fs_pool[d].name, path, strlen(path) + 1);
    return d;
}

/* Hands a descriptor back without a word to the host. For a caller that
 * knows the binding is worthless -- the log across a restore, whose
 * position came out of the blob and points into the middle of the file
 * it is trying to record. Flushing that would commit the wrong offset,
 * and there is nothing to flush that reopening will not redo. */
void fs_release(int desc)
{
    if (fs_desc(desc) < 0)
        return;
    fs_pool[desc].used = false;
    fs_pool[desc].cache_len = 0;
}

/* A close flushes: there is no close command, so this is the only thing
 * that puts a write on the card. It blocks, unlike sync, because
 * std_stop discards what close returns and would drop a STD_PENDING. */
std_rw_result fs_std_close(int desc, api_errno *err)
{
    (void)err;
    if (desc == FS_DESC_ROM && fs_rom.used)
    {
        /* The store keeps the bytes, but only for the next pull to
         * overwrite: a ROM: read after this answers EBADF until
         * fs_rom_open or fs_rom_adopt hands the descriptor out again. */
        fs_rom.used = false;
        fs_rom.pos = 0;
        return STD_OK;
    }
    if (fs_desc(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    /* Nothing below this needs the host, so it is answered while a
     * restore is still in flight: a descriptor with nothing to flush is
     * released from this side alone. Taken before the guard because a
     * close that costs no round trip should not wait for one. */
    if (!fs_pool[desc].writable || fs_flush_state == FS_FLUSH_NEVER)
    {
        fs_pool[desc].used = false;
        fs_pool[desc].cache_len = 0;
        return STD_OK;
    }
    /* Everything past here talks to the host, and a close carried
     * across a sleep arrives before the fixups have run: api_task is a
     * whole pass ahead of sst_task, so the syscall the sleeping session
     * left outstanding is re-dispatched against a bridge still holding
     * the dead session's command and a slot the wake rebound. fs_rebind
     * is no help there -- stale comes out of the blob as false, so it
     * does nothing -- and the flush below then stacks a toggle onto a
     * live command, which the bridge drops. The answer was EIO, and it
     * printed nothing.
     *
     * Deferring is lossless: used stays set, nothing is released, and
     * the next pass re-issues the same close against a drained bridge.
     * The one loss is a break landing inside std_stop's own teardown,
     * which discards what close returns -- the slot leaks for the
     * session there rather than flushing into a bridge that cannot hear
     * it, which is the better of the two. */
    if (fs_adrift())
        return STD_PENDING;
    fs_rebind(desc);
    std_rw_result res = STD_OK;
    {
        /* A read or write left in flight has to land before the flush
         * can start; the descriptor is going away either way. */
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
    /* Released even when the flush failed, the way close always does. */
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
        /* Out of the staging store, never the bridge, so a read here works
         * even mid-restore -- which is what lets ROM: assets and the loader
         * share this one descriptor with the files. */
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
        return STD_OK; /* short or zero at the end, which is EOF */

    /* Already here, so nothing goes to the card at all. */
    uint32_t at = fs_pool[desc].cache_at, have = fs_pool[desc].cache_len;
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
                printf("fs: read %u off=%u len=%u st=%02x\n", (unsigned)desc,
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
    /* Again after the copy, not only before it: the window the bytes
     * came out of is the board's and no blob carries it, so a freeze
     * that landed anywhere in that loop lifted the tail of this buffer
     * out of a store the wake rebuilt. Asked once more here, the whole
     * read is re-taken instead of half-committed. */
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
    if (!fs_pool[desc].writable)
    {
        *err = API_EACCES;
        return STD_ERROR;
    }
    /* Writes do not cache, and they make whatever the window holds a
     * different file. */
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
                *err = API_ENOSPC;
                return STD_ERROR;
            }
            fs_grow = true;
        }
        if (!fs_try_open_poll(FS_W(desc), &rc))
            return STD_PENDING;
        fs_grow = false;
        if (rc > 1)
        {
            *err = API_ENOSPC;
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
            printf("fs: write %u off=%u len=%u st=%02x\n", (unsigned)desc,
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
    /* fs_flush_state is sticky, so a verdict reached against a
     * reconfigured bridge would condemn every flush for the rest of
     * the session. */
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
    if (from < 0 || from + off < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    fs_pool[desc].pos = (uint32_t)(from + off);
    *pos = from + off;
    return 0;
}
