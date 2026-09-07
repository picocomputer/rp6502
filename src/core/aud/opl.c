/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <emu8950/emu8950.h>

#define OPL_CLOCK_RATE 3579552

static OPL *opl_emu8950;

/* Where the register page sits, and the first of the four OR-mask rows the
 * slots point into. The base is taken after a reset, which sets every slot
 * to row zero, so a slot's row is that many uint16_t[4] on from here. That
 * index is what goes on the wire; the address is this build's own. */
static uint16_t opl_xaddr = 0xFFFF;
static const uint16_t *opl_wave_base;

#pragma GCC push_options
#pragma GCC optimize("O3")
int16_t opl_sample(void)
{
    int16_t next;
    OPL_calc_buffer(opl_emu8950, &next, 1);
    /* Four times hot, and the clamp lets the loud parts square off — the
     * machine has always run its OPL this way. It used to reach the same
     * ratio by shifting emu8950's sixteen bits down to ten, which threw
     * six of them away at the source, before any host with a better
     * converter than the RP2350's PWM could see them. Multiplying instead
     * of shifting keeps every bit and clips in exactly the same place. */
    int32_t s = (int32_t)next * 4;
    if (s < AUD_SAMPLE_MIN)
        s = AUD_SAMPLE_MIN;
    if (s > AUD_SAMPLE_MAX)
        s = AUD_SAMPLE_MAX;

    // Update opl regs from xram
    uint8_t max_work = 8;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        atomic_thread_fence(memory_order_acquire); /* the entry behind the head */
        uint8_t tail = ++xram_queue_tail;
        OPL_writeReg(opl_emu8950,
                     xram_queue[tail][0],
                     xram_queue[tail][1]);
    }
    return (int16_t)s;
}

/* What a mixer registers: the one voice this chip has, on both sides. */
void opl_stereo(int16_t *left, int16_t *right)
{
    *left = *right = opl_sample();
}
#pragma GCC pop_options

uint16_t opl_xaddr_get(void) { return opl_xaddr; }

void opl_park(void) { opl_xaddr = 0xFFFF; }

bool opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        /* Giving up control resets the chip and hands the mix back, so a
         * stopped program's last chord does not hold. */
        if (opl_emu8950)
            OPL_reset(opl_emu8950);
        opl_xaddr = 0xFFFF;
        aud_stop();
        return word == 0xFFFF;
    }
    // Would be nice to not malloc but initializeTables() is static
    if (!opl_emu8950)
        /* A YM3812 samples at its clock over 72, and that is the rate the
         * whole soft machine adopted; AUD_NATIVE_RATE is this chip's number
         * before it is anyone else's. */
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, AUD_NATIVE_RATE);
    assert(opl_emu8950); // OPL_new only fails under memory pressure (a debug build)
    OPL_reset(opl_emu8950);
    opl_wave_base = opl_emu8950->slot[0].wav_or_table;
    opl_xaddr = word;
    xram_queue_page = word >> 8;
    memset((uint8_t *)&xram[word], 0, 256);
    xram_queue_tail = xram_queue_head;
    /* One engine sounds at a time; see psg_xreg. */
    psg_park();
    aud_setup(aud_dev_opl);
    return true;
}

static void opl_put_patch(sst_cursor_t *c, const OPL_PATCH *p)
{
    sst_put_u8(c, p->TL4);
    sst_put_u8(c, p->KL_SHIFT);
    sst_put_u8(c, p->FB);
    sst_put_u8(c, p->EG);
    sst_put_u8(c, p->ML);
    sst_put_u8(c, p->AR);
    sst_put_u8(c, p->DR);
    sst_put_u8(c, p->SL);
    sst_put_u8(c, p->RR);
    sst_put_u8(c, p->KR);
    sst_put_u8(c, p->AM);
    sst_put_u8(c, p->PM);
    sst_put_u8(c, p->WS);
}

static void opl_get_patch(sst_cursor_t *c, OPL_PATCH *p)
{
    p->TL4 = sst_get_u8(c);
    p->KL_SHIFT = sst_get_u8(c);
    p->FB = sst_get_u8(c);
    p->EG = sst_get_u8(c);
    p->ML = sst_get_u8(c);
    p->AR = sst_get_u8(c);
    p->DR = sst_get_u8(c);
    p->SL = sst_get_u8(c);
    p->RR = sst_get_u8(c);
    p->KR = sst_get_u8(c);
    p->AM = sst_get_u8(c);
    p->PM = sst_get_u8(c);
    p->WS = sst_get_u8(c);
}

static void opl_put_slot(sst_cursor_t *c, const OPL_SLOT *s)
{
    sst_put_u8(c, s->number);
    sst_put_u8(c, s->type);
    opl_put_patch(c, &s->__patch);
    sst_put_i32(c, s->output[0]);
    sst_put_i32(c, s->output[1]);
    sst_put_u8(c, (uint8_t)((s->wav_or_table - opl_wave_base) / 4));
    sst_put_u32(c, s->pg_phase);
    sst_put_u32(c, s->pg_out);
    sst_put_u8(c, s->pg_keep);
    sst_put_u16(c, s->blk_fnum);
    sst_put_u16(c, s->fnum);
    sst_put_u8(c, s->blk);
    sst_put_u8(c, s->eg_state);
    sst_put_u16(c, s->tll);
    sst_put_u8(c, s->rks);
    sst_put_u8(c, s->eg_rate_h);
    sst_put_u8(c, s->eg_rate_l);
    sst_put_u32(c, s->eg_shift);
    sst_put_u16(c, (uint16_t)s->eg_out);
    sst_put_u32(c, s->update_requests);
}

static bool opl_get_slot(sst_cursor_t *c, OPL_SLOT *s)
{
    s->number = sst_get_u8(c);
    s->type = sst_get_u8(c);
    opl_get_patch(c, &s->__patch);
    s->patch = &s->__patch; /* every slot plays its own, always */
    s->output[0] = sst_get_i32(c);
    s->output[1] = sst_get_i32(c);
    uint8_t row = sst_get_u8(c);
    if (row > 3)
        return false;
    s->wav_or_table = (uint16_t *)(opl_wave_base + row * 4);
    s->pg_phase = sst_get_u32(c);
    s->pg_out = sst_get_u32(c);
    s->pg_keep = sst_get_u8(c);
    s->blk_fnum = sst_get_u16(c);
    s->fnum = sst_get_u16(c);
    s->blk = sst_get_u8(c);
    s->eg_state = sst_get_u8(c);
    s->tll = sst_get_u16(c);
    s->rks = sst_get_u8(c);
    s->eg_rate_h = sst_get_u8(c);
    s->eg_rate_l = sst_get_u8(c);
    s->eg_shift = sst_get_u32(c);
    s->eg_out = (int16_t)sst_get_u16(c);
    s->update_requests = sst_get_u32(c);
    return true;
}

void opl_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const OPL *o = opl_emu8950;
    sst_put_bool(c, o != NULL);
    sst_put_u16(c, opl_xaddr);
    if (!o)
    {
        /* The slot is still a fixed slot, so the walk pads it out. */
        return;
    }
    for (int i = 0; i < 0x100; i++)
        sst_put_u8(c, o->reg[i]);
    for (int i = 0; i < 9; i++)
        sst_put_u8(c, o->ch_alg[i]);
    sst_put_u8(c, o->csm_mode);
    sst_put_u8(c, o->csm_key_count);
    sst_put_u8(c, o->notesel);
    sst_put_u32(c, o->slot_key_status);
    sst_put_u8(c, o->perc_mode);
    sst_put_u32(c, o->eg_counter);
    sst_put_u32(c, o->pm_phase);
    sst_put_u32(c, o->pm_dphase);
    sst_put_u8(c, o->am_phase_index);
    sst_put_u8(c, o->lfo_am);
    sst_put_u32(c, o->noise);
    sst_put_u8(c, o->short_noise);
    sst_put_u8(c, o->am_mode);
    sst_put_u8(c, o->pm_mode);
    sst_put_u32(c, o->timer1_counter);
    sst_put_u32(c, o->timer2_counter);
    sst_put_u8(c, o->status);
    for (int i = 0; i < 15; i++)
        sst_put_u16(c, (uint16_t)o->ch_out[i]);
    for (int i = 0; i < 18; i++)
        opl_put_slot(c, &o->slot[i]);
}

bool opl_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool present = sst_get_bool(c);
    uint16_t at = sst_get_u16(c);
    if (!sst_ok(c) || (at != 0xFFFF && (at & 0x00FF)))
        return false;
    opl_xaddr = at;
    if (!present)
    {
        /* A chip this machine allocated stays allocated -- OPL_new is a
         * malloc and a table build, not machine state -- but it goes back
         * to what a program would find on taking it. */
        if (opl_emu8950)
            OPL_reset(opl_emu8950);
        return true;
    }
    if (!opl_emu8950)
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, AUD_NATIVE_RATE);
    if (!opl_emu8950)
        return false;
    /* Reset first: it is what settles the members this blob does not carry,
     * and it is where the wave rows are addressed from. */
    OPL_reset(opl_emu8950);
    opl_wave_base = opl_emu8950->slot[0].wav_or_table;

    OPL *o = opl_emu8950;
    for (int i = 0; i < 0x100; i++)
        o->reg[i] = sst_get_u8(c);
    for (int i = 0; i < 9; i++)
        o->ch_alg[i] = sst_get_u8(c);
    o->csm_mode = sst_get_u8(c);
    o->csm_key_count = sst_get_u8(c);
    o->notesel = sst_get_u8(c);
    o->slot_key_status = sst_get_u32(c);
    o->perc_mode = sst_get_u8(c);
    o->eg_counter = sst_get_u32(c);
    o->pm_phase = sst_get_u32(c);
    o->pm_dphase = sst_get_u32(c);
    o->am_phase_index = sst_get_u8(c);
    o->lfo_am = sst_get_u8(c);
    o->noise = sst_get_u32(c);
    o->short_noise = sst_get_u8(c);
    o->am_mode = sst_get_u8(c);
    o->pm_mode = sst_get_u8(c);
    o->timer1_counter = sst_get_u32(c);
    o->timer2_counter = sst_get_u32(c);
    o->status = sst_get_u8(c);
    for (int i = 0; i < 15; i++)
        o->ch_out[i] = (int16_t)sst_get_u16(c);
    for (int i = 0; i < 18; i++)
        if (!opl_get_slot(c, &o->slot[i]))
            return false;
    return sst_ok(c);
}
