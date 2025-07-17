/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_config.h"
#include "tusb.h"
#include "usb/xinput.h"
#include "usb/pad.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include <string.h>

#define DEBUG_RIA_USB_XINPUT

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_XINPUT)
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
    bool is_xbone;
    uint8_t interface_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t slot_idx;                 // pad slot index (CFG_TUH_HID + slot)
    uint8_t report_buffer[64];        // buffer for incoming reports
    tusb_desc_endpoint_t ep_in_desc;  // IN endpoint descriptor
    tusb_desc_endpoint_t ep_out_desc; // OUT endpoint descriptor
} xbox_device_t;

static xbox_device_t xbox_devices[PAD_PLAYER_LEN];

// Forward declarations
static void xinput_start_interrupt_transfer(uint8_t dev_addr, int slot);
static void xinput_send_xbox_one_init(uint8_t dev_addr, int slot);

void xinput_init(void)
{
    // This function is called from application code, not the class driver system
    memset(xbox_devices, 0, sizeof(xbox_devices));
    DBG("XInput: Initialized\n");
}

static int xinput_find_device_slot(uint8_t dev_addr)
{
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
    {
        if (xbox_devices[i].valid && xbox_devices[i].dev_addr == dev_addr)
        {
            return i;
        }
    }
    return -1;
}

static int xinput_find_free_slot(void)
{
    for (int i = 0; i < PAD_PLAYER_LEN; i++)
    {
        if (!xbox_devices[i].valid)
        {
            return i;
        }
    }
    return -1;
}

// Class driver implementation
bool xinputh_init(void)
{
    // Called by TinyUSB during initialization
    memset(xbox_devices, 0, sizeof(xbox_devices));
    DBG("XInput: Class driver initialized\n");
    return true;
}

bool xinputh_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
    (void)rhport;

    DBG("XInput: Class driver open called for dev_addr=%d, interface=%d\n", dev_addr, desc_itf->bInterfaceNumber);

    // Check if this is an Xbox controller interface
    if (desc_itf->bInterfaceClass != 0xFF) // Must be vendor specific
    {
        return false;
    }

    // Xbox One/Series: Class=0xFF, Subclass=0x47, Protocol=0xD0
    // Xbox 360: Class=0xFF, Subclass=0x5D, Protocol=0x01 or 0x02
    bool is_x360 = false;
    bool is_xbone = false;
    if (desc_itf->bInterfaceSubClass == 0x47 && desc_itf->bInterfaceProtocol == 0xD0)
    {
        is_xbone = true;
        DBG("XInput: Detected Xbox One/Series controller interface\n");
    }
    else if (desc_itf->bInterfaceSubClass == 0x5D &&
             (desc_itf->bInterfaceProtocol == 0x01 || desc_itf->bInterfaceProtocol == 0x02))
    {
        is_x360 = true;
        DBG("XInput: Detected Xbox 360 controller interface\n");
    }

    if (!is_xbone && !is_x360)
    {
        return false;
    }

    // Find a free slot
    int slot = xinput_find_free_slot();
    if (slot < 0)
    {
        DBG("XInput: No free slots available\n");
        return false;
    }

    // Parse endpoints
    uint8_t const *p_desc = (uint8_t const *)desc_itf;
    uint8_t const *desc_end = p_desc + max_len;
    p_desc = tu_desc_next(p_desc); // Skip interface descriptor

    uint8_t ep_in = 0, ep_out = 0;
    tusb_desc_endpoint_t ep_in_desc = {0}, ep_out_desc = {0};

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
                    DBG("XInput: Found interrupt IN endpoint 0x%02X\n", ep_in);
                }
                else if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_OUT)
                {
                    ep_out = desc_ep->bEndpointAddress;
                    ep_out_desc = *desc_ep;
                    DBG("XInput: Found interrupt OUT endpoint 0x%02X\n", ep_out);
                }
            }
        }
        p_desc = tu_desc_next(p_desc);
    }

    if (ep_in == 0)
    {
        DBG("XInput: No interrupt IN endpoint found\n");
        return false;
    }

    // Initialize slot
    xbox_devices[slot].dev_addr = dev_addr;
    xbox_devices[slot].valid = true;
    xbox_devices[slot].is_xbone = is_xbone;
    xbox_devices[slot].interface_num = desc_itf->bInterfaceNumber;
    xbox_devices[slot].ep_in = ep_in;
    xbox_devices[slot].ep_out = ep_out;
    xbox_devices[slot].slot_idx = CFG_TUH_HID + slot;
    xbox_devices[slot].ep_in_desc = ep_in_desc;
    if (ep_out != 0)
    {
        xbox_devices[slot].ep_out_desc = ep_out_desc;
    }

    // Mount in pad system
    uint16_t vendor_id, product_id;
    if (tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
    {
        pad_mount(xbox_devices[slot].slot_idx, 0, 0, dev_addr, vendor_id, product_id);
        if (!pad_is_valid(xbox_devices[slot].slot_idx))
        {
            DBG("XInput: Failed to mount in pad system\n");
            memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
            return false;
        }
    }
    else
    {
        DBG("XInput: Failed to get VID/PID\n");
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
        return false;
    }

    DBG("XInput: Successfully opened Xbox controller in slot %d\n", slot);
    return true;
}

bool xinputh_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    (void)itf_num;

    DBG("XInput: Set config called for dev_addr=%d, itf_num=%d\n", dev_addr, itf_num);

    int slot = xinput_find_device_slot(dev_addr);
    if (slot < 0)
    {
        DBG("XInput: Device not found in set_config\n");
        return false;
    }

    // Open the endpoints
    if (xbox_devices[slot].ep_in != 0)
    {
        if (!tuh_edpt_open(dev_addr, &xbox_devices[slot].ep_in_desc))
        {
            DBG("XInput: Failed to open IN endpoint\n");
            return false;
        }
        DBG("XInput: Opened IN endpoint 0x%02X\n", xbox_devices[slot].ep_in);
    }

    if (xbox_devices[slot].ep_out != 0)
    {
        if (!tuh_edpt_open(dev_addr, &xbox_devices[slot].ep_out_desc))
        {
            DBG("XInput: Failed to open OUT endpoint\n");
            return false;
        }
        DBG("XInput: Opened OUT endpoint 0x%02X\n", xbox_devices[slot].ep_out);
    }

    // Start receiving reports
    xinput_start_interrupt_transfer(dev_addr, slot);

    // Send Xbox One initialization command if applicable
    if (xbox_devices[slot].ep_out != 0)
    {
        xinput_send_xbox_one_init(dev_addr, slot);
    }

    return true;
}

bool xinputh_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)ep_addr;

    int slot = xinput_find_device_slot(dev_addr);
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

        pad_report(xbox_devices[slot].slot_idx, xbox_devices[slot].report_buffer, (uint16_t)xferred_bytes);

        // Restart the transfer to continue receiving reports
        xinput_start_interrupt_transfer(dev_addr, slot);
    }
    else
    {
        DBG("XInput: Transfer failed for slot %d, result=%d, len=%lu\n", slot, result, xferred_bytes);

        // On failure, try to restart
        // xinput_start_interrupt_transfer(dev_addr, slot);
    }

    return true;
}

void xinputh_close(uint8_t dev_addr)
{
    DBG("XInput: Close called for dev_addr=%d\n", dev_addr);

    int slot = xinput_find_device_slot(dev_addr);
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
        pad_umount(xbox_devices[slot].slot_idx);

        // Clear the slot
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
    }
}

// Start interrupt transfer for Xbox controller
static void xinput_start_interrupt_transfer(uint8_t dev_addr, int slot)
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
static void xinput_send_xbox_one_init(uint8_t dev_addr, int slot)
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

bool xinput_is_xbox_one(uint8_t dev_addr)
{
    int slot = xinput_find_device_slot(dev_addr);
    return slot >= 0 && xbox_devices[slot].is_xbone;
}

bool xinput_is_xbox_360(uint8_t dev_addr)
{
    int slot = xinput_find_device_slot(dev_addr);
    return slot >= 0 && !xbox_devices[slot].is_xbone;
}

//--------------------------------------------------------------------+
// Application driver implementation for TinyUSB
//--------------------------------------------------------------------+

// Define the XInput class driver
static const usbh_class_driver_t xinput_class_driver = {
    .name = "XInput",
    .init = xinputh_init,
    .deinit = NULL, // No cleanup needed
    .open = xinputh_open,
    .set_config = xinputh_set_config,
    .xfer_cb = xinputh_xfer_cb,
    .close = xinputh_close};

// Required callback for TinyUSB to get application drivers
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &xinput_class_driver;
}
