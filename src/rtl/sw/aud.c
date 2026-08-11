/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg_xreg's validation over this machine's engine. Writing a pointer
 * register resets that engine's envelopes, noise seeds and gates in
 * hardware, 0xFFFF included.
 *
 * Setting up either engine parks the other, which is the only exclusion
 * there is: nothing gates the mix, and rp6502.sv sums every engine and
 * the bell together.
 */

#include "aud.h"
#include "bel.h"
#include "mmio.h"

#include <pico/time.h>

#include <stdio.h>

#include <string.h>

/* Where each engine is pointed. The registers are write-only fabric and
 * a savestate has to put them back, so the answer is kept here, in the
 * soft CPU's memory, which is the one thing the blob does carry. */
static uint16_t aud_psg_at = 0xFFFF;
static uint16_t aud_opl_at = 0xFFFF;

/* The platform's reset is not the engines': they hold what the last
 * session left them, so a host reset would come back still playing. */
void aud_init(void)
{
    aud_stop();
    bel_init();
}

/* Free-running hardware, so without this the last sound plays forever.
 * The bell is the soft CPU's and rings through a program stop. */
void aud_stop(void)
{
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    aud_psg_at = 0xFFFF;
    aud_opl_at = 0xFFFF;
}

/* Every byte of a block written over itself, which is that block
 * arriving as far as an engine that learns only from writes. */
static void aud_replay(uint16_t at, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uint8_t v = XRAM_WIN[at + i];
        XRAM_WIN[at + i] = v;
    }
}

/* The nine key-on registers and the rhythm byte: "is this page a song
 * that was playing", in ten bytes and one line.
 *
 * Printed at both ends of a sleep because a resume that comes back
 * silent has two quite different causes and nothing else separates
 * them. The same bytes at both ends means the blob carried the page and
 * the fault is downstream of it -- the replay, or the program's own
 * streaming. Bytes that differ mean XRAM did not come back, and no
 * amount of replaying will help. */
/* The same question for the other engine: the gate byte of each of the
 * eight channels, which is what says a voice is sounding. Offset six of
 * a seven-byte block on an eight-byte stride, the way aud_psg.sv reads
 * it. */
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

/* For the wake and create logs: which engine a program has claimed. */
uint16_t aud_psg_at_get(void)
{
    return aud_psg_at;
}

uint16_t aud_opl_at_get(void)
{
    return aud_opl_at;
}

/* MTIME counts whole microseconds, so a spin that ends when the counter
 * reads start+n has waited anywhere from n-1 to n of them. Both callers
 * below are waiting out a walk in fabric whose length is known exactly,
 * and want the floor rather than the hope, so this asks for one more. */
static void aud_wait_us(uint32_t us)
{
    uint64_t until = time_us_64() + us + 1;
    while (time_us_64() < until)
        ;
}

/* A restore brings back the block in XRAM and the pointer from here,
 * but not what the engine made of them: the phase, the envelope and the
 * noise are its own and they start again. The registers are replayed so
 * that everything the block does say is back in force -- without it a
 * restored machine plays whatever the engine happened to hold.
 *
 * The OPL's page is not zeroed on the way in the way a fresh program of
 * it is. There is nothing to hide from a program reading back its own
 * registers here; the page is already the one it wrote. */
void aud_restore(void)
{
    uint16_t psg = aud_psg_at, opl = aud_opl_at;
    /* Which engine the blob came back pointing at, and where. 0xFFFF is
     * "no program has claimed it", and a restore that came back with
     * that when the program was playing means the pointer did not
     * survive rather than that the replay went wrong. */
    printf("aud: psg=%04x opl=%04x\n", (unsigned)psg, (unsigned)opl);
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    if (psg != 0xFFFF)
    {
        AUD_PSG_XADDR = psg;
        /* Installing the pointer releases every voice at the engine's
         * next idle -- one sample walk away -- so the replay has to
         * come after that or the notes it strikes are released again
         * behind it. A sample at 48 kHz is 20.9 us. */
        aud_wait_us(21);
        /* And the replay's gate bits have to count. Without this the
         * engine ignores them -- a gate is the 6502's to make -- and a
         * voice that was sounding comes back silent for good. */
        AUD_PSG_REPLAY = 1;
        aud_replay(psg, 64);
        AUD_PSG_REPLAY = 0;
    }
    else if (opl != 0xFFFF)
    {
        AUD_OPL_XADDR = opl;
        /* Installing the pointer is also how this chip is reset, and
         * aud_opl.sv holds that reset for 255 machine clocks while it
         * walks its register file clear. A replay begun inside the walk
         * has its first registers walked over -- the head of the page,
         * which is where the operator settings are. 255 clocks at
         * 50.4 MHz is 5.06 us. */
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
    /* The engine learns from writes and never reads the block back, so a
     * block programmed before the pointer would be invisible. Writing
     * each byte over itself is that block arriving. From here and not the
     * 6502: only the 6502's writes strike gates. */
    aud_replay(word, 64);
    return true;
}

/* Page-aligned is the whole validation: the device is a 256-byte mirror
 * of the chip's register file. The zeroing is for a program that reads
 * back what it just programmed; the engine never reads the page. */
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
