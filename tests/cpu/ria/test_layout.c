/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The keyboard layout database against def/keyboard_*.def.
 *
 * The def files are what a contributor edits and what keyboard.c used to
 * include directly, as X macros expanding to flash tables. They now go
 * through a Python generator instead, which is a second reader of a
 * format only a preprocessor understood — so this file includes the
 * same manifest the same way and compares every code point, every caps
 * lock flag and every dead key against what layout.c reads back.
 *
 * A layout that types the wrong character is not something a machine
 * notices, and on a Pocket the tables are an asset that a build can
 * quietly leave stale. This is what says otherwise.
 */

#include "core/hid/layout.h"
#include "utest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The manifest builds its tables here the way keyboard.c once did. */
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

/* Sized to the longest layout rather than counted per layout, and
 * terminated the way the tables keyboard.c walked were. */
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

/* Every mismatch is reported rather than the first, because one wrong column
 * in a generator is a hundred and twenty-seven of them and the pattern is the
 * diagnosis. Capped, for the same reason. */
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
    ASSERT_TRUE(layout_init());
    ASSERT_EQ(layout_count(), REF_COUNT);

    for (int lay = 0; lay < REF_COUNT; lay++)
    {
        char name[LAYOUT_NAME_MAX];
        char desc[LAYOUT_DESC_MAX];
        layout_name(lay, name);
        layout_description(lay, desc);
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

/* A layout that is not there reads empty rather than reading something else:
 * this is what a machine whose asset failed to load does with every key. */
UTEST(layout, a_layout_out_of_range_reads_empty)
{
    ASSERT_TRUE(layout_init());

    char name[LAYOUT_NAME_MAX];
    layout_name(REF_COUNT, name);
    ASSERT_EQ(name[0], 0);
    ASSERT_EQ(layout_code_point(REF_COUNT, 0x04, 0), 0);
    ASSERT_EQ(layout_code_point(-1, 0x04, 0), 0);
}

UTEST_MAIN();
