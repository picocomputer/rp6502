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
void aud_restore(void);

#include "core/aud/opl.h"
#include "core/aud/psg.h"

#endif /* _HOST_POCKET_SW_AUD_H_ */
