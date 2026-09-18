/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vcpu.h"
#include "Vcpu___024root.h"

#include "utest.h"

#include <cstring>
#include <vector>

#define ST_WORDS 5

static Vcpu *dut;
static uint8_t mem[0x10000];
static uint8_t mem_at_freeze[0x10000];

struct Cycle
{
    uint16_t addr;
    uint8_t data;
    bool we;
    bool sync;
};

static void clock_cycle(void)
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

static void make_dut(void)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vcpu;
    dut->clk = 0;
    dut->en = 1;
    dut->irq_i = 0;
    dut->nmi_i = 0;
    dut->rdy_i = 0;
    dut->res_i = 0;
    dut->data_i = 0;
    dut->st_idx = 0;
    dut->st_jam = 0;
    for (int i = 0; i < ST_WORDS; i++)
        dut->st_jam_data[i] = 0;
    dut->rst_n = 0;
    dut->eval();
    clock_cycle();
    clock_cycle();
    dut->rst_n = 1;
    dut->eval();
}

static Cycle step(void)
{
    Cycle c;
    c.addr = dut->cpu_addr;
    c.we = dut->cpu_we;
    c.sync = dut->rootp->cpu__DOT__cpu_sync;
    if (c.we)
    {
        c.data = dut->cpu_data;
        mem[c.addr] = c.data;
    }
    else
    {
        c.data = mem[c.addr];
        dut->data_i = c.data;
        dut->eval();
    }
    clock_cycle();
    return c;
}

static void step_frozen(void)
{
    dut->eval();
}

static void capture(uint32_t *st)
{
    for (int i = 0; i < ST_WORDS; i++)
    {
        dut->st_idx = i;
        dut->eval();
        st[i] = dut->cpu_st_rdata;
    }
    dut->st_idx = 0;
    dut->eval();
}

static void restore(const uint32_t *st)
{
    for (int i = 0; i < ST_WORDS; i++)
        dut->st_jam_data[i] = st[i];
    dut->st_jam = 1;
    dut->eval();
    clock_cycle();
    dut->st_jam = 0;
    dut->eval();
}

static void load(uint16_t org, const std::vector<uint8_t> &code)
{
    memset(mem, 0, sizeof mem);
    for (size_t i = 0; i < code.size(); i++)
        mem[org + i] = code[i];
    mem[0xFFFC] = org & 0xFF;
    mem[0xFFFD] = org >> 8;
    mem[0xFFFE] = 0x00;
    mem[0xFFFF] = 0x90;
    mem[0x9000] = 0x40; /* RTI */
}

static std::vector<Cycle> reference(int cycles)
{
    make_dut();
    std::vector<Cycle> t;
    for (int i = 0; i < cycles; i++)
        t.push_back(step());
    return t;
}

enum Land
{
    LAND_ANY,
    LAND_STALLED,
};

static uint32_t frozen_pip;

static std::vector<Cycle> frozen(bool *landed, int cycles, int freeze_at,
                                 int hold, int irq_pulse, Land land)
{
    make_dut();
    std::vector<Cycle> t;
    int i = 0;
    for (; i < cycles; i++)
    {
        if (i == irq_pulse)
        {
            dut->irq_i = 1;
            dut->eval();
            t.push_back(step());
            dut->irq_i = 0;
            dut->eval();
            continue;
        }
        if (i >= freeze_at
            && (land == LAND_STALLED
                    ? (dut->cpu_stp || dut->rootp->cpu__DOT__wait_flag)
                    : (dut->rootp->cpu__DOT__cpu_sync || dut->cpu_stp
                       || dut->rootp->cpu__DOT__wait_flag)))
            break;
        t.push_back(step());
    }
    *landed = i < cycles
        && (land != LAND_STALLED || dut->cpu_stp
            || dut->rootp->cpu__DOT__wait_flag);

    uint32_t st[ST_WORDS];
    capture(st);
    frozen_pip = st[3];
    memcpy(mem_at_freeze, mem, sizeof mem);

    for (int k = 0; k < hold; k++)
        step_frozen();

    make_dut();
    memcpy(mem, mem_at_freeze, sizeof mem);
    restore(st);

    for (; i < cycles; i++)
        t.push_back(step());
    return t;
}

static void compare(int *utest_result, const std::vector<Cycle> &a,
                    const std::vector<Cycle> &b, size_t from)
{
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = from; i < a.size(); i++)
    {
        ASSERT_EQ(a[i].addr, b[i].addr);
        ASSERT_EQ((int)a[i].we, (int)b[i].we);
        ASSERT_EQ((int)a[i].sync, (int)b[i].sync);
        ASSERT_EQ(a[i].data, b[i].data);
    }
}

static const std::vector<uint8_t> BUSY = {
    0x58,             // CLI
    0xA9, 0x37,       // LDA #$37
    0xA2, 0x10,       // LDX #$10
    0xA0, 0x05,       // LDY #$05
    0x9D, 0x00, 0x02, // STA $0200,X   <- loop
    0x48,             // PHA
    0x69, 0x11,       // ADC #$11
    0xEE, 0x00, 0x02, // INC $0200
    0x68,             // PLA
    0xCA,             // DEX
    0x88,             // DEY
    0xD0, 0xF2,       // BNE loop
    0x4C, 0x00, 0x40, // JMP $4000
};

UTEST(freeze, a_restored_core_drives_the_same_bus)
{
    load(0x4000, BUSY);
    std::vector<Cycle> ref = reference(400);
    for (int at : {7, 23, 51, 96, 150})
    {
        load(0x4000, BUSY);
        bool landed = false;
        std::vector<Cycle> got = frozen(&landed, 400, at, 40, -1, LAND_ANY);
        ASSERT_TRUE(landed);
        compare(utest_result, ref, got, 0);
    }
}

UTEST(freeze, holding_longer_changes_nothing)
{
    load(0x4000, BUSY);
    std::vector<Cycle> ref = reference(300);
    for (int hold : {0, 1, 2, 999})
    {
        load(0x4000, BUSY);
        bool landed = false;
        std::vector<Cycle> got = frozen(&landed, 300, 40, hold, -1, LAND_ANY);
        ASSERT_TRUE(landed);
        compare(utest_result, ref, got, 0);
    }
}

UTEST(freeze, an_interrupt_still_in_the_pipeline_survives_the_freeze)
{
    int in_flight = 0, taken = 0;
    for (int pulse = 55; pulse < 80; pulse++)
    {
        load(0x4000, BUSY);
        make_dut();
        std::vector<Cycle> ref;
        for (int i = 0; i < 200; i++)
        {
            if (i == pulse)
            {
                dut->irq_i = 1;
                dut->eval();
                ref.push_back(step());
                dut->irq_i = 0;
                dut->eval();
                continue;
            }
            ref.push_back(step());
        }
        for (const Cycle &c : ref)
            if (c.sync && c.addr == 0x9000)
            {
                taken++;
                break;
            }

        load(0x4000, BUSY);
        bool landed = false;
        std::vector<Cycle> got =
            frozen(&landed, 200, pulse + 1, 25, pulse, LAND_ANY);
        ASSERT_TRUE(landed);
        if (frozen_pip)
            in_flight++;
        compare(utest_result, ref, got, 0);
    }
    ASSERT_TRUE(taken > 0);
    ASSERT_TRUE(in_flight > 0);
}

UTEST(freeze, a_core_waiting_is_restored_still_waiting)
{
    const std::vector<uint8_t> wai = {
        0xA9, 0x5A, // LDA #$5A
        0xCB,       // WAI
        0xA2, 0x11, // LDX #$11
        0x4C, 0x00, 0x40,
    };
    load(0x4000, wai);
    std::vector<Cycle> ref = reference(120);
    load(0x4000, wai);
    bool landed = false;
    std::vector<Cycle> got = frozen(&landed, 120, 8, 50, -1, LAND_STALLED);
    ASSERT_TRUE(landed);
    compare(utest_result, ref, got, 0);
}

UTEST(freeze, a_stopped_core_is_restored_stopped)
{
    const std::vector<uint8_t> stp = {
        0xA9, 0x11, // LDA #$11
        0xDB,       // STP
    };
    load(0x4000, stp);
    std::vector<Cycle> ref = reference(60);
    load(0x4000, stp);
    bool landed = false;
    std::vector<Cycle> got = frozen(&landed, 60, 6, 30, -1, LAND_STALLED);
    ASSERT_TRUE(landed);
    compare(utest_result, ref, got, 0);
    ASSERT_TRUE((int)dut->cpu_stp);
}

UTEST_MAIN()
