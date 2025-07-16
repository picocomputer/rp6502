/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb_config.h"
#include "tusb.h"
#include "usb/xinput.h"
#include "usb/pad.h"
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
    uint8_t interface_num;
    uint8_t ep_in;
    uint8_t ep_out;
} xbox_device_t;

static xbox_device_t xbox_devices[PAD_PLAYER_LEN];

void xinput_init(void)
{
    memset(xbox_devices, 0, sizeof(xbox_devices));
    DBG("XInput: Initialized (device-level detection)\n");
}

static int xinput_find_device_slot(uint8_t dev_addr)
{
    for (int i = 0; i < CFG_TUH_DEVICE_MAX; i++)
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
    for (int i = 0; i < CFG_TUH_DEVICE_MAX; i++)
    {
        if (!xbox_devices[i].valid)
        {
            return i;
        }
    }
    return -1;
}

void tuh_mount_cb(uint8_t dev_addr)
{
    uint16_t vendor_id, product_id;
    if (!tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
        return;

    DBG("XInput: Checking device dev_addr=%d, VID=0x%04X, PID=0x%04X\n",
        dev_addr, vendor_id, product_id);

    int slot = xinput_find_free_slot();
    if (slot >= 0)
    {
        pad_mount(CFG_TUH_HID + slot, 0, 0, dev_addr, vendor_id, product_id);
        if (pad_is_valid(CFG_TUH_HID + slot))
        {
            DBG("XInput: Device mounted in slot %d at address %d\n", slot, dev_addr);
            xbox_devices[slot].dev_addr = dev_addr;
            xbox_devices[slot].valid = true;
        }
    }
}

void tuh_umount_cb(uint8_t dev_addr)
{
    DBG("XInput: Device unmounted at address %d\n", dev_addr);
    int slot = xinput_find_device_slot(dev_addr);
    if (slot >= 0)
    {
        DBG("XInput: Unmounting Xbox controller from slot %d\n", slot);

        // Notify pad module using slot index
        pad_umount(CFG_TUH_HID + slot);

        // Clear the slot
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
    }
}

// Check if device is Xbox controller based on interface descriptors
// Returns: 0=none, 1=Xbox 360, 2=Xbox One/Series
int xinput_xbox_controller_type(uint8_t dev_addr)
{
    // Buffer to hold configuration descriptor
    uint8_t config_desc_buffer[256];

    // Get configuration descriptor synchronously
    tusb_xfer_result_t result = tuh_descriptor_get_configuration_sync(
        dev_addr, 0, config_desc_buffer, sizeof(config_desc_buffer));

    if (result != XFER_RESULT_SUCCESS)
    {
        DBG("XInput: Failed to get configuration descriptor\n");
        return 0;
    }

    tusb_desc_configuration_t const *desc_cfg = (tusb_desc_configuration_t const *)config_desc_buffer;
    uint8_t const *desc_end = config_desc_buffer + tu_le16toh(desc_cfg->wTotalLength);
    uint8_t const *p_desc = tu_desc_next(desc_cfg);

    DBG("XInput: Parsing configuration descriptor (wTotalLength = %u)\n", tu_le16toh(desc_cfg->wTotalLength));

    // Parse each interface
    while (p_desc < desc_end)
    {
        // Skip interface association descriptors
        if (TUSB_DESC_INTERFACE_ASSOCIATION == tu_desc_type(p_desc))
        {
            p_desc = tu_desc_next(p_desc);
            continue;
        }

        // Must be interface descriptor
        if (TUSB_DESC_INTERFACE != tu_desc_type(p_desc))
        {
            p_desc = tu_desc_next(p_desc);
            continue;
        }

        tusb_desc_interface_t const *desc_itf = (tusb_desc_interface_t const *)p_desc;

        // DBG("XInput: Interface %u - Class=0x%02X, Subclass=0x%02X, Protocol=0x%02X\n",
        //     desc_itf->bInterfaceNumber, desc_itf->bInterfaceClass,
        //     desc_itf->bInterfaceSubClass, desc_itf->bInterfaceProtocol);

        // Check for Xbox controller interface patterns
        if (desc_itf->bInterfaceClass == 0xFF) // Vendor Specific
        {
            // Xbox One/Series: Class=0xFF, Subclass=0x47, Protocol=0xD0
            if (desc_itf->bInterfaceSubClass == 0x47 && desc_itf->bInterfaceProtocol == 0xD0)
            {
                DBG("XInput: Found Xbox One/Series controller interface\n");
                return 2; // Xbox One/Series
            }

            // Xbox 360: Class=0xFF, Subclass=0x5D, Protocol=0x01 or 0x02
            if (desc_itf->bInterfaceSubClass == 0x5D &&
                (desc_itf->bInterfaceProtocol == 0x01 || desc_itf->bInterfaceProtocol == 0x02))
            {
                DBG("XInput: Found Xbox 360 controller interface\n");
                return 1; // Xbox 360
            }

            // Some third-party controllers use different subclass/protocol combinations
            // but still use vendor-specific class 0xFF with gamepad-like endpoint configurations
            if (desc_itf->bNumEndpoints >= 1)
            {
                // Check endpoint configuration to see if it looks like a gamepad
                uint8_t const *ep_desc = tu_desc_next(p_desc);
                while (ep_desc < desc_end && tu_desc_type(ep_desc) != TUSB_DESC_INTERFACE)
                {
                    if (tu_desc_type(ep_desc) == TUSB_DESC_ENDPOINT)
                    {
                        tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)ep_desc;

                        // Xbox controllers typically have:
                        // - Interrupt IN endpoint
                        // - Sometimes interrupt OUT endpoint
                        // - Packet sizes like 32 or 64 bytes
                        if ((desc_ep->bEndpointAddress & 0x80) && // IN endpoint
                            desc_ep->bmAttributes.xfer == 3 &&    // Interrupt transfer (TUSB_XFER_INTERRUPT = 3)
                            (tu_edpt_packet_size(desc_ep) == 32 || tu_edpt_packet_size(desc_ep) == 64))
                        {
                            DBG("XInput: Found potential Xbox-compatible controller (vendor-specific with interrupt IN)\n");
                            return 1; // Default to Xbox 360 for generic controllers
                        }
                    }
                    ep_desc = tu_desc_next(ep_desc);
                }
            }
        }

        // Move to next interface
        // Calculate interface total length
        uint16_t itf_len = sizeof(tusb_desc_interface_t);
        uint8_t const *next_desc = tu_desc_next(p_desc);

        while (next_desc < desc_end && tu_desc_type(next_desc) != TUSB_DESC_INTERFACE &&
               tu_desc_type(next_desc) != TUSB_DESC_INTERFACE_ASSOCIATION)
        {
            itf_len += tu_desc_len(next_desc);
            next_desc = tu_desc_next(next_desc);
        }

        p_desc += itf_len;
    }

    return 0; // No Xbox controller found
}
