/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_config.h"
#include "tusb.h"
#include "usb/xin.h"
#include "usb/pad.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include <string.h>

#define DEBUG_RIA_USB_XIN

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_XIN)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

// Xbox controller tracking
typedef struct
{
    bool valid;
    bool is_xbox_one; // If not it's Xbox 360
    uint8_t dev_addr;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t report_buffer[64]; // XInput max 64 bytes
} xbox_device_t;

static xbox_device_t xbox_devices[PAD_MAX_PLAYERS];

static int xin_find_device_slot(uint8_t dev_addr)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (xbox_devices[i].valid && xbox_devices[i].dev_addr == dev_addr)
            return i;
    return -1;
}

static int xin_find_free_slot(void)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (!xbox_devices[i].valid)
            return i;
    return -1;
}

static uint8_t xin_slot_to_pad_idx(int slot)
{
    return CFG_TUH_HID + slot;
}

bool xin_is_xbox_one(uint8_t dev_addr)
{
    int slot = xin_find_device_slot(dev_addr);
    return slot >= 0 && xbox_devices[slot].is_xbox_one;
}

bool xin_is_xbox_360(uint8_t dev_addr)
{
    int slot = xin_find_device_slot(dev_addr);
    return slot >= 0 && !xbox_devices[slot].is_xbox_one;
}

static bool xin_class_driver_init(void)
{
    memset(xbox_devices, 0, sizeof(xbox_devices));
    return true;
}

static bool xin_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
    (void)rhport;

    // Must be vendor specific to proceed
    if (desc_itf->bInterfaceClass != 0xFF)
        return false;

    // Xbox One/Series: Class=0xFF, Subclass=0x47, Protocol=0xD0
    // Xbox 360: Class=0xFF, Subclass=0x5D, Protocol=0x01 or 0x02
    bool is_x360 = false;
    bool is_xbox_one = false;
    if (desc_itf->bInterfaceSubClass == 0x47 && desc_itf->bInterfaceProtocol == 0xD0)
    {
        is_xbox_one = true;
        DBG("XInput: Detected Xbox One/Series controller interface\n");
    }
    else if (desc_itf->bInterfaceSubClass == 0x5D &&
             (desc_itf->bInterfaceProtocol == 0x01 || desc_itf->bInterfaceProtocol == 0x02))
    {
        is_x360 = true;
        DBG("XInput: Detected Xbox 360 controller interface\n");
    }

    if (!is_xbox_one && !is_x360)
    {
        return false;
    }

    // All Xinput controllers have in and out endpoints
    uint8_t const *p_desc = (uint8_t const *)desc_itf;
    uint8_t const *desc_end = p_desc + max_len;
    uint8_t ep_in = 0, ep_out = 0;
    tusb_desc_endpoint_t ep_in_desc = {0}, ep_out_desc = {0};
    p_desc = tu_desc_next(p_desc); // Skip interface descriptor
    while (p_desc < desc_end)
    {
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT)
        {
            tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;
            if (desc_ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT)
            {
                if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN)
                {
                    ep_in = desc_ep->bEndpointAddress;
                    ep_in_desc = *desc_ep;
                }
                else if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_OUT)
                {
                    ep_out = desc_ep->bEndpointAddress;
                    ep_out_desc = *desc_ep;
                }
            }
        }
        p_desc = tu_desc_next(p_desc);
    }
    if (ep_in == 0 || ep_out == 0)
    {
        return false;
    }

    // Find a free slot
    int slot = xin_find_free_slot();
    if (slot < 0)
        return false;

    // Initialize slot
    xbox_devices[slot].dev_addr = dev_addr;
    xbox_devices[slot].valid = true;
    xbox_devices[slot].is_xbox_one = is_xbox_one;
    xbox_devices[slot].ep_in = ep_in;
    xbox_devices[slot].ep_out = ep_out;

    // Mount in pad system
    uint16_t vendor_id, product_id;
    if (tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
    {
        uint8_t pad_idx = xin_slot_to_pad_idx(slot);
        if (!pad_mount(pad_idx, 0, 0, dev_addr, vendor_id, product_id))
        {
            DBG("XInput: Failed to mount in pad system\n");
            memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
            return false;
        }
    }
    else
    {
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
        return false;
    }

    // Open the endpoints immediately (like HID does)
    if (!tuh_edpt_open(dev_addr, &ep_in_desc))
    {
        DBG("XInput: Failed to open IN endpoint during open\n");
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
        return false;
    }
    if (!tuh_edpt_open(dev_addr, &ep_out_desc))
    {
        DBG("XInput: Failed to open OUT endpoint during open\n");
        tuh_edpt_abort_xfer(dev_addr, ep_in);
        tuh_edpt_close(dev_addr, ep_in);
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
        return false;
    }

    DBG("XInput: Successfully opened Xbox controller in slot %d\n", slot);
    return true;
}

static bool xin_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    int slot = xin_find_device_slot(dev_addr);
    if (slot < 0)
        return false;

    // If this is Xbox One, send init command
    if (xbox_devices[slot].is_xbox_one)
    {
        // Xbox One GIP initialization packet to start input reports
        static const uint8_t xbox_one_init[] = {
            0x05, 0x20, 0x00, 0x01, 0x00 // GIP command to enable input reports
        };
        tuh_xfer_t xfer = {
            .daddr = dev_addr,
            .ep_addr = xbox_devices[slot].ep_out,
            .buflen = sizeof(xbox_one_init),
            .buffer = (void *)xbox_one_init,
            .complete_cb = NULL,
            .user_data = (uintptr_t)slot};
        if (!tuh_edpt_xfer(&xfer))
            DBG("XInput: Failed to send Xbox One init command\n");
    }

    // Configuration complete
    DBG("XInput: Configuration complete for slot %d\n", slot);
    usbh_driver_set_config_complete(dev_addr, itf_num);

    return true;
}

static bool xin_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)ep_addr;

    int slot = xin_find_device_slot(dev_addr);
    if (slot < 0)
        return false;

    if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0)
    {
        // Handle just like an HID gamepad report
        pad_report(xin_slot_to_pad_idx(slot), xbox_devices[slot].report_buffer, (uint16_t)xferred_bytes);
        // Restart the transfer to continue receiving reports
        tuh_xfer_t xfer = {
            .daddr = dev_addr,
            .ep_addr = xbox_devices[slot].ep_in,
            .buflen = sizeof(xbox_devices[slot].report_buffer),
            .buffer = xbox_devices[slot].report_buffer,
            .complete_cb = NULL,
            .user_data = (uintptr_t)slot};
        if (!tuh_edpt_xfer(&xfer))
            DBG("XInput: Failed to start interrupt transfer for slot %d\n", slot);
    }
    else
        DBG("XInput: Transfer failed for slot %d, result=%d, len=%lu\n", slot, result, xferred_bytes);

    return true;
}

static void xin_class_driver_close(uint8_t dev_addr)
{
    int slot = xin_find_device_slot(dev_addr);
    if (slot < 0)
        return;

    DBG("XInput: Closing Xbox controller from slot %d\n", slot);

    // Abort any ongoing transfers and close endpoints
    tuh_edpt_abort_xfer(dev_addr, xbox_devices[slot].ep_in);
    tuh_edpt_close(dev_addr, xbox_devices[slot].ep_in);
    tuh_edpt_abort_xfer(dev_addr, xbox_devices[slot].ep_out);
    tuh_edpt_close(dev_addr, xbox_devices[slot].ep_out);

    // Notify pad module to unmount
    pad_umount(xin_slot_to_pad_idx(slot));

    // Clear the slot
    memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
}

// Define the XInput class driver
static const usbh_class_driver_t xin_class_driver = {
    .name = "XInput",
    .init = xin_class_driver_init,
    .deinit = NULL,
    .open = xin_class_driver_open,
    .set_config = xin_class_driver_set_config,
    .xfer_cb = xin_class_driver_xfer_cb,
    .close = xin_class_driver_close};

// Required callback for TinyUSB to get application drivers
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &xin_class_driver;
}
