/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/layout.h"
#include "layout_db.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t layout_db_image[LAYOUT_DB_BYTES];
static int layout_db_loaded;

static void layout_bin_load(void)
{
    FILE *f = fopen(LAYOUT_DB_BIN_PATH, "rb");
    if (!f)
    {
        fprintf(stderr, "layout_bin: cannot open %s\n", LAYOUT_DB_BIN_PATH);
        exit(1);
    }
    size_t n = fread(layout_db_image, 1, sizeof layout_db_image, f);
    if (n != sizeof layout_db_image || fgetc(f) != EOF)
    {
        fprintf(stderr, "layout_bin: %s is %zu bytes, expected %zu\n",
                LAYOUT_DB_BIN_PATH, n, sizeof layout_db_image);
        exit(1);
    }
    fclose(f);
    layout_db_loaded = 1;
}

uint16_t layout_word(uint32_t index)
{
    if (!layout_db_loaded)
        layout_bin_load();
    if (index >= LAYOUT_DB_WORDS)
        return 0;
    uint32_t at = index * 2;
    return (uint16_t)layout_db_image[at] | ((uint16_t)layout_db_image[at + 1] << 8);
}
