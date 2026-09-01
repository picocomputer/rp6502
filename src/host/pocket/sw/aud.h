/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_AUD_H_
#define _HOST_POCKET_SW_AUD_H_

#include <stdbool.h>
#include <stdint.h>

void aud_init(void);
void aud_stop(void);
/* A savestate carries the blocks and the pointers but not what the
 * engines made of them; this is the pointers put back and the blocks
 * replayed. */
void aud_restore(void);

/* The two XREG registers, answering core/aud/psg.h and core/aud/opl.h over
 * this machine's engines: the same validation, a different thing told. */
#include "core/aud/opl.h"
#include "core/aud/psg.h"

#endif /* _HOST_POCKET_SW_AUD_H_ */
