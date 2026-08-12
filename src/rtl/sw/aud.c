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
#include "com.h"
#include "log.h"
#include "mmio.h"

#include <pico/time.h>

#include <stdio.h>

#include <string.h>

/* Where each engine is pointed. The registers are write-only fabric and
 * a savestate has to put them back, so the answer is kept here, in the
 * soft CPU's memory, which is the one thing the blob does carry. */
static uint16_t aud_psg_at = 0xFFFF;
static uint16_t aud_opl_at = 0xFFFF;

#ifdef RP6502_LOG_FILE

/* Where the engine is pointed and how long its block is. */
static uint16_t aud_at(void)
{
    return aud_psg_at != 0xFFFF ? aud_psg_at : aud_opl_at;
}

static uint16_t aud_len(void)
{
    return aud_psg_at != 0xFFFF ? 64 : 256;
}

static uint32_t aud_sum(void)
{
    uint16_t at = aud_at();
    uint32_t sum = 0;
    if (at != 0xFFFF)
        for (uint16_t i = 0, n = aud_len(); i < n; i++)
            sum += XRAM_WIN[at + i];
    return sum;
}

/* How many times the block has changed since the restore, sampled
 * rather than compared at the log lines: a block written and written
 * back looks untouched from two snapshots a second apart, and whether
 * anything is writing it at all is the fork this turns on. */
static uint32_t aud_chg;
static uint32_t aud_seen_sum;
static absolute_time_t aud_poll_at;

/* Whether this window can write the block at all. A marker into the
 * last register of the page, read back, and put away again; 0xFF is
 * undefined on a YM3812 so poking it costs nothing. 0x100 is "no
 * engine". */
static unsigned aud_probe(void)
{
    uint16_t at = aud_at();
    if (at == 0xFFFF)
        return 0x100;
    volatile uint8_t *cell = &XRAM_WIN[at + aud_len() - 1];
    uint8_t was = *cell;
    *cell = 0xA5;
    uint8_t got = *cell;
    *cell = was;
    return got;
}

/* The block itself, not a digest of it: a sum once said an OPL page was
 * empty on a title that was audibly playing, and a sum cannot say which
 * end of that is wrong. */
static void aud_log(const char *tag)
{
    uint16_t at = aud_at();
    uint16_t len = aud_len();
    LOG_SAY("aud: %s psg=%04x opl=%04x frame=%u sum=%08x chg=%u probe=%03x\n",
            tag, (unsigned)aud_psg_at, (unsigned)aud_opl_at,
            (unsigned)VID_FRAME, (unsigned)aud_sum(), (unsigned)aud_chg,
            aud_probe());
    if (at == 0xFFFF)
        return;
    for (uint16_t i = 0; i < len; i += 16)
    {
        LOG_SAY("aud: %02x:", (unsigned)i);
        for (uint16_t j = 0; j < 16; j++)
            LOG_SAY(" %02x", (unsigned)XRAM_WIN[at + i + j]);
        LOG_SAY("\n");
    }
}

/* The restore's last question, which cannot be answered at the restore:
 * whether anything picked the engine back up afterwards. Zero is
 * disarmed, and each deadline fires once. */
static absolute_time_t aud_log_at_1s, aud_log_at_3s;

void aud_task(void)
{
    if (!aud_poll_at || time_reached(aud_poll_at))
    {
        uint32_t sum = aud_sum();
        if (sum != aud_seen_sum)
        {
            aud_seen_sum = sum;
            aud_chg++;
        }
        aud_poll_at = make_timeout_time_ms(8);
    }
    if (aud_log_at_1s && time_reached(aud_log_at_1s))
    {
        aud_log_at_1s = 0;
        aud_log("t+1s");
    }
    if (aud_log_at_3s && time_reached(aud_log_at_3s))
    {
        aud_log_at_3s = 0;
        aud_log("t+3s");
    }
}

/* The counters restart with the restored session. */
static void aud_arm_deferred(void)
{
    aud_chg = 0;
    aud_seen_sum = aud_sum();
    aud_log_at_1s = make_timeout_time_ms(1000);
    aud_log_at_3s = make_timeout_time_ms(3000);
}

#else

#define aud_log(tag) ((void)0)
static inline void aud_arm_deferred(void) {}
void aud_task(void) {}

#endif

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
    /* Before the pointers go: a restore's cold boot runs the ROM the
     * host announced and stops it when the blob starts arriving, and
     * this is that stop. */
    aud_log("stop");
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
    uint32_t waited = 0;
    /* Which engine the blob came back pointing at, and where. 0xFFFF is
     * "no program has claimed it", and a restore that came back with
     * that when the program was playing means the pointer did not
     * survive rather than that the replay went wrong. */
    aud_log("pre");
    AUD_PSG_XADDR = 0xFFFF;
    AUD_OPL_XADDR = 0xFFFF;
    if (psg != 0xFFFF)
    {
        AUD_PSG_XADDR = psg;
        /* Installing the pointer releases every voice at the engine's
         * next idle -- one sample walk away -- so the replay has to
         * come after that or the notes it strikes are released again
         * behind it. A sample is 48 kHz; twenty-five microseconds is
         * one with room. */
        uint64_t began = time_us_64();
        uint64_t until = began + 25;
        while (time_us_64() < until)
            ;
        waited = (uint32_t)(time_us_64() - began);
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
         * which is where the operator settings are. Six microseconds is
         * those 255 clocks with room. */
        uint64_t began = time_us_64();
        uint64_t until = began + 6;
        while (time_us_64() < until)
            ;
        waited = (uint32_t)(time_us_64() - began);
        aud_replay(opl, 256);
    }
    aud_psg_at = psg;
    aud_opl_at = opl;
    /* 255 machine clocks of chip reset is 5.06 us, measured against a
     * counter that steps in whole microseconds. Whether the deadline
     * was met is not something to assume. */
    LOG_SAY("aud: waited=%uus\n", (unsigned)waited);
    (void)waited;
    aud_arm_deferred();
    aud_log("post");
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
    aud_log("xreg");
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
    aud_log("xreg");
    return true;
}
