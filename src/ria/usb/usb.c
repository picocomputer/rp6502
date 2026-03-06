/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "str/str.h"
#include "usb/vcp.h"
#include "usb/msc.h"
#include "usb/usb.h"
#include "usb/xin.h"
#include <tusb.h>
#include "host/hcd.h"
#include <pico/time.h>
#include <stdio.h>

#define DEBUG_RIA_USB_USB

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint8_t usb_pad_led_dev;
static uint8_t usb_pad_led_idx;
static uint8_t usb_hid_leds;
static uint8_t usb_hid_leds_dev;
static uint8_t usb_hid_leds_idx;
static uint8_t usb_count_hid_kbd;
static uint8_t usb_count_hid_mou;
static uint8_t usb_count_hid_pad;
static absolute_time_t usb_boot_enum_timeout;
static uint8_t usb_hub_binterval_ms;

static inline int usb_idx_to_hid_slot(int idx)
{
    return HID_USB_START + idx;
}

void usb_init(void)
{
    tusb_rhport_init_t rh_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(TUH_OPT_RHPORT, &rh_init);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
}

void usb_task(void)
{
    tuh_task();
    if (usb_pad_led_dev)
    {
        uint8_t led_buf[PAD_LED_REPORT_MAX];
        uint8_t report_id;
        uint16_t report_len;
        if (pad_build_led_report(usb_idx_to_hid_slot(usb_pad_led_idx), led_buf,
                                 &report_id, &report_len) &&
            tuh_hid_send_report(usb_pad_led_dev, usb_pad_led_idx,
                                report_id, led_buf, report_len))
            usb_pad_led_dev = 0;
    }
    while (usb_hid_leds_dev)
    {
        while (usb_hid_leds_idx < CFG_TUH_HID)
        {
            if (tuh_hid_interface_protocol(usb_hid_leds_dev, usb_hid_leds_idx) == HID_ITF_PROTOCOL_KEYBOARD)
                if (!tuh_hid_set_report(usb_hid_leds_dev, usb_hid_leds_idx, 0, HID_REPORT_TYPE_OUTPUT,
                                        &usb_hid_leds, sizeof(usb_hid_leds)))
                    return; // Control endpoint busy, resume next task
            usb_hid_leds_idx++;
        }
        usb_hid_leds_idx = 0;
        if (++usb_hid_leds_dev > CFG_TUH_DEVICE_MAX)
            usb_hid_leds_dev = 0;
    }
}

int usb_status_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    int count_gamepad = usb_count_hid_pad + xin_pad_count();
    int count_storage = msc_status_count();
    int count_serial = vcp_status_count();
    snprintf(buf, buf_size, STR_STATUS_USB,
             usb_count_hid_kbd, usb_count_hid_kbd == 1 ? STR_KEYBOARD_SINGULAR : STR_KEYBOARD_PLURAL,
             usb_count_hid_mou, usb_count_hid_mou == 1 ? STR_MOUSE_SINGULAR : STR_MOUSE_PLURAL,
             count_gamepad, count_gamepad == 1 ? STR_GAMEPAD_SINGULAR : STR_GAMEPAD_PLURAL,
             count_storage, count_storage == 1 ? STR_STORAGE_SINGULAR : STR_STORAGE_PLURAL,
             count_serial, count_serial == 1 ? STR_SERIAL_SINGULAR : STR_SERIAL_PLURAL);
    return -1;
}

static void usb_hid_leds_restart(void)
{
    usb_hid_leds_dev = 1;
    usb_hid_leds_idx = 0;
}

void usb_set_hid_leds(uint8_t leds)
{
    usb_hid_leds = leds;
    usb_hid_leds_restart();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    kbd_report(usb_idx_to_hid_slot(idx), report, len);
    mou_report(usb_idx_to_hid_slot(idx), report, len);
    pad_report(usb_idx_to_hid_slot(idx), report, len);
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    bool valid = false;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    uint16_t vendor_id;
    uint16_t product_id;
    tuh_vid_pid_get(dev_addr, &vendor_id, &product_id);

    DBG("HID: %lums HID dev=%d idx=%d protocol=%d desc_len=%d\n",
        to_ms_since_boot(get_absolute_time()), dev_addr, idx, itf_protocol, desc_len);

    if (kbd_mount(usb_idx_to_hid_slot(idx), desc_report, desc_len))
    {
        ++usb_count_hid_kbd;
        usb_hid_leds_restart();
        valid = true;
    }
    if (mou_mount(usb_idx_to_hid_slot(idx), desc_report, desc_len))
    {
        ++usb_count_hid_mou;
        valid = true;
    }
    if (pad_mount(usb_idx_to_hid_slot(idx), desc_report, desc_len, vendor_id, product_id))
    {
        ++usb_count_hid_pad;
        valid = true;

        // Defer player LED send — not safe during mount callback
        usb_pad_led_dev = dev_addr;
        usb_pad_led_idx = idx;
    }

    if (valid)
        tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)dev_addr;
    if (kbd_umount(usb_idx_to_hid_slot(idx)))
        --usb_count_hid_kbd;
    if (mou_umount(usb_idx_to_hid_slot(idx)))
        --usb_count_hid_mou;
    if (pad_umount(usb_idx_to_hid_slot(idx)))
        --usb_count_hid_pad;
}

// The only way to detect when USB is done enumerating at boot is
// with timers. tuh_connected(0) is true only while a device holds
// address 0 (the addr-0 enumeration phase). There is no "all done"
// callback in TinyUSB; tuh_mount_cb is not fired for hubs.
//
// Timer roles:
//   ATTACH_MS  – backstop covering the entire addr-0 enumeration phase
//                (ATTACH → CONNECT). tuh_event_hook_cb fires before
//                enumerating_daddr is set, so if the polling loop misses
//                the brief tuh_connected(0)==true window, this is the
//                only safety net. Static delays in usbh.c alone sum to
//                212ms (150 debounce + 50 root-reset + 2 post-reset +
//                10 recovery); observed ≈263ms. Needs margin for LS
//                devices and system load.
//   CONNECT_MS – after tuh_connected(0) drops (address assigned),
//                covers remaining enumeration + hub gaps before
//                tuh_mount_cb fires. Overridden by IDLE_MS on mount.
//   IDLE_MS    – minimum quiet time after the last tuh_mount_cb.
//                When a hub is present, the actual idle timeout is
//                hub_binterval + IDLE_MS.

#define ATTACH_MS 500
#define CONNECT_MS 500
#define IDLE_MS 100

bool usb_boot_enumerating(void)
{
    static bool usb_boot_enum_finished;
    static bool was_connected;
    if (usb_boot_enum_finished)
        return false;
    // true when a device being enumerated
    bool connected = tuh_connected(0);
    if (connected)
    {
        was_connected = true;
        return true;
    }
    if (was_connected)
    {
        DBG("HID: %lums CONNECT\n",
            to_ms_since_boot(get_absolute_time()));
        was_connected = false;
        usb_boot_enum_timeout = make_timeout_time_ms(CONNECT_MS);
    }
    // Not currently enumerating — wait for the timeout before finishing
    if (time_reached(usb_boot_enum_timeout))
    {
        usb_boot_enum_finished = true;
        DBG("HID: %lums READY !!!\n",
            to_ms_since_boot(get_absolute_time()));
        return false;
    }
    return true;
}

void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    (void)in_isr;
    if (eventid == HCD_EVENT_DEVICE_ATTACH)
    {
        usb_boot_enum_timeout = make_timeout_time_ms(ATTACH_MS);
        DBG("HID: %lums ATTACH rhport=%u\n",
            to_ms_since_boot(get_absolute_time()), rhport);
    }
}

void tuh_mount_cb(uint8_t daddr)
{
    tuh_bus_info_t bi;
    tuh_bus_info_get(daddr, &bi);
    uint32_t idle_ms = usb_hub_binterval_ms ? (uint32_t)usb_hub_binterval_ms + IDLE_MS
                                            : 255u + IDLE_MS;
    usb_boot_enum_timeout = make_timeout_time_ms(idle_ms);
    DBG("HID: %lums MOUNT dev=%u hub=%u port=%u idle=%lums\n",
        to_ms_since_boot(get_absolute_time()), daddr, bi.hub_addr, bi.hub_port, idle_ms);
}

void tuh_enum_descriptor_device_cb(uint8_t daddr, const tusb_desc_device_t *desc_device)
{
    (void)daddr;
    (void)desc_device;
}

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index,
                                          const tusb_desc_configuration_t *desc_config)
{
    (void)daddr;
    (void)cfg_index;
    const uint8_t *p = (const uint8_t *)desc_config;
    const uint8_t *end = p + tu_le16toh(desc_config->wTotalLength);
    for (p = tu_desc_next(p); tu_desc_in_bounds(p, end); p = tu_desc_next(p))
    {
        if (tu_desc_type(p) != TUSB_DESC_INTERFACE)
            continue;
        const tusb_desc_interface_t *itf = (const tusb_desc_interface_t *)p;
        if (itf->bInterfaceClass != TUSB_CLASS_HUB)
            continue;
        // walk endpoints of this interface to find the interrupt EP
        const uint8_t *ep = tu_desc_next(p);
        while (tu_desc_in_bounds(ep, end) && tu_desc_type(ep) != TUSB_DESC_INTERFACE)
        {
            if (tu_desc_type(ep) == TUSB_DESC_ENDPOINT)
            {
                const tusb_desc_endpoint_t *edpt = (const tusb_desc_endpoint_t *)ep;
                if (edpt->bmAttributes.xfer == TUSB_XFER_INTERRUPT)
                {
                    usb_hub_binterval_ms = edpt->bInterval;
                    DBG("HID: %lums HUB bInterval=%ums\n", 
                        to_ms_since_boot(get_absolute_time()),
                        usb_hub_binterval_ms);
                }
            }
            ep = tu_desc_next(ep);
        }
    }
    return true;
}
