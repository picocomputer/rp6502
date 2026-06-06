/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_USB_MSC_HOST_H_
#define _RIA_USB_MSC_HOST_H_

/* USB Mass Storage host transport. Custom TinyUSB application class driver.
 */

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

const usbh_class_driver_t *msc_get_class_driver(void);

#endif /* _RIA_USB_MSC_HOST_H_ */
