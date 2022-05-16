/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"

static scsi_inquiry_resp_t inquiry_resp;

bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const *cbw, msc_csw_t const *csw)
{
    if (csw->status != 0)
    {
        printf("tuh_msc_inquiry failed, status = %d\n", csw->status);
        return false;
    }
    printf("%.8s %.16s rev %.4s\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev);
    uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
    uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);
    printf("Disk Size: %lu MB\r\n", block_count / ((1024 * 1024) / block_size));
    return true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    printf("USB mass storage device mount\n");
    tuh_msc_inquiry(dev_addr, 0, &inquiry_resp, inquiry_complete_cb);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
    printf("USB mass storage device unmount\n");
}
