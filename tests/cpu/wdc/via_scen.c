/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The register numbers follow the 6522 register map. Registers 0x0 and 0x1 are
 * ORB and ORA when written and IRB and IRA when read, 0x2 and 0x3 are DDRB and
 * DDRA, 0x4 and 0x5 are T1CL and T1CH, 0x6 and 0x7 are T1LL and T1LH, 0x8 and
 * 0x9 are T2CL and T2CH, 0xA is SR, 0xB is ACR, 0xC is PCR, 0xD is IFR, 0xE is
 * IER, and 0xF is register 0x1 without the port A handshake. Writes to 0x4 and
 * 0x8 set the low bytes of the T1 and T2 latches, and writes to 0x5 and 0x9
 * set the high bytes and then load each timer's counter from its latch.
 */

#include "via_scen.h"

#define VIA_SCEN(name, ...)                                  \
    const via_op_t via_scen_##name[] = __VA_ARGS__;       \
    const size_t via_scen_##name##_n =                       \
        sizeof via_scen_##name / sizeof *via_scen_##name;

VIA_SCEN(t1_oneshot, {
    {VIA_OP_WRITE, 0xE, 0xC0, 0},
    {VIA_OP_WRITE, 0x4, 10, 0},
    {VIA_OP_WRITE, 0x5, 0, 0},
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
    {VIA_OP_READ, 0x4, 0, 0},
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
})

VIA_SCEN(t1_continuous, {
    {VIA_OP_WRITE, 0xB, 0x40, 0},
    {VIA_OP_WRITE, 0xE, 0xC0, 0},
    {VIA_OP_WRITE, 0x4, 6, 0},
    {VIA_OP_WRITE, 0x5, 0, 0},
    {VIA_OP_IDLE, 0, 0, 30},
    {VIA_OP_READ, 0x4, 0, 0},
    {VIA_OP_IDLE, 0, 0, 30},
    {VIA_OP_READ, 0xD, 0, 0},
})

VIA_SCEN(t1_pb7, {
    {VIA_OP_WRITE, 0x2, 0x80, 0},
    {VIA_OP_WRITE, 0xB, 0xC0, 0},
    {VIA_OP_WRITE, 0x4, 4, 0},
    {VIA_OP_WRITE, 0x5, 0, 0},
    {VIA_OP_IDLE, 0, 0, 3},
    {VIA_OP_READ, 0x0, 0, 0},
    {VIA_OP_IDLE, 0, 0, 3},
    {VIA_OP_READ, 0x0, 0, 0},
    {VIA_OP_IDLE, 0, 0, 3},
    {VIA_OP_READ, 0x0, 0, 0},
    {VIA_OP_IDLE, 0, 0, 3},
    {VIA_OP_READ, 0x0, 0, 0},
})

VIA_SCEN(t2_oneshot, {
    {VIA_OP_WRITE, 0xE, 0xA0, 0},
    {VIA_OP_WRITE, 0x8, 8, 0},
    {VIA_OP_WRITE, 0x9, 0, 0},
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
    {VIA_OP_READ, 0x8, 0, 0},
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
})

/* In PB6 count mode, T2 counts falling edges on PB6. Port inputs read zero
 * and are compared with the PB6 level driven on the previous cycle, so T2
 * counts down on every cycle while PB6 is driven high. */
VIA_SCEN(t2_pb6_quirk, {
    {VIA_OP_WRITE, 0x2, 0x40, 0},
    {VIA_OP_WRITE, 0x0, 0x40, 0},
    {VIA_OP_WRITE, 0xB, 0x20, 0},
    {VIA_OP_WRITE, 0xE, 0xA0, 0},
    {VIA_OP_WRITE, 0x8, 5, 0},
    {VIA_OP_WRITE, 0x9, 0, 0},
    {VIA_OP_IDLE, 0, 0, 20},
    {VIA_OP_READ, 0xD, 0, 0},
    {VIA_OP_READ, 0x8, 0, 0},
})

VIA_SCEN(ifr_ier, {
    {VIA_OP_WRITE, 0xE, 0xE0, 0},
    {VIA_OP_READ, 0xE, 0, 0},
    {VIA_OP_WRITE, 0xE, 0x40, 0},
    {VIA_OP_READ, 0xE, 0, 0},
    {VIA_OP_WRITE, 0x4, 4, 0},
    {VIA_OP_WRITE, 0x5, 0, 0},
    {VIA_OP_IDLE, 0, 0, 20},
    {VIA_OP_READ, 0xD, 0, 0},
    {VIA_OP_WRITE, 0xD, 0x7F, 0},
    {VIA_OP_READ, 0xD, 0, 0},
})

VIA_SCEN(readback_all, {
    {VIA_OP_WRITE, 0x0, 0xAA, 0},
    {VIA_OP_WRITE, 0x1, 0x55, 0},
    {VIA_OP_WRITE, 0x2, 0xF0, 0},
    {VIA_OP_WRITE, 0x3, 0x0F, 0},
    {VIA_OP_WRITE, 0xB, 0x00, 0},
    {VIA_OP_WRITE, 0xC, 0x21, 0},
    {VIA_OP_READ, 0x0, 0, 0},
    {VIA_OP_READ, 0x1, 0, 0},
    {VIA_OP_READ, 0x2, 0, 0},
    {VIA_OP_READ, 0x3, 0, 0},
    {VIA_OP_READ, 0x6, 0, 0},
    {VIA_OP_READ, 0x7, 0, 0},
    {VIA_OP_READ, 0xA, 0, 0},
    {VIA_OP_READ, 0xB, 0, 0},
    {VIA_OP_READ, 0xC, 0, 0},
    {VIA_OP_READ, 0xF, 0, 0},
})

void via_fuzz_next(uint16_t *lfsr, via_op_t *out)
{
    uint16_t s = *lfsr;
    s = (uint16_t)((s >> 1) ^ (-(int)(s & 1) & 0xB400));
    *lfsr = s;
    out->kind = VIA_OP_IDLE;
    out->rs = 0;
    out->data = 0;
    out->repeat = 0;
    if ((s & 0x0F) < 3)
    {
        out->kind = VIA_OP_WRITE;
        out->rs = (uint8_t)((s >> 4) & 0x0F);
        out->data = (uint8_t)(s >> 8);
    }
    else if ((s & 0x0F) < 6)
    {
        out->kind = VIA_OP_READ;
        out->rs = (uint8_t)((s >> 4) & 0x0F);
    }
}
