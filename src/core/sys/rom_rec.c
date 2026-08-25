/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See rom_rec.h.
 */

#include "core/sys/rom_rec.h"
#include "core/str/str.h"

rom_rec_result rom_rec_parse(const char *line, uint32_t max_len, rom_rec_t *rec)
{
    if (!line[0] || line[0] == '#')
        return ROM_REC_SKIP;
    const char *p = line;
    if (!str_parse_uint32(&p, &rec->addr) ||
        !str_parse_uint32(&p, &rec->len) ||
        !str_parse_uint32(&p, &rec->crc) ||
        !str_parse_end(p))
        return ROM_REC_MALFORMED;
    if (rec->addr > 0x1FFFF || rec->len == 0 ||
        rec->len > 0x20000 - rec->addr ||
        (rec->addr < 0x10000 && rec->len > 0x10000 - rec->addr) ||
        (max_len && rec->len > max_len))
        return ROM_REC_RANGE;
    return ROM_REC_OK;
}
