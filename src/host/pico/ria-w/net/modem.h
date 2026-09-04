/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_W_NET_MODEM_H_
#define _RIA_W_NET_MODEM_H_

/* Modem emulator.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/api.h"
#include "core/api/std.h"

#define MODEM_PHONEBOOK_ENTRIES 4

typedef struct
{
    uint8_t echo;
    uint8_t quiet;
    uint8_t verbose;
    uint8_t progress;
    uint8_t auto_answer;
    uint8_t esc_char;
    uint8_t cr_char;
    uint8_t lf_char;
    uint8_t bs_char;
    uint8_t s_pointer;
    uint8_t net_mode;
    uint16_t listen_port;
    char tty_type[41];
} modem_settings_t;

/* Main events
 */

void modem_init(void);
void modem_task(void);
void modem_stop(void);

/* STDIO
 */

bool modem_std_handles(const char *filename);
int modem_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result modem_std_close(int desc, api_errno *err);
std_rw_result modem_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result modem_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

/* Modem control interface.
 * Functions below operate on the current connection set by modem_set_conn().
 */

modem_settings_t *modem_settings(void);
void modem_set_conn(int desc);
bool modem_settings_persistent(void);
// A response generator: snprintf()s the next chunk and returns the next state,
// or a negative state when done. A negative state in means cancel (close any
// open files). Guaranteed 80 columns plus a newline and null.
typedef int (*modem_response_fn)(char *buf, size_t size, int state, unsigned width);
void modem_set_response_fn(modem_response_fn fn); // state 0
void modem_set_response_fn_state(modem_response_fn fn, int state);
void modem_factory_settings(modem_settings_t *settings);
bool modem_write_settings(const modem_settings_t *settings);
bool modem_read_settings(modem_settings_t *settings);
bool modem_write_phonebook_entry(const char *entry, unsigned index);
const char *modem_read_phonebook_entry(unsigned index);
bool modem_dial(const char *s);
bool modem_connect(void);
bool modem_hangup(void);
bool modem_answer(void);
uint8_t modem_get_ring_count(void);
bool modem_set_listen_port(uint16_t port);
bool modem_conns_is_open(int desc);
uint16_t modem_conns_listen_port(int desc);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define MODEM_DRIVER DRIVER(modem_init, modem_task, nul_task, nul_run, modem_stop, nul_break, nul_config, nul_config)

/* This driver's stdio row: the std_driver_t initializer core/api/std.c
 * builds this machine's table from. A stream: no seek, nothing to flush. */
#define MODEM_STD_DRIVER           \
    {                                 \
        .handles = modem_std_handles, \
        .open = modem_std_open,       \
        .close = modem_std_close,     \
        .read = modem_std_read,       \
        .write = modem_std_write,     \
    }

#endif /* _RIA_W_NET_MODEM_H_ */
