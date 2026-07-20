/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_NET_MDM_H_
#define _RIA_NET_MDM_H_

/* Modem emulator.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ria/api/api.h"
#include "ria/api/std.h"

#define MDM_PHONEBOOK_ENTRIES 4

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
} mdm_settings_t;

/* Main events
 */

void mdm_init(void);
void mdm_task(void);
void mdm_stop(void);

/* STDIO
 */

bool mdm_std_handles(const char *filename);
int mdm_std_open(const char *path, uint8_t flags, api_errno *err);
std_rw_result mdm_std_close(int desc, api_errno *err);
std_rw_result mdm_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result mdm_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

/* Modem control interface.
 * Functions below operate on the current connection set by mdm_set_conn().
 */

mdm_settings_t *mdm_settings(void);
void mdm_set_conn(int desc);
bool mdm_settings_persistent(void);
// A response generator: snprintf()s the next chunk and returns the next state,
// or a negative state when done. A negative state in means cancel (close any
// open files). Guaranteed 80 columns plus a newline and null.
typedef int (*mdm_response_fn)(char *buf, size_t size, int state, unsigned width);
void mdm_set_response_fn(mdm_response_fn fn); // state 0
void mdm_set_response_fn_state(mdm_response_fn fn, int state);
void mdm_factory_settings(mdm_settings_t *settings);
bool mdm_write_settings(const mdm_settings_t *settings);
bool mdm_read_settings(mdm_settings_t *settings);
bool mdm_write_phonebook_entry(const char *entry, unsigned index);
const char *mdm_read_phonebook_entry(unsigned index);
bool mdm_dial(const char *s);
bool mdm_connect(void);
bool mdm_hangup(void);
bool mdm_answer(void);
uint8_t mdm_get_ring_count(void);
bool mdm_set_listen_port(uint16_t port);
bool mdm_conns_is_open(int desc);
uint16_t mdm_conns_listen_port(int desc);

#endif /* _RIA_NET_MDM_H_ */
