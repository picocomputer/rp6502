/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lockstep.h"

#include <stdio.h>
#include <string.h>

/* Each CPU runs against its own copy: a divergence must not let one side's
 * writes contaminate the other's view. */
static uint8_t lockstep_mem[2][0x10000];

typedef struct
{
    uint16_t addr;
    uint8_t data;
    bool read, sync;
} lockstep_cycle_t;

static void step(const dut_t *cpu, uint8_t *mem, lockstep_cycle_t *out)
{
    cpu->bus(&out->addr, &out->read, &out->sync);
    out->data = mem[out->addr];
    cpu->tick(&out->data);
    if (!out->read)
        mem[out->addr] = out->data;
}

bool lockstep_run(const dut_t *ref, const dut_t *dut,
                  const uint8_t *image,
                  const lockstep_ev_t *evs, size_t n_evs,
                  uint64_t cycles, lockstep_result_t *result)
{
    memset(result, 0, sizeof *result);
    memcpy(lockstep_mem[0], image, sizeof lockstep_mem[0]);
    memcpy(lockstep_mem[1], image, sizeof lockstep_mem[1]);

    ref->pins(false, false, false, false);
    dut->pins(false, false, false, false);
    ref->reset();
    dut->reset();

    size_t ev = 0;
    for (uint64_t c = 0; c < cycles; c++)
    {
        if (ev < n_evs && evs[ev].cycle == c)
        {
            ref->pins(evs[ev].irq, evs[ev].nmi, evs[ev].rdy, evs[ev].res);
            dut->pins(evs[ev].irq, evs[ev].nmi, evs[ev].rdy, evs[ev].res);
            ev++;
        }

        lockstep_cycle_t rc, dc;
        step(ref, lockstep_mem[0], &rc);
        step(dut, lockstep_mem[1], &dc);
        result->cycles = c + 1;

        if (rc.addr != dc.addr || rc.data != dc.data ||
            rc.read != dc.read || rc.sync != dc.sync)
        {
            snprintf(result->detail, sizeof result->detail,
                     "cycle %llu: %s %04X %02X %s%s vs %s %04X %02X %s%s",
                     (unsigned long long)c,
                     ref->name, rc.addr, rc.data, rc.read ? "r" : "w",
                     rc.sync ? " sync" : "",
                     dut->name, dc.addr, dc.data, dc.read ? "r" : "w",
                     dc.sync ? " sync" : "");
            return false;
        }
    }
    return true;
}
