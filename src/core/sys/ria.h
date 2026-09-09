/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The bus between the 6502 and the machine, as everything above it needs to
 * see it: whether the RIA is driving the bus itself, and the SIGINT a Ctrl-C
 * latches. */

#ifndef _CORE_SYS_RIA_H_
#define _CORE_SYS_RIA_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* True while the RIA is driving the 6502 itself, which on a Pico is a
     * chunked mbuf transfer in either direction or a compare: the run and stop
     * fan-outs it borrows belong to the transfer rather than to a program, and
     * the register window is the transfer's while it lasts. Always false on a
     * machine that has no such transfer. */
    bool ria_active(void);

    /* Takes the latch rather than reads it, so a caller that asks without
     * meaning to act on it swallows the Ctrl-C. */
    bool ria_get_sigint(void);
    void ria_trigger_sigint(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_RIA_H_ */
