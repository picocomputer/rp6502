/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define CHIPS_IMPL
#include "chips/chips/m6522.h"
#include "core/wdc/via.h"

static m6522_t via;

/* Every field of the vendored model, because the delay pipelines inside the
 * timers and the interrupt logic cannot be re-derived from the registers: a
 * timer one cycle from underflowing looks like any other. */
#define VIA_PORT_ROWS(Y, p)                                     \
    Y(U8, p.inpr) Y(U8, p.outr) Y(U8, p.ddr) Y(U8, p.pins)      \
    Y(BOOL, p.c1_in) Y(BOOL, p.c1_out) Y(BOOL, p.c1_triggered)  \
    Y(BOOL, p.c2_in) Y(BOOL, p.c2_out) Y(BOOL, p.c2_triggered)
#define VIA_TIMER_ROWS(Y, t)                                    \
    Y(U16, t.latch) Y(U16, t.counter) Y(BOOL, t.t_bit)          \
    Y(BOOL, t.t_out) Y(U16, t.pip)
#define VIA_SST(X)                                              \
    X(STRUCT, via.pa, VIA_PORT_ROWS)                            \
    X(STRUCT, via.pb, VIA_PORT_ROWS)                            \
    X(STRUCT, via.t1, VIA_TIMER_ROWS)                           \
    X(STRUCT, via.t2, VIA_TIMER_ROWS)                           \
    X(U8, via.intr.ier)                                         \
    X(U8, via.intr.ifr)                                         \
    X(U16, via.intr.pip)                                        \
    X(U8, via.acr)                                              \
    X(U8, via.pcr)                                              \
    X(U64, via.pins)
SST_TABLE(via, VIA_SST)

void via_reset(void)
{
    m6522_init(&via);
}

void *via_chip(void) { return &via; }

/* The read runs on a copy, so the flags and handshakes it would clear stay as
 * they were. */
uint8_t via_reg_peek(uint16_t addr)
{
    m6522_t copy = via;
    return _m6522_read(&copy, (uint8_t)(addr & 0xF));
}

/* Each register is set to what a read of it returns. A port sets its output
 * register, because nothing is wired to drive its inputs. The model has no
 * shift register. */
void via_reg_poke(uint16_t addr, uint8_t data)
{
    switch (addr & 0xF)
    {
    case M6522_REG_RB:
        via.pb.outr = data;
        via.pb.pins = data & via.pb.ddr;
        break;
    case M6522_REG_RA:
    case M6522_REG_RA_NOH:
        via.pa.outr = data;
        via.pa.pins = data & via.pa.ddr;
        break;
    case M6522_REG_DDRB:
        via.pb.ddr = data;
        break;
    case M6522_REG_DDRA:
        via.pa.ddr = data;
        break;
    case M6522_REG_T1CL:
        via.t1.counter = (uint16_t)((via.t1.counter & 0xFF00) | data);
        break;
    case M6522_REG_T1CH:
        via.t1.counter = (uint16_t)((via.t1.counter & 0x00FF) | data << 8);
        break;
    case M6522_REG_T1LL:
        via.t1.latch = (uint16_t)((via.t1.latch & 0xFF00) | data);
        break;
    case M6522_REG_T1LH:
        via.t1.latch = (uint16_t)((via.t1.latch & 0x00FF) | data << 8);
        break;
    case M6522_REG_T2CL:
        via.t2.counter = (uint16_t)((via.t2.counter & 0xFF00) | data);
        break;
    case M6522_REG_T2CH:
        via.t2.counter = (uint16_t)((via.t2.counter & 0x00FF) | data << 8);
        break;
    case M6522_REG_ACR:
        via.acr = data;
        break;
    case M6522_REG_PCR:
        via.pcr = data;
        break;
    case M6522_REG_IFR:
        via.intr.ifr = data;
        break;
    case M6522_REG_IER:
        via.intr.ier = data & 0x7F;
        break;
    }
}

void via_reg_write(uint16_t addr, uint8_t data)
{
    _m6522_write(&via, (uint8_t)(addr & 0xF), data);
}

bool via_tick(uint16_t addr, bool read, uint8_t *data)
{
    /* The VIA's pin word is built fresh from the bus each cycle. PA/PB/CA/CB
     * are left clear because nothing is wired to this VIA's ports, so its
     * inputs read low. Carrying the pin word across cycles instead would feed
     * the chip its own driven outputs back as inputs, which latches a bit high
     * forever once DDR flips that line to an input. */
    const bool selected = addr >= VIA_MMAP_LO && addr <= VIA_MMAP_HI;
    uint64_t pins = addr & M6522_RS_PINS;
    if (read)
        pins |= M6522_RW;
    else
        M6522_SET_DATA(pins, *data);
    if (selected)
        pins |= M6522_CS1; /* CS2 is held low, so CS1 high == selected */

    pins = m6522_tick(&via, pins);

    if (selected && read)
        *data = M6522_GET_DATA(pins);
    return (pins & M6522_IRQ) != 0;
}
