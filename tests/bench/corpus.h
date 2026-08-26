/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What shape the generated ROMs are.
 *
 * The corpus is built, not committed, so it cannot be enumerated from a
 * suite; it states its own shape instead, one line of name, width and height
 * per ROM. Every suite that renders one asks here, because a canvas asserted
 * from the manifest rather than from a machine is the difference between
 * proving the geometry and watching a machine agree with itself.
 *
 * ROMS_DIR names the corpus directory; rp6502_test_corpus writes the manifest.
 */

#ifndef _BENCH_CORPUS_H_
#define _BENCH_CORPUS_H_

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static inline bool corpus_size(const char *name, int *width, int *height)
{
    FILE *f = fopen(ROMS_DIR "/manifest.txt", "r");
    if (!f)
        return false;
    char n[128];
    int w, h;
    bool found = false;
    while (fscanf(f, "%127s %d %d", n, &w, &h) == 3)
        if (!strcmp(n, name))
        {
            *width = w, *height = h, found = true;
            break;
        }
    fclose(f);
    return found;
}

#endif /* _BENCH_CORPUS_H_ */
