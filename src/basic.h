/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _BASIC_H_
#define _BASIC_H_

#include "pico/platform.h"

#define BASIC_ROM_START 0xD000
#define BASIC_ROM_SIZE 0x3000

extern uint8_t __in_flash("basicrom") basicrom[BASIC_ROM_SIZE];

#endif /* _MON_H_ */
