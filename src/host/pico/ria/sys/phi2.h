/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PHI2_H_
#define _RIA_SYS_PHI2_H_

#include "core/wdc/phi2.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

int phi2_response(char *buf, size_t buf_size, int state, unsigned width);

#endif /* _RIA_SYS_PHI2_H_ */
