/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The bus between the 6502 and the machine, as everything above it needs to
 * see it: whether a transfer is in flight, and the SIGINT a Ctrl-C latches.
 * A machine with no such transfer answers false and never latches.
 *
 * Its own header because six translation units want nothing but these three
 * and were reaching them through the machine contract, which drags
 * core/api/std.h and core/api/api.h behind it. The implementation is
 * core/ria/ria.c on a software machine, host/pico/ria/sys/ria.c on the
 * firmware, and host/pocket/sw/main.c on a machine whose bus is fabric.
 */

#ifndef _CORE_SYS_RIA_H_
#define _CORE_SYS_RIA_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // True while a memory transfer to or from the 6502 is in flight.
    bool ria_active(void);

    // Returns true once per latched SIGINT, then clears.
    bool ria_get_sigint(void);
    void ria_trigger_sigint(void);

    /* Take back a byte this machine's register window staged ahead of a
     * reader. Answering a ready bit commits a byte out of the console, so a
     * program reading the console some other way has to be able to get it
     * back. False on a machine that stages nothing, which is one whose bus
     * is fabric. */
    bool ria_rx_reclaim(char *ch);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_RIA_H_ */
