/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * sys.c and sst.c expand the driver list when they are compiled, so they are
 * compiled here against roster/drivers.h. The test links no machine, so no
 * other drivers.h is on the include path.
 */

#include "core/sys/sys.c"
#include "core/sys/sst.c"

#include "utest.h"

#include <string.h>

UTEST_MAIN();

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

static bool resb_down;
void resb_init(void) { resb_down = true; }
void resb_assert(void) { resb_down = true; }
void resb_release(void) { resb_down = false; }

const std_driver_t *std_drivers(size_t *count)
{
    static const std_driver_t none[1];
    *count = 0;
    return none;
}

static uint8_t a_held, c_held;
static bool a_refuses, c_refuses;
static bool a_overruns;

void a_sst_save(sst_cursor_t *cur, unsigned flags)
{
    note("aS");
    sst_put_u8(cur, a_held);
    sst_put_u32(cur, flags);
    if (a_overruns)
        sst_put_u32(cur, 0);
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

UTEST(roster, the_task_columns_are_two_walks)
{
    from_stopped();
    sys_task();
    ASSERT_STREQ("atbtct", walked);
    walked[0] = '\0';
    sys_io_task();
    ASSERT_STREQ("aoboco", walked);
}

UTEST(roster, the_reset_line_outlives_the_run_walk)
{
    from_stopped();
    ASSERT_TRUE(resb_down);
    sys_run();
    ASSERT_TRUE(resb_down);
    sys_commit();
    ASSERT_FALSE(resb_down);
    sys_stop();
    ASSERT_TRUE(resb_down); /* sys_stop lowers RESB before any stop hook runs */
}

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
    ASSERT_STREQ("aScS", walked);

    walked[0] = '\0';
    ASSERT_EQ(sst_load(blob, sizeof blob, 0, NULL), (const char *)NULL);
    /* An untrusted load first saves the machine into a scratch copy, so every
     * row's save runs before its load. */
    ASSERT_STREQ("aScSaLcL", walked);
}

/* The blob is an 18-byte header, a 10-byte chunk header and a slot for each
 * of A and C, and an 8-byte trailer. */
UTEST(roster, a_row_with_no_chunk_takes_no_bytes)
{
    ASSERT_EQ(sst_size(), (size_t)(18 + (10 + A_SST_SIZE) + (10 + C_SST_SIZE) + 8));
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_EQ(memcmp(blob + 18, "AAAA", 4), 0);
    ASSERT_EQ(memcmp(blob + 18 + 10 + A_SST_SIZE, "CCCC", 4), 0);
}

UTEST(roster, a_row_cannot_write_past_its_slot)
{
    a_committed_machine();
    a_overruns = true;
    ASSERT_NE(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    a_overruns = false;
}

UTEST(roster, the_unused_tail_of_a_slot_is_zero)
{
    a_committed_machine();
    a_held = 0xA5;
    memset(blob, 0xEE, sizeof blob);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    /* A writes a byte and a four-byte flags word into its six-byte slot and C
     * writes a byte into its three, so A's last byte and C's last two are
     * spare. */
    ASSERT_EQ(blob[18 + 10 + 5], 0);
    ASSERT_EQ(blob[18 + 10 + A_SST_SIZE + 10 + 1], 0);
    ASSERT_EQ(blob[18 + 10 + A_SST_SIZE + 10 + 2], 0);
}

/* C's load fails and C comes after A, so A has already loaded the blob's
 * byte, and the rollback from the scratch copy has to put 0x33 back. */
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
    ASSERT_EQ((int)a_held, 0x33);
    ASSERT_EQ((int)c_held, 0x44);
    c_refuses = false;
}

UTEST(roster, every_row_is_handed_the_same_flags)
{
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, SST_SHARED), (const char *)NULL);
    const uint8_t *slot = blob + 18 + 10;
    uint32_t saw = ((uint32_t)slot[1] << 24) | ((uint32_t)slot[2] << 16) |
                   ((uint32_t)slot[3] << 8) | slot[4];
    ASSERT_EQ(saw, (uint32_t)SST_SHARED);
}

UTEST(roster, a_trusted_load_takes_no_scratch)
{
    a_committed_machine();
    ASSERT_EQ(sst_save(blob, sizeof blob, SST_TRUSTED), (const char *)NULL);
    walked[0] = '\0';
    ASSERT_EQ(sst_load(blob, sizeof blob, SST_TRUSTED, NULL), (const char *)NULL);
    ASSERT_STREQ("aLcL", walked);
}
