/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/layout.h"
#include "core/hid/keymap.h"
#include "core/str/str.h"
#include "utest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XDEAD_PICK(_1, _2, _3, _4, NAME, ...) NAME
#define XDEAD(...) XDEAD_PICK(__VA_ARGS__, XDEAD3, XDEAD2, , )(__VA_ARGS__)

#define XBEGIN(code, desc) {code, desc},
#define XKEY(kc, u, s, a, sa, caps)
#define XDEAD2(d, b, r)
#define XDEAD3(d1, d2, b, r)
#define XEND()
static const struct
{
    const char *name;
    const char *desc;
} ref_layouts[] = {
#include "core/def/keyboard.def"
};
#undef XBEGIN
#undef XKEY
#undef XDEAD2
#undef XDEAD3
#undef XEND

#define REF_COUNT ((int)(sizeof ref_layouts / sizeof ref_layouts[0]))

#define XBEGIN(code, desc) {
#define XKEY(kc, u, s, a, sa, caps) [kc] = {u, s, a, sa, caps},
#define XDEAD2(d, b, r)
#define XDEAD3(d1, d2, b, r)
#define XEND() \
    }          \
    ,
static const uint32_t ref_keys[REF_COUNT][128][5] = {
#include "core/def/keyboard.def"
};
#undef XBEGIN
#undef XKEY
#undef XDEAD2
#undef XDEAD3
#undef XEND

#define REF_DEAD_MAX 128

#define XBEGIN(code, desc) {
#define XKEY(kc, u, s, a, sa, caps)
#define XDEAD2(d, b, r) {d, b, r},
#define XDEAD3(d1, d2, b, r)
#define XEND() \
    }          \
    ,
static const uint32_t ref_dead2[REF_COUNT][REF_DEAD_MAX][3] = {
#include "core/def/keyboard.def"
};
#undef XBEGIN
#undef XKEY
#undef XDEAD2
#undef XDEAD3
#undef XEND

#define XBEGIN(code, desc) {
#define XKEY(kc, u, s, a, sa, caps)
#define XDEAD2(d, b, r)
#define XDEAD3(d1, d2, b, r) {d1, d2, b, r},
#define XEND() \
    }          \
    ,
static const uint32_t ref_dead3[REF_COUNT][REF_DEAD_MAX][4] = {
#include "core/def/keyboard.def"
};
#undef XBEGIN
#undef XKEY
#undef XDEAD2
#undef XDEAD3
#undef XEND

#define FAIL(...)                            \
    do                                       \
    {                                        \
        fprintf(stderr, "FAIL: " __VA_ARGS__); \
        *utest_result = 1;                   \
        if (++failures > 20)                 \
        {                                    \
            fprintf(stderr, "too many\n");   \
            return;                          \
        }                                    \
    } while (0)

static unsigned ref_dead2_count(int lay)
{
    unsigned n = 0;
    while (n < REF_DEAD_MAX && ref_dead2[lay][n][0])
        n++;
    return n;
}

static unsigned ref_dead3_count(int lay)
{
    unsigned n = 0;
    while (n < REF_DEAD_MAX && ref_dead3[lay][n][0])
        n++;
    return n;
}

UTEST(layout, every_layout_matches_its_def)
{
    int failures = 0;
    ASSERT_EQ(layout_count(), REF_COUNT);

    for (int lay = 0; lay < REF_COUNT; lay++)
    {
        const char *name = layout_name(lay);
        const char *desc = layout_description(lay);
        if (strcmp(name, ref_layouts[lay].name))
            FAIL("layout %d is %s, def says %s\n",
                 lay, name, ref_layouts[lay].name);
        if (strcmp(desc, ref_layouts[lay].desc))
            FAIL("%s description is %s, def says %s\n",
                 name, desc, ref_layouts[lay].desc);

        for (int kc = 0; kc < 128; kc++)
        {
            for (unsigned col = 0; col < 4; col++)
            {
                uint16_t got = layout_code_point(lay, kc, col);
                uint32_t want = ref_keys[lay][kc][col];
                if (got != want)
                    FAIL("%s keycode 0x%02X column %u is U+%04X, "
                         "def says U+%04X\n",
                         name, kc, col, got, want);
            }
            bool caps = layout_use_caps(lay, kc);
            bool want_caps = ref_keys[lay][kc][4] != 0;
            if (caps != want_caps)
                FAIL("%s keycode 0x%02X caps lock is %d, def says %d\n",
                     name, kc, caps, want_caps);
        }

        unsigned n2 = ref_dead2_count(lay);
        unsigned n3 = ref_dead3_count(lay);
        if (layout_dead2_count(lay) != n2)
            FAIL("%s has %u two-key dead entries, def has %u\n",
                 name, layout_dead2_count(lay), n2);
        if (layout_dead3_count(lay) != n3)
            FAIL("%s has %u three-key dead entries, def has %u\n",
                 name, layout_dead3_count(lay), n3);
        for (unsigned i = 0; i < n2; i++)
            for (unsigned j = 0; j < 3; j++)
                if (layout_dead2(lay, i, j) != ref_dead2[lay][i][j])
                    FAIL("%s dead2 %u field %u is U+%04X, "
                         "def says U+%04X\n",
                         name, i, j, layout_dead2(lay, i, j),
                         ref_dead2[lay][i][j]);
        for (unsigned i = 0; i < n3; i++)
            for (unsigned j = 0; j < 4; j++)
                if (layout_dead3(lay, i, j) != ref_dead3[lay][i][j])
                    FAIL("%s dead3 %u field %u is U+%04X, "
                         "def says U+%04X\n",
                         name, i, j, layout_dead3(lay, i, j),
                         ref_dead3[lay][i][j]);
    }

}

/* What layout.c's list reaches outside itself: the setting's storage, which
 * checks, stores and applies as core/sys/config.c does, the keymap, and the
 * two answers' formats. */
static char list_store[LAYOUT_LIST_SIZE];
static int used_layout;

const char STR_SET_KB_RESPONSE[] = "%s (%s)";
const char STR_SET_KB_LIST_RESPONSE[] = "%s";

const char *layout_get_list(void)
{
    return list_store;
}

bool layout_set_list(const char *v)
{
    char checked[LAYOUT_LIST_SIZE];
    if (!layout_check_list(v, checked))
        return false;
    strcpy(list_store, checked);
    layout_apply_list(list_store, true);
    return true;
}

void keymap_use_layout(int index)
{
    used_layout = index;
}

static int layout_named(const char *want)
{
    for (int i = 0; i < REF_COUNT; i++)
        if (!strcmp(ref_layouts[i].name, want))
            return i;
    return -1;
}

UTEST(layout, a_machine_with_no_list_adopts_us)
{
    list_store[0] = 0;
    used_layout = -1;
    layout_list_init();
    ASSERT_STREQ(layout_get_list(), "US");
    ASSERT_EQ(used_layout, layout_named("US"));
}

UTEST(layout, a_list_is_stored_in_the_databases_spelling)
{
    char out[LAYOUT_LIST_SIZE];
    ASSERT_TRUE(layout_check_list("  de   us-intl ", out));
    ASSERT_STREQ(out, "DE US-INTL");
    ASSERT_FALSE(layout_check_list("", out));
    ASSERT_FALSE(layout_check_list("US XX", out));
    ASSERT_FALSE(layout_check_list("US DE us", out));
    /* Every layout fits in no list, and USA is not US. */
    ASSERT_FALSE(layout_check_list("US DE DK FR PL SE US-INTL US DE DK FR PL", out));
    ASSERT_FALSE(layout_check_list("USA", out));
}

UTEST(layout, the_list_cycles_and_goes_around)
{
    ASSERT_TRUE(layout_set_list("US DE FR"));
    ASSERT_EQ(used_layout, layout_named("US"));
    layout_list_cycle();
    ASSERT_EQ(used_layout, layout_named("DE"));
    layout_list_cycle();
    ASSERT_EQ(used_layout, layout_named("FR"));
    layout_list_cycle();
    ASSERT_EQ(used_layout, layout_named("US"));

    /* A list of one has nowhere to go. */
    ASSERT_TRUE(layout_set_list("SE"));
    used_layout = -1;
    layout_list_cycle();
    ASSERT_EQ(used_layout, -1);
}

UTEST(layout, a_new_list_keeps_the_layout_in_use_when_it_can)
{
    ASSERT_TRUE(layout_set_list("US DE FR"));
    layout_list_cycle();
    ASSERT_TRUE(layout_set_list("FR DE"));
    ASSERT_EQ(used_layout, layout_named("DE"));
    layout_list_cycle();
    ASSERT_EQ(used_layout, layout_named("FR"));
    ASSERT_TRUE(layout_set_list("US SE"));
    ASSERT_EQ(used_layout, layout_named("US"));
    /* A refused list changes nothing. */
    ASSERT_FALSE(layout_set_list("US XX"));
    ASSERT_STREQ(layout_get_list(), "US SE");
}

UTEST(layout, the_answer_names_one_layout_or_lists_several)
{
    char buf[80];
    ASSERT_TRUE(layout_set_list("DE"));
    ASSERT_EQ(layout_list_response(buf, sizeof buf, 0, 80), -1);
    char want[80];
    snprintf(want, sizeof want, "DE (%s)", ref_layouts[layout_named("DE")].desc);
    ASSERT_STREQ(buf, want);
    ASSERT_TRUE(layout_set_list("DE US"));
    layout_list_response(buf, sizeof buf, 0, 80);
    ASSERT_STREQ(buf, "DE US");

    /* The help lists the database, a line per call. */
    int state = 0, lines = 0;
    while ((state = layout_help_response(buf, sizeof buf, state, 80)) >= 0)
        lines++;
    ASSERT_EQ(lines, REF_COUNT);
}

UTEST_MAIN();
