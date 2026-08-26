/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * layout_word from the image on disk, which is the only copy a Pocket has.
 *
 * src/core/hid/layout.h calls this the one thing a port has to write, and
 * there are two ports: a machine with flash links kbdlay.c and reads an
 * array, and a machine without stages keyboard.bin and reads a window.
 * Both come out of one generator, so both are checked — the same
 * test_layout.c over the staged bytes instead.
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
    /* Exactly the image, and nothing after it: a short file means the
     * generator and kbdlay.h disagree about how big the layouts are,
     * and data.json's size_exact is derived from the same number. */
    if (n != sizeof kbdlay_image || fgetc(f) != EOF)
    {
        fprintf(stderr, "layout_bin: %s is %zu bytes, expected %zu\n",
                KBDLAY_BIN_PATH, n, sizeof kbdlay_image);
        exit(1);
    }
    fclose(f);
    kbdlay_loaded = 1;
}

/* Little-endian halfwords, which is how src/rtl/sw/layout.c reads them out
 * of the staging window a byte at a time. */
uint16_t layout_word(uint32_t index)
{
    if (!kbdlay_loaded)
        layout_bin_load();
    if (index >= KBDLAY_WORDS)
        return 0;
    uint32_t at = index * 2;
    return (uint16_t)kbdlay_image[at] | ((uint16_t)kbdlay_image[at + 1] << 8);
}
