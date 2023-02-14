/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _API_H_
#define _API_H_

#include "mem/vstack.h"
#include "mem/regs.h"
#include <stdint.h>

#define API_OPCODE REGS(0xFFEF)
#define API_AX REGSW(0xFFED)
#define API_STACK_RW REGSW(0xFFEC)
#define API_RETURN_VAL(val)               \
    {                                     \
        vstack_ptr = VSTACK_SIZE;         \
        REGS(0xFFF5) = (val >> 8) & 0xFF; \
        REGS(0xFFF3) = val & 0xFF;        \
        REGS(0xFFF1) = 0;                 \
        API_OPCODE |= 0x80;               \
    }

#define API_RETURN_VAL_ERR(val, err) \
    API_AX = err;                    \
    API_RETURN_VAL(val);

void api_task();
void api_stop();
void api_reset();

#endif /* _API_H_ */
