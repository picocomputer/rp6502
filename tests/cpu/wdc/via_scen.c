/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The scenarios themselves. Register numbers are the 6522's, so 0x4/0x5 are
 * T1CL/T1CH, 0x8/0x9 are T2CL/T2CH, 0xB is ACR, 0xD is IFR and 0xE is IER.
 */

#include "via_scen.h"

#define VIA_SCEN(name, ...)                                  \
    const via_op_t via_scen_##name[] = __VA_ARGS__;       \
    const size_t via_scen_##name##_n =                       \
        sizeof via_scen_##name / sizeof *via_scen_##name;

/* T1 one-shot: interrupt once, cleared by reading T1CL. */
VIA_SCEN(t1_oneshot, {
    {VIA_OP_WRITE, 0xE, 0xC0, 0}, /* IER: enable T1 */
    {VIA_OP_WRITE, 0x4, 10, 0},   /* T1 latch low */
    {VIA_OP_WRITE, 0x5, 0, 0},    /* T1 go */
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0}, /* IFR */
    {VIA_OP_READ, 0x4, 0, 0}, /* T1CL: clears */
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
})

/* T1 continuous: interrupt on every underflow. */
VIA_SCEN(t1_continuous, {
    {VIA_OP_WRITE, 0xB, 0x40, 0}, /* ACR: continuous */
    {VIA_OP_WRITE, 0xE, 0xC0, 0},
    {VIA_OP_WRITE, 0x4, 6, 0},
    {VIA_OP_WRITE, 0x5, 0, 0},
    {VIA_OP_IDLE, 0, 0, 30},
    {VIA_OP_READ, 0x4, 0, 0},
    {VIA_OP_IDLE, 0, 0, 30},
    {VIA_OP_READ, 0xD, 0, 0},
})

/* T1 drives PB7 when ACR bit 7 is set. */
VIA_SCEN(t1_pb7, {
    {VIA_OP_WRITE, 0x2, 0x80, 0}, /* DDRB: PB7 out */
    {VIA_OP_WRITE, 0xB, 0xC0, 0}, /* ACR: continuous + PB7 */
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

/* T2 one-shot, cleared by reading T2CL; no reload from latch. */
VIA_SCEN(t2_oneshot, {
    {VIA_OP_WRITE, 0xE, 0xA0, 0}, /* IER: enable T2 */
    {VIA_OP_WRITE, 0x8, 8, 0},
    {VIA_OP_WRITE, 0x9, 0, 0},
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
    {VIA_OP_READ, 0x8, 0, 0},
    {VIA_OP_IDLE, 0, 0, 40},
    {VIA_OP_READ, 0xD, 0, 0},
})

/* The PB6 count-mode quirk: with PB6 driven high, the reference sees a
 * falling edge against its zeroed port inputs every cycle. */
VIA_SCEN(t2_pb6_quirk, {
    {VIA_OP_WRITE, 0x2, 0x40, 0}, /* DDRB: PB6 out */
    {VIA_OP_WRITE, 0x0, 0x40, 0}, /* ORB: PB6 high */
    {VIA_OP_WRITE, 0xB, 0x20, 0}, /* ACR: T2 counts PB6 */
    {VIA_OP_WRITE, 0xE, 0xA0, 0},
    {VIA_OP_WRITE, 0x8, 5, 0},
    {VIA_OP_WRITE, 0x9, 0, 0},
    {VIA_OP_IDLE, 0, 0, 20},
    {VIA_OP_READ, 0xD, 0, 0},
    {VIA_OP_READ, 0x8, 0, 0},
})

/* IER set/clear addressing, IFR write-to-clear, bit 7 readback. */
VIA_SCEN(ifr_ier, {
    {VIA_OP_WRITE, 0xE, 0xE0, 0}, /* set T1+T2 enables */
    {VIA_OP_READ, 0xE, 0, 0},
    {VIA_OP_WRITE, 0xE, 0x40, 0}, /* clear T1 enable */
    {VIA_OP_READ, 0xE, 0, 0},
    {VIA_OP_WRITE, 0x4, 4, 0},
    {VIA_OP_WRITE, 0x5, 0, 0},
    {VIA_OP_IDLE, 0, 0, 20},
    {VIA_OP_READ, 0xD, 0, 0},     /* T1 flag, no master (masked) */
    {VIA_OP_WRITE, 0xD, 0x7F, 0}, /* clear all flags */
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
