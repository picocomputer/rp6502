/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_PICO_RTT_H_
#define _HOST_PICO_RTT_H_

/* The console over a debug probe, as SEGGER RTT channel 0, in a firmware
 * built with RP6502_RTT_DEBUG. A debugger on the SWD port finds a small
 * structure in RAM and reads and writes two rings through it while the chip
 * runs, so the bench reaches the console with no UART, USB or network.
 * Nothing here knows whether a debugger is there.
 *
 * The probe is one more end of the console's UART: it hears what the UART
 * is sent, and what it types arrives as the UART's.
 *
 * The output ring holds 22 ms of a console at 115200 baud, so a debugger
 * has to read it more often than that. tests/hil/pico/rig.py does. OpenOCD's
 * own RTT reads every 100 ms unless it is told otherwise, and loses most of
 * a busy console.
 *
 * Everything is called from the main loop of core 0. Without
 * RP6502_RTT_DEBUG none of it is built and the calls are nothing. */

#include <stdbool.h>
#include <stddef.h>

#if RP6502_RTT_DEBUG

/* Bytes for the debugger. Never waits: what does not fit is dropped and
 * counted. */
void rtt_write(const char *buf, size_t length);

/* Bytes from the debugger, which holds what it has until they are read. */
size_t rtt_read(char *buf, size_t length);

/* True once for each break the debugger asks for. RTT has no break of its
 * own, so the debugger writes a word beside the rings. */
bool rtt_break(void);

#else

static inline void rtt_write(const char *buf, size_t length)
{
    (void)buf;
    (void)length;
}

static inline size_t rtt_read(char *buf, size_t length)
{
    (void)buf;
    (void)length;
    return 0;
}

static inline bool rtt_break(void)
{
    return false;
}

#endif

#endif /* _HOST_PICO_RTT_H_ */
