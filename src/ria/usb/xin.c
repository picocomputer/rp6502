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
    uint8_t dev_addr;
    bool valid;
    bool is_xbox_one; // If not it's Xbox 360
    uint8_t interface_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t report_buffer[64];        // Xbox controllers use up to 64 bytes
    tusb_desc_endpoint_t ep_in_desc;  // IN endpoint descriptor
    tusb_desc_endpoint_t ep_out_desc; // OUT endpoint descriptor
} xbox_device_t;

// Configuration state machine
enum
{
    CONFIG_START_TRANSFERS = 0,
    CONFIG_SEND_XBOX_ONE_INIT,
    CONFIG_COMPLETE
};

static xbox_device_t xbox_devices[PAD_MAX_PLAYERS];

// Forward declarations
static void xin_start_interrupt_transfer(uint8_t dev_addr, int slot);
static void xin_send_xbox_one_init(uint8_t dev_addr, int slot);

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

// Class driver implementation
static bool xin_class_driver_init(void)
{
    memset(xbox_devices, 0, sizeof(xbox_devices));
    DBG("XInput: Class driver initialized\n");
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
    xbox_devices[slot].interface_num = desc_itf->bInterfaceNumber;
    xbox_devices[slot].ep_in = ep_in;
    xbox_devices[slot].ep_out = ep_out;
    xbox_devices[slot].ep_in_desc = ep_in_desc;
    xbox_devices[slot].ep_out_desc = ep_out_desc;

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

static void process_set_config(tuh_xfer_t *xfer)
{
    // Process the configuration state machine
    uintptr_t const state = xfer->user_data;
    uint8_t const itf_num = (uint8_t)tu_le16toh(xfer->setup->wIndex);
    uint8_t const daddr = xfer->daddr;

    int slot = xin_find_device_slot(daddr);
    if (slot < 0)
        return;

    switch (state)
    {
    case CONFIG_START_TRANSFERS:
        xin_start_interrupt_transfer(daddr, slot);

        // If this is Xbox One, send init command
        if (xbox_devices[slot].is_xbox_one)
        {
            xin_send_xbox_one_init(daddr, slot);
            xfer->user_data = CONFIG_SEND_XBOX_ONE_INIT;
            process_set_config(xfer);
        }
        else // XBox 360, no init needed
        {
            xfer->user_data = CONFIG_COMPLETE;
            process_set_config(xfer);
        }
        break;

    case CONFIG_SEND_XBOX_ONE_INIT:
        // Xbox One init was sent, now complete
        xfer->user_data = CONFIG_COMPLETE;
        process_set_config(xfer);
        break;

    case CONFIG_COMPLETE:
        DBG("XInput: Configuration complete for slot %d\n", slot);
        // Notify usbh that driver enumeration is complete
        usbh_driver_set_config_complete(daddr, itf_num);
        break;

    default:
        break;
    }
}

static bool xin_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    // Create a fake transfer to kick off the state machine (like HID does)
    tusb_control_request_t request;
    request.wIndex = tu_htole16((uint16_t)itf_num);

    tuh_xfer_t xfer;
    xfer.daddr = dev_addr;
    xfer.result = XFER_RESULT_SUCCESS;
    xfer.setup = &request;
    xfer.user_data = CONFIG_START_TRANSFERS;

    // Start the configuration state machine
    process_set_config(&xfer);

    return true;
}

bool xin_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)ep_addr;

    int slot = xin_find_device_slot(dev_addr);
    if (slot < 0)
    {
        DBG("XInput: Unknown device in xfer_cb\n");
        return false;
    }

    if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0)
    {
        // DBG("XInput: Received %lu bytes from slot %d: ", xferred_bytes, slot);
        // for (uint32_t i = 0; i < xferred_bytes && i < 10; i++) // Show first 10 bytes
        // {
        //     DBG("%02X ", xbox_devices[slot].report_buffer[i]);
        // }
        // DBG("\n");

        pad_report(xin_slot_to_pad_idx(slot), xbox_devices[slot].report_buffer, (uint16_t)xferred_bytes);

        // Restart the transfer to continue receiving reports
        xin_start_interrupt_transfer(dev_addr, slot);
    }
    else
    {
        DBG("XInput: Transfer failed for slot %d, result=%d, len=%lu\n", slot, result, xferred_bytes);

        // On failure, try to restart
        // xin_start_interrupt_transfer(dev_addr, slot);
    }

    return true;
}

void xin_class_driver_close(uint8_t dev_addr)
{
    DBG("XInput: Close called for dev_addr=%d\n", dev_addr);

    int slot = xin_find_device_slot(dev_addr);
    if (slot >= 0)
    {
        DBG("XInput: Closing Xbox controller from slot %d\n", slot);

        // Abort any ongoing transfers and close endpoints
        if (xbox_devices[slot].ep_in != 0)
        {
            tuh_edpt_abort_xfer(dev_addr, xbox_devices[slot].ep_in);
            tuh_edpt_close(dev_addr, xbox_devices[slot].ep_in);
            DBG("XInput: Closed IN endpoint 0x%02X\n", xbox_devices[slot].ep_in);
        }

        if (xbox_devices[slot].ep_out != 0)
        {
            tuh_edpt_abort_xfer(dev_addr, xbox_devices[slot].ep_out);
            tuh_edpt_close(dev_addr, xbox_devices[slot].ep_out);
            DBG("XInput: Closed OUT endpoint 0x%02X\n", xbox_devices[slot].ep_out);
        }

        // Notify pad module using slot index
        pad_umount(xin_slot_to_pad_idx(slot));

        // Clear the slot
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
    }
}

// Start interrupt transfer for Xbox controller
static void xin_start_interrupt_transfer(uint8_t dev_addr, int slot)
{
    if (xbox_devices[slot].ep_in == 0)
    {
        DBG("XInput: No IN endpoint to start transfer\n");
        return;
    }

    tuh_xfer_t xfer = {
        .daddr = dev_addr,
        .ep_addr = xbox_devices[slot].ep_in,
        .buflen = sizeof(xbox_devices[slot].report_buffer),
        .buffer = xbox_devices[slot].report_buffer,
        .complete_cb = NULL, // Use class driver callback instead
        .user_data = (uintptr_t)slot};

    if (!tuh_edpt_xfer(&xfer))
    {
        DBG("XInput: Failed to start interrupt transfer for slot %d\n", slot);
    }
    else
    {
        // DBG("XInput: Started interrupt transfer for slot %d\n", slot);
    }
}

// Send Xbox One initialization command to start input reports
static void xin_send_xbox_one_init(uint8_t dev_addr, int slot)
{
    if (xbox_devices[slot].ep_out == 0)
    {
        DBG("XInput: No OUT endpoint for Xbox One init\n");
        return;
    }

    // Xbox One GIP initialization packet to start input reports
    // This tells the controller to start sending input data
    static const uint8_t xbox_one_init[] = {
        0x05, 0x20, 0x00, 0x01, 0x00 // GIP command to enable input reports
    };

    DBG("XInput: Sending Xbox One initialization command\n");

    tuh_xfer_t xfer = {
        .daddr = dev_addr,
        .ep_addr = xbox_devices[slot].ep_out,
        .buflen = sizeof(xbox_one_init),
        .buffer = (void *)xbox_one_init,
        .complete_cb = NULL,
        .user_data = (uintptr_t)slot};

    if (!tuh_edpt_xfer(&xfer))
    {
        DBG("XInput: Failed to send Xbox One init command\n");
    }
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
