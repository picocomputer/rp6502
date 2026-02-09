/*
 * Copyright (c) 2025 Rumbledethumps
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
#include "api/api.h"
#include "api/std.h"

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
int mdm_std_close(int desc, api_errno *err);
std_rw_result mdm_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result mdm_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

/* Modem control interface
 */

extern mdm_settings_t mdm_settings;
int mdm_response_code(char *buf, size_t buf_size, int state);
void mdm_set_response_fn(int (*fn)(char *, size_t, int), int state);
void mdm_factory_settings(mdm_settings_t *settings);
bool mdm_write_settings(const mdm_settings_t *settings);
bool mdm_read_settings(mdm_settings_t *settings);
bool mdm_write_phonebook_entry(const char *entry, unsigned index);
const char *mdm_read_phonebook_entry(unsigned index);
bool mdm_dial(const char *s);
bool mdm_connect(void);
bool mdm_hangup(void);
void mdm_carrier_lost(void);

#endif /* _RIA_NET_MDM_H_ */
