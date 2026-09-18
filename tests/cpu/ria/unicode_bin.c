/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/unicode.h"
#include "oemcp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t oemcp_image[OEMCP_BYTES];
static int oemcp_loaded;

static void unicode_bin_load(void)
{
    FILE *f = fopen(OEMCP_BIN_PATH, "rb");
    if (!f)
    {
        fprintf(stderr, "unicode_bin: cannot open %s\n", OEMCP_BIN_PATH);
        exit(1);
    }
    size_t n = fread(oemcp_image, 1, sizeof oemcp_image, f);
    if (n != sizeof oemcp_image || fgetc(f) != EOF)
    {
        fprintf(stderr, "unicode_bin: %s is %zu bytes, expected %zu\n",
                OEMCP_BIN_PATH, n, sizeof oemcp_image);
        exit(1);
    }
    fclose(f);
    oemcp_loaded = 1;
}

uint16_t unicode_word(uint32_t index)
{
    if (!oemcp_loaded)
        unicode_bin_load();
    if (index >= OEMCP_WORDS)
        return 0;
    uint32_t at = index * 2;
    return (uint16_t)oemcp_image[at] | ((uint16_t)oemcp_image[at + 1] << 8);
}
