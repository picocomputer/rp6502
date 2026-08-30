/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See probe.h. The suite is a snapshot taken all at once and then read
 * out one line at a time, which is the whole trick.
 *
 * The console leaves this machine four bytes to a host command, through
 * a 128-byte FIFO that drops what it cannot hold. A block of readings
 * printed in one go is several hundred bytes and comes back interleaved
 * with itself -- measured, on the first hardware run, where the slot
 * lines arrived as "P0 ct=(noP0 snameP0 slot3name". So the readings are
 * taken together, at one instant, and then spelled out slowly enough for
 * the FIFO to drain between them. Spacing the lines spaces the Get File
 * calls with them, which keeps the bridge clear as well.
 *
 * Every line starts "P<phase> " and then a key, so two phases diff
 * mechanically and a missing line is as loud as a changed one.
 */

#include "probe.h"

#ifdef RP6502_POCKET_PROBE

#include "fs.h"
#include "mmio.h"
#include "proc.h"

#include "core/api/api.h"
#include "core/api/std.h"
#include "core/com.h"
#include "core/sys.h"
#include "core/rom/rom.h"

#include "host.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Slot 8, the last of the drive's eight, taken directly rather than
 * through fs_std_open.
 *
 * The console is the wrong place to keep these readings. It leaves four
 * bytes at a time through a host command, it is the same channel the
 * thing under test competes for, and Analogue's own log stops writing
 * across a sleep -- which is exactly where the answers are. A file on
 * the card survives all of that.
 *
 * Bound here with the bridge's own commands, not the drive's: fs.c is
 * what these readings are about, and a probe that goes through it
 * cannot say anything about it. Taking slot 8 outright also leaves the
 * pool alone, so a program's own descriptors are undisturbed.
 *
 * One file per phase. A wake is a new phase and a new file, so the
 * before and after sit side by side on the card instead of being two
 * halves of a log that ended when the core did. */
#define PROBE_SLOT 8u
#define PROBE_LOG_MAX 4096u

static char probe_log[PROBE_LOG_MAX];
static uint16_t probe_log_len;
static bool probe_log_full;

/* Between lines. The FIFO holds 128 bytes and a line is forty, so this
 * is far longer than it needs to be -- a phase still reads out in under
 * two seconds, and nothing here is in a hurry. */
#define PROBE_LINE_US 100000u

/* The phase this session is in. Zero is the boot the user started; every
 * restore and every re-announce opens the next one. Carried in the blob
 * like every other static, which is what makes a wake's phase number one
 * past the sleeping session's rather than back at zero. */
static uint8_t probe_phase;

/* Edge state for the two events that end a phase. */
static bool probe_was_restoring;
static uint8_t probe_upd_seen;
static uint32_t probe_slot_seen;

/* A button for a reading on demand. Bit 4 of the pad word is A; apf.c's
 * descriptor is where that number comes from. */
#define PROBE_KEY_A 0x0010u
static bool probe_key_was_down;

/* Whether this session has staged a ROM yet. main_stage is the event a
 * new ROM really is -- the menu's re-announce is cleared inside it,
 * before any watcher out here can sample it, which is why a swap used to
 * pass unnoticed and its readings landed under the previous phase. */
static bool probe_staged_once;

/* The phase whose file is open, and how much of it is already written.
 * A phase can read itself more than once -- the button does that -- and
 * each run must add to its file rather than replace it. The first run
 * used to truncate the boot's own readings away. */
static uint8_t probe_file_phase = 0xFF;
static uint32_t probe_file_len;

/* The snapshot, and where the readout has got to. Step 0 is idle. */
static const char *probe_why;
static uint8_t probe_step;
static uint64_t probe_next_us;
static uint64_t probe_us;
static uint32_t probe_ctl, probe_slot, probe_upd, probe_staged, probe_crc;
static uint32_t probe_crc_was;

#define PROBE_SLOT_FIRST 5              /* the step that prints slot 0 */
#define PROBE_SLOT_LAST (PROBE_SLOT_FIRST + 8)
#define PROBE_STEP_STALE (PROBE_SLOT_LAST + 1)
#define PROBE_STEP_AGAIN (PROBE_STEP_STALE + 1)
#define PROBE_STEP_FSGET (PROBE_STEP_AGAIN + 1)
/* One open-flag case per step, so each is paced like every other line
 * and its bridge commands are spread with it. */
#define PROBE_STEP_OPEN_FIRST (PROBE_STEP_FSGET + 1)
#define PROBE_STEP_OPEN_LAST (PROBE_STEP_OPEN_FIRST + 3)
#define PROBE_STEP_MARKS (PROBE_STEP_OPEN_LAST + 1)
#define PROBE_STEP_ASSET (PROBE_STEP_MARKS + 1)
#define PROBE_STEP_END (PROBE_STEP_ASSET + 1)

/* How many of this session's Get File commands came back with the
 * fabric's wrote bit set. The firmware refuses an answer without it, so
 * a session where this stays zero is a session where every name the host
 * gave was thrown away. */
static uint16_t probe_gf_calls, probe_gf_wrote;

/* CRC of the staging store: the only way to ask "is this the same image"
 * without a name. Bounded because the answer only has to be comparable,
 * and the window is read a byte at a time -- ROM_IMG is a view onto
 * SDRAM, not memory. */
#define PROBE_CRC_MAX 0x8000u

/* The console, and a copy for the card. printf-shaped so the call sites
 * read the same either way; a phase that overruns the buffer says so
 * rather than dropping the tail in silence. */
__printflike(1, 2) static void probe_say(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)sizeof line - 1;
    com_printf("%s", line);
    if (probe_log_len + (uint16_t)n >= PROBE_LOG_MAX)
    {
        probe_log_full = true;
        return;
    }
    memcpy(probe_log + probe_log_len, line, (size_t)n);
    probe_log_len = probe_log_len + (uint16_t)n;
}

/* The bridge's own open, spelled the way fs.c spells it: the path in the
 * window's first 256 bytes, then the flags word, then the size. Create
 * and resize together, because a create on its own makes nothing -- the
 * resize is what gives the file its length. */
#define PROBE_PARAM_FLAGS 256u
#define PROBE_PARAM_SIZE 260u
#define PROBE_DS_CREATE 1u
#define PROBE_DS_RESIZE 2u

static void probe_win_put(uint32_t off, const uint8_t *src, uint32_t len)
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

static uint32_t probe_command(uint32_t op)
{
    uint32_t st;
    FILE_CTL = op;
    do
        st = FILE_CTL;
    while (st & (FILE_ST_BUSY | FILE_ST_DRAIN));
    return st;
}

/* One Open File, with the parameter struct laid out the way the device
 * reads it: the path as a byte stream in the first 256, then two words.
 * Returns the host's own result code, or 0xFF if the bridge stopped. */
static uint32_t probe_open(const char *path, uint32_t flags, uint32_t size)
{
    uint8_t pad[256];
    size_t n = strlen(path);
    if (n >= sizeof pad)
        return 0xFFu;
    memcpy(pad, path, n);
    memset(pad + n, 0, sizeof pad - n);
    probe_win_put(0, pad, sizeof pad);
    FILE_WIN[PROBE_PARAM_FLAGS >> 2] = flags;
    FILE_WIN[PROBE_PARAM_SIZE >> 2] = size;
    FILE_ID = PROBE_SLOT;
    uint32_t st = probe_command(FILE_OP_OPEN);
    if (st & FILE_ST_TIMEOUT)
        return 0xFFu;
    return (st & FILE_ST_ERR) >> 1;
}

/* What each open flag actually does, asked rather than assumed.
 *
 * The tree carries three claims about this that nothing here has ever
 * measured: that the create bit alone makes no file, that resize is what
 * makes one, and that creating into a folder that is not there answers
 * with success and writes nothing. They may be true. They are also the
 * kind of thing that gets written down once and repeated, and one of
 * them -- that a flush poisons the drive for the session -- has already
 * been contradicted by this device answering 4391 of them and carrying
 * on.
 *
 * So: ask whether the file is there, do the thing, ask again. The
 * before and after are both printed, because a name that already exists
 * makes a different question of the same command, and only the pair
 * says which question was asked. Names carry the boot's wall clock so
 * every run starts with files that are not there yet.
 *
 * Result codes are the host's: 0 ok, 1 created, 2 slot undefined,
 * 3 not found, 4 malformed, 5 error, and 0xFF for a bridge that
 * stopped answering. */
static void probe_open_case(uint8_t which)
{
    static const struct
    {
        const char *tag;
        const char *dir;
        char kind;
        uint32_t flags;
    } cases[4] = {
        {"create-only", FS_SAVES_PATH, 'c', PROBE_DS_CREATE},
        {"resize-only", FS_SAVES_PATH, 'r', PROBE_DS_RESIZE},
        {"create+resize", FS_SAVES_PATH, 'b',
         PROBE_DS_CREATE | PROBE_DS_RESIZE},
        {"missing-folder", FS_SAVES_PATH "nodir/", 'd',
         PROBE_DS_CREATE | PROBE_DS_RESIZE},
    };
    char path[96];
    snprintf(path, sizeof path, "%st%u%c%08x.bin", cases[which].dir,
             (unsigned)probe_phase, cases[which].kind, (unsigned)RTC_EPOCH);

    uint32_t pre = probe_open(path, 0, 0);
    uint32_t op = probe_open(path, cases[which].flags, 64);
    uint32_t post = probe_open(path, 0, 0);
    uint32_t len = 0;
    bool got = post <= 1 && fs_slot_len(PROBE_SLOT, &len);
    probe_say("P%u open %-14s pre=%u op=%u post=%u len=%s%u\n",
              probe_phase, cases[which].tag, (unsigned)pre, (unsigned)op,
              (unsigned)post, got ? "" : "?", (unsigned)len);
}

static uint32_t probe_getfile_raw(uint32_t slot, char *out, size_t cap);
static void probe_begin(const char *why);

/* The ladder: one Get File at each known moment, kept until the phase
 * that follows prints them. Small, because the interesting part is the
 * first second and a boot only has one of those. */
#define PROBE_MARKS 8
static struct
{
    const char *when;
    uint32_t st;
    uint8_t len;
    uint32_t at_ms;
} probe_marks[PROBE_MARKS];
static uint8_t probe_mark_n;

/* Measured from the last main_stage, because that is the host-driven
 * moment the hypothesis is about. */
static uint64_t probe_ladder_from;
static uint8_t probe_ladder_step;
static const uint32_t probe_ladder_us[] = {1000u, 10000u, 100000u, 1000000u};
static const char *const probe_ladder_name[] = {"+1ms", "+10ms", "+100ms", "+1s"};

void probe_mark(const char *when)
{
    char name[48];
    uint64_t now = host_clock_us();
    uint32_t st = probe_getfile_raw(0, name, sizeof name);
    if (probe_mark_n < PROBE_MARKS)
    {
        probe_marks[probe_mark_n].when = when;
        probe_marks[probe_mark_n].st = st;
        probe_marks[probe_mark_n].len = (uint8_t)strlen(name);
        probe_marks[probe_mark_n].at_ms = (uint32_t)(now / 1000u);
        probe_mark_n++;
    }
    /* main_stage is the host's own moment; the ladder walks away from it,
     * and every one after the first opens a phase. */
    if (when[0] == 's')
    {
        probe_ladder_from = now;
        probe_ladder_step = 0;
        if (probe_staged_once)
        {
            probe_phase++;
            com_printf("\n*** ROM STAGED -- now phase %u ***\n", probe_phase);
            probe_begin("after rom load");
        }
        else
            probe_staged_once = true;
    }
}

/* The phase's readings onto the card, as their own file. */
static void probe_log_flush(void)
{
    if (!probe_log_len)
        return;
    if (probe_file_phase != probe_phase)
    {
        probe_file_phase = probe_phase;
        probe_file_len = 0;
    }
    char path[64];
    snprintf(path, sizeof path, "%sprobe%u.log", FS_SAVES_PATH,
             (unsigned)probe_phase);

    /* Open File answers 0 for a file that was there and 1 for one it
     * made, and both of those are this working. Reading any non-zero
     * code as a refusal cost a whole hardware run: the host created the
     * file, said so with a 1, and this gave up before writing a byte --
     * leaving a correctly sized file full of whatever the card had in
     * those sectors. fs.c has always known better; this did not. */
    uint32_t rc = probe_open(path, PROBE_DS_CREATE | PROBE_DS_RESIZE,
                             probe_file_len + probe_log_len);
    if (rc > 1)
    {
        com_printf("P%u log open failed rc=%u\n", probe_phase,
                   (unsigned)rc);
        probe_log_len = 0;
        return;
    }
    uint32_t st;

    /* One bridge window at a time, which is all a write carries. */
    for (uint32_t at = 0; at < probe_log_len; at += FILE_WIN_SIZE)
    {
        uint32_t part = probe_log_len - at;
        if (part > FILE_WIN_SIZE)
            part = FILE_WIN_SIZE;
        probe_win_put(0, (const uint8_t *)probe_log + at, part);
        FILE_ID = PROBE_SLOT;
        FILE_OFFSET = probe_file_len + at;
        FILE_BRIDGE = FILE_WIN_BASE;
        FILE_LENGTH = part;
        st = probe_command(FILE_OP_WRITE);
        if ((st & FILE_ST_TIMEOUT) || ((st & FILE_ST_ERR) >> 1) > 1)
        {
            com_printf("P%u log write failed at=%u st=%02x\n", probe_phase,
                       (unsigned)at, (unsigned)(st & 0xFFu));
            break;
        }
    }
    probe_file_len += probe_log_len;
    com_printf("P%u log %s %u bytes%s\n", probe_phase, path,
               (unsigned)probe_file_len, probe_log_full ? " (TRUNCATED)" : "");
    probe_log_len = 0;
    probe_log_full = false;
}

static uint32_t probe_stage_crc(uint32_t len)
{
    if (len > PROBE_CRC_MAX)
        len = PROBE_CRC_MAX;
    /* The polynomial mem_crc32 keeps, spelled here because that one takes
     * a plain pointer and this is a volatile window. */
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= ROM_IMG[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Take every fabric reading at one instant, then let probe_pump spell
 * them out. The Get File calls are not here: each one is a bridge round
 * trip, and they are worth spacing as much as the lines are. */
static void probe_begin(const char *why)
{
    if (probe_step)
        return; /* a readout in flight is not interrupted */
    probe_why = why;
    probe_us = host_clock_us();
    probe_ctl = SST_CTL;
    probe_slot = MMIO_SLOT;
    probe_upd = MMIO_UPD_N & 0xFFu;
    probe_staged = fs_rom_staged_len();
    probe_crc_was = probe_crc;
    probe_crc = probe_stage_crc(probe_staged);
    probe_step = 1;
    probe_next_us = 0; /* the first line goes out at once */
}

/* Get File on one slot, asked here rather than through fs_getfile.
 *
 * The first hardware run had the host answering with the right name --
 * the APF log says so, on every call -- while the firmware reported
 * nothing bound on nine of ten. Only one of those two can be right, and
 * fs_getfile folds four different refusals into one false, so this puts
 * the same command out and prints what actually came back: the status
 * byte, and the first bytes of the window the host was told to write.
 * Deliberately not a call into fs.c, which is the code under suspicion.
 *
 * The status bits are worth reading side by side with the APF log:
 * TIMEOUT says the bridge stopped, ERR carries the host's own result
 * code (1 = slot not defined), and WROTE is the fabric's own record of
 * whether any bridge write landed while the command was outstanding. */
static uint32_t probe_getfile_raw(uint32_t slot, char *out, size_t cap)
{
    FILE_ID = slot;
    FILE_BRIDGE = GETFILE_BRIDGE;
    FILE_CTL = FILE_OP_GETFILE;
    uint32_t st;
    do
        st = FILE_CTL;
    while (st & (FILE_ST_BUSY | FILE_ST_DRAIN));
    /* Straight out of the window, before any conversion can lose it. */
    size_t n = 0;
    while (n + 1 < cap && GETFILE_WIN[n])
        out[n] = (char)GETFILE_WIN[n], n++;
    out[n] = 0;
    probe_gf_calls++;
    if (st & FILE_ST_WROTE)
        probe_gf_wrote++;
    return st;
}

static void probe_say_slot(uint32_t slot)
{
    uint32_t len = 0;
    bool got_len = fs_slot_len(slot, &len);
    char raw[40];
    uint32_t st = probe_getfile_raw(slot, raw, sizeof raw);
    unsigned n = (unsigned)strlen(raw);

    probe_say("P%u slot%u len=%s%u st=%02x wrote=%u err=%u tmo=%u win[%u]=%s\n",
               probe_phase, slot, got_len ? "" : "?", len,
               (unsigned)(st & 0xFFu),
               (unsigned)((st & FILE_ST_WROTE) != 0),
               (unsigned)((st & FILE_ST_ERR) >> 1),
               (unsigned)((st & FILE_ST_TIMEOUT) != 0),
               n, n ? raw : "(empty)");
}

/* What to do next. The physical actions are the experiment -- nothing
 * here can sleep the Pocket or pick a ROM -- so a run is a conversation:
 * read the block, do the thing, read the next block. */
static void probe_say_prompt(void)
{
    switch (probe_phase)
    {
    case 0:
        probe_say("P0 NEXT: sleep the Pocket, then wake it.\n");
        break;
    case 1:
        probe_say("P1 NEXT: load the OTHER rom from the Core Settings menu.\n");
        break;
    case 2:
        probe_say("P2 NEXT: sleep, then wake. The music breaks here.\n");
        break;
    default:
        probe_say("P%u NEXT: sleep, swap, or press A to read again.\n",
                   probe_phase);
        break;
    }
}

/* One line per call, and only when the last one has had time to leave. */
static void probe_pump(void)
{
    if (!probe_step)
        return;
    uint64_t now = host_clock_us();
    if (now < probe_next_us)
        return;
    probe_next_us = now + PROBE_LINE_US;

    uint8_t step = probe_step;
    if (step >= PROBE_SLOT_FIRST && step <= PROBE_SLOT_LAST)
        probe_say_slot(step - PROBE_SLOT_FIRST);
    else if (step >= PROBE_STEP_OPEN_FIRST && step <= PROBE_STEP_OPEN_LAST)
        probe_open_case((uint8_t)(step - PROBE_STEP_OPEN_FIRST));
    else
        switch (step)
        {
        case 1:
            probe_say("P%u ---- %s t=%u:%u\n", probe_phase, probe_why,
                       (unsigned)(probe_us >> 32), (unsigned)probe_us);
            break;
        case 2:
            probe_say("P%u ctl=%02x seen=%u restored=%u err=%u under=%u\n",
                       probe_phase, (unsigned)(probe_ctl & 0xFFu),
                       (unsigned)((probe_ctl & SST_BLOB_SEEN) != 0),
                       (unsigned)((probe_ctl & SST_RESTORED) != 0),
                       (unsigned)((probe_ctl & SST_RESTORE_ERR) != 0),
                       (unsigned)((probe_ctl & SST_UNDERRUN) != 0));
            break;
        case 3:
            /* The store against the descriptor that is supposed to be
             * reading it. A crc that moved with no restage is the host
             * having refilled slot 0 underneath the session. */
            probe_say("P%u store crc=%08x prev=%08x%s staged=%u\n",
                       probe_phase, (unsigned)probe_crc,
                       (unsigned)probe_crc_was,
                       (probe_crc_was && probe_crc != probe_crc_was)
                           ? " CHANGED" : "",
                       (unsigned)probe_staged);
            break;
        case 4:
        {
            /* What the session thinks it is running, against what the
             * host says slot 0 is bound to. The two disagreeing is the
             * whole bug. */
            const char *want = proc_staged_path();
            probe_say("P%u announce slot=%u upd=%u want=%s\n", probe_phase,
                       (unsigned)probe_slot, (unsigned)probe_upd,
                       (want && *want) ? want : "(none)");
            break;
        }
        case PROBE_STEP_STALE:
        {
            /* The question the whole fix turns on: when a slot is bound
             * to nothing, does the host blank the response window, or
             * leave whatever the last ask put there?
             *
             * Slot 0 first, so the window is known to hold its path.
             * Then slot 1, which nothing is bound to. If slot 0's path
             * is still sitting there, the window is stale and its
             * contents cannot tell a bound slot from an empty one --
             * which is exactly what the fabric's wrote bit was added to
             * answer, and the reason a name cannot simply be trusted
             * because it is there. If it comes back empty, the host
             * blanked it and the window says so itself.
             *
             * Both readings look identical on a cold boot, because the
             * staging store starts zeroed. Asking in this order is what
             * separates them. */
            char a[40], b[40];
            probe_getfile_raw(0, a, sizeof a);
            probe_getfile_raw(1, b, sizeof b);
            probe_say("P%u stale bound=[%s] then-unbound=[%s] %s\n",
                       probe_phase, a, b,
                       b[0] ? "STALE-WINDOW" : "host-blanked");
            break;
        }
        case PROBE_STEP_AGAIN:
        {
            /* The same slot twice in a row. If the first ask of a
             * session is the one that works, this is where it shows. */
            char a[40], b[40];
            uint32_t s1 = probe_getfile_raw(0, a, sizeof a);
            uint32_t s2 = probe_getfile_raw(0, b, sizeof b);
            probe_say("P%u twice st=%02x/%02x wrote=%u/%u same=%u\n",
                       probe_phase, (unsigned)(s1 & 0xFFu),
                       (unsigned)(s2 & 0xFFu),
                       (unsigned)((s1 & FILE_ST_WROTE) != 0),
                       (unsigned)((s2 & FILE_ST_WROTE) != 0),
                       (unsigned)(strcmp(a, b) == 0));
            break;
        }
        case PROBE_STEP_FSGET:
        {
            /* The firmware's own call, beside the raw one, so the two
             * are compared in the same session rather than across runs.
             * This is the function proc_restage and every wake use. */
            char name[PROC_PATH_MAX];
            bool ok = fs_getfile(FS_SLOT_ROM, name, sizeof name);
            probe_say("P%u fs_getfile=%u [%s] wrote %u of %u asks\n",
                       probe_phase, (unsigned)ok, ok ? name : "",
                       (unsigned)probe_gf_wrote, (unsigned)probe_gf_calls);
            break;
        }
        case PROBE_STEP_MARKS:
            for (uint8_t i = 0; i < probe_mark_n; i++)
                probe_say("P%u when %-6s t=%ums wrote=%u name=%u\n",
                          probe_phase, probe_marks[i].when,
                          (unsigned)probe_marks[i].at_ms,
                          (unsigned)((probe_marks[i].st & FILE_ST_WROTE) != 0),
                          (unsigned)probe_marks[i].len);
            probe_mark_n = 0;
            break;
        case PROBE_STEP_ASSET:
        {
            /* The symptom, in the same file as the mechanism.
             *
             * The running ROM streams this asset and prints what it
             * gets, but that goes to the console -- which is a separate
             * file per boot and the one thing a wake does not carry.
             * The firmware can read the very same asset through the very
             * same descriptor, so the answer to "is the store serving
             * this program its own file" lands here beside the readings
             * that explain it. The letters are the variant's: an A here
             * under probe-b is the bug, spelled out. */
            char head[24];
            long n = rom_read_asset("song", head, sizeof head);
            if (n < 0)
                probe_say("P%u asset UNREADABLE\n", probe_phase);
            else
                probe_say("P%u asset [%s]\n", probe_phase, head);
            break;
        }
        case PROBE_STEP_END:
            probe_say("P%u ==== end\n", probe_phase);
            break;
        default:
            probe_say_prompt();
            /* The phase is closed here, which is what puts it on the
             * card under its own name. */
            probe_log_flush();
            probe_step = 0;
            return;
        }
    probe_step = step + 1;
}

void probe_init(void)
{
    /* One line, on the first instructions, because on a wake the engine
     * is about to halt this CPU and write the sleeping session's memory
     * over ours. What has already gone out the fabric console is the only
     * reading of this boot that survives that -- and blob_seen is the
     * question sst.c's boot check exists for and has only ever answered
     * no. */
    uint32_t ctl = SST_CTL;
    com_printf("PROBE boot ctl=%02x slot=%u upd=%u seen=%u\n",
               (unsigned)(ctl & 0xFFu), (unsigned)MMIO_SLOT,
               (unsigned)(MMIO_UPD_N & 0xFFu),
               (unsigned)((ctl & SST_BLOB_SEEN) != 0));
    probe_upd_seen = (uint8_t)MMIO_UPD_N;
    probe_slot_seen = MMIO_SLOT;
    probe_was_restoring = (ctl & SST_RESTORED) != 0;
    /* The first instructions, before main_stage has asked for anything. */
    probe_mark("init");
}

void probe_task(void)
{
    uint32_t ctl = SST_CTL;
    bool restoring = (ctl & SST_RESTORED) != 0;

    /* The falling edge, not the rising one: inside the window the drive
     * defers every operation, so a suite taken there would read nothing
     * but STD_PENDING. This opens on the pass after the ack, with the
     * session put back and the machine released. */
    if (probe_was_restoring && !restoring)
    {
        probe_phase++;
        /* Said the instant it happens, ahead of the readings and ahead
         * of any file: a wake that goes unremarked is indistinguishable
         * from a probe that never noticed one, and that ambiguity has
         * already cost a hardware run. */
        com_printf("\n*** WOKE -- now phase %u ***\n", probe_phase);
        probe_begin("after wake");
    }
    probe_was_restoring = restoring;

    /* A re-announce is the user picking a ROM from the menu. Both
     * readings move for a hot reload; taking both keeps it from opening
     * a phase twice. */
    uint8_t upd = (uint8_t)MMIO_UPD_N;
    uint32_t slot = MMIO_SLOT;
    if (upd != probe_upd_seen || (slot && slot != probe_slot_seen))
    {
        probe_upd_seen = upd;
        probe_slot_seen = slot;
        if (!restoring)
        {
            probe_phase++;
            com_printf("\n*** ROM RELOADED -- now phase %u ***\n",
                       probe_phase);
            probe_begin("after menu load");
        }
    }

    bool down = (MMIO_CONT_KEY(0) & PROBE_KEY_A) != 0;
    if (down && !probe_key_was_down && !restoring)
        probe_begin("on request");
    probe_key_was_down = down;

    /* The boot phase's own reading, once the drive is up enough to
     * answer. Deferred to here rather than taken in probe_init because
     * Get File at boot burns a bridge deadline against the staging still
     * in flight, which is a cost sst.c already paid once. */
    static bool probe_booted;
    if (!probe_booted && sys_active() && !restoring)
    {
        probe_booted = true;
        probe_begin("boot");
    }

    /* The ladder runs regardless of the readout, so its timing is the
     * host's distance from main_stage rather than the suite's pacing. */
    if (probe_ladder_step < 4 && probe_ladder_from
        && host_clock_us() >= probe_ladder_from
                                  + probe_ladder_us[probe_ladder_step])
    {
        uint8_t i = probe_ladder_step++;
        probe_mark(probe_ladder_name[i]);
    }

    if (!restoring)
        probe_pump();
}

#endif /* RP6502_POCKET_PROBE */
