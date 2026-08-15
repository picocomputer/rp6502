/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * chips/chips/w65c02.h on its own — held to a recorded bus trace.
 *
 * The corpora do the real work here: SingleStepTests and Klaus are external
 * evidence and say the CPU is right, not merely unchanged. But both arrive by
 * OPTIONAL submodule, so a checkout without them runs neither, and then
 * nothing in any tree touches the vendored CPU at all. This is the floor that
 * survives that: it needs no download, and it covers the pins the corpora
 * leave quiet, which is the half w65c02_gen.py is most likely to disturb.
 *
 * Same scenarios as test_lockstep, so the two cannot come to disagree about
 * what they are asking. Regenerate with
 *
 *     test_w65c02_chips --emit > tests/wdc/w65c02_golden.txt
 *
 * and read what moved before committing it.
 */

#include "chips_dut.h"
#include "lockstep_scen.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>

static uint8_t image[0x10000];
static uint8_t mem[0x10000];

static uint32_t crc32_up(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    return crc;
}

/* One implementation over lockstep.c's memory rule: what the CPU drives, the
 * byte it sees, and the byte it writes back. lockstep.c compares this stream
 * between two CPUs; here it becomes the recording. */
static uint32_t trace(const lockstep_scen_t *scen)
{
    lockstep_scen_image(image, scen->entry);
    memcpy(mem, image, sizeof mem);

    chips_dut.pins(false, false, false, false);
    chips_dut.reset();

    uint32_t crc = 0xFFFFFFFFu;
    size_t ev = 0;
    for (uint64_t c = 0; c < scen->cycles; c++)
    {
        if (ev < scen->n_evs && scen->evs[ev].cycle == c)
        {
            chips_dut.pins(scen->evs[ev].irq, scen->evs[ev].nmi,
                           scen->evs[ev].rdy, scen->evs[ev].res);
            ev++;
        }
        uint16_t addr;
        bool read, sync;
        chips_dut.bus(&addr, &read, &sync);
        uint8_t data = mem[addr];
        chips_dut.tick(&data);
        if (!read)
            mem[addr] = data;
        crc = crc32_up(crc, (uint8_t)(addr >> 8));
        crc = crc32_up(crc, (uint8_t)addr);
        crc = crc32_up(crc, data);
        crc = crc32_up(crc, (uint8_t)((read ? 1 : 0) | (sync ? 2 : 0)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t trace_fuzz(void)
{
    static lockstep_ev_t evs[LOCKSTEP_FUZZ_EVENTS];
    lockstep_scen_fuzz(evs);
    lockstep_scen_t scen = {"fuzz", LOCKSTEP_FUZZ_ENTRY, LOCKSTEP_FUZZ_CYCLES,
                            evs, LOCKSTEP_FUZZ_EVENTS};
    return trace(&scen);
}

#define GOLDEN_MAX 32
static struct
{
    char name[40];
    uint32_t crc;
} golden[GOLDEN_MAX];
static int n_golden;

static bool golden_load(void)
{
    FILE *f = fopen(W65C02_GOLDEN, "r");
    if (!f)
        return false;
    char line[128];
    n_golden = 0;
    while (fgets(line, sizeof line, f) && n_golden < GOLDEN_MAX)
    {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%39s %x", golden[n_golden].name,
                   &golden[n_golden].crc) == 2)
            n_golden++;
    }
    fclose(f);
    return n_golden > 0;
}

static bool golden_check(const char *name, uint32_t crc, char *detail,
                         size_t cap)
{
    for (int i = 0; i < n_golden; i++)
        if (!strcmp(golden[i].name, name))
        {
            if (golden[i].crc != crc)
            {
                snprintf(detail, cap,
                         "%s trace %08X, recorded %08X — the vendored 65C02 "
                         "changed behaviour",
                         name, crc, golden[i].crc);
                return false;
            }
            return true;
        }
    snprintf(detail, cap, "%s is not in %s", name, W65C02_GOLDEN);
    return false;
}

#define W65C02_TRACE(name)                                               \
    UTEST(w65c02_chips, name)                                            \
    {                                                                    \
        char detail[160] = "";                                           \
        uint32_t crc = trace(&lockstep_scen_##name);                     \
        ASSERT_TRUE(golden_load());                                      \
        bool ok = golden_check(#name, crc, detail, sizeof detail);       \
        if (!ok)                                                         \
            printf("%s\n", detail);                                      \
        ASSERT_TRUE(ok);                                                 \
    }

LOCKSTEP_SCENARIOS(W65C02_TRACE)

UTEST(w65c02_chips, pin_fuzz)
{
    char detail[160] = "";
    uint32_t crc = trace_fuzz();
    ASSERT_TRUE(golden_load());
    bool ok = golden_check("pin_fuzz", crc, detail, sizeof detail);
    if (!ok)
        printf("%s\n", detail);
    ASSERT_TRUE(ok);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    if (argc == 2 && !strcmp(argv[1], "--emit"))
    {
        printf("# chips/chips/w65c02.h bus traces, per scenario: name crc32.\n"
               "# Regenerate with test_w65c02_chips --emit and read the diff.\n");
#define W65C02_EMIT(name) \
    printf("%s %08X\n", #name, trace(&lockstep_scen_##name));
        LOCKSTEP_SCENARIOS(W65C02_EMIT)
#undef W65C02_EMIT
        printf("pin_fuzz %08X\n", trace_fuzz());
        return 0;
    }
    return utest_main(argc, argv);
}
