/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_STD_H_
#define _RIA_API_STD_H_

/* Provides STDIO to the 6502.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void std_run(void);
void std_stop(void);

/* The API implementation for stdio support.
 */

bool std_api_open(void);
bool std_api_close(void);
bool std_api_read_xstack(void);
bool std_api_read_xram(void);
bool std_api_write_xstack(void);
bool std_api_write_xram(void);
bool std_api_lseek_cc65(void);
bool std_api_lseek_llvm(void);
bool std_api_syncfs(void);

/* Readline configuration getters/setters for atr.c
 */

bool std_get_disable_nl_expansion(void);
void std_set_disable_nl_expansion(bool v);

bool std_get_suppress_end_move(void);
void std_set_suppress_end_move(bool v);

bool std_get_suppress_newline(void);
void std_set_suppress_newline(bool v);

bool std_get_enable_history(void);
void std_set_enable_history(bool v);

uint8_t std_get_max_length(void);
void std_set_max_length(uint8_t v);

uint8_t std_get_timeout(void);
void std_set_timeout(uint8_t v);

uint32_t std_get_ctrl_bits(void);
void std_set_ctrl_bits(uint32_t v);

uint8_t std_get_end_char(void);

bool std_get_timed_out(void);

uint8_t std_get_cursor_pos(void);
void std_set_cursor_pos(uint8_t v);

#endif /* _RIA_API_STD_H_ */
