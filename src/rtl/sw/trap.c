/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

#define TRAP_CONSOLE (*(volatile uint32_t *)0xF0000000u)

static void trap_hex(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        TRAP_CONSOLE = (uint8_t)hex[(v >> i) & 15];
    TRAP_CONSOLE = '\n';
}

static void trap_spin(uint32_t cause, uint32_t epc, uint32_t addr)
{
    TRAP_CONSOLE = '!';
    trap_hex(cause);
    trap_hex(epc);
    trap_hex(addr);
    for (;;)
        ;
}

void __assert_func(const char *file, int line, const char *func,
                   const char *expr)
{
    (void)func;
    (void)expr;
    TRAP_CONSOLE = '!';
    for (const char *s = file; *s; s++)
        TRAP_CONSOLE = (uint8_t)*s;
    TRAP_CONSOLE = ':';
    trap_hex((uint32_t)line);
    for (;;)
        ;
}

typedef struct
{
    uint32_t x[31];
} trap_frame_t;

static uint32_t reg_get(const trap_frame_t *f, uint32_t r)
{
    return r ? f->x[r - 1] : 0;
}

static void reg_set(trap_frame_t *f, uint32_t r, uint32_t v)
{
    if (r)
        f->x[r - 1] = v;
}

static uint32_t load_bytes(uint32_t addr, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++)
        v |= (uint32_t)(*(volatile uint8_t *)(addr + i)) << (8 * i);
    return v;
}

static void store_bytes(uint32_t addr, uint32_t size, uint32_t v)
{
    for (uint32_t i = 0; i < size; i++)
        *(volatile uint8_t *)(addr + i) = (uint8_t)(v >> (8 * i));
}

void trap_dispatch(trap_frame_t *frame)
{
    uint32_t cause, epc;
    __asm__ volatile("csrr %0, mcause" : "=r"(cause));
    __asm__ volatile("csrr %0, mepc" : "=r"(epc));
    uint32_t addr = 0;

    if (cause != 4 && cause != 6)
        trap_spin(cause, epc, 0);

    uint16_t lo = *(const uint16_t *)epc;
    uint32_t next = epc;

    if ((lo & 3) == 3)
    {
        uint32_t insn = lo | ((uint32_t)(*(const uint16_t *)(epc + 2)) << 16);
        uint32_t funct3 = (insn >> 12) & 7;
        uint32_t base = reg_get(frame, (insn >> 15) & 31);
        next += 4;
        if (cause == 4)
        {
            uint32_t rd = (insn >> 7) & 31;
            addr = base + (uint32_t)(((int32_t)insn) >> 20);
            if (funct3 == 1)
                reg_set(frame, rd, (uint32_t)(int32_t)(int16_t)load_bytes(addr, 2));
            else if (funct3 == 5)
                reg_set(frame, rd, load_bytes(addr, 2));
            else if (funct3 == 2)
                reg_set(frame, rd, load_bytes(addr, 4));
            else
                trap_spin(cause, epc, addr);
        }
        else
        {
            uint32_t rs2 = (insn >> 20) & 31;
            addr = base + (uint32_t)((((int32_t)insn >> 20) & ~31)
                                     | (int32_t)((insn >> 7) & 31));
            if (funct3 == 1)
                store_bytes(addr, 2, reg_get(frame, rs2));
            else if (funct3 == 2)
                store_bytes(addr, 4, reg_get(frame, rs2));
            else
                trap_spin(cause, epc, addr);
        }
    }
    else
    {
        uint32_t op = lo & 3;
        uint32_t funct3 = (lo >> 13) & 7;
        uint32_t cbase = reg_get(frame, 8 + ((lo >> 7) & 7));
        addr = cbase + ((((lo >> 5) & 1) << 6)
                        | (((lo >> 10) & 7) << 3) | (((lo >> 6) & 1) << 2));
        next += 2;
        if (op == 0 && funct3 == 2)
            reg_set(frame, 8 + ((lo >> 2) & 7), load_bytes(addr, 4));
        else if (op == 0 && funct3 == 6)
            store_bytes(addr, 4, reg_get(frame, 8 + ((lo >> 2) & 7)));
        else
            trap_spin(cause, epc, addr);
    }

    __asm__ volatile("csrw mepc, %0" : : "r"(next));
}
