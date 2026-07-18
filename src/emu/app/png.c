/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/app/png.h"
#include "emu/host/rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_be32(FILE *f, uint32_t v)
{
    fputc((v >> 24) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
    fputc(v & 0xFF, f);
}

static void put_be32_buf(uint8_t *dst, uint32_t v)
{
    dst[0] = (v >> 24) & 0xFF;
    dst[1] = (v >> 16) & 0xFF;
    dst[2] = (v >> 8) & 0xFF;
    dst[3] = v & 0xFF;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    put_be32(f, len);
    fwrite(type, 1, 4, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t crc = rom_crc32(0, type, 4);
    crc = rom_crc32(crc, data, len);
    put_be32(f, crc);
}

static uint32_t adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++)
    {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

bool png_write(const char *path, int w, int h, const uint32_t *rgba)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "rp6502-emu: cannot write PNG '%s'\n", path);
        return false;
    }

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put_be32_buf(ihdr, (uint32_t)w);
    put_be32_buf(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* color type: truecolor + alpha */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    write_chunk(f, "IHDR", ihdr, sizeof(ihdr));

    /* Raw filtered image: each row is a 0 filter byte + RGBA bytes. */
    size_t row_bytes = (size_t)w * 4 + 1;
    size_t raw_len = row_bytes * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw)
    {
        fclose(f);
        return false;
    }
    for (int y = 0; y < h; y++)
    {
        uint8_t *row = raw + (size_t)y * row_bytes;
        *row++ = 0;
        const uint32_t *src = rgba + (size_t)y * w;
        for (int x = 0; x < w; x++)
        {
            uint32_t p = src[x];
            *row++ = p & 0xFF;         /* R */
            *row++ = (p >> 8) & 0xFF;  /* G */
            *row++ = (p >> 16) & 0xFF; /* B */
            *row++ = (p >> 24) & 0xFF; /* A */
        }
    }

    /* zlib stream: 2-byte header + stored DEFLATE blocks + adler32. */
    size_t cap = raw_len + (raw_len / 65535 + 1) * 5 + 6;
    uint8_t *zs = (uint8_t *)malloc(cap);
    if (!zs)
    {
        free(raw);
        fclose(f);
        return false;
    }
    size_t zn = 0;
    zs[zn++] = 0x78;
    zs[zn++] = 0x01;
    size_t off = 0;
    while (off < raw_len)
    {
        size_t block = raw_len - off;
        if (block > 65535)
            block = 65535;
        int final = (off + block >= raw_len);
        zs[zn++] = final ? 1 : 0;
        zs[zn++] = block & 0xFF;
        zs[zn++] = (block >> 8) & 0xFF;
        zs[zn++] = (~block) & 0xFF;
        zs[zn++] = ((~block) >> 8) & 0xFF;
        memcpy(zs + zn, raw + off, block);
        zn += block;
        off += block;
    }
    uint32_t ad = adler32(raw, raw_len);
    put_be32_buf(zs + zn, ad);
    zn += 4;

    write_chunk(f, "IDAT", zs, (uint32_t)zn);
    write_chunk(f, "IEND", NULL, 0);

    free(zs);
    free(raw);
    /* ferror() catches any short write across the unchecked fputc/fwrite above;
     * fclose() surfaces a deferred flush failure. Either means a truncated PNG. */
    bool ok = (ferror(f) == 0);
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "rp6502-emu: error writing PNG '%s'\n", path);
    return ok;
}
