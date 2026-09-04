/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_NFC_H_
#define _RIA_USB_NFC_H_

/* NFC driver for USB VCP connected PN532
 */

#include "core/api/api.h"
#include "core/api/std.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void nfc_task(void);

/* Configuration setting NFC
 */

void nfc_init(void);
bool nfc_check_enabled(uint8_t *v);
void nfc_apply_enabled(uint8_t val, bool changed);
int nfc_enabled_response(char *buf, size_t buf_size, int state, unsigned width);

// Parse raw NDEF TLV bytes and extract the first Well Known Text record
// into buf (NUL-terminated). Returns false if no text record is found.
bool nfc_parse_text(const uint8_t *tag_data, size_t len, char *buf, size_t buf_size);

/* 6502 std driver interface
 */

bool nfc_std_handles(const char *name);
int nfc_std_open(const char *name, uint8_t flags, api_errno *err);
std_rw_result nfc_std_close(int desc, api_errno *err);
std_rw_result nfc_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result nfc_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

/* This driver's row in a machine's driver list; see core/sys/driver.h. Can arm an exec, so it runs after ROM and before API in the io column. */
#define NFC_CONFIG_ENABLED CONFIG_INT(N, nfc, enabled, uint8_t, 0, \
    nfc_check_enabled, nfc_apply_enabled, STR_NFC, nfc_enabled_response, \
    STR_HELP_SET_NFC, NULL)
#define NFC_DRIVER DRIVER(nfc_init, nul_task, nfc_task, nul_run, nul_stop, nul_break, \
    NFC_CONFIG_ENABLED, nul_config)

/* This driver's stdio row: the std_driver_t initializer core/api/std.c
 * builds this machine's table from. A stream: no seek, nothing to flush. */
#define NFC_STD_DRIVER           \
    {                               \
        .handles = nfc_std_handles, \
        .open = nfc_std_open,       \
        .close = nfc_std_close,     \
        .read = nfc_std_read,       \
        .write = nfc_std_write,     \
    }

#endif /* _RIA_USB_NFC_H_ */
