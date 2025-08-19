/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/pad.h"
#include "usb/hid.h"
#include "usb/xin.h"
#include <tusb.h>
#include <host/usbh_pvt.h>
#include <string.h>

// This is a driver for USB XInput used by XBox gamepads.

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_XIN)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// c'mon tusb, there has to be a better way
#define XIN_START_360_DELAY_MS 900

// Xbox controller tracking
typedef struct
{
    bool valid;
    bool is_xbox_one; // If not it's Xbox 360
    uint8_t dev_addr;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t report_buffer[64]; // XInput max 64 bytes
    bool start_360_pending;
    absolute_time_t start_360_time;
} xbox_device_t;

static xbox_device_t xbox_devices[PAD_MAX_PLAYERS];

static int xin_find_index_by_dev_addr(uint8_t dev_addr)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (xbox_devices[i].valid && xbox_devices[i].dev_addr == dev_addr)
            return i;
    return -1;
}

static int xin_find_free_index(void)
{
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (!xbox_devices[i].valid)
            return i;
    return -1;
}

// We can use the same indexing as hid as long as we keep clear
static int xin_idx_to_hid_slot(int idx)
{
    return HID_XIN_START + idx;
}

bool xin_is_xbox_one(int slot)
{
    unsigned int idx = slot - HID_XIN_START;
    return idx < PAD_MAX_PLAYERS &&
           xbox_devices[idx].valid &&
           xbox_devices[idx].is_xbox_one;
}

bool xin_is_xbox_360(int slot)
{
    unsigned int idx = slot - HID_XIN_START;
    return idx < PAD_MAX_PLAYERS &&
           xbox_devices[idx].valid &&
           !xbox_devices[idx].is_xbox_one;
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
        return false;

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
        return false;

    int idx = xin_find_free_index();
    if (idx < 0)
        return false;

    xbox_devices[idx].dev_addr = dev_addr;
    xbox_devices[idx].valid = true;
    xbox_devices[idx].is_xbox_one = is_xbox_one;
    xbox_devices[idx].ep_in = ep_in;
    xbox_devices[idx].ep_out = ep_out;

    // Mount in pad system
    uint16_t vendor_id, product_id;
    if (tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
    {
        int slot = xin_idx_to_hid_slot(idx);
        if (!pad_mount(slot, 0, 0, vendor_id, product_id))
        {
            DBG("XInput: Failed to mount in pad system\n");
            memset(&xbox_devices[idx], 0, sizeof(xbox_device_t));
            return false;
        }
    }
    else
    {
        memset(&xbox_devices[idx], 0, sizeof(xbox_device_t));
        return false;
    }

    // Open the endpoints immediately (like HID does)
    if (!tuh_edpt_open(dev_addr, &ep_in_desc))
    {
        DBG("XInput: Failed to open IN endpoint during open\n");
        memset(&xbox_devices[idx], 0, sizeof(xbox_device_t));
        return false;
    }
    if (!tuh_edpt_open(dev_addr, &ep_out_desc))
    {
        DBG("XInput: Failed to open OUT endpoint during open\n");
        tuh_edpt_abort_xfer(dev_addr, ep_in);
        // tuh_edpt_close(dev_addr, ep_in);
        memset(&xbox_devices[idx], 0, sizeof(xbox_device_t));
        return false;
    }

    DBG("XInput: Successfully opened Xbox controller in index %d\n", idx);
    return true;
}

static bool xin_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0)
        return false;

    xbox_device_t *device = &xbox_devices[idx];

    // Send initialization packet after config is set
    if (device->is_xbox_one)
    {
        // Xbox One GIP initialization packet to start input reports
        static const uint8_t xbox_one_init[] = {
            0x05, 0x20, 0x00, 0x01, 0x00};
        tuh_xfer_t xfer = {
            .daddr = device->dev_addr,
            .ep_addr = device->ep_out,
            .buflen = sizeof(xbox_one_init),
            .buffer = (void *)xbox_one_init,
            .complete_cb = NULL,
            .user_data = (uintptr_t)idx};
        if (!tuh_edpt_xfer(&xfer))
            DBG("XInput: Failed to send Xbox One init command\n");
    }
    else
    {
        // Defer Xbox 360 LED command which starts the in transfers
        device->start_360_pending = true;
        device->start_360_time = delayed_by_us(get_absolute_time(),
                                               XIN_START_360_DELAY_MS * 1000);
    }

    DBG("XInput: Configuration complete for index %d\n", idx);
    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool xin_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)ep_addr;

    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0)
        return false;

    if (result != XFER_RESULT_SUCCESS)
    {
        DBG("XInput: Transfer failed for index %d, result=%d, len=%lu\n", idx, result, xferred_bytes);
        return false;
    }

    xbox_device_t *device = &xbox_devices[idx];
    uint8_t *report = device->report_buffer;
    // For Xbox One/Series, check for GIP_CMD_VIRTUAL_KEY 0x07
    if (device->is_xbox_one && xferred_bytes > 4 && report[0] == 0x07)
    {
        uint8_t home = report[4] & 0x01;
        DBG("XInput: home button state: %d\n", home);
        pad_home_button(xin_idx_to_hid_slot(idx), home);
    }
    else
    {
        // Handle just like an HID gamepad report
        pad_report(xin_idx_to_hid_slot(idx), report, (uint16_t)xferred_bytes);
    }
    // Restart the transfer to continue receiving reports
    tuh_xfer_t xfer = {
        .daddr = dev_addr,
        .ep_addr = device->ep_in,
        .buflen = sizeof(device->report_buffer),
        .buffer = device->report_buffer,
        .complete_cb = NULL,
        .user_data = (uintptr_t)idx};
    if (!tuh_edpt_xfer(&xfer))
        DBG("XInput: Failed to start interrupt transfer for index %d\n", idx);

    return true;
}

static void xin_class_driver_close(uint8_t dev_addr)
{
    int index = xin_find_index_by_dev_addr(dev_addr);
    if (index < 0)
        return;

    DBG("XInput: Closing Xbox controller from index %d\n", index);

    tuh_edpt_abort_xfer(dev_addr, xbox_devices[index].ep_in);
    // tuh_edpt_close(dev_addr, xbox_devices[index].ep_in);
    tuh_edpt_abort_xfer(dev_addr, xbox_devices[index].ep_out);
    // tuh_edpt_close(dev_addr, xbox_devices[index].ep_out);

    pad_umount(xin_idx_to_hid_slot(index));

    memset(&xbox_devices[index], 0, sizeof(xbox_device_t));
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

void xin_task(void)
{
    absolute_time_t now = get_absolute_time();
    for (int idx = 0; idx < PAD_MAX_PLAYERS; ++idx)
    {
        xbox_device_t *device = &xbox_devices[idx];
        if (device->valid && device->start_360_pending && absolute_time_diff_us(now, device->start_360_time) <= 0)
        {
            device->start_360_pending = false;
            int pnum = pad_get_player_num(xin_idx_to_hid_slot(idx));
            uint8_t led_cmd[] = {0x01, 0x03, (uint8_t)(0x08 + (pnum & 0x03))};
            tuh_xfer_t led_xfer = {
                .daddr = device->dev_addr,
                .ep_addr = device->ep_out,
                .buflen = sizeof(led_cmd),
                .buffer = led_cmd,
                .complete_cb = NULL,
                .user_data = (uintptr_t)idx};
            if (!tuh_edpt_xfer(&led_xfer))
                DBG("XInput: Failed to send deferred LED command\n");
            else
                DBG("XInput: Sent deferred Xbox 360 LED command for index %d\n", idx);
        }
    }
}

int xin_pad_count(void)
{
    int count = 0;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++)
        if (xbox_devices[i].valid)
            ++count;
    return count;
}
