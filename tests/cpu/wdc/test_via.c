/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "via_dut.h"

#include "via_scen.h"
#include "host/host.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>

static uint8_t rd(uint8_t rs)
{
    via_op_t op = {VIA_OP_READ, rs, 0, 0};
    uint8_t data;
    bool irq;
    via_step(&op, &data, &irq);
    return data;
}

static void wr(uint8_t rs, uint8_t data)
{
    via_op_t op = {VIA_OP_WRITE, rs, data, 0};
    uint8_t d;
    bool irq;
    via_step(&op, &d, &irq);
}

static bool idle_until_irq(int limit)
{
    via_op_t op = {VIA_OP_IDLE, 0, 0, 0};
    uint8_t data;
    bool irq = false;
    for (int i = 0; i < limit; i++)
    {
        via_step(&op, &data, &irq);
        if (irq)
            return true;
    }
    return false;
}

static uint32_t crc32_up(uint32_t crc, uint8_t byte)
{
    return host_crc32(crc, &byte, 1);
}

static uint32_t trace_script(const via_op_t *ops, size_t n_ops, uint64_t *cycles)
{
    via_reset();
    uint32_t crc = 0;
    uint64_t n = 0;
    for (size_t i = 0; i < n_ops; i++)
        for (uint16_t r = 0; r <= ops[i].repeat; r++)
        {
            uint8_t data;
            bool irq;
            via_step(&ops[i], &data, &irq);
            crc = crc32_up(crc, ops[i].kind == VIA_OP_READ ? data : 0);
            crc = crc32_up(crc, irq ? 1 : 0);
            n++;
        }
    *cycles = n;
    return crc;
}

static uint32_t trace_fuzz(uint64_t *cycles)
{
    via_reset();
    uint32_t crc = 0;
    uint16_t lfsr = VIA_FUZZ_SEED;
    for (int i = 0; i < VIA_FUZZ_CYCLES; i++)
    {
        via_op_t op;
        via_fuzz_next(&lfsr, &op);
        uint8_t data;
        bool irq;
        via_step(&op, &data, &irq);
        crc = crc32_up(crc, op.kind == VIA_OP_READ ? data : 0);
        crc = crc32_up(crc, irq ? 1 : 0);
    }
    *cycles = VIA_FUZZ_CYCLES;
    return crc;
}

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

UTEST(via, t1_oneshot_interrupts_once)
{
    via_reset();
    wr(0xE, 0xC0);
    wr(0x4, 10);
    wr(0x5, 0);
    ASSERT_TRUE(idle_until_irq(64));
    ASSERT_TRUE((rd(0xD) & 0x40) != 0);
    rd(0x4); /* Reading T1CL clears the T1 interrupt flag. */
    ASSERT_TRUE((rd(0xD) & 0x40) == 0);
    ASSERT_FALSE(idle_until_irq(64));
}

UTEST(via, t1_continuous_reloads)
{
    via_reset();
    wr(0xB, 0x40);
    wr(0xE, 0xC0);
    wr(0x4, 6);
    wr(0x5, 0);
    ASSERT_TRUE(idle_until_irq(64));
    rd(0x4);
    ASSERT_TRUE(idle_until_irq(64));
}

UTEST(via, t2_oneshot_does_not_reload)
{
    via_reset();
    wr(0xE, 0xA0);
    wr(0x8, 8);
    wr(0x9, 0);
    ASSERT_TRUE(idle_until_irq(64));
    ASSERT_TRUE((rd(0xD) & 0x20) != 0);
    rd(0x8); /* Reading T2CL clears the T2 interrupt flag. */
    ASSERT_TRUE((rd(0xD) & 0x20) == 0);
    ASSERT_FALSE(idle_until_irq(64));
}

UTEST(via, ier_masks_the_irq_line_not_the_flag)
{
    via_reset();
    wr(0xE, 0x40); /* Bit 7 is clear, so this write disables T1's interrupt. */
    wr(0x4, 4);
    wr(0x5, 0);
    ASSERT_FALSE(idle_until_irq(64));
    ASSERT_TRUE((rd(0xD) & 0x40) != 0);
    ASSERT_TRUE((rd(0xD) & 0x80) == 0);
}

UTEST(via, ifr_is_write_to_clear)
{
    via_reset();
    wr(0x4, 4);
    wr(0x5, 0);
    for (int i = 0; i < 32; i++)
        rd(0xF);
    ASSERT_TRUE((rd(0xD) & 0x40) != 0);
    wr(0xD, 0x7F);
    ASSERT_TRUE((rd(0xD) & 0x40) == 0);
}

UTEST(via, ier_readback_sets_bit7)
{
    via_reset();
    wr(0xE, 0xE0);
    ASSERT_TRUE((rd(0xE) & 0x80) != 0);
    ASSERT_TRUE((rd(0xE) & 0x60) == 0x60);
    wr(0xE, 0x40);
    ASSERT_TRUE((rd(0xE) & 0x40) == 0);
    ASSERT_TRUE((rd(0xE) & 0x20) != 0);
}

UTEST(via, t1_toggles_pb7)
{
    via_reset();
    wr(0x2, 0x80);
    wr(0xB, 0xC0);
    wr(0x4, 4);
    wr(0x5, 0);
    uint8_t seen = 0;
    via_op_t idle = {VIA_OP_IDLE, 0, 0, 0};
    for (int i = 0; i < 64; i++)
    {
        uint8_t data;
        bool irq;
        via_step(&idle, &data, &irq);
        seen |= (uint8_t)(rd(0x0) & 0x80) ? 2 : 1;
    }
    ASSERT_EQ(3, seen);
}

#define VIA_TRACE(name)                                                     \
    UTEST(via, trace_##name)                                          \
    {                                                                          \
        char detail[160] = "";                                                 \
        uint64_t cycles;                                                       \
        uint32_t crc = trace_script(via_scen_##name, via_scen_##name##_n,\
                                    &cycles);                                  \
        ASSERT_TRUE(golden_load());                                            \
        bool ok = golden_check(#name, crc, cycles, detail, sizeof detail);     \
        if (!ok)                                                               \
            printf("%s\n", detail);                                            \
        ASSERT_TRUE(ok);                                                       \
    }

VIA_SCRIPTS(VIA_TRACE)

UTEST(via, trace_fuzz)
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
    via_dut_init(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--emit"))
    {
        uint64_t cycles;
        printf("# The 6522 traces, per scenario: name crc32 cycles.\n"
               "# Regenerate with test_via --emit and read the diff.\n");
#define VIA_EMIT(name)                                                      \
    {                                                                          \
        uint32_t crc = trace_script(via_scen_##name, via_scen_##name##_n,\
                                    &cycles);                                  \
        printf("%s %08X %llu\n", #name, crc, (unsigned long long)cycles);      \
    }
        VIA_SCRIPTS(VIA_EMIT)
#undef VIA_EMIT
        uint32_t crc = trace_fuzz(&cycles);
        printf("fuzz %08X %llu\n", crc, (unsigned long long)cycles);
        via_dut_free();
        return 0;
    }
    int rc = utest_main(argc, argv);
    via_dut_free();
    return rc;
}
