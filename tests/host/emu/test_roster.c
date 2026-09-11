/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which way each walk goes.
 *
 * A machine is a list of rows and core/sys/sys.c is what walks it: init, run
 * and the two task columns forward, stop and break backward. Every one of
 * those directions is load-bearing somewhere -- the Pocket's roster puts fs
 * before std so the reversal closes files before the driver that owns them,
 * and the RIA's puts com near the end so a break's newline lands after what
 * the other breaks printed -- and none of it was witnessed by anything. Turn
 * DRIVERS_REVERSE into DRIVERS_FORWARD and every other suite stays green.
 *
 * sys.c is compiled in rather than linked: sys_on_stop and sys_on_break are
 * static, because performing a fan-out is the loop's alone, and this is the
 * one caller that is not sys_commit. The roster beside this file is what it
 * finds for "drivers.h" -- the test links no machine, so no other roster is
 * on the path to shadow it.
 *
 * sst.c comes in the same way and for the same reason: its five expansions of
 * the ninth column are the only place the savestate walks exist, and they are
 * private to it. Two of the three rows carry a chunk, so what can be seen
 * here is what no real machine's suite can -- which row was handed which
 * bytes, in what order, and that a row with no chunk gets none.
 */

#include "core/sys/sys.c"
#include "core/sys/sst.c"

#include "utest.h"

#include <string.h>

UTEST_MAIN();

/* Each hook writes its row's letter and its column's, in the order called. */
static char walked[64];
static void note(const char *what)
{
    if (strlen(walked) + 2 < sizeof walked)
        strcat(walked, what);
}

/* clang-format off */
void a_init(void) { note("ai"); } void a_task(void) { note("at"); }
void a_io(void)   { note("ao"); } void a_run(void)  { note("ar"); }
void a_stop(void) { note("as"); } void a_break(void){ note("ab"); }
void b_init(void) { note("bi"); } void b_task(void) { note("bt"); }
void b_io(void)   { note("bo"); } void b_run(void)  { note("br"); }
void b_stop(void) { note("bs"); } void b_break(void){ note("bb"); }
void c_init(void) { note("ci"); } void c_task(void) { note("ct"); }
void c_io(void)   { note("co"); } void c_run(void)  { note("cr"); }
void c_stop(void) { note("cs"); } void c_break(void){ note("cb"); }
/* clang-format on */

/* The line this machine has not got. sys.c is the one file that lowers and
 * raises it, so the test answers for it and reads it back where it matters. */
static bool resb_down;
void resb_init(void) { resb_down = true; }
void resb_assert(void) { resb_down = true; }
void resb_release(void) { resb_down = false; }
bool resb_running(void) { return !resb_down; }
void resb_restore(bool down) { resb_down = down; }

/* The stdio table sst.c folds into the manifest, because a descriptor's
 * driver index means nothing except against this list. A machine of nothing
 * has no drives, and an empty table is still a shape to agree on. */
const std_driver_t *std_drivers(size_t *count)
{
    static const std_driver_t none[1];
    *count = 0;
    return none;
}

/* What the two rowed drivers hold: a byte apiece, and a note of every walk
 * that touched them. A save writes the byte and pads its slot out to prove
 * the walker zeroes the rest; a load takes it back. */
static uint8_t a_held, c_held;
static bool a_refuses, c_refuses;
static bool a_overruns;

void a_sst_save(sst_cursor_t *cur, unsigned flags)
{
    note("aS");
    sst_put_u8(cur, a_held);
    sst_put_u32(cur, flags);
    if (a_overruns)
        sst_put_u32(cur, 0); /* two bytes past a six-byte slot */
}

bool a_sst_load(sst_cursor_t *cur, unsigned flags)
{
    note("aL");
    uint8_t got = sst_get_u8(cur);
    uint32_t saw = sst_get_u32(cur);
    if (!sst_ok(cur) || a_refuses)
        return false;
    a_held = got;
    (void)saw, (void)flags;
    return true;
}

void c_sst_save(sst_cursor_t *cur, unsigned flags)
{
    (void)flags;
    note("cS");
    sst_put_u8(cur, c_held);
}

bool c_sst_load(sst_cursor_t *cur, unsigned flags)
{
    (void)flags;
    note("cL");
    uint8_t got = sst_get_u8(cur);
    if (!sst_ok(cur) || c_refuses)
        return false;
    c_held = got;
    return true;
}

/* Each case establishes the state it needs, so they hold in any order. */
static void from_stopped(void)
{
    sys_stop();
    sys_commit();
    walked[0] = '\0';
}

UTEST(roster, init_walks_the_list_forward)
{
    walked[0] = '\0';
    sys_init();
    ASSERT_STREQ("aibici", walked);
}

UTEST(roster, a_run_walks_forward)
{
    from_stopped();
    sys_run();
    sys_commit();
    ASSERT_STREQ("arbrcr", walked);
}

/* Backward, so a row is put away before whatever came up before it. */
UTEST(roster, a_stop_walks_the_run_in_reverse)
{
    from_stopped();
    sys_run();
    sys_commit();
    walked[0] = '\0';
    sys_stop();
    sys_commit();
    ASSERT_STREQ("csbsas", walked);
}

/* The performed-on-the-spot stop takes the same direction as the deferred one;
 * proc_boot reaches it with a program's RAM about to be written over. */
UTEST(roster, stop_now_walks_the_same_way)
{
    from_stopped();
    sys_run();
    sys_commit();
    walked[0] = '\0';
    sys_stop_now();
    ASSERT_STREQ("csbsas", walked);
    ASSERT_FALSE(sys_active());
}

/* A break is a teardown too, and follows the stop it implies. */
UTEST(roster, a_break_walks_in_reverse_after_the_stop)
{
    from_stopped();
    sys_run();
    sys_commit();
    walked[0] = '\0';
    sys_break_request();
    sys_commit();
    ASSERT_STREQ("csbsascbbbab", walked);
}

/* Two columns and two walks. A machine whose file operations block re-enters
 * sys_task while a transfer completes, so a row whose work may touch a
 * filesystem is in the other one; putting it in the wrong column is invisible
 * on a machine that calls the two back to back. */
UTEST(roster, the_task_columns_are_two_walks)
{
    from_stopped();
    sys_task();
    ASSERT_STREQ("atbtct", walked);
    walked[0] = '\0';
    sys_io_task();
    ASSERT_STREQ("aoboco", walked);
}

/* RESB is sys.c's own, and no row could hold it: down before the walk, down
 * again in every ask, up only once the run fan-out has finished. */
UTEST(roster, the_reset_line_outlives_the_run_walk)
{
    from_stopped();
    ASSERT_FALSE(resb_running());
    sys_run();
    ASSERT_FALSE(resb_running()); /* the ask is not the doing */
    sys_commit();
    ASSERT_TRUE(resb_running());
    sys_stop();
    ASSERT_FALSE(resb_running()); /* inside the ask, ahead of the walk */
}

/* ---- the savestate walks ---- */

/* A machine standing still, so a case can save it and put it back. */
static void a_committed_machine(void)
{
    sys_run();
    sys_commit();
    a_refuses = c_refuses = false;
    a_overruns = false;
    walked[0] = '\0';
}

static uint8_t blob[64];

UTEST(roster, both_savestate_walks_go_forward)
{
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_STREQ("aScS", walked); /* B has no chunk and is not asked */

    walked[0] = '\0';
    ASSERT_EQ(sst_load(blob, sizeof blob, 0, NULL), (const char *)NULL);
    /* The load takes the scratch first, which is a save walk of its own. */
    ASSERT_STREQ("aScSaLcL", walked);
}

/* The size is the roster's, summed at compile time: a header, two chunks with
 * their own headers and slots, and a trailer. B contributes nothing, which is
 * the only way that claim can be made -- a real machine has no row that is
 * both present and absent. */
UTEST(roster, a_row_with_no_chunk_takes_no_bytes)
{
    ASSERT_EQ(sst_size(), (size_t)(18 + (10 + A_SST_SIZE) + (10 + C_SST_SIZE) + 8));
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_EQ(memcmp(blob + 18, "AAAA", 4), 0);
    ASSERT_EQ(memcmp(blob + 18 + 10 + A_SST_SIZE, "CCCC", 4), 0);
}

/* Each row is handed its own slot and no more. A row that wrote past its end
 * would otherwise shift every row after it, and the load would read C's byte
 * out of A's padding with nothing to say anything was wrong. */
UTEST(roster, a_row_cannot_write_past_its_slot)
{
    a_committed_machine();
    a_overruns = true;
    ASSERT_NE(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    a_overruns = false;
}

/* The tail of a slot is zeroed rather than left. Rewind deltas one state
 * against the next word by word, so a tail carrying whatever the frontend's
 * buffer held would make a still row move every frame. */
UTEST(roster, the_unused_tail_of_a_slot_is_zero)
{
    a_committed_machine();
    a_held = 0xA5;
    memset(blob, 0xEE, sizeof blob);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    /* A writes one byte plus the flags word, so one byte of its six is spare. */
    ASSERT_EQ(blob[18 + 10 + 5], 0);
    ASSERT_EQ(blob[18 + 10 + A_SST_SIZE + 10 + 1], 0); /* and two of C's three */
    ASSERT_EQ(blob[18 + 10 + A_SST_SIZE + 10 + 2], 0);
}

/* A row that refuses takes the whole load with it, and the machine goes back
 * to what it was rather than being left half rewritten. C refuses, which is
 * the row after A, so A has already been overwritten when it happens. */
UTEST(roster, a_row_that_refuses_puts_the_machine_back)
{
    a_committed_machine();
    a_held = 0x11;
    c_held = 0x22;
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);

    a_held = 0x33;
    c_held = 0x44;
    c_refuses = true;
    ASSERT_NE(sst_load(blob, sizeof blob, 0, NULL), (const char *)NULL);
    ASSERT_EQ((int)a_held, 0x33); /* the scratch, not the blob */
    ASSERT_EQ((int)c_held, 0x44);
    c_refuses = false;
}

/* The flags the walk was given reach every row unchanged, which is how a row
 * knows to leave a host path off the wire. A writes them down, so the blob
 * says what it was told. */
UTEST(roster, every_row_is_handed_the_same_flags)
{
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, SST_SHARED), (const char *)NULL);
    const uint8_t *slot = blob + 18 + 10;
    uint32_t saw = ((uint32_t)slot[1] << 24) | ((uint32_t)slot[2] << 16) |
                   ((uint32_t)slot[3] << 8) | slot[4];
    ASSERT_EQ(saw, (uint32_t)SST_SHARED);
}

/* A trusted blob skips the scratch, because the caller made it and is loading
 * it straight back. One fewer save walk is the whole difference. */
UTEST(roster, a_trusted_load_takes_no_scratch)
{
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, SST_TRUSTED), (const char *)NULL);
    walked[0] = '\0';
    ASSERT_EQ(sst_load(blob, sizeof blob, SST_TRUSTED, NULL), (const char *)NULL);
    ASSERT_STREQ("aLcL", walked);
}
