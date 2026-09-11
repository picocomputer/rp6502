/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_COM_TTY_H_
#define _CORE_COM_TTY_H_

#include <stdbool.h>
#include <stddef.h>

void tty_write(const char *buf, int len);

void tty_set_wire(void (*tx)(const char *buf, int len),
                  size_t (*rx)(char *buf, size_t max), bool stream);

#endif /* _CORE_COM_TTY_H_ */
