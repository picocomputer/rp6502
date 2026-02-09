/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/vip.h"
#include "str/str.h"
#include <pico/rand.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_VIP)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define VIP_LIST /*                           */ \
    /* Patreon  */                               \
    X(0, "Shawn Hyam")                           \
    X(1, "Romain Fontaine")                      \
    X(2, "bdash")                                \
    X(3, "Vitali Filinkou")                      \
    X(4, "Andy Herron")                          \
    X(5, "Sean Franklin")                        \
    X(6, "ulften")                               \
    X(7, "Larryvc")                              \
    X(8, "ingmar meins")                         \
    X(9, "Alexander Sharikhin")                  \
    X(10, "Tom Smith")                           \
    X(11, "michael sarr")                        \
    X(12, "Kai Wells")                           \
    X(13, "Andy Petrie")                         \
    X(14, "Paul Gorlinsky")                      \
    X(15, "Christian Lott")                      \
    X(16, "Everett Rubel")                       \
    X(17, "Cole Rise")                           \
    X(18, "Randy Gardner")                       \
    X(19, "Etienne Moreau")                      \
    X(20, "EJ012345")                            \
    X(21, "Ronald Lens")                         \
    X(22, "Geoff Waldron")                       \
    X(23, "Snake")                               \
    X(24, "Kirk Davis")                          \
    X(25, "Tomasz Sterna")                       \
    X(26, "Brian E-RAD Simmons")                 \
    X(27, "Robert Brown")                        \
    X(28, "Andrew C. Young")                     \
    X(29, "Jack Chidley")                        \
    X(30, "tonyvr")                              \
    X(31, "Jos Vermoesen")                       \
    X(32, "James Temple")                        \
    X(33, "Wojciech Gwiozdzik")                  \
    X(34, "Volodymyr Vialyi")                    \
    X(35, "markbo")                              \
    X(36, "James Will")                          \
    X(37, "David Raulo")                         \
    X(38, "Sodiumlightbaby")                     \
    X(39, "Paul S. Jenkins")                     \
    X(40, "Muhammad A")                          \
    X(41, "Ville Kivivuori")                     \
    X(42, "Kamil Devel")                         \
    X(43, "Jason Howard")                        \
    X(44, "Bart DeMeulmeester")                  \
    X(45, "Francis Cunningham")                  \
    /* YouTube  */                               \
    X(46, "AJ_Whitney")                          \
    /* Other  */                                 \
    X(47, "Jesse Warford")

#define X(suffix, name)                       \
    static const char __in_flash("vip_names") \
        VIP_NAME_##suffix[] = name;
VIP_LIST
#undef X

static uint32_t vip_rand_seed = 0;

int vip_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
        return state;
    (void)buf_size;
#define X(suffix, name) \
    VIP_NAME_##suffix,
    const char *vips[] = {VIP_LIST};
#undef X
    const unsigned VIP_COUNT = sizeof(vips) / sizeof(char *);
    while (!vip_rand_seed)
        vip_rand_seed = get_rand_32();
    uint32_t rng_state = vip_rand_seed;
    for (unsigned i = 0; i < VIP_COUNT; i++)
    {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        unsigned swap = (VIP_COUNT * (rng_state & 0xFFFF)) >> 16;
        const char *tmp = vips[i];
        vips[i] = vips[swap];
        vips[swap] = tmp;
    }
    int row_prefix_len = strlen(STR_HELP_ABOUT_VIP);
    int row = 0;
    if (state == row)
    {
        sprintf(buf, STR_HELP_ABOUT_VIP);
    }
    int col = row_prefix_len;
    for (unsigned i = 0; i < VIP_COUNT - 1; i++)
    {
        if (i)
        {
            if (state == row)
                buf[col] = ',';
            col += 1;
        }
        size_t len = strlen(vips[i]);
        if (col + len > 79 - 2)
        {
            if (state == row)
            {
                buf[col] = '\n';
                buf[++col] = 0;
            }
            row += 1;
            if (state == row)
            {
                for (int sp = 0; sp < row_prefix_len; sp++)
                    buf[sp] = ' ';
                sprintf(buf + row_prefix_len, "%s", vips[i]);
            }
            col = row_prefix_len + len;
        }
        else
        {
            if (col > row_prefix_len)
            {
                if (state == row)
                    buf[col] = ' ';
                col += 1;
            }
            if (state == row)
                sprintf(buf + col, "%s", vips[i]);
            col += len;
        }
    }
    if (state == row)
    {
        buf[col++] = '.';
        buf[col] = '\n';
        buf[++col] = 0;
        return -1;
    }
    return state + 1;
}
