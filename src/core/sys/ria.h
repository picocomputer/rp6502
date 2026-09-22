/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The bus between the 6502 and the machine, as everything above it needs to
 * see it: the SIGINT a Ctrl-C latches, and the page of XRAM whose writes the
 * RW engine reports to the audio engines. */

#ifndef _CORE_SYS_RIA_H_
#define _CORE_SYS_RIA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Consumes the flag rather than only reading it, so a caller that asks
     * without meaning to act on it swallows the Ctrl-C. */
    bool ria_get_sigint(void);
    void ria_trigger_sigint(void);

    /* Watch the page xaddr is on, and hand every 6502 write that lands there
     * to aud_xram_write; 0xFFFF to watch none. How the write gets there is
     * the RW engine's own business, and the two differ: a software machine
     * calls straight through, while the Pico's engine is core1 and leaves the
     * write for the audio interrupt on core0 to pick up. */
    void ria_aud_watch(uint16_t xaddr);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_RIA_H_ */
