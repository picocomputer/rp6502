/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PHI2_H_
#define _RIA_SYS_PHI2_H_

/* PHI2 on the board: the RIA's write state machine side-sets the pin, so
 * this owns the divider that machine runs at, and the fan-out to everything
 * else clocked from it. */

#include "core/wdc/phi2.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The row and the rest of the contract are core/wdc/phi2.h's, which every
 * machine shares. This is the one column only a machine with a monitor
 * defines: SET's reply line. The row must still come up after RIA and PIX,
 * whose inits create the state machines a reclock reprograms. */
int phi2_response(char *buf, size_t buf_size, int state, unsigned width);

#endif /* _RIA_SYS_PHI2_H_ */
