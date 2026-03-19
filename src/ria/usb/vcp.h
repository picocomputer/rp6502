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
#include "api/api.h"
#include "api/std.h"

/* Status
 */

int vcp_status_count(void);
int vcp_status_response(char *buf, size_t buf_size, int state);

/* STDIO
 */

bool vcp_std_handles(const char *name);
int vcp_std_open(const char *name, uint8_t flags, api_errno *err);
int vcp_std_close(int desc, api_errno *err);
std_rw_result vcp_std_read(int desc, char *buf, uint32_t buf_size, uint32_t *bytes_read, api_errno *err);
std_rw_result vcp_std_write(int desc, const char *buf, uint32_t buf_size, uint32_t *bytes_written, api_errno *err);

/* NFC device tracking
 */

void vcp_load_nfc_device_hash(const char *str);
const char *vcp_get_nfc_device_hash(void);
void vcp_set_nfc_device_name(const char *name);
int vcp_nfc_open(void);

#endif /* _RIA_USB_VCP_H_ */

/* TODO

nfc should continue to scan with vcp_std_open and vcp_set_nfc_device_name.

After a vcp device is mounted and its strings obtrained,
it needs to be hash tested to see if its the nfc device.
There can be only one, so use a global index to track.

vcp_set_nfc_device_name needs to save.
vcp_set_nfc_device_name will search mounted devices to set global indexx.

vcp_get_nfc_device_name with be replaced with a int vcp_nfc_open(void).
It simply opens and returns the nfc global index if there is one.
vcp_std_close() should work for ccolosing.

unmount needs to clean appropriuate global index.


*/
