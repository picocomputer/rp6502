/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The bus between the 6502 and the machine, as everything above it needs to
 * see it: the SIGINT a Ctrl-C latches. */

#ifndef _CORE_SYS_RIA_H_
#define _CORE_SYS_RIA_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Consumes the flag rather than only reading it, so a caller that asks
     * without meaning to act on it swallows the Ctrl-C. */
    bool ria_get_sigint(void);
    void ria_trigger_sigint(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_SYS_RIA_H_ */
