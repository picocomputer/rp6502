/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_COM_COM_H_
#define _CORE_COM_COM_H_

#include "core/sys/com.h"
#include <stddef.h>
#include "core/sys/sst.h"
#include <stdint.h>

void com_keyboard_push(const char *s, size_t n);
void com_keyboard_push_byte(uint8_t b);
size_t com_keyboard_free(void);

size_t com_keyboard_read(char *buf, size_t length);
void com_keyboard_clear(void);
#define COM_KEYBOARD_SOURCE {.read = com_keyboard_read, .clear = com_keyboard_clear, .dwell_us = COM_WIRE_DWELL_US}

void com_uart_push(const char *s, size_t n);
void com_stream_push(const char *s, size_t n);
size_t com_uart_free(void);

size_t com_uart_read(char *buf, size_t length);
int com_uart_peek(void);
void com_uart_clear(void);
#define COM_UART_SOURCE {.read = com_uart_read, .peek = com_uart_peek, .clear = com_uart_clear, .dwell_us = COM_WIRE_DWELL_US}

bool com_input_idle(void);

void com_init(void);

void com_break(void);

void com_stop(void);

void com_run(void);

void com_tx_write(const char *buf, int len);

void com_crlf_write(const char *buf, int len);

void com_set_tx_tap(void (*tap)(const char *buf, int len));

void com_set_std_tap(void (*tap)(int fd, const char *buf, int len));

void com_set_stderr_sink(void (*sink)(const char *buf, int len));

void com_task(void);

/* The 44 fixed bytes are the reply length (1), the reply buffer (32), the BEL
 * enable (1), the CRLF latch (1), and the 9 bytes com_rx_save writes. Each of
 * the two rings adds COM_RING_SIZE bytes and its two 16-bit indices. */
#define COM_SST_SIZE (2 * (COM_RING_SIZE + 4) + 44)
void com_sst_save(sst_cursor_t *c, unsigned flags);
bool com_sst_load(sst_cursor_t *c, unsigned flags);

#define COM_DRIVER DRIVER(com_init, com_task, nul_task, com_run, com_stop, com_break, \
    nul_config, nul_config, SST(COM_, 1, COM_SST_SIZE, com_sst_save, com_sst_load))

#endif /* _CORE_COM_COM_H_ */
