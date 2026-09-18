/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_VCP_H_
#define _RIA_USB_VCP_H_

/* USB VCP (Virtual COM Port)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/api/api.h"
#include "core/api/std.h"

/* Main events
 */

void vcp_task(void);

/* Status
 */

int vcp_status_response(char *buf, size_t buf_size, int state, unsigned width);

/* STDIO
 */

bool vcp_std_handles(const char *name);
int vcp_std_open(const char *name, uint8_t flags, api_errno *err);
std_rw_result vcp_std_close(int desc, api_errno *err);
std_rw_result vcp_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err);
std_rw_result vcp_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err);

/* NFC device tracking
 */

void vcp_load_nfc_device_hash(const char *str);
const char *vcp_get_nfc_device_hash(void);
int vcp_nfc_open(void);

/* A hash from usb_device_id_hash is at most 110 characters: 15 for the VID,
 * PID and bcdDevice fields, each four hex digits and a colon, then up to 31
 * for each of the three descriptor strings with a colon between each pair. */
#define VCP_NFC_HASH_SIZE 128

bool vcp_check_nfc_device_hash(const char *in, char *out);
void vcp_apply_nfc_device_hash(const char *hash, bool changed);

/* vcp_task is in the io_task column of VCP_DRIVER because building a device
 * hash waits on USB string fetches that call sys_task, and the io_task column
 * is never re-entered. */
#define VCP_CONFIG_NFC_HASH CONFIG_HIDDEN(H, vcp, nfc_device_hash, VCP_NFC_HASH_SIZE, "", \
    vcp_check_nfc_device_hash, vcp_apply_nfc_device_hash)
#define VCP_DRIVER DRIVER(nul_init, nul_task, vcp_task, nul_run, nul_stop, nul_break, \
    VCP_CONFIG_NFC_HASH, nul_config, nul_sst)

#define VCP_STD_DRIVER           \
    {                               \
        .handles = vcp_std_handles, \
        .open = vcp_std_open,       \
        .close = vcp_std_close,     \
        .read = vcp_std_read,       \
        .write = vcp_std_write,     \
    }

#endif /* _RIA_USB_VCP_H_ */
