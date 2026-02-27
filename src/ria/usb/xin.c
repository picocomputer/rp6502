/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/pad.h"
#include "usb/xin.h"
#include <tusb.h>
#include <host/usbh_pvt.h>
#include <string.h>

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_XIN)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// GIP init packet definition
typedef struct
{
    uint16_t vid; // 0 = match all
    uint16_t pid; // 0 = match all
    const uint8_t *data;
    uint8_t len;
} gip_init_packet_t;

// GIP init packets from Linux xpad driver (order matters)
// clang-format off
static const uint8_t gip_power_on[]   = {0x05, 0x20, 0x00, 0x01, 0x00};
static const uint8_t gip_s_init[]     = {0x05, 0x20, 0x00, 0x0f, 0x06};
static const uint8_t gip_hori_ack[]   = {0x01, 0x20, 0x00, 0x09, 0x00, 0x04, 0x20, 0x3a,
                                         0x00, 0x00, 0x00, 0x80, 0x00};
static const uint8_t gip_led_on[]     = {0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14};
static const uint8_t gip_auth_done[]  = {0x06, 0x20, 0x00, 0x02, 0x01, 0x00};
static const uint8_t gip_extra_input[]= {0x4d, 0x10, 0x01, 0x02, 0x07, 0x00};
static const uint8_t gip_rumble_on[]  = {0x09, 0x00, 0x00, 0x09, 0x00, 0x0f,
                                         0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00};
static const uint8_t gip_rumble_off[] = {0x09, 0x00, 0x00, 0x09, 0x00, 0x0f,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const gip_init_packet_t gip_init_packets[] = {
    {0x0e6f, 0x0165, gip_hori_ack,    sizeof(gip_hori_ack)},
    {0x0f0d, 0x0067, gip_hori_ack,    sizeof(gip_hori_ack)},
    {0x1430, 0x079b, gip_hori_ack,    sizeof(gip_hori_ack)},
    {0x0000, 0x0000, gip_power_on,    sizeof(gip_power_on)},
    {0x045e, 0x02ea, gip_s_init,      sizeof(gip_s_init)},
    {0x045e, 0x0b00, gip_s_init,      sizeof(gip_s_init)},
    {0x045e, 0x0b00, gip_extra_input, sizeof(gip_extra_input)},
    {0x0e6f, 0x0000, gip_led_on,      sizeof(gip_led_on)},
    {0x1430, 0x079b, gip_led_on,      sizeof(gip_led_on)},
    {0x20d6, 0xa01a, gip_led_on,      sizeof(gip_led_on)},
    {0x0e6f, 0x0000, gip_auth_done,   sizeof(gip_auth_done)},
    {0x1430, 0x079b, gip_auth_done,   sizeof(gip_auth_done)},
    {0x20d6, 0xa01a, gip_auth_done,   sizeof(gip_auth_done)},
    {0x24c6, 0x541a, gip_rumble_on,   sizeof(gip_rumble_on)},
    {0x24c6, 0x542a, gip_rumble_on,   sizeof(gip_rumble_on)},
    {0x24c6, 0x543a, gip_rumble_on,   sizeof(gip_rumble_on)},
    {0x24c6, 0x541a, gip_rumble_off,  sizeof(gip_rumble_off)},
    {0x24c6, 0x542a, gip_rumble_off,  sizeof(gip_rumble_off)},
    {0x24c6, 0x543a, gip_rumble_off,  sizeof(gip_rumble_off)},
};
// clang-format on

#define GIP_INIT_PACKET_COUNT (sizeof(gip_init_packets) / sizeof(gip_init_packets[0]))

// Xbox controller tracking
typedef struct
{
    bool active;
    bool is_xbox_one; // If not it's Xbox 360
    uint8_t dev_addr;
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t vid;
    uint16_t pid;
    uint8_t gip_seq;           // GIP sequence number (out_cmd[2]), for all OUT
    uint8_t init_seq;          // index into gip_init_packets
    bool init_done;            // true after GIP init sequence sent
    bool in_data_received;     // true after first IN xfer callback
    uint8_t report_buffer[64]; // XInput max 64 bytes
    uint8_t out_cmd[16];       // OUT command buffer (persists for async DMA xfer)
    uint8_t ack_cmd[16];       // Separate buffer for home button ACK (avoids out_cmd race)
} xbox_device_t;

#define XIN_MAX_DEVICES 4

static xbox_device_t xbox_devices[XIN_MAX_DEVICES];

// clang-format off

// Synthetic HID descriptors allow use of HID gamepad driver
__in_flash("xin_hid_descriptors") static const uint8_t xbox_one_fake_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop Controls)
    0x09, 0x05, // Usage (Game Pad)
    0xa1, 0x01, // Collection (Application)
    0x85, 0x20, // Report ID (32) - MUST be 0x20 for Xbox One

    // Skip to bit 26 where Menu button goes (3*8+2 = 26)
    0x75, 0x1A, // Report Size (26 bits)
    0x95, 0x01, // Report Count (1)
    0x81, 0x01, // Input (Const,Array,Abs) - padding

    // Menu button at bit 26 (button index 11)
    0x05, 0x09, // Usage Page (Button)
    0x19, 0x0C, // Usage Minimum (0x0C) - button 12
    0x29, 0x0C, // Usage Maximum (0x0C) - button 12
    0x15, 0x00, // Logical Minimum (0)
    0x25, 0x01, // Logical Maximum (1)
    0x95, 0x01, // Report Count (1)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // View button at bit 27 (button index 10)
    0x19, 0x0B, // Usage Minimum (0x0B) - button 11
    0x29, 0x0B, // Usage Maximum (0x0B) - button 11
    0x95, 0x01, // Report Count (1)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // A/B/X/Y buttons at bits 28-29 (button indices 0,1)
    0x19, 0x01, // Usage Minimum (0x01) - A button (index 0)
    0x29, 0x02, // Usage Maximum (0x02) - B button (index 1)
    0x95, 0x02, // Report Count (2)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // X/Y buttons at bits 30-31 (button indices 3,4)
    0x19, 0x04, // Usage Minimum (0x04) - X button (index 3)
    0x29, 0x05, // Usage Maximum (0x05) - Y button (index 4)
    0x95, 0x02, // Report Count (2)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // D-pad buttons at bits 32-35 (button indices 16-19)
    0x19, 0x11, // Usage Minimum (0x11) - button 17
    0x29, 0x14, // Usage Maximum (0x14) - button 20
    0x95, 0x04, // Report Count (4)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // LB/RB buttons at bits 36-37 (button indices 6,7)
    0x19, 0x07, // Usage Minimum (0x07) - LB
    0x29, 0x08, // Usage Maximum (0x08) - RB
    0x95, 0x02, // Report Count (2)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // L3/R3 buttons at bits 38-39 (button indices 13,14)
    0x19, 0x0E, // Usage Minimum (0x0E) - L3
    0x29, 0x0F, // Usage Maximum (0x0F) - R3
    0x95, 0x02, // Report Count (2)
    0x75, 0x01, // Report Size (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Left trigger (Rx) at bit 40, size 10 bits
    0x05, 0x01,       // Usage Page (Generic Desktop Controls)
    0x09, 0x33,       // Usage (Rx)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xff, 0x03, // Logical Maximum (1023)
    0x75, 0x0a,       // Report Size (10)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Padding 6 bits to get to bit 56
    0x75, 0x06, // Report Size (6)
    0x95, 0x01, // Report Count (1)
    0x81, 0x01, // Input (Const,Array,Abs)

    // Right trigger (Ry) at bit 56, size 10 bits
    0x09, 0x34,       // Usage (Ry)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xff, 0x03, // Logical Maximum (1023)
    0x75, 0x0a,       // Report Size (10)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Padding 6 bits to get to bit 72
    0x75, 0x06, // Report Size (6)
    0x95, 0x01, // Report Count (1)
    0x81, 0x01, // Input (Const,Array,Abs)

    // Left stick X at bit 72, size 16 bits
    0x09, 0x30,       // Usage (X)
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xff, 0x7f, // Logical Maximum (32767)
    0x75, 0x10,       // Report Size (16)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Left stick Y at bit 88, size 16 bits
    0x09, 0x31,       // Usage (Y)
    0x16, 0xff, 0x7f, // Logical Minimum (32767) - REVERSED!
    0x26, 0x00, 0x80, // Logical Maximum (-32768) - REVERSED!
    0x75, 0x10,       // Report Size (16)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Right stick X at bit 104, size 16 bits
    0x09, 0x32,       // Usage (Z)
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xff, 0x7f, // Logical Maximum (32767)
    0x75, 0x10,       // Report Size (16)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Right stick Y at bit 120, size 16 bits
    0x09, 0x35,       // Usage (Rz)
    0x16, 0xff, 0x7f, // Logical Minimum (32767) - REVERSED!
    0x26, 0x00, 0x80, // Logical Maximum (-32768) - REVERSED!
    0x75, 0x10,       // Report Size (16)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    0xc0, // End Collection
};

__in_flash("xin_hid_descriptors") static const uint8_t xbox_360_fake_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop Controls)
    0x09, 0x05, // Usage (Game Pad)
    0xa1, 0x01, // Collection (Application)

    // Skip to byte 2 (16 bits total)
    0x75, 0x10, // Report Size (16 bits)
    0x95, 0x01, // Report Count (1)
    0x81, 0x01, // Input (Const,Array,Abs) - padding

    // Byte 2, Bit 0: D-pad Up (maps to button[16])
    0x05, 0x09, // Usage Page (Button)
    0x09, 0x11, // Usage (0x11 = button 17)
    0x15, 0x00, // Logical Minimum (0)
    0x25, 0x01, // Logical Maximum (1)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 1: D-pad Down (maps to button[17])
    0x09, 0x12, // Usage (0x12 = button 18)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 2: D-pad Left (maps to button[18])
    0x09, 0x13, // Usage (0x13 = button 19)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 3: D-pad Right (maps to button[19])
    0x09, 0x14, // Usage (0x14 = button 20)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 4: Start button (maps to button[11])
    0x09, 0x0C, // Usage (0x0C = button 12)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 5: Back button (maps to button[10])
    0x09, 0x0B, // Usage (0x0B = button 11)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 6: Left stick button (maps to button[13])
    0x09, 0x0E, // Usage (0x0E = button 14)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 2, Bit 7: Right stick button (maps to button[14])
    0x09, 0x0F, // Usage (0x0F = button 15)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 0: LB (maps to button[6])
    0x09, 0x07, // Usage (0x07 = button 7)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 1: RB (maps to button[7])
    0x09, 0x08, // Usage (0x08 = button 8)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 2: Home button (maps to button[12])
    0x09, 0x0D, // Usage (0x0D = button 13)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 3: Reserved
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x01, // Input (Const,Array,Abs) - padding

    // Byte 3, Bit 4: A button (maps to button[0])
    0x09, 0x01, // Usage (0x01 = button 1)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 5: B button (maps to button[1])
    0x09, 0x02, // Usage (0x02 = button 2)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 6: X button (maps to button[3])
    0x09, 0x04, // Usage (0x04 = button 4)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    // Byte 3, Bit 7: Y button (maps to button[4])
    0x09, 0x05, // Usage (0x05 = button 5)
    0x75, 0x01, // Report Size (1 bit)
    0x95, 0x01, // Report Count (1)
    0x81, 0x02, // Input (Data,Var,Abs)

    0x05, 0x01, // Usage Page (Generic Desktop Controls)

    // Byte 4: Left trigger (Rx)
    0x09, 0x33,       // Usage (Rx)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xff, 0x00, // Logical Maximum (255) - using 16-bit form
    0x75, 0x08,       // Report Size (8 bits)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Byte 5: Right trigger (Ry)
    0x09, 0x34,       // Usage (Ry)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xff, 0x00, // Logical Maximum (255) - using 16-bit form
    0x75, 0x08,       // Report Size (8 bits)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Bytes 6-7: Left stick X
    0x09, 0x30,       // Usage (X)
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xff, 0x7f, // Logical Maximum (32767)
    0x75, 0x10,       // Report Size (16 bits)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Bytes 8-9: Left stick Y (REVERSED)
    0x09, 0x31,       // Usage (Y)
    0x16, 0xff, 0x7f, // Logical Minimum (32767) - REVERSED!
    0x26, 0x00, 0x80, // Logical Maximum (-32768) - REVERSED!
    0x75, 0x10,       // Report Size (16 bits)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Bytes 10-11: Right stick X
    0x09, 0x32,       // Usage (Z)
    0x16, 0x00, 0x80, // Logical Minimum (-32768)
    0x26, 0xff, 0x7f, // Logical Maximum (32767)
    0x75, 0x10,       // Report Size (16 bits)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    // Bytes 12-13: Right stick Y (REVERSED)
    0x09, 0x35,       // Usage (Rz)
    0x16, 0xff, 0x7f, // Logical Minimum (32767) - REVERSED!
    0x26, 0x00, 0x80, // Logical Maximum (-32768) - REVERSED!
    0x75, 0x10,       // Report Size (16 bits)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x02,       // Input (Data,Var,Abs)

    0xc0, // End Collection
};

// clang-format on

static int xin_find_index_by_dev_addr(uint8_t dev_addr)
{
    for (int i = 0; i < XIN_MAX_DEVICES; i++)
        if (xbox_devices[i].active && xbox_devices[i].dev_addr == dev_addr)
            return i;
    return -1;
}

static int xin_find_free_index(void)
{
    for (int i = 0; i < XIN_MAX_DEVICES; i++)
        if (!xbox_devices[i].active)
            return i;
    return -1;
}

// We can use the same indexing as hid as long as we keep clear
static inline int xin_idx_to_hid_slot(int idx)
{
    return HID_XIN_START + idx;
}

static bool xin_class_driver_init(void)
{
    memset(xbox_devices, 0, sizeof(xbox_devices));
    return true;
}

static bool xin_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
    (void)rhport;

    DBG("XInput: class=0x%02X sub=0x%02X proto=0x%02X itf=%d\n",
        desc_itf->bInterfaceClass, desc_itf->bInterfaceSubClass,
        desc_itf->bInterfaceProtocol, desc_itf->bInterfaceNumber);

    // Only handle vendor-specific interfaces
    if (desc_itf->bInterfaceClass != 0xFF)
        return false;

    // Already claimed this device — consume extra vendor interfaces
    if (xin_find_index_by_dev_addr(dev_addr) >= 0)
    {
        DBG("XInput: Consuming extra interface for dev_addr %d\n", dev_addr);
        return true;
    }

    // Identify controller type
    bool is_xbox_one = (desc_itf->bInterfaceSubClass == 0x47 &&
                        desc_itf->bInterfaceProtocol == 0xD0);
    bool is_x360 = (desc_itf->bInterfaceSubClass == 0x5D &&
                    desc_itf->bInterfaceProtocol == 0x01);

    // Don't consume — could be a non-Xbox vendor device
    if (!is_xbox_one && !is_x360)
        return false;

    DBG("XInput: Detected %s controller interface\n",
        is_xbox_one ? "Xbox One/Series" : "Xbox 360");

    // Find interrupt IN and OUT endpoints
    uint8_t const *p_desc = tu_desc_next(desc_itf);
    uint8_t const *desc_end = (uint8_t const *)desc_itf + max_len;
    tusb_desc_endpoint_t const *ep_in_desc = NULL;
    tusb_desc_endpoint_t const *ep_out_desc = NULL;
    while (p_desc < desc_end && tu_desc_type(p_desc) != TUSB_DESC_INTERFACE)
    {
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT)
        {
            tusb_desc_endpoint_t const *desc_ep = (tusb_desc_endpoint_t const *)p_desc;
            if (desc_ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT)
            {
                uint16_t packet_size = tu_edpt_packet_size(desc_ep);
                if (!ep_in_desc && tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN && packet_size >= 20)
                {
                    ep_in_desc = desc_ep;
                    DBG("XInput: IN endpoint 0x%02X, maxPacket=%d\n", desc_ep->bEndpointAddress, packet_size);
                }
                else if (!ep_out_desc && tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_OUT && packet_size >= 3)
                {
                    ep_out_desc = desc_ep;
                    DBG("XInput: OUT endpoint 0x%02X, maxPacket=%d\n", desc_ep->bEndpointAddress, packet_size);
                }
            }
        }
        p_desc = tu_desc_next(p_desc);
    }

    if (!ep_in_desc || !ep_out_desc)
    {
        DBG("XInput: Missing endpoints (in=%p out=%p)\n", ep_in_desc, ep_out_desc);
        return false;
    }

    int idx = xin_find_free_index();
    if (idx < 0)
    {
        DBG("XInput: No free device slots\n");
        return false;
    }

    if (!tuh_edpt_open(dev_addr, ep_in_desc) ||
        !tuh_edpt_open(dev_addr, ep_out_desc))
    {
        DBG("XInput: Failed to open endpoints\n");
        return false;
    }

    xbox_devices[idx].active = true;
    xbox_devices[idx].dev_addr = dev_addr;
    xbox_devices[idx].itf_num = desc_itf->bInterfaceNumber;
    xbox_devices[idx].is_xbox_one = is_xbox_one;
    xbox_devices[idx].ep_in = ep_in_desc->bEndpointAddress;
    xbox_devices[idx].ep_out = ep_out_desc->bEndpointAddress;
    xbox_devices[idx].gip_seq = 0;
    xbox_devices[idx].init_seq = 0;

    // Mount in pad system with synthetic HID descriptor
    uint8_t const *desc_data = is_xbox_one ? xbox_one_fake_desc : xbox_360_fake_desc;
    uint16_t desc_len = is_xbox_one ? sizeof(xbox_one_fake_desc) : sizeof(xbox_360_fake_desc);
    uint16_t vendor_id, product_id;
    if (!tuh_vid_pid_get(dev_addr, &vendor_id, &product_id) ||
        !pad_mount(xin_idx_to_hid_slot(idx), desc_data, desc_len, vendor_id, product_id))
    {
        DBG("XInput: Failed to mount in pad system\n");
        memset(&xbox_devices[idx], 0, sizeof(xbox_device_t));
        return false;
    }

    xbox_devices[idx].vid = vendor_id;
    xbox_devices[idx].pid = product_id;

    DBG("XInput: Claimed Xbox controller in index %d (VID=%04X PID=%04X)\n", idx, vendor_id, product_id);
    return true;
}

// Send the next applicable GIP init packet, returns true if one was sent
static bool xin_send_next_init(xbox_device_t *device)
{
    while (device->init_seq < GIP_INIT_PACKET_COUNT)
    {
        const gip_init_packet_t *pkt = &gip_init_packets[device->init_seq++];

        // Skip packets not for this device
        if (pkt->vid != 0 && pkt->vid != device->vid)
            continue;
        if (pkt->pid != 0 && pkt->pid != device->pid)
            continue;

        memcpy(device->out_cmd, pkt->data, pkt->len);
        device->out_cmd[2] = device->gip_seq++; // set sequence number

        tuh_xfer_t xfer = {
            .daddr = device->dev_addr,
            .ep_addr = device->ep_out,
            .buflen = pkt->len,
            .buffer = device->out_cmd,
            .complete_cb = NULL,
            .user_data = 0};
        if (tuh_edpt_xfer(&xfer))
        {
            DBG("XInput: Queued GIP init %d/%d (cmd=0x%02X, %d bytes, seq=%d) on EP 0x%02X\n",
                device->init_seq, (int)GIP_INIT_PACKET_COUNT,
                pkt->data[0], pkt->len, device->out_cmd[2], device->ep_out);
            return true;
        }
        DBG("XInput: FAILED to queue GIP init %d - tuh_edpt_xfer returned false\n", device->init_seq - 1);
    }
    DBG("XInput: GIP init sequence complete\n");
    device->init_done = true;
    return false;
}

// Start Xbox One controller — queue IN then begin GIP init.
// IN is queued first so we catch GIP_CMD_ANNOUNCE (0x02) if the
// controller fires it.  The init sequence is also started immediately
// for freshly-powered controllers.  If ANNOUNCE arrives the sequence
// is restarted from the top (harmless, matches Linux xpad).
static void xin_start_xbox_one(xbox_device_t *device, int idx)
{
    DBG("XInput: Xbox One — queuing IN then starting GIP init\n");
    tuh_xfer_t in_xfer = {
        .daddr = device->dev_addr,
        .ep_addr = device->ep_in,
        .buflen = sizeof(device->report_buffer),
        .buffer = device->report_buffer,
        .complete_cb = NULL,
        .user_data = (uintptr_t)idx};
    if (!tuh_edpt_xfer(&in_xfer))
        DBG("XInput: FAILED to queue IN\n");
    xin_send_next_init(device);
}

// Callback after SET_INTERFACE to disable audio completes
static void xin_audio_disable_cb(tuh_xfer_t *xfer)
{
    int idx = (int)xfer->user_data;
    if (idx < 0 || idx >= XIN_MAX_DEVICES || !xbox_devices[idx].active)
        return;

    xbox_device_t *device = &xbox_devices[idx];

    if (xfer->result != XFER_RESULT_SUCCESS)
        DBG("XInput: Audio interface disable failed (result=%d), continuing\n",
            xfer->result);

    xin_start_xbox_one(device, idx);
    usbh_driver_set_config_complete(xfer->daddr, device->itf_num);
}

static bool xin_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0 || xbox_devices[idx].itf_num != itf_num)
    {
        // Consumed secondary interface — skip.
        usbh_driver_set_config_complete(dev_addr, itf_num);
        return true;
    }

    xbox_device_t *device = &xbox_devices[idx];
    DBG("XInput: set_config for dev_addr %d index %d\n", dev_addr, idx);

    if (device->is_xbox_one)
    {
        // Disable the audio interface — some controllers (e.g., PowerA
        // 0x20d6:0x200e) won't report the guide button unless this is done.
        // The callback continues with GIP init after the control transfer.
        if (tuh_interface_set(dev_addr, 1 /*GIP_WIRED_INTF_AUDIO*/, 0,
                              xin_audio_disable_cb, (uintptr_t)idx))
            return true; // init continues in callback
        // Control transfer failed (no audio interface?) — proceed directly
        DBG("XInput: Audio disable skipped, starting GIP init directly\n");
        xin_start_xbox_one(device, idx);
    }
    else
    {
        // Xbox 360: queue IN immediately, then send LED command
        tuh_xfer_t in_xfer = {
            .daddr = dev_addr,
            .ep_addr = device->ep_in,
            .buflen = sizeof(device->report_buffer),
            .buffer = device->report_buffer,
            .complete_cb = NULL,
            .user_data = (uintptr_t)idx};
        tuh_edpt_xfer(&in_xfer);

        int pnum = pad_get_player_num(xin_idx_to_hid_slot(idx));
        device->out_cmd[0] = 0x01;
        device->out_cmd[1] = 0x03;
        device->out_cmd[2] = (uint8_t)(0x06 + (pnum & 0x03));
        tuh_xfer_t xfer = {
            .daddr = dev_addr,
            .ep_addr = device->ep_out,
            .buflen = 3,
            .buffer = device->out_cmd,
            .complete_cb = NULL,
            .user_data = (uintptr_t)idx};
        if (!tuh_edpt_xfer(&xfer))
            DBG("XInput: Failed to send Xbox 360 LED cmd for index %d\n", idx);
    }

    usbh_driver_set_config_complete(dev_addr, itf_num);
    return true;
}

static bool xin_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0)
        return false;

    xbox_device_t *device = &xbox_devices[idx];

    // OUT completion — send next init packet in sequence
    if (ep_addr == device->ep_out)
    {
        DBG("XInput: OUT complete on EP 0x%02X, result=%d, %lu bytes\n", ep_addr, result, xferred_bytes);
        if (device->is_xbox_one && !device->init_done)
            xin_send_next_init(device);
        return true;
    }

    // IN completion
    device->in_data_received = true;
    DBG("XInput: IN on EP 0x%02X, result=%d, %lu bytes\n", ep_addr, result, xferred_bytes);

    if (result == XFER_RESULT_STALLED)
    {
        DBG("XInput: EP 0x%02X STALLed, re-queuing\n", ep_addr);
        tuh_xfer_t xfer = {
            .daddr = dev_addr,
            .ep_addr = device->ep_in,
            .buflen = sizeof(device->report_buffer),
            .buffer = device->report_buffer,
            .complete_cb = NULL,
            .user_data = (uintptr_t)idx};
        if (!tuh_edpt_xfer(&xfer))
            DBG("XInput: FAILED to re-queue IN after STALL\n");
        return true;
    }

    if (result != XFER_RESULT_SUCCESS)
    {
        DBG("XInput: IN transfer FAILED for index %d, result=%d\n", idx, result);
        return false;
    }

    uint8_t *report = device->report_buffer;
    if (!device->is_xbox_one)
    {
        // Xbox 360: type 0x00 means input report, ignore others (LED acks, etc.)
        if (report[0] == 0x00 && xferred_bytes >= 14)
            pad_report(xin_idx_to_hid_slot(idx), report, (uint16_t)xferred_bytes);
    }
    else
    {
        uint8_t gip_cmd = report[0];
        DBG("XInput: GIP cmd=0x%02X opts=0x%02X seq=%d len_field=0x%02X\n",
            gip_cmd,
            xferred_bytes > 1 ? report[1] : 0,
            xferred_bytes > 2 ? report[2] : 0,
            xferred_bytes > 3 ? report[3] : 0);

        if (gip_cmd == 0x02 && xferred_bytes >= 4)
        {
            // GIP_CMD_ANNOUNCE — controller requesting (re-)initialization.
            // This happens when the controller resets or changes power state.
            // Re-run the full GIP init sequence (mirrors Linux xpad behavior).
            DBG("XInput: GIP announce received, restarting init sequence\n");
            device->init_seq = 0;
            device->gip_seq = 0;
            device->init_done = false;
            xin_send_next_init(device);
        }
        else if (gip_cmd == 0x03)
        {
            // GIP_CMD_ACK — controller acknowledging a command we sent.
            // Expected and harmless; suppress noisy log.
        }
        else if (gip_cmd == 0x07 && xferred_bytes > 4)
        {
            // GIP_CMD_VIRTUAL_KEY — home button.
            // Payload format: pairs of [state, 0x5B], len_field/2 pairs total.
            // Only the final state in the burst matters for our use.
            {
                uint8_t num_pairs = report[3] / 2;
                if (num_pairs > 0)
                {
                    uint16_t last_off = (uint16_t)(4u + (uint16_t)(num_pairs - 1u) * 2u);
                    if (last_off < xferred_bytes)
                    {
                        uint8_t si = report[last_off] & 0x01;
                        DBG("XInput: home button state: %d\n", si);
                        pad_home_button(xin_idx_to_hid_slot(idx), si);
                    }
                }
            }
            // ACK for mode button reports — uses ack_cmd to avoid
            // racing with out_cmd which may still be in-flight for init/LED.
            if ((report[1] & 0x10) && device->init_done)
            {
                device->ack_cmd[0] = 0x01;      // GIP_CMD_ACK
                device->ack_cmd[1] = 0x20;      // GIP_OPT_INTERNAL
                device->ack_cmd[2] = report[2]; // echo sequence number
                device->ack_cmd[3] = 0x09;      // GIP_PL_LEN(9)
                device->ack_cmd[4] = 0x00;
                device->ack_cmd[5] = report[0]; // echo original cmd (0x07)
                device->ack_cmd[6] = report[1]; // echo original opts
                device->ack_cmd[7] = report[3]; // echo original len_field
                memset(&device->ack_cmd[8], 0, 5);
                tuh_xfer_t ack_xfer = {
                    .daddr = dev_addr,
                    .ep_addr = device->ep_out,
                    .buflen = 13,
                    .buffer = device->ack_cmd,
                    .complete_cb = NULL,
                    .user_data = (uintptr_t)idx};
                if (!tuh_edpt_xfer(&ack_xfer))
                    DBG("XInput: Failed to send home button ACK\n");
            }
        }
        else if (gip_cmd == 0x20)
        {
            // GIP_CMD_INPUT — standard input report
            pad_report(xin_idx_to_hid_slot(idx), report, (uint16_t)xferred_bytes);
        }
        else
        {
            DBG("XInput: Unhandled GIP cmd 0x%02X (%lu bytes)\n", gip_cmd, xferred_bytes);
        }
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
        DBG("XInput: FAILED to re-queue IN for index %d\n", idx);
    return true;
}

static void xin_class_driver_close(uint8_t dev_addr)
{
    int index = xin_find_index_by_dev_addr(dev_addr);
    if (index < 0)
        return;

    DBG("XInput: Closing Xbox controller from index %d\n", index);

    pad_umount(xin_idx_to_hid_slot(index));

    // Mark inactive first — xfer_cb checks active via find_index_by_dev_addr
    // so any in-flight transfer completion after this will be safely ignored.
    // The report_buffer and out_cmd live in the struct and remain valid memory
    // until memset, which is safe since TinyUSB cancels endpoint transfers
    // before calling close.
    xbox_devices[index].active = false;
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

int xin_pad_count(void)
{
    int count = 0;
    for (int i = 0; i < XIN_MAX_DEVICES; i++)
        if (xbox_devices[i].active)
            ++count;
    return count;
}
