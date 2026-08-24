/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * uni_word from the image on disk, which is the only copy a Pocket has.
 *
 * src/core/api/uni.h calls this the one thing a port has to write, and
 * there are two ports: a machine that can afford five kilobytes of
 * flash links oemcp.c and reads an array, and a machine that cannot
 * stages oemcp.bin and reads a window. Both come out of one generator,
 * and until this file existed nothing compared them — so a generator
 * that emitted the two differently would have turned every accented
 * filename on a Pocket into mojibake with the whole suite still green.
 *
 * This is the same test_uni.c the linked table runs, over the staged
 * bytes instead. The image is read once and held, because the Pocket's
 * window is a store and not a stream.
 */

#include "core/api/uni.h"
#include "oemcp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t oemcp_image[OEMCP_BYTES];
static int oemcp_loaded;

static void uni_bin_load(void)
{
    FILE *f = fopen(OEMCP_BIN_PATH, "rb");
    if (!f)
    {
        fprintf(stderr, "uni_bin: cannot open %s\n", OEMCP_BIN_PATH);
        exit(1);
    }
    size_t n = fread(oemcp_image, 1, sizeof oemcp_image, f);
    /* Exactly the image, and nothing after it: a short file means the
     * generator and oemcp.h disagree about how big the tables are, which
     * is the failure this whole file exists to catch. */
    if (n != sizeof oemcp_image || fgetc(f) != EOF)
    {
        fprintf(stderr, "uni_bin: %s is %zu bytes, expected %zu\n",
                OEMCP_BIN_PATH, n, sizeof oemcp_image);
        exit(1);
    }
    fclose(f);
    oemcp_loaded = 1;
}

/* Little-endian halfwords, which is how src/rtl/sw/uni.c reads them out
 * of the staging window a byte at a time. */
uint16_t uni_word(uint32_t index)
{
    if (!oemcp_loaded)
        uni_bin_load();
    if (index >= OEMCP_WORDS)
        return 0;
    uint32_t at = index * 2;
    return (uint16_t)oemcp_image[at] | ((uint16_t)oemcp_image[at + 1] << 8);
}
