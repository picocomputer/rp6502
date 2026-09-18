/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "klaus.h"

#include <stdio.h>
#include <string.h>

static uint8_t klaus_mem[0x10000];

/* Klaus Dormann's test images are assembled with their code at $0400, and
 * each image's reset vector points at a trap that jumps to itself, since a
 * reset during a test is an error. */
#define KLAUS_ENTRY 0x0400

bool klaus_run(const char *path, const dut_t *cpu, uint64_t max_cycles,
               klaus_result_t *result)
{
    memset(result, 0, sizeof *result);

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(klaus_mem, 1, sizeof klaus_mem, f);
    fclose(f);
    if (n != sizeof klaus_mem)
        return false;

    klaus_mem[0xFFFC] = KLAUS_ENTRY & 0xFF;
    klaus_mem[0xFFFD] = KLAUS_ENTRY >> 8;

    cpu->reset();

    /* Both a pass and a failure end in an instruction that jumps or branches
     * to itself, so the test is over when one opcode fetch repeats the
     * address of the one before it. */
    uint32_t last_sync = 0xFFFFFFFF;
    for (uint64_t c = 0; c < max_cycles; c++)
    {
        uint16_t addr;
        bool read, sync;
        cpu->bus(&addr, &read, &sync);

        uint8_t data = klaus_mem[addr];
        cpu->tick(&data);
        if (!read)
            klaus_mem[addr] = data;

        result->cycles = c + 1;
        if (sync)
        {
            if (last_sync == addr)
            {
                result->trapped = true;
                result->trap_pc = addr;
                return true;
            }
            last_sync = addr;
        }
    }

    result->timed_out = true;
    return true;
}
