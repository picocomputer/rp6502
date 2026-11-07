/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/unicode.h"
#include "unicode_oem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t unicode_oem_image[UNICODE_OEM_BYTES];
static int unicode_oem_loaded;

static void unicode_bin_load(void)
{
    FILE *f = fopen(UNICODE_OEM_BIN_PATH, "rb");
    if (!f)
    {
        fprintf(stderr, "unicode_bin: cannot open %s\n", UNICODE_OEM_BIN_PATH);
        exit(1);
    }
    size_t n = fread(unicode_oem_image, 1, sizeof unicode_oem_image, f);
    if (n != sizeof unicode_oem_image || fgetc(f) != EOF)
    {
        fprintf(stderr, "unicode_bin: %s is %zu bytes, expected %zu\n",
                UNICODE_OEM_BIN_PATH, n, sizeof unicode_oem_image);
        exit(1);
    }
    fclose(f);
    unicode_oem_loaded = 1;
}

uint16_t unicode_word(uint32_t index)
{
    if (!unicode_oem_loaded)
        unicode_bin_load();
    if (index >= UNICODE_OEM_WORDS)
        return 0;
    uint32_t at = index * 2;
    return (uint16_t)unicode_oem_image[at] | ((uint16_t)unicode_oem_image[at + 1] << 8);
}
