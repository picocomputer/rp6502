/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The scripted terminal session, as an image, written once.
 *
 * Two suites boot it. tests/cpu/vga holds the picture it leaves to a CRC on
 * whichever machine that tree built; tests/rtl/vga borrows the state it
 * scrolled into place to make a claim about the fabric's blink phase, which
 * only that machine has. Written here so the two cannot come to be booting
 * different programs.
 *
 * An indexed page reaches 255 bytes, so the printer chains: each block prints
 * its part and jumps to the next; the last one stops.
 */

#ifndef _TESTS_CPU_VID_SESSION_PROG_H_
#define _TESTS_CPU_VID_SESSION_PROG_H_

#include "tb_rom.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool session_rom(std::vector<uint8_t> &rom)
{
    std::string script = "\33[0m\33[2J\33[H\33[?25l";
    /* Forty lines walk the scroll past the 30-row screen. */
    for (int i = 0; i < 40; i++)
    {
        char line[32];
        snprintf(line, sizeof(line), "scroll line %02d\r\n", i);
        script += line;
    }
    /* A region scroll inside DECSTBM margins permutes row_idx. */
    script += "\33[5;10r\33[10;1H\nregion a\nregion b\nregion c\n\33[r";
    /* Attributes, an underline color, DEC graphics, italic, an EL. */
    script += "\33[15;1H\33[1;33;44mbold yellow on blue\33[0m "
              "\33[7mreverse\33[0m \33[4;58;5;196mulcolor\33[0m "
              "\33[3mitalic\33[0m \33(0lqqk\33(B";
    script += "\33[16;1Hpartial line\33[8G\33[K";
    /* Alt screen round trip: its content must not survive the return. */
    script += "\33[?1049h\33[2J\33[HALT SCREEN\33[?1049l";
    /* A steady block cursor parked mid-screen renders on both sides. */
    script += "\33[20;5H\33[2 q\33[?25h";

    /* An indexed page reaches 255 bytes, so the printer chains: each
     * block prints its part and jumps to the next; the last one stops. */
    std::vector<std::string> parts;
    for (size_t at = 0; at < script.size(); at += 255)
        parts.push_back(script.substr(at, 255));
    uint16_t org = 0x0300;
    std::vector<uint8_t> image;
    uint16_t next = org;
    for (size_t p = 0; p < parts.size(); p++)
    {
        uint16_t msg = (uint16_t)(next + 22);
        bool last = p + 1 == parts.size();
        uint16_t after = (uint16_t)(msg + parts[p].size() + 1);
        std::vector<uint8_t> b = {
            0xA2, 0x00,
            0xBD, (uint8_t)(msg & 0xFF), (uint8_t)(msg >> 8),
            0xF0, 0x0C,
            0x2C, 0xE0, 0xFF,
            0x10, 0xFB,
            0x8D, 0xE1, 0xFF,
            0xE8,
            0xD0, 0xF0,
            0xEA,
        };
        if (last)
        {
            b.push_back(0xDB);
            b.push_back(0xEA);
            b.push_back(0xEA);
        }
        else
        {
            b.push_back(0x4C);
            b.push_back((uint8_t)(after & 0xFF));
            b.push_back((uint8_t)(after >> 8));
        }
        if (b.size() != 22)
            return false; /* the printer stub is a fixed 22 bytes */
        b.insert(b.end(), parts[p].begin(), parts[p].end());
        b.push_back(0);
        image.insert(image.end(), b.begin(), b.end());
        next = (uint16_t)(next + b.size());
    }
    static const uint8_t vectors[] = {0x00, 0x03};

    const char magic[] = "#!RP6502\n";
    rom.clear();
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, org, image.data(), image.size());
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return true;
}

#endif /* _TESTS_CPU_VID_SESSION_PROG_H_ */
