/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_ROM_RECORD_H_
#define _CORE_ROM_RECORD_H_

/* A .rp6502 memory record: where it goes, how much of it there is, and what it
 * should add up to. The three numbers and the rules about them are the file
 * format, not any machine's reading of it, so they are decided here and each
 * loader does its own moving of bytes and its own complaining.
 */

#include <stdbool.h>
#include <stdint.h>

/* The largest record the format produces: the packer caps every memory chunk
 * at 1024 bytes and never crosses a 64 KB page (tools/rp6502.py). A machine's
 * record buffer is this big and the parse refuses anything bigger. */
#define ROM_RECORD_MAX 1024

typedef struct
{
    uint32_t addr, len, crc;
} rom_record_t;

typedef enum
{
    ROM_RECORD_OK,
    ROM_RECORD_SKIP,      /* blank line or comment: not a record, not an error */
    ROM_RECORD_MALFORMED, /* not three numbers and nothing else */
    ROM_RECORD_RANGE,     /* would land somewhere no record may */
} rom_record_result;

/* Read one line. RAM is below 0x10000 and XRAM above, and a record may not
 * straddle them or run off the end of either. max_len caps a record to what
 * this machine can hold in one piece; 0 for a machine that streams. */
rom_record_result rom_record_parse(const char *line, uint32_t max_len, rom_record_t *rec);

/* The reset vector is what makes an image loadable, and it arrives as two
 * ordinary bytes inside whichever record happens to cover $FFFC and $FFFD.
 * Noted only once a record has landed, so a truncated or corrupt one cannot
 * vouch for the vector it was carrying. */
typedef struct
{
    bool lo, hi;
} rom_record_vectors_t;

static inline void rom_record_note(rom_record_vectors_t *v, const rom_record_t *rec)
{
    if (rec->addr <= 0xFFFC && rec->addr + rec->len > 0xFFFC)
        v->lo = true;
    if (rec->addr <= 0xFFFD && rec->addr + rec->len > 0xFFFD)
        v->hi = true;
}

static inline bool rom_record_complete(const rom_record_vectors_t *v)
{
    return v->lo && v->hi;
}

#endif /* _CORE_ROM_RECORD_H_ */
