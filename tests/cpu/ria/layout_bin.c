/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/layout.h"
#include "kbdlay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t kbdlay_image[KBDLAY_BYTES];
static int kbdlay_loaded;

static void layout_bin_load(void)
{
    FILE *f = fopen(KBDLAY_BIN_PATH, "rb");
    if (!f)
    {
        fprintf(stderr, "layout_bin: cannot open %s\n", KBDLAY_BIN_PATH);
        exit(1);
    }
    size_t n = fread(kbdlay_image, 1, sizeof kbdlay_image, f);
    if (n != sizeof kbdlay_image || fgetc(f) != EOF)
    {
        fprintf(stderr, "layout_bin: %s is %zu bytes, expected %zu\n",
                KBDLAY_BIN_PATH, n, sizeof kbdlay_image);
        exit(1);
    }
    fclose(f);
    kbdlay_loaded = 1;
}

uint16_t layout_word(uint32_t index)
{
    if (!kbdlay_loaded)
        layout_bin_load();
    if (index >= KBDLAY_WORDS)
        return 0;
    uint32_t at = index * 2;
    return (uint16_t)kbdlay_image[at] | ((uint16_t)kbdlay_image[at + 1] << 8);
}
