/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_MSC_HOST_H_
#define _RIA_USB_MSC_HOST_H_

/* USB Mass Storage host transport. Custom TinyUSB application class driver.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "class/msc/msc.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"

#ifndef CFG_TUH_MSC_MAXLUN
#define CFG_TUH_MSC_MAXLUN 4
#endif

// Superset of msc_csw_status_t with an additional timeout value.
typedef enum
{
    MSC_STATUS_PASSED,      // == MSC_CSW_STATUS_PASSED
    MSC_STATUS_FAILED,      // == MSC_CSW_STATUS_FAILED
    MSC_STATUS_PHASE_ERROR, // == MSC_CSW_STATUS_PHASE_ERROR
    MSC_STATUS_TIMED_OUT,   // returned on I/O timeout
} msc_status_t;

uint8_t tuh_msc_protocol(uint8_t dev_addr);
uint8_t tuh_msc_get_maxlun(uint8_t dev_addr);
msc_status_t tuh_msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                               const void *data, uint32_t timeout_ms);

// TinyUSB host class-driver callbacks.
bool msch_class_driver_init(void);
uint16_t msch_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len);
bool msch_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num);
bool msch_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void msch_class_driver_close(uint8_t dev_addr);

#endif /* _RIA_USB_MSC_HOST_H_ */
