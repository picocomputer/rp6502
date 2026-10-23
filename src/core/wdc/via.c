/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define CHIPS_IMPL
#include "chips/chips/m6522.h"
#include "core/wdc/via.h"

static m6522_t via;

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

static void via_put_port(sst_cursor_t *c, const m6522_port_t *p)
{
    sst_put_u8(c, p->inpr);
    sst_put_u8(c, p->outr);
    sst_put_u8(c, p->ddr);
    sst_put_u8(c, p->pins);
    sst_put_bool(c, p->c1_in);
    sst_put_bool(c, p->c1_out);
    sst_put_bool(c, p->c1_triggered);
    sst_put_bool(c, p->c2_in);
    sst_put_bool(c, p->c2_out);
    sst_put_bool(c, p->c2_triggered);
}

static void via_get_port(sst_cursor_t *c, m6522_port_t *p)
{
    p->inpr = sst_get_u8(c);
    p->outr = sst_get_u8(c);
    p->ddr = sst_get_u8(c);
    p->pins = sst_get_u8(c);
    p->c1_in = sst_get_bool(c);
    p->c1_out = sst_get_bool(c);
    p->c1_triggered = sst_get_bool(c);
    p->c2_in = sst_get_bool(c);
    p->c2_out = sst_get_bool(c);
    p->c2_triggered = sst_get_bool(c);
}

static void via_put_timer(sst_cursor_t *c, const m6522_timer_t *t)
{
    sst_put_u16(c, t->latch);
    sst_put_u16(c, t->counter);
    sst_put_bool(c, t->t_bit);
    sst_put_bool(c, t->t_out);
    sst_put_u16(c, t->pip);
}

static void via_get_timer(sst_cursor_t *c, m6522_timer_t *t)
{
    t->latch = sst_get_u16(c);
    t->counter = sst_get_u16(c);
    t->t_bit = sst_get_bool(c);
    t->t_out = sst_get_bool(c);
    t->pip = sst_get_u16(c);
}

void via_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    via_put_port(c, &via.pa);
    via_put_port(c, &via.pb);
    via_put_timer(c, &via.t1);
    via_put_timer(c, &via.t2);
    sst_put_u8(c, via.intr.ier);
    sst_put_u8(c, via.intr.ifr);
    sst_put_u16(c, via.intr.pip);
    sst_put_u8(c, via.acr);
    sst_put_u8(c, via.pcr);
    sst_put_u64(c, via.pins);
}

bool via_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    m6522_t in;
    via_get_port(c, &in.pa);
    via_get_port(c, &in.pb);
    via_get_timer(c, &in.t1);
    via_get_timer(c, &in.t2);
    in.intr.ier = sst_get_u8(c);
    in.intr.ifr = sst_get_u8(c);
    in.intr.pip = sst_get_u16(c);
    in.acr = sst_get_u8(c);
    in.pcr = sst_get_u8(c);
    in.pins = sst_get_u64(c);
    if (!sst_ok(c))
        return false;
    via = in;
    return true;
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
