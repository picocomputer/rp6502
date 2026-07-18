/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_MSC_H_
#define _RIA_USB_MSC_H_

/* USB Mass Storage Controller
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ria/api/api.h"
#include "ria/api/std.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"

/* Status
 */

int msc_status_count(void);
int msc_status_response(char *buf, size_t buf_size, int state, unsigned width);

/* Disk utility (mon/dsk.c) support. A logical volume index (0..FF_VOLUMES-1)
 * identifies an MSCn: drive; these resolve it to its physical device.
 */

typedef struct
{
    bool present;
    bool removable;
    bool write_prot;
    bool is_floppy; // CBI/UFI/SFF floppy (vs BOT/SCSI flash)
    uint64_t block_count;
    uint32_t block_size;
    uint8_t gen;  // mount generation; changes when the slot is reused (TOCTOU guard)
    char path[6]; // canonical "MSCn:" FatFs path for this volume
} msc_dsk_info_t;

int msc_dsk_vol_from_name(const char *name); // "MSCn"/"MSCn:"/"n:" -> index, or -1
bool msc_dsk_get_info(uint8_t vol, msc_dsk_info_t *out);
bool msc_dsk_inquiry_strings(uint8_t vol, char vendor[9], char product[17], char rev[5]);
bool msc_dsk_serial(uint8_t vol, char *dst, size_t dst_size);
bool msc_dsk_read(uint8_t vol, void *buf, uint64_t lba, uint32_t count);
bool msc_dsk_write(uint8_t vol, const void *buf, uint64_t lba, uint32_t count);
bool msc_dsk_format_track(uint8_t vol, uint8_t track, uint8_t head);
void msc_dsk_reenumerate(uint8_t pdrv); // remount after format/erase

/* TinyUSB host class-driver callbacks.
 */

bool msc_class_driver_init(void);
uint16_t msc_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len);
bool msc_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num);
bool msc_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void msc_class_driver_close(uint8_t dev_addr);

#endif /* _RIA_USB_MSC_H_ */
