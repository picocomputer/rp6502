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
 * to row zero, so a slot's row is that many uint16_t[4] on from here. The
 * savestate carries that row index, because the address is this build's
 * own. */
static uint16_t opl_xaddr = 0xFFFF;
static const uint16_t *opl_wave_base;

#pragma GCC push_options
#pragma GCC optimize("O3")
int16_t opl_sample(void)
{
    int16_t next;
    OPL_calc_buffer(opl_emu8950, &next, 1);
    /* Four is the gain that reaches the level the RTL YM3812 sets, where
     * opl.sv's SAMPLE_SHIFT of 5 is unity. The two constants are not
     * independent: move one alone and the platforms drift 12 dB apart. The
     * clamp is what makes both clip at the same 8192 emu8950 units. */
    int32_t s = (int32_t)next * 4;
    if (s < AUD_SAMPLE_MIN)
        s = AUD_SAMPLE_MIN;
    if (s > AUD_SAMPLE_MAX)
        s = AUD_SAMPLE_MAX;

    /* The XRAM page mirrors the chip's register file, so the low byte of a
     * queued write's address is the register number. */
    uint8_t max_work = 8;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        /* Pairs with the release fence in ria.c: the entry is written
         * before the head that publishes it, and read after. */
        atomic_thread_fence(memory_order_acquire);
        uint8_t tail = ++xram_queue_tail;
        OPL_writeReg(opl_emu8950,
                     xram_queue[tail][0],
                     xram_queue[tail][1]);
    }
    return (int16_t)s;
}

void opl_stereo(int16_t *left, int16_t *right)
{
    *left = *right = opl_sample();
}
#pragma GCC pop_options

uint16_t opl_xaddr_get(void) { return opl_xaddr; }

void opl_park(void) { opl_xaddr = 0xFFFF; }

/* The chip is made on demand and given back only here, because a machine stop
 * does not park the mixer and a host that pulls audio on its own thread can be
 * inside opl_sample when one happens. aud_shutdown is the caller, and it runs
 * where nothing else does. */
void opl_shutdown(void)
{
    opl_xaddr = 0xFFFF;
    opl_wave_base = NULL;
    if (opl_emu8950)
        OPL_delete(opl_emu8950), opl_emu8950 = NULL;
}

bool opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        /* Giving up the engine resets the chip and hands the mix back, so
         * a stopped program's last chord does not hold. */
        if (opl_emu8950)
            OPL_reset(opl_emu8950);
        opl_xaddr = 0xFFFF;
        aud_stop();
        return word == 0xFFFF;
    }
    /* emu8950 builds its shared tables inside OPL_new, which callocs the
     * chip, so there is no way to hold one statically. Its rate converter
     * is compiled out, so the chip steps once per call at its own clock
     * over 72, which is what AUD_NATIVE_RATE is. */
    if (!opl_emu8950)
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, AUD_NATIVE_RATE);
    assert(opl_emu8950); // OPL_new returns NULL only when its calloc fails
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
    /* reset_slot is the only assignment to slot->patch, so the pointer is
     * reconstructed here rather than carried in the blob. */
    s->patch = &s->__patch;
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
        /* A chunk is a fixed size, so what this does not write is padding. */
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
        /* An allocation is not machine state, so a chip this machine
         * allocated stays allocated; the reset puts it back to what a
         * program would find on taking it. */
        if (opl_emu8950)
            OPL_reset(opl_emu8950);
        return true;
    }
    if (!opl_emu8950)
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, AUD_NATIVE_RATE);
    if (!opl_emu8950)
        return false;
    /* Reset first: it settles the members this blob does not carry, and it
     * is what puts every slot on wave row zero, which is where the rows are
     * addressed from. */
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
