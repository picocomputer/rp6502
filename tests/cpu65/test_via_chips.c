/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * chips/chips/m6522.h on its own, so a vendored VIA that changes is noticed
 * by a build with no simulator in it. test_via asks whether via.sv agrees
 * with chips; this asks whether chips still does what it did, which is the
 * question every tree can answer.
 *
 * Two kinds of evidence, because a recording alone says only that something
 * moved. The directed cases assert the 6522's documented behaviour outright,
 * so a failure names the rule; the traces cover everything else, including
 * the fuzz, where writing the expectation by hand is not possible.
 *
 * Regenerating the traces accepts whatever changed, which is the point: it
 * is a commit, and the diff is the notification. Run
 *
 *     test_via_chips --emit > tests/cpu65/via_golden.txt
 *
 * and read what moved before committing it.
 */

#define CHIPS_IMPL
#include "chips/chips/m6522.h"

#include "via_scen.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>

static m6522_t via;

/* The machine's wiring: CS1 asserted, ports unwired, RW high to read. See
 * emu/emu/via.c, which is what this reproduces. */
static void step(const via_op_t *op, uint8_t *data, bool *irq)
{
    uint64_t pins = 0;
    if (op->kind != VIA_OP_IDLE)
    {
        pins |= op->rs & M6522_RS_PINS;
        pins |= M6522_CS1;
        if (op->kind == VIA_OP_READ)
            pins |= M6522_RW;
        else
            M6522_SET_DATA(pins, op->data);
    }
    pins = m6522_tick(&via, pins);
    *data = M6522_GET_DATA(pins);
    *irq = (pins & M6522_IRQ) != 0;
}

static void reset(void) { m6522_init(&via); }

/* One op, for the directed cases. */
static uint8_t rd(uint8_t rs)
{
    via_op_t op = {VIA_OP_READ, rs, 0, 0};
    uint8_t data;
    bool irq;
    step(&op, &data, &irq);
    return data;
}

static void wr(uint8_t rs, uint8_t data)
{
    via_op_t op = {VIA_OP_WRITE, rs, data, 0};
    uint8_t d;
    bool irq;
    step(&op, &d, &irq);
}

static bool idle_until_irq(int limit)
{
    via_op_t op = {VIA_OP_IDLE, 0, 0, 0};
    uint8_t data;
    bool irq = false;
    for (int i = 0; i < limit; i++)
    {
        step(&op, &data, &irq);
        if (irq)
            return true;
    }
    return false;
}

/* CRC-32, so a trace is a number small enough to commit and read. The
 * suites here cannot link emu_core — chips_dut.c explains why — so this is
 * the one place it is spelled out again. */
static uint32_t crc32_up(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    return crc;
}

/* Every cycle contributes its read data and its IRQ level, which is exactly
 * what test_via compares against the RTL. */
static uint32_t trace_script(const via_op_t *ops, size_t n_ops, uint64_t *cycles)
{
    reset();
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t n = 0;
    for (size_t i = 0; i < n_ops; i++)
        for (uint16_t r = 0; r <= ops[i].repeat; r++)
        {
            uint8_t data;
            bool irq;
            step(&ops[i], &data, &irq);
            crc = crc32_up(crc, ops[i].kind == VIA_OP_READ ? data : 0);
            crc = crc32_up(crc, irq ? 1 : 0);
            n++;
        }
    *cycles = n;
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t trace_fuzz(uint64_t *cycles)
{
    reset();
    uint32_t crc = 0xFFFFFFFFu;
    uint16_t lfsr = VIA_FUZZ_SEED;
    for (int i = 0; i < VIA_FUZZ_CYCLES; i++)
    {
        via_op_t op;
        via_fuzz_next(&lfsr, &op);
        uint8_t data;
        bool irq;
        step(&op, &data, &irq);
        crc = crc32_up(crc, op.kind == VIA_OP_READ ? data : 0);
        crc = crc32_up(crc, irq ? 1 : 0);
    }
    *cycles = VIA_FUZZ_CYCLES;
    return crc ^ 0xFFFFFFFFu;
}

/* The committed traces, read once. */
#define GOLDEN_MAX 32
static struct
{
    char name[32];
    uint32_t crc;
    uint64_t cycles;
} golden[GOLDEN_MAX];
static int n_golden;

static bool golden_load(void)
{
    FILE *f = fopen(VIA_GOLDEN, "r");
    if (!f)
        return false;
    char line[128];
    n_golden = 0;
    while (fgets(line, sizeof line, f) && n_golden < GOLDEN_MAX)
    {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%31s %x %llu", golden[n_golden].name,
                   &golden[n_golden].crc,
                   (unsigned long long *)&golden[n_golden].cycles) == 3)
            n_golden++;
    }
    fclose(f);
    return n_golden > 0;
}

static bool golden_check(const char *name, uint32_t crc, uint64_t cycles,
                         char *detail, size_t cap)
{
    for (int i = 0; i < n_golden; i++)
        if (!strcmp(golden[i].name, name))
        {
            if (golden[i].cycles != cycles)
            {
                snprintf(detail, cap, "%s ran %llu cycles, recorded %llu",
                         name, (unsigned long long)cycles,
                         (unsigned long long)golden[i].cycles);
                return false;
            }
            if (golden[i].crc != crc)
            {
                snprintf(detail, cap,
                         "%s trace %08X, recorded %08X — the vendored VIA "
                         "changed behaviour",
                         name, crc, golden[i].crc);
                return false;
            }
            return true;
        }
    snprintf(detail, cap, "%s is not in %s", name, VIA_GOLDEN);
    return false;
}

/* --- The documented behaviour, asserted rather than recorded --- */

UTEST(via_chips, t1_oneshot_interrupts_once)
{
    reset();
    wr(0xE, 0xC0); /* IER: enable T1 */
    wr(0x4, 10);   /* T1 latch low */
    wr(0x5, 0);    /* T1 high, starts the count */
    ASSERT_TRUE(idle_until_irq(64));
    ASSERT_TRUE((rd(0xD) & 0x40) != 0); /* IFR T1 */
    rd(0x4);                            /* reading T1CL clears it */
    ASSERT_TRUE((rd(0xD) & 0x40) == 0);
    /* One-shot: it must not come back. */
    ASSERT_FALSE(idle_until_irq(64));
}

UTEST(via_chips, t1_continuous_reloads)
{
    reset();
    wr(0xB, 0x40); /* ACR: continuous */
    wr(0xE, 0xC0);
    wr(0x4, 6);
    wr(0x5, 0);
    ASSERT_TRUE(idle_until_irq(64));
    rd(0x4); /* clear */
    /* Continuous: the latch reloads and it fires again. */
    ASSERT_TRUE(idle_until_irq(64));
}

UTEST(via_chips, t2_oneshot_does_not_reload)
{
    reset();
    wr(0xE, 0xA0); /* IER: enable T2 */
    wr(0x8, 8);
    wr(0x9, 0);
    ASSERT_TRUE(idle_until_irq(64));
    ASSERT_TRUE((rd(0xD) & 0x20) != 0); /* IFR T2 */
    rd(0x8);                            /* reading T2CL clears it */
    ASSERT_TRUE((rd(0xD) & 0x20) == 0);
    ASSERT_FALSE(idle_until_irq(64));
}

UTEST(via_chips, ier_masks_the_irq_line_not_the_flag)
{
    reset();
    wr(0xE, 0x40); /* IER: bit 7 clear, so this DISABLES T1 */
    wr(0x4, 4);
    wr(0x5, 0);
    /* The flag still sets; the line stays low. */
    ASSERT_FALSE(idle_until_irq(64));
    ASSERT_TRUE((rd(0xD) & 0x40) != 0);
    ASSERT_TRUE((rd(0xD) & 0x80) == 0); /* no master */
}

UTEST(via_chips, ifr_is_write_to_clear)
{
    reset();
    wr(0x4, 4);
    wr(0x5, 0);
    for (int i = 0; i < 32; i++)
        rd(0xF);
    ASSERT_TRUE((rd(0xD) & 0x40) != 0);
    wr(0xD, 0x7F);
    ASSERT_TRUE((rd(0xD) & 0x40) == 0);
}

UTEST(via_chips, ier_readback_sets_bit7)
{
    reset();
    wr(0xE, 0xE0);                      /* set T1 + T2 */
    ASSERT_TRUE((rd(0xE) & 0x80) != 0); /* reads back with bit 7 high */
    ASSERT_TRUE((rd(0xE) & 0x60) == 0x60);
    wr(0xE, 0x40); /* clear T1 only */
    ASSERT_TRUE((rd(0xE) & 0x40) == 0);
    ASSERT_TRUE((rd(0xE) & 0x20) != 0);
}

UTEST(via_chips, t1_toggles_pb7)
{
    reset();
    wr(0x2, 0x80); /* DDRB: PB7 output */
    wr(0xB, 0xC0); /* ACR: continuous + PB7 */
    wr(0x4, 4);
    wr(0x5, 0);
    /* Sample ORB across several reloads; PB7 must not sit still. */
    uint8_t seen = 0;
    via_op_t idle = {VIA_OP_IDLE, 0, 0, 0};
    for (int i = 0; i < 64; i++)
    {
        uint8_t data;
        bool irq;
        step(&idle, &data, &irq);
        seen |= (uint8_t)(rd(0x0) & 0x80) ? 2 : 1;
    }
    ASSERT_EQ(3, seen); /* both levels observed */
}

/* --- Everything else, against the recording --- */

#define VIA_TRACE(name)                                                  \
    UTEST(via_chips, trace_##name)                                       \
    {                                                                    \
        char detail[160] = "";                                           \
        uint64_t cycles;                                                 \
        uint32_t crc = trace_script(via_scen_##name, via_scen_##name##_n,\
                                    &cycles);                            \
        ASSERT_TRUE(golden_load());                                      \
        bool ok = golden_check(#name, crc, cycles, detail, sizeof detail);\
        if (!ok)                                                         \
            printf("%s\n", detail);                                      \
        ASSERT_TRUE(ok);                                                 \
    }

VIA_SCRIPTS(VIA_TRACE)

UTEST(via_chips, trace_fuzz)
{
    char detail[160] = "";
    uint64_t cycles;
    uint32_t crc = trace_fuzz(&cycles);
    ASSERT_TRUE(golden_load());
    bool ok = golden_check("fuzz", crc, cycles, detail, sizeof detail);
    if (!ok)
        printf("%s\n", detail);
    ASSERT_TRUE(ok);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    if (argc == 2 && !strcmp(argv[1], "--emit"))
    {
        uint64_t cycles;
        printf("# chips/chips/m6522.h traces, per scenario: name crc32 cycles.\n"
               "# Regenerate with test_via_chips --emit and read the diff.\n");
#define VIA_EMIT(name)                                                   \
    {                                                                    \
        uint32_t crc = trace_script(via_scen_##name, via_scen_##name##_n,\
                                    &cycles);                            \
        printf("%s %08X %llu\n", #name, crc, (unsigned long long)cycles);\
    }
        VIA_SCRIPTS(VIA_EMIT)
#undef VIA_EMIT
        uint32_t crc = trace_fuzz(&cycles);
        printf("fuzz %08X %llu\n", crc, (unsigned long long)cycles);
        return 0;
    }
    return utest_main(argc, argv);
}
