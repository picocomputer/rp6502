/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The scenarios themselves. Register numbers are the 6522's, so 0x4/0x5 are
 * T1CL/T1CH, 0x8/0x9 are T2CL/T2CH, 0xB is ACR, 0xD is IFR and 0xE is IER.
 */

#include "w65c22_scen.h"

#define W65C22_SCEN(name, ...)                                  \
    const w65c22_op_t w65c22_scen_##name[] = __VA_ARGS__;       \
    const size_t w65c22_scen_##name##_n =                       \
        sizeof w65c22_scen_##name / sizeof *w65c22_scen_##name;

/* T1 one-shot: interrupt once, cleared by reading T1CL. */
W65C22_SCEN(t1_oneshot, {
    {W65C22_OP_WRITE, 0xE, 0xC0, 0}, /* IER: enable T1 */
    {W65C22_OP_WRITE, 0x4, 10, 0},   /* T1 latch low */
    {W65C22_OP_WRITE, 0x5, 0, 0},    /* T1 go */
    {W65C22_OP_IDLE, 0, 0, 40},
    {W65C22_OP_READ, 0xD, 0, 0}, /* IFR */
    {W65C22_OP_READ, 0x4, 0, 0}, /* T1CL: clears */
    {W65C22_OP_IDLE, 0, 0, 40},
    {W65C22_OP_READ, 0xD, 0, 0},
})

/* T1 continuous: interrupt on every underflow. */
W65C22_SCEN(t1_continuous, {
    {W65C22_OP_WRITE, 0xB, 0x40, 0}, /* ACR: continuous */
    {W65C22_OP_WRITE, 0xE, 0xC0, 0},
    {W65C22_OP_WRITE, 0x4, 6, 0},
    {W65C22_OP_WRITE, 0x5, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 30},
    {W65C22_OP_READ, 0x4, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 30},
    {W65C22_OP_READ, 0xD, 0, 0},
})

/* T1 drives PB7 when ACR bit 7 is set. */
W65C22_SCEN(t1_pb7, {
    {W65C22_OP_WRITE, 0x2, 0x80, 0}, /* DDRB: PB7 out */
    {W65C22_OP_WRITE, 0xB, 0xC0, 0}, /* ACR: continuous + PB7 */
    {W65C22_OP_WRITE, 0x4, 4, 0},
    {W65C22_OP_WRITE, 0x5, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 3},
    {W65C22_OP_READ, 0x0, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 3},
    {W65C22_OP_READ, 0x0, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 3},
    {W65C22_OP_READ, 0x0, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 3},
    {W65C22_OP_READ, 0x0, 0, 0},
})

/* T2 one-shot, cleared by reading T2CL; no reload from latch. */
W65C22_SCEN(t2_oneshot, {
    {W65C22_OP_WRITE, 0xE, 0xA0, 0}, /* IER: enable T2 */
    {W65C22_OP_WRITE, 0x8, 8, 0},
    {W65C22_OP_WRITE, 0x9, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 40},
    {W65C22_OP_READ, 0xD, 0, 0},
    {W65C22_OP_READ, 0x8, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 40},
    {W65C22_OP_READ, 0xD, 0, 0},
})

/* The PB6 count-mode quirk: with PB6 driven high, the reference sees a
 * falling edge against its zeroed port inputs every cycle. */
W65C22_SCEN(t2_pb6_quirk, {
    {W65C22_OP_WRITE, 0x2, 0x40, 0}, /* DDRB: PB6 out */
    {W65C22_OP_WRITE, 0x0, 0x40, 0}, /* ORB: PB6 high */
    {W65C22_OP_WRITE, 0xB, 0x20, 0}, /* ACR: T2 counts PB6 */
    {W65C22_OP_WRITE, 0xE, 0xA0, 0},
    {W65C22_OP_WRITE, 0x8, 5, 0},
    {W65C22_OP_WRITE, 0x9, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 20},
    {W65C22_OP_READ, 0xD, 0, 0},
    {W65C22_OP_READ, 0x8, 0, 0},
})

/* IER set/clear addressing, IFR write-to-clear, bit 7 readback. */
W65C22_SCEN(ifr_ier, {
    {W65C22_OP_WRITE, 0xE, 0xE0, 0}, /* set T1+T2 enables */
    {W65C22_OP_READ, 0xE, 0, 0},
    {W65C22_OP_WRITE, 0xE, 0x40, 0}, /* clear T1 enable */
    {W65C22_OP_READ, 0xE, 0, 0},
    {W65C22_OP_WRITE, 0x4, 4, 0},
    {W65C22_OP_WRITE, 0x5, 0, 0},
    {W65C22_OP_IDLE, 0, 0, 20},
    {W65C22_OP_READ, 0xD, 0, 0},     /* T1 flag, no master (masked) */
    {W65C22_OP_WRITE, 0xD, 0x7F, 0}, /* clear all flags */
    {W65C22_OP_READ, 0xD, 0, 0},
})

W65C22_SCEN(readback_all, {
    {W65C22_OP_WRITE, 0x0, 0xAA, 0},
    {W65C22_OP_WRITE, 0x1, 0x55, 0},
    {W65C22_OP_WRITE, 0x2, 0xF0, 0},
    {W65C22_OP_WRITE, 0x3, 0x0F, 0},
    {W65C22_OP_WRITE, 0xB, 0x00, 0},
    {W65C22_OP_WRITE, 0xC, 0x21, 0},
    {W65C22_OP_READ, 0x0, 0, 0},
    {W65C22_OP_READ, 0x1, 0, 0},
    {W65C22_OP_READ, 0x2, 0, 0},
    {W65C22_OP_READ, 0x3, 0, 0},
    {W65C22_OP_READ, 0x6, 0, 0},
    {W65C22_OP_READ, 0x7, 0, 0},
    {W65C22_OP_READ, 0xA, 0, 0},
    {W65C22_OP_READ, 0xB, 0, 0},
    {W65C22_OP_READ, 0xC, 0, 0},
    {W65C22_OP_READ, 0xF, 0, 0},
})

void w65c22_fuzz_next(uint16_t *lfsr, w65c22_op_t *out)
{
    uint16_t s = *lfsr;
    s = (uint16_t)((s >> 1) ^ (-(int)(s & 1) & 0xB400));
    *lfsr = s;
    out->kind = W65C22_OP_IDLE;
    out->rs = 0;
    out->data = 0;
    out->repeat = 0;
    if ((s & 0x0F) < 3)
    {
        out->kind = W65C22_OP_WRITE;
        out->rs = (uint8_t)((s >> 4) & 0x0F);
        out->data = (uint8_t)(s >> 8);
    }
    else if ((s & 0x0F) < 6)
    {
        out->kind = W65C22_OP_READ;
        out->rs = (uint8_t)((s >> 4) & 0x0F);
    }
}
