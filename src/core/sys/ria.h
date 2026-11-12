/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The bus between the 6502 and the machine, as everything above it needs to
 * see it: the SIGINT a Ctrl-C latches, and the byte waiting for the 6502 in
 * the UART's receive register. */

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

    /* Offer puts a byte in the receive register for the 6502. Reclaim takes
     * it back unread, false when there is none, and peek reads it without
     * taking it, negative when there is none. */
    void ria_uart_rx_offer(uint8_t ch);
    bool ria_uart_rx_reclaim(uint8_t *ch);
    int ria_uart_rx_peek(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_RIA_H_ */
