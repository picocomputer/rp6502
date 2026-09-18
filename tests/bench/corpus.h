/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
