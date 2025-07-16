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
    uint16_t vendor_id;
    uint16_t product_id;
    bool valid;
    bool is_xinput;
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
    return;
    DBG("XInput: Device mounted at address %d\n", dev_addr);
    uint16_t vendor_id, product_id;
    if (!tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
    {
        return;
    }

    DBG("XInput: Checking device dev_addr=%d, VID=0x%04X, PID=0x%04X\n",
        dev_addr, vendor_id, product_id);

    int slot = xinput_find_free_slot();
    if (slot >= 0)
    {
        // Try to mount as Xbox controller - pad module will determine if it's valid
        pad_mount_xbox_controller(CFG_TUH_HID + slot, vendor_id, product_id);

        // Check if mounting was successful
        if (pad_is_valid(CFG_TUH_HID + slot))
        {
            DBG("XInput: Found Xbox controller dev_addr=%d, VID=0x%04X, PID=0x%04X\n", dev_addr, vendor_id, product_id);

            xbox_devices[slot].dev_addr = dev_addr;
            xbox_devices[slot].vendor_id = vendor_id;
            xbox_devices[slot].product_id = product_id;
            xbox_devices[slot].valid = true;
            xbox_devices[slot].is_xinput = true;

            DBG("XInput: Xbox controller mounted in slot %d\n", slot);
        }
        else
        {
            DBG("XInput: Device is not a valid Xbox controller\n");
        }
    }
    else
    {
        DBG("XInput: No free slots for Xbox controller\n");
    }
}

void tuh_umount_cb(uint8_t dev_addr)
{
    return;
    DBG("XInput: Device unmounted at address %d\n", dev_addr);
    int slot = xinput_find_device_slot(dev_addr);
    if (slot >= 0)
    {
        DBG("XInput: Unmounting Xbox controller from slot %d\n", slot);

        // Notify pad module using slot index
        pad_umount_xbox_controller(CFG_TUH_HID + slot);

        // Clear the slot
        memset(&xbox_devices[slot], 0, sizeof(xbox_device_t));
    }
}
