/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CPU conformance harness for the Klaus Dormann 6502/65C02 functional tests.
 * Loads a 64KB test image at $0000, points the reset vector at $0400, and
 * runs the w65c02 core against flat RAM until it self-traps (jmp *). The test
 * passes iff the trap address equals the image's documented success address;
 * any other self-trap is a failing subtest. Usage:
 *
 *   test_cpu_conformance <image.bin> <success_hex>
 *
 * This unit embeds its own CPU implementation and flat-RAM bus (it must NOT
 * link emu_core, whose w65c02.c also defines CHIPS_IMPL).
 */

#define CHIPS_IMPL
#include "emu/sys/w65c02.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint8_t mem[0x10000];

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <image.bin> <success_hex>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    uint16_t success = (uint16_t)strtol(argv[2], NULL, 16);

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    size_t n = fread(mem, 1, sizeof(mem), f);
    fclose(f);
    if (n != sizeof(mem))
    {
        fprintf(stderr, "%s must be exactly %zu bytes (got %zu)\n",
                path, sizeof(mem), n);
        return 2;
    }
    mem[0xFFFC] = 0x00; /* reset vector -> $0400 (test entry) */
    mem[0xFFFD] = 0x04;

    m6502_t cpu;
    uint64_t pins = m6502_init(&cpu, &(m6502_desc_t){0});

    uint16_t prev_pc = 0xFFFF;
    int have_prev = 0;
    uint64_t instr = 0;
    const uint64_t MAX_TICKS = 4000ULL * 1000 * 1000;
    for (uint64_t t = 0; t < MAX_TICKS; t++)
    {
        pins = m6502_tick(&cpu, pins);
        uint16_t addr = M6502_GET_ADDR(pins);
        if (pins & M6502_RW)
        {
            M6502_SET_DATA(pins, mem[addr]);
        }
        else
            mem[addr] = M6502_GET_DATA(pins);

        if (pins & M6502_SYNC) /* opcode fetch: addr is the instruction's PC */
        {
            instr++;
            if (addr == success)
            {
                printf("PASS  %s  reached success $%04X (%llu instrs)\n",
                       path, addr, (unsigned long long)instr);
                return 0;
            }
            if (have_prev && addr == prev_pc) /* jmp * -> trapped */
            {
                fprintf(stderr,
                        "FAIL  %s  trapped at $%04X (%llu instrs); "
                        "expected success $%04X\n",
                        path, addr, (unsigned long long)instr, success);
                return 1;
            }
            prev_pc = addr;
            have_prev = 1;
        }
    }
    fprintf(stderr, "TIMEOUT  %s  after %llu ticks\n",
            path, (unsigned long long)MAX_TICKS);
    return 3;
}
