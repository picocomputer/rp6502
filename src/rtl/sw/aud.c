/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud.h"
#include "bel.h"
#include "mmio.h"

#include <pico/time.h>

#include <stdio.h>

#include <string.h>

static uint16_t aud_psg_at = 0xFFFF;
static uint16_t aud_opl_at = 0xFFFF;

void aud_init(void)
{
    aud_stop();
    bel_init();
}

void aud_stop(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    aud_psg_at = 0xFFFF;
    aud_opl_at = 0xFFFF;
}

static void aud_replay(uint16_t at, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uint8_t v = XRAM_WIN[at + i];
        XRAM_WIN[at + i] = v;
    }
}

void aud_log_psg(const char *when)
{
    if (aud_psg_at == 0xFFFF)
        return;
    printf("aud: %s psg gate=", when);
    for (uint16_t c = 0; c < 8; c++)
        printf("%02x", XRAM_WIN[aud_psg_at + c * 8 + 6]);
    printf("\n");
}

void aud_log_opl(const char *when)
{
    if (aud_opl_at == 0xFFFF)
        return;
    printf("aud: %s opl b0=", when);
    for (uint16_t r = 0xB0; r <= 0xB8; r++)
        printf("%02x", XRAM_WIN[aud_opl_at + r]);
    printf(" bd=%02x\n", (unsigned)XRAM_WIN[aud_opl_at + 0xBD]);
}

uint16_t aud_psg_at_get(void)
{
    return aud_psg_at;
}

uint16_t aud_opl_at_get(void)
{
    return aud_opl_at;
}

static void aud_wait_us(uint32_t us)
{
    uint64_t until = time_us_64() + us + 1;
    while (time_us_64() < until)
        ;
}

void aud_restore(void)
{
    uint16_t psg = aud_psg_at, opl = aud_opl_at;
    printf("aud: psg=%04x opl=%04x\n", (unsigned)psg, (unsigned)opl);
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    if (psg != 0xFFFF)
    {
        AUD_PSG_XADDR = psg;
        aud_wait_us(21);
        AUD_PSG_REPLAY = 1;
        aud_replay(psg, 64);
        AUD_PSG_REPLAY = 0;
    }
    else if (opl != 0xFFFF)
    {
        AUD_OPL_XADDR = opl;
        aud_wait_us(6);
        aud_replay(opl, 256);
        aud_log_opl("restored");
    }
    aud_psg_at = psg;
    aud_opl_at = opl;
    bel_init();
}

bool aud_psg_xreg(uint16_t word)
{
    if (word & 0x0001 || word > 0x10000 - 64 ||
        ((word >> 8) != ((word + 63) >> 8)))
    {
        aud_stop();
        return word == 0xFFFF;
    }
    AUD_OPL_XADDR = 0xFFFF;
    AUD_PSG_XADDR = word;
    aud_opl_at = 0xFFFF;
    aud_psg_at = word;
    aud_replay(word, 64);
    return true;
}

bool aud_opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        aud_stop();
        return word == 0xFFFF;
    }
    memset((void *)&XRAM_WIN[word], 0, 256);
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = word;
    aud_psg_at = 0xFFFF;
    aud_opl_at = word;
    return true;
}
