/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/hid.h"
#include "hid/kbd.h"
#include "hid/mou.h"
#include "hid/pad.h"
#include "host/hcd.h"
#include "str/str.h"
#include "usb/msc.h"
#include "usb/usb.h"
#include "usb/vcp.h"
#include "usb/xin.h"
#include <pico/time.h>
#include <stdio.h>
#include <tusb.h>

extern int hcd_free_ep_count(void);

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

_Static_assert(CFG_TUH_HID <= 8, "usb_pad_led_pending bitmask is 8 bits");

static uint8_t usb_pad_led_pending;
static uint8_t usb_pad_led_dev[CFG_TUH_HID];
static uint8_t usb_hid_leds;
static uint8_t usb_hid_leds_next_dev;
static uint8_t usb_hid_leds_next_idx;
static uint8_t usb_count_hid_kbd;
static uint8_t usb_count_hid_mou;
static uint8_t usb_count_hid_pad;
static absolute_time_t usb_enum_timeout;
static bool usb_boot_enum_finished;

// Max bInterval is 255ms, plus slack for the slowest driver to mount.
#define USB_ENUM_WINDOW_MS (255 + 100)

static inline int usb_idx_to_hid_slot(int idx)
{
    return HID_USB_START + idx;
}

static inline void usb_enum_kick(void)
{
    usb_enum_timeout = make_timeout_time_ms(USB_ENUM_WINDOW_MS);
}

void usb_init(void)
{
    tusb_rhport_init_t rh_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(TUH_OPT_RHPORT, &rh_init);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
    usb_enum_kick();
    DBG("USB: %lums INIT\n", to_ms_since_boot(get_absolute_time()));
}

void usb_task(void)
{
    tuh_task();
    while (usb_pad_led_pending)
    {
        int i = __builtin_ctz(usb_pad_led_pending);
        uint8_t led_buf[PAD_LED_REPORT_MAX];
        uint8_t report_id;
        uint16_t report_len;
        if (pad_build_led_report(usb_idx_to_hid_slot(i), led_buf,
                                 &report_id, &report_len))
        {
            if (!tuh_hid_send_report(usb_pad_led_dev[i], i,
                                     report_id, led_buf, report_len))
                break; // EP busy, resume next task
        }
        usb_pad_led_pending &= ~(1u << i);
    }
    while (usb_hid_leds_next_dev)
    {
        while (usb_hid_leds_next_idx < CFG_TUH_HID)
        {
            if (tuh_hid_interface_protocol(usb_hid_leds_next_dev, usb_hid_leds_next_idx) == HID_ITF_PROTOCOL_KEYBOARD)
                if (!tuh_hid_set_report(usb_hid_leds_next_dev, usb_hid_leds_next_idx, 0, HID_REPORT_TYPE_OUTPUT,
                                        &usb_hid_leds, sizeof(usb_hid_leds)))
                    return; // Control endpoint busy, resume next task
            usb_hid_leds_next_idx++;
        }
        usb_hid_leds_next_idx = 0;
        if (++usb_hid_leds_next_dev > CFG_TUH_DEVICE_MAX)
            usb_hid_leds_next_dev = 0;
    }
}

int usb_status_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    int count_gamepad = usb_count_hid_pad + xin_pad_count();
    int count_storage = msc_status_count();
    int count_serial = vcp_status_count();
    int count_ep_free = hcd_free_ep_count();
    snprintf(buf, buf_size, STR_STATUS_USB,
             usb_count_hid_kbd, usb_count_hid_kbd == 1 ? STR_KEYBOARD_SINGULAR : STR_KEYBOARD_PLURAL,
             usb_count_hid_mou, usb_count_hid_mou == 1 ? STR_MOUSE_SINGULAR : STR_MOUSE_PLURAL,
             count_gamepad, count_gamepad == 1 ? STR_GAMEPAD_SINGULAR : STR_GAMEPAD_PLURAL,
             count_storage, count_storage == 1 ? STR_STORAGE_SINGULAR : STR_STORAGE_PLURAL,
             count_serial, count_serial == 1 ? STR_SERIAL_SINGULAR : STR_SERIAL_PLURAL,
             count_ep_free, count_ep_free == 1 ? STR_EP_FREE_SINGULAR : STR_EP_FREE_PLURAL);
    return -1;
}

static void usb_hid_leds_restart(void)
{
    usb_hid_leds_next_dev = 1;
    usb_hid_leds_next_idx = 0;
}

void usb_set_hid_leds(uint8_t leds)
{
    usb_hid_leds = leds;
    usb_hid_leds_restart();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len)
{
    int slot = usb_idx_to_hid_slot(idx);
    kbd_report(slot, report, len);
    mou_report(slot, report, len);
    pad_report(slot, report, len);
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    bool valid = false;
    int slot = usb_idx_to_hid_slot(idx);

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    uint16_t vendor_id;
    uint16_t product_id;
    tuh_vid_pid_get(dev_addr, &vendor_id, &product_id);

    DBG("USB: %lums HID dev=%d idx=%d protocol=%d desc_len=%d\n",
        to_ms_since_boot(get_absolute_time()), dev_addr, idx, itf_protocol, desc_len);

    if (kbd_mount(slot, desc_report, desc_len, vendor_id, product_id))
    {
        ++usb_count_hid_kbd;
        usb_hid_leds_restart();
        valid = true;
    }
    if (mou_mount(slot, desc_report, desc_len))
    {
        ++usb_count_hid_mou;
        valid = true;
    }
    if (pad_mount(slot, desc_report, desc_len, vendor_id, product_id))
    {
        ++usb_count_hid_pad;
        valid = true;

        // Defer player LED send — not safe during mount callback
        usb_pad_led_dev[idx] = dev_addr;
        usb_pad_led_pending |= (1u << idx);
    }

    if (valid)
        tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)dev_addr;
    int slot = usb_idx_to_hid_slot(idx);
    if (kbd_umount(slot))
        --usb_count_hid_kbd;
    if (mou_umount(slot))
        --usb_count_hid_mou;
    if (pad_umount(slot))
        --usb_count_hid_pad;
}

bool usb_boot_enumerating(void)
{
    if (usb_boot_enum_finished)
        return false;
    if (time_reached(usb_enum_timeout))
    {
        usb_boot_enum_finished = true;
        DBG("USB: %lums READY !!!\n", to_ms_since_boot(get_absolute_time()));
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
        usb_enum_kick();
        DBG("USB: %lums ATTACH rhport=%u\n",
            to_ms_since_boot(get_absolute_time()), rhport);
    }
}

void tuh_mount_cb(uint8_t daddr)
{
    tuh_bus_info_t bi;
    tuh_bus_info_get(daddr, &bi);
    usb_enum_kick();
    DBG("USB: %lums MOUNT dev=%u hub=%u port=%u\n",
        to_ms_since_boot(get_absolute_time()), daddr, bi.hub_addr, bi.hub_port);
}

void tuh_enum_descriptor_device_cb(uint8_t daddr, const tusb_desc_device_t *desc_device)
{
    (void)daddr;
    (void)desc_device;
    usb_enum_kick();
    DBG("USB: %lums DESC DEVICE\n", to_ms_since_boot(get_absolute_time()));
}

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index,
                                          const tusb_desc_configuration_t *desc_config)
{
    (void)daddr;
    (void)cfg_index;
    (void)desc_config;
    usb_enum_kick();
    DBG("USB: %lums DESC CONFIG\n", to_ms_since_boot(get_absolute_time()));
    return true;
}
