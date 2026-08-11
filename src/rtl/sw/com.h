/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_COM_H_
#define _FPGA_SW_COM_H_

#include "ria/sys/com.h"

void com_set_term_out(void (*out_chars)(const char *buf, int len));

void com_in_write_reply(const char *s, size_t n);

#endif
