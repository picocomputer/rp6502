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

// Xbox One controller vendor/product IDs
typedef struct
{
    uint16_t vendor_id;
    uint16_t product_id;
    const char *name;
} xbox_controller_t;

static const xbox_controller_t xbox_controllers[] = {
    // Microsoft controllers
    {0x045E, 0x02D1, "Xbox One Controller"},
    {0x045E, 0x02DD, "Xbox One Controller (Firmware 2015)"},
    {0x045E, 0x02E3, "Xbox One Elite Controller"},
    {0x045E, 0x02EA, "Xbox One S Controller"},
    {0x045E, 0x02FD, "Xbox One S Controller"},
    {0x045E, 0x0B00, "Xbox One Elite Series 2"},
    {0x045E, 0x0B05, "Xbox One Elite Series 2 (Bluetooth)"},
    {0x045E, 0x0B12, "Xbox Series X/S Controller"},
    {0x045E, 0x0B13, "Xbox Series X/S Controller (Bluetooth)"},
    {0x045E, 0x0B20, "Xbox Series X/S Controller"},
    {0x045E, 0x0B21, "Xbox Series X/S Controller (Bluetooth)"},

    // Third-party controllers
    {0x0E6F, 0x0000, "PDP Xbox Controller"},    // Range match - any PDP controller
    {0x24C6, 0x0000, "PowerA Xbox Controller"}, // Range match - any PowerA controller
    {0x0F0D, 0x0000, "Hori Xbox Controller"},   // Range match - any Hori controller
    {0x1532, 0x0000, "Razer Xbox Controller"},  // Range match - any Razer controller
};

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

bool xinput_is_xbox_controller(uint16_t vendor_id, uint16_t product_id)
{
    for (size_t i = 0; i < sizeof(xbox_controllers) / sizeof(xbox_controllers[0]); i++)
    {
        if (xbox_controllers[i].vendor_id == vendor_id)
        {
            // For third-party controllers, we use 0x0000 as wildcard for product_id
            if (xbox_controllers[i].product_id == 0x0000 ||
                xbox_controllers[i].product_id == product_id)
            {
                return true;
            }
        }
    }
    return false;
}

static const char *xinput_get_controller_name(uint16_t vendor_id, uint16_t product_id)
{
    for (size_t i = 0; i < sizeof(xbox_controllers) / sizeof(xbox_controllers[0]); i++)
    {
        if (xbox_controllers[i].vendor_id == vendor_id)
        {
            if (xbox_controllers[i].product_id == 0x0000 ||
                xbox_controllers[i].product_id == product_id)
            {
                return xbox_controllers[i].name;
            }
        }
    }
    return "Unknown Xbox Controller";
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

void xinput_check_device(uint8_t dev_addr)
{
    uint16_t vendor_id, product_id;
    if (!tuh_vid_pid_get(dev_addr, &vendor_id, &product_id))
    {
        return;
    }

    DBG("XInput: Checking device dev_addr=%d, VID=0x%04X, PID=0x%04X\n",
        dev_addr, vendor_id, product_id);

    if (xinput_is_xbox_controller(vendor_id, product_id))
    {
        DBG("XInput: Found Xbox controller: %s\n", xinput_get_controller_name(vendor_id, product_id));

        int slot = xinput_find_free_slot();
        if (slot >= 0)
        {
            xbox_devices[slot].dev_addr = dev_addr;
            xbox_devices[slot].vendor_id = vendor_id;
            xbox_devices[slot].product_id = product_id;
            xbox_devices[slot].valid = true;
            xbox_devices[slot].is_xinput = true;

            // Create a gamepad descriptor for the pad module using slot index
            pad_mount_xbox_controller(CFG_TUH_HID + slot, vendor_id, product_id);

            DBG("XInput: Xbox controller mounted in slot %d\n", slot);
        }
        else
        {
            DBG("XInput: No free slots for Xbox controller\n");
        }
    }
}

void xinput_device_unmount(uint8_t dev_addr)
{
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

// TinyUSB device-level callbacks
void tuh_mount_cb(uint8_t dev_addr)
{
    return;
    DBG("XInput: Device mounted at address %d\n", dev_addr);
    xinput_check_device(dev_addr);
}

void tuh_umount_cb(uint8_t dev_addr)
{
    return;
    DBG("XInput: Device unmounted at address %d\n", dev_addr);
    xinput_device_unmount(dev_addr);
}
