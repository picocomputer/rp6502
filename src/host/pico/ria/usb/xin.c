/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * XInput controllers, as gamepads ria/hid can read. The protocol is not
 * HID and the controller describes nothing, so what its packet holds is
 * written out here rather than parsed.
 */

#include "core/hid/hid.h"
#include "core/hid/gamepad.h"
#include "ria/usb/xin.h"
#include <tusb.h>
#include <host/usbh_pvt.h>
#include <string.h>

#if defined(DEBUG_USB) || defined(DEBUG_USB_XIN)
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
    uint8_t gip_seq;           // GIP sequence byte for Xbox One OUT packets
    uint8_t init_seq;          // index into gip_init_packets
    bool init_done;            // true after GIP init sequence sent
    int8_t slot;               // where ria/hid mounted it, -1 for nothing
    uint8_t report_buffer[64]; // XInput max 64 bytes
    uint8_t out_cmd[16];       // OUT command buffer (persists for async DMA xfer)
    uint8_t ack_cmd[16];       // Separate buffer for home button ACK (independent of out_cmd)
} xin_device_t;

#define XIN_MAX_DEVICES 4

static xin_device_t xin_devices[XIN_MAX_DEVICES];

// clang-format off

/* XInput is not HID: the controller sends a fixed packet and says nothing
 * about it, so where every field sits is known here rather than read.
 * Button numbers are the ones gamepad.c files at index n-1, so 1-16 land in
 * the two button bytes and 17-20 are read as the d-pad.
 *
 * Y and Rz are declared with their range inverted because the sticks
 * report north as positive and the report block wants it negative. */

static const gamepad_connection_t xin_xbox_360_desc = {
    .valid = true,
    .x_absolute = true,
    .x_offset = 6 * 8, .x_size = 16, .x_min = -32768, .x_max = 32767, // left stick X
    .y_offset = 8 * 8, .y_size = 16, .y_min = 32767, .y_max = -32768, // left stick Y
    .z_offset = 10 * 8, .z_size = 16, .z_min = -32768, .z_max = 32767, // right stick X
    .rz_offset = 12 * 8, .rz_size = 16, .rz_min = 32767, .rz_max = -32768, // right stick Y
    .rx_offset = 4 * 8, .rx_size = 8, .rx_min = 0, .rx_max = 255, // left trigger
    .ry_offset = 5 * 8, .ry_size = 8, .ry_min = 0, .ry_max = 255, // right trigger
    .button_offsets = {
        // A, B, unused, X, Y, unused, LB, RB
        28, 29, HID_ABSENT, 30, 31, HID_ABSENT, 24, 25,
        // L2, R2 (analog only), back, start, guide, L3, R3, unused
        HID_ABSENT, HID_ABSENT, 21, 20, 26, 22, 23, HID_ABSENT,
        // d-pad up, down, left, right
        16, 17, 18, 19}};

/* The Xbox One gamepads its report with a leading id byte of 0x20, so every
 * offset is eight bits further in and the triggers are ten bits wide. */
static const gamepad_connection_t xin_xbox_one_desc = {
    .valid = true,
    .x_absolute = true,
    .report_id = 0x20,
    .x_offset = 9 * 8, .x_size = 16, .x_min = -32768, .x_max = 32767,
    .y_offset = 11 * 8, .y_size = 16, .y_min = 32767, .y_max = -32768,
    .z_offset = 13 * 8, .z_size = 16, .z_min = -32768, .z_max = 32767,
    .rz_offset = 15 * 8, .rz_size = 16, .rz_min = 32767, .rz_max = -32768,
    .rx_offset = 5 * 8, .rx_size = 10, .rx_min = 0, .rx_max = 1023,
    .ry_offset = 7 * 8, .ry_size = 10, .ry_min = 0, .ry_max = 1023,
    .button_offsets = {
        28, 29, HID_ABSENT, 30, 31, HID_ABSENT, 36, 37,
        HID_ABSENT, HID_ABSENT, 27, 26, HID_ABSENT, 38, 39, HID_ABSENT,
        32, 33, 34, 35}};

// clang-format on

static int xin_find_index_by_dev_addr(uint8_t dev_addr)
{
    for (int i = 0; i < XIN_MAX_DEVICES; i++)
        if (xin_devices[i].active && xin_devices[i].dev_addr == dev_addr)
            return i;
    return -1;
}

static int xin_find_free_index(void)
{
    for (int i = 0; i < XIN_MAX_DEVICES; i++)
        if (!xin_devices[i].active)
            return i;
    return -1;
}

static bool xin_queue_in(xin_device_t *device, int idx)
{
    tuh_xfer_t xfer = {
        .daddr = device->dev_addr,
        .ep_addr = device->ep_in,
        .buflen = sizeof(device->report_buffer),
        .buffer = device->report_buffer,
        .complete_cb = NULL,
        .user_data = (uintptr_t)idx};
    return tuh_edpt_xfer(&xfer);
}

bool __in_flash("xin_class_driver_init") xin_class_driver_init(void)
{
    memset(xin_devices, 0, sizeof(xin_devices));
    return true;
}

uint16_t xin_class_driver_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
    (void)rhport;

    DBG("XInput: class=0x%02X sub=0x%02X proto=0x%02X itf=%d\n",
        desc_itf->bInterfaceClass, desc_itf->bInterfaceSubClass,
        desc_itf->bInterfaceProtocol, desc_itf->bInterfaceNumber);

    // Walk descriptors to find endpoints and compute drv_len
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
    uint16_t const drv_len = (uint16_t)((uintptr_t)p_desc - (uintptr_t)desc_itf);

    // Already claimed this device (XInput) — consume all remaining interfaces
    // regardless of class, preventing HID from opening them and wasting ep slots.
    if (xin_find_index_by_dev_addr(dev_addr) >= 0)
    {
        DBG("XInput: Consuming extra interface for dev_addr %d\n", dev_addr);
        return drv_len;
    }

    // Only handle vendor-specific interfaces
    if (desc_itf->bInterfaceClass != 0xFF)
        return 0;

    // Identify controller type
    bool is_xbox_one = (desc_itf->bInterfaceSubClass == 0x47 &&
                        desc_itf->bInterfaceProtocol == 0xD0);
    bool is_x360 = (desc_itf->bInterfaceSubClass == 0x5D &&
                    desc_itf->bInterfaceProtocol == 0x01);

    // Don't consume — could be a non-Xbox vendor device
    if (!is_xbox_one && !is_x360)
        return 0;

    DBG("XInput: Detected %s controller interface\n",
        is_xbox_one ? "Xbox One/Series" : "Xbox 360");

    if (!ep_in_desc || !ep_out_desc)
    {
        DBG("XInput: Missing endpoints (in=%p out=%p)\n", ep_in_desc, ep_out_desc);
        return 0;
    }

    int idx = xin_find_free_index();
    if (idx < 0)
    {
        DBG("XInput: No free device slots\n");
        return 0;
    }

    if (!tuh_edpt_open(dev_addr, ep_in_desc) ||
        !tuh_edpt_open(dev_addr, ep_out_desc))
    {
        DBG("XInput: Failed to open endpoints\n");
        return 0;
    }

    xin_devices[idx].active = true;
    xin_devices[idx].dev_addr = dev_addr;
    xin_devices[idx].itf_num = desc_itf->bInterfaceNumber;
    xin_devices[idx].is_xbox_one = is_xbox_one;
    xin_devices[idx].ep_in = ep_in_desc->bEndpointAddress;
    xin_devices[idx].ep_out = ep_out_desc->bEndpointAddress;
    xin_devices[idx].gip_seq = 0;
    xin_devices[idx].init_seq = 0;

    const gamepad_connection_t *desc = is_xbox_one ? &xin_xbox_one_desc : &xin_xbox_360_desc;
    uint16_t vendor_id, product_id;
    if (!tuh_vid_pid_get(dev_addr, &vendor_id, &product_id) ||
        (xin_devices[idx].slot = (int8_t)hid_mount(NULL, NULL, NULL, desc, vendor_id,
                                                   product_id, GAMEPAD_TYPE_WESTERN)) < 0)
    {
        DBG("XInput: Failed to mount in gamepad system\n");
        tuh_edpt_close(dev_addr, ep_in_desc->bEndpointAddress);
        tuh_edpt_close(dev_addr, ep_out_desc->bEndpointAddress);
        memset(&xin_devices[idx], 0, sizeof(xin_device_t));
        return 0;
    }

    xin_devices[idx].vid = vendor_id;
    xin_devices[idx].pid = product_id;

    DBG("XInput: Claimed Xbox controller in index %d (VID=%04X PID=%04X)\n", idx, vendor_id, product_id);
    return drv_len;
}

// Send the next applicable GIP init packet, returns true if one was sent
static bool xin_send_next_init(xin_device_t *device)
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
        device->out_cmd[2] = device->gip_seq;

        tuh_xfer_t xfer = {
            .daddr = device->dev_addr,
            .ep_addr = device->ep_out,
            .buflen = pkt->len,
            .buffer = device->out_cmd,
            .complete_cb = NULL,
            .user_data = 0};
        if (tuh_edpt_xfer(&xfer))
        {
            device->gip_seq++;
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

// Queue IN first so we catch a GIP_CMD_ANNOUNCE if the controller fires one;
// an announce restarts init from the top (matches Linux xpad).
static void xin_start_xbox_one(xin_device_t *device, int idx)
{
    DBG("XInput: Xbox One — queuing IN then starting GIP init\n");
    if (!xin_queue_in(device, idx))
        DBG("XInput: FAILED to queue IN\n");
    xin_send_next_init(device);
}

// Callback after SET_INTERFACE to disable audio completes
static void xin_audio_disable_cb(tuh_xfer_t *xfer)
{
    int idx = (int)xfer->user_data;
    if (!xin_devices[idx].active)
        return;

    xin_device_t *device = &xin_devices[idx];

    if (xfer->result != XFER_RESULT_SUCCESS)
        DBG("XInput: Audio interface disable failed (result=%d), continuing\n",
            xfer->result);

    xin_start_xbox_one(device, idx);
    usbh_driver_set_config_complete(xfer->daddr, device->itf_num);
}

bool xin_class_driver_set_config(uint8_t dev_addr, uint8_t itf_num)
{
    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0 || xin_devices[idx].itf_num != itf_num)
    {
        // Consumed secondary interface — skip.
        usbh_driver_set_config_complete(dev_addr, itf_num);
        return true;
    }

    xin_device_t *device = &xin_devices[idx];
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
        if (!xin_queue_in(device, idx))
            DBG("XInput: FAILED to queue IN for index %d\n", idx);

        int pnum = gamepad_get_player_num(xin_devices[idx].slot);
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

bool xin_class_driver_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0)
        return false;

    xin_device_t *device = &xin_devices[idx];

    // OUT completion — send next init packet in sequence
    if (ep_addr == device->ep_out)
    {
        DBG("XInput: OUT complete on EP 0x%02X, result=%d, %lu bytes\n", ep_addr, result, xferred_bytes);
        if (result == XFER_RESULT_SUCCESS && device->is_xbox_one && !device->init_done)
            xin_send_next_init(device);
        return true;
    }

    // IN completion
    DBG("XInput: IN on EP 0x%02X, result=%d, %lu bytes\n", ep_addr, result, xferred_bytes);

    if (result == XFER_RESULT_STALLED)
    {
        // Endpoint is halted; re-queuing would loop forever since only
        // CLEAR_FEATURE(ENDPOINT_HALT) can recover it. Stop polling and
        // let the controller drop/reconnect (matches Linux xpad behaviour).
        DBG("XInput: EP 0x%02X STALLed, halting poll\n", ep_addr);
        return true;
    }

    if (result != XFER_RESULT_SUCCESS)
    {
        // Transient RX timeout / data-seq error, not a halt — re-arm and keep polling.
        DBG("XInput: IN transfer FAILED for index %d, result=%d, re-arming\n", idx, result);
        if (!xin_queue_in(device, idx))
            DBG("XInput: FAILED to re-queue IN after error for index %d\n", idx);
        return true;
    }

    uint8_t *report = device->report_buffer;
    if (!device->is_xbox_one)
    {
        // Xbox 360: type 0x00 means input report, ignore others (LED acks, etc.)
        if (report[0] == 0x00 && xferred_bytes >= 14)
            hid_report(xin_devices[idx].slot, report, (uint16_t)xferred_bytes);
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
            // GIP status/heartbeat report — expected and harmless; suppress noisy log.
        }
        else if (gip_cmd == 0x07 && xferred_bytes > 4)
        {
            // GIP_CMD_VIRTUAL_KEY — home button.
            // Payload format: pairs of [state, 0x5B], len_field/2 pairs total.
            // Only the final state in the burst matters for our use.
            uint8_t num_pairs = report[3] / 2;
            if (num_pairs > 0)
            {
                uint16_t last_off = (uint16_t)(4u + (uint16_t)(num_pairs - 1u) * 2u);
                if (last_off < xferred_bytes)
                {
                    uint8_t pressed = report[last_off] & 0x01;
                    DBG("XInput: home button state: %d\n", pressed);
                    gamepad_home_button(xin_devices[idx].slot, pressed);
                }
            }
            // Courtesy ACK for the virtual-key report; the button was already
            // delivered via gamepad_home_button, so a drop on a busy ep_out is fine.
            if ((report[1] & 0x10) && device->init_done)
            {
                device->ack_cmd[0] = 0x01;      // GIP_CMD_ACK
                device->ack_cmd[1] = 0x20;      // GIP_OPT_INTERNAL
                device->ack_cmd[2] = report[2]; // echo sequence number
                device->ack_cmd[3] = 0x09;      // GIP_PL_LEN(9)
                device->ack_cmd[4] = 0x00;
                device->ack_cmd[5] = report[0]; // echo original cmd (0x07)
                device->ack_cmd[6] = 0x20;      // GIP_OPT_INTERNAL
                device->ack_cmd[7] = 0x02;      // GIP_PL_LEN(2)
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
            hid_report(xin_devices[idx].slot, report, (uint16_t)xferred_bytes);
        }
        else
        {
            DBG("XInput: Unhandled GIP cmd 0x%02X (%lu bytes)\n", gip_cmd, xferred_bytes);
        }
    }
    // Restart the transfer to continue receiving reports
    if (!xin_queue_in(device, idx))
        DBG("XInput: FAILED to re-queue IN for index %d\n", idx);
    return true;
}

void xin_class_driver_close(uint8_t dev_addr)
{
    int idx = xin_find_index_by_dev_addr(dev_addr);
    if (idx < 0)
        return;

    DBG("XInput: Closing Xbox controller from index %d\n", idx);

    hid_umount(xin_devices[idx].slot);

    memset(&xin_devices[idx], 0, sizeof(xin_device_t));
}

int xin_status_count(void)
{
    int count = 0;
    for (int i = 0; i < XIN_MAX_DEVICES; i++)
        if (xin_devices[i].active)
            ++count;
    return count;
}
