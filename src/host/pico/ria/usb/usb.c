/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/oem.h"
#include "fatfs/ff.h"
#include "core/hid/parse.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/hid/gamepad.h"
#include "host/hcd.h"
#include "ria/main.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/usb/msc.h"
#include "ria/ble/ble.h"
#include "ria/usb/usb.h"
#include "ria/usb/xin.h"
#include <pico/time.h>
#include <stdio.h>
#include <string.h>
#include <tusb.h>

extern int hcd_free_ep_count(void);

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_USB)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

_Static_assert(CFG_TUH_HID <= 8, "usb_gamepad_led_pending bitmask is 8 bits");

static uint8_t usb_gamepad_led_pending;
static uint8_t usb_gamepad_led_dev[CFG_TUH_HID];

/* TinyUSB hands out one interface index across every device, so it is
 * what a mounted device is remembered by here. -1 is nothing mounted. */
static int8_t usb_hid_slot[CFG_TUH_HID];
static uint8_t usb_hid_leds;
static uint8_t usb_hid_leds_next_dev;
static uint8_t usb_hid_leds_next_idx;
static uint8_t usb_count_hid_keyboard;
static uint8_t usb_count_hid_mouse;
static uint8_t usb_count_hid_gamepad;
static absolute_time_t usb_enum_timeout;
static bool usb_boot_enum_finished;

// Max bInterval is 255ms, plus slack for the slowest driver to mount.
#define USB_ENUM_WINDOW_MS (255 + 100)

static inline void usb_enum_kick(void)
{
    usb_enum_timeout = make_timeout_time_ms(USB_ENUM_WINDOW_MS);
}

// Custom application class drivers registered with the TinyUSB host.
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    static const usbh_class_driver_t drivers[] = {
        {
            .name = "XInput",
            .init = xin_class_driver_init,
            .deinit = NULL,
            .open = xin_class_driver_open,
            .set_config = xin_class_driver_set_config,
            .xfer_cb = xin_class_driver_xfer_cb,
            .close = xin_class_driver_close,
        },
        {
            .name = "MSC",
            .init = msc_class_driver_init,
            .deinit = NULL,
            .open = msc_class_driver_open,
            .set_config = msc_class_driver_set_config,
            .xfer_cb = msc_class_driver_xfer_cb,
            .close = msc_class_driver_close,
        },
    };
    *driver_count = TU_ARRAY_SIZE(drivers);
    return drivers;
}

void __in_flash("usb_init") usb_init(void)
{
    for (int i = 0; i < CFG_TUH_HID; i++)
        usb_hid_slot[i] = -1;
    tusb_rhport_init_t rh_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(TUH_OPT_RHPORT, &rh_init);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
    usb_enum_kick();
    DBG("USB: %lums INIT\n", to_ms_since_boot(get_absolute_time()));
}

void usb_task(void)
{
    tuh_task();
    while (usb_gamepad_led_pending)
    {
        int i = __builtin_ctz(usb_gamepad_led_pending);
        uint8_t led_buf[GAMEPAD_LED_REPORT_MAX];
        uint8_t report_id;
        uint16_t report_len;
        if (gamepad_build_led_report(usb_hid_slot[i], led_buf,
                                     &report_id, &report_len))
        {
            if (!tuh_hid_send_report(usb_gamepad_led_dev[i], i,
                                     report_id, led_buf, report_len))
                break; // EP busy, resume next task
        }
        usb_gamepad_led_pending &= ~(1u << i);
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

int usb_status_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    int count_gamepad = usb_count_hid_gamepad + xin_status_count();
    int count_ep_free = hcd_free_ep_count();
    com_snprintf_utf8(buf, buf_size, STR_STATUS_USB,
                      usb_count_hid_keyboard, usb_count_hid_keyboard == 1 ? S(STR_KEYBOARD_SINGULAR) : S(STR_KEYBOARD_PLURAL),
                      usb_count_hid_mouse, usb_count_hid_mouse == 1 ? S(STR_MOUSE_SINGULAR) : S(STR_MOUSE_PLURAL),
                      count_gamepad, count_gamepad == 1 ? S(STR_GAMEPAD_SINGULAR) : S(STR_GAMEPAD_PLURAL),
                      count_ep_free, count_ep_free == 1 ? S(STR_EP_FREE_SINGULAR) : S(STR_EP_FREE_PLURAL));
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
    hid_report(usb_hid_slot[idx], report, len);
    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    uint16_t vendor_id;
    uint16_t product_id;
    tuh_vid_pid_get(dev_addr, &vendor_id, &product_id);

    DBG("USB: %lums HID dev=%d idx=%d protocol=%d desc_len=%d\n",
        to_ms_since_boot(get_absolute_time()), dev_addr, idx, itf_protocol, desc_len);

    hid_parsed_t parsed;
    hid_parse(desc_report, desc_len, &parsed);

    /* Generic HID says nothing about its labels; gamepad.c knows the Sony ids. */
    int slot = hid_mount(&parsed.keyboard, &parsed.mouse, &parsed.tablet, &parsed.gamepad,
                         vendor_id, product_id, GAMEPAD_TYPE_UNKNOWN);
    if (slot < 0)
        return;
    usb_hid_slot[idx] = (int8_t)slot;
    uint8_t claims = hid_slot_claims(slot);

    if (claims & HID_CLAIM_KEYBOARD)
    {
        ++usb_count_hid_keyboard;
        usb_hid_leds_restart();
    }
    if (claims & HID_CLAIM_MOUSE)
        ++usb_count_hid_mouse;
    if (claims & HID_CLAIM_PAD)
    {
        ++usb_count_hid_gamepad;

        // Defer player LED send — not safe during mount callback
        usb_gamepad_led_dev[idx] = dev_addr;
        usb_gamepad_led_pending |= (1u << idx);
    }

    tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
    (void)dev_addr;
    int slot = usb_hid_slot[idx];
    if (slot < 0)
        return;
    usb_hid_slot[idx] = -1;
    uint8_t claims = hid_slot_claims(slot);
    if (claims & HID_CLAIM_KEYBOARD)
        --usb_count_hid_keyboard;
    if (claims & HID_CLAIM_MOUSE)
        --usb_count_hid_mouse;
    if (claims & HID_CLAIM_PAD)
        --usb_count_hid_gamepad;
    hid_umount(slot);
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

// UTF-16 char count in a string descriptor, clamped to the buffer capacity.
uint16_t usb_desc_string_ulen(const void *desc_buf, size_t desc_buf_size)
{
    const tusb_desc_string_t *desc = desc_buf;
    if (desc->bDescriptorType != TUSB_DESC_STRING || desc->bLength < 2)
        return 0;
    uint16_t ulen = (desc->bLength - 2) / 2;
    uint16_t max_ulen = (desc_buf_size - sizeof(tusb_desc_string_t)) / sizeof(uint16_t);
    if (ulen > max_ulen)
        ulen = max_ulen;
    // Some devices over-report bLength and gamepad the string with NUL.
    while (ulen > 0 && desc->utf16le[ulen - 1] == 0)
        ulen--;
    return ulen;
}

// Convert USB string descriptor to OEM for display.
void usb_desc_string_to_oem(const void *desc_buf, size_t desc_buf_size, char *dest, size_t dest_size)
{
    uint16_t ulen = usb_desc_string_ulen(desc_buf, desc_buf_size);
    if (ulen > USB_DESC_STRING_MAX_CHAR_LEN)
        ulen = USB_DESC_STRING_MAX_CHAR_LEN;
    // The packed descriptor's utf16le member isn't alignment-safe to address.
    uint16_t w[USB_DESC_STRING_MAX_CHAR_LEN];
    memcpy(w, (const uint8_t *)desc_buf + offsetof(tusb_desc_string_t, utf16le),
           ulen * sizeof(uint16_t));
    oem_from_wide_n(w, ulen, dest, dest_size);
}

// One fetch at a time; the pending flag holds the buffer until the
// callback releases it, even across a timed-out wait.
static uint8_t usb_string_buf[USB_DESC_STRING_BUF_SIZE];
static bool usb_string_pending;
static xfer_result_t usb_string_result;

static void usb_string_fetch_cb(tuh_xfer_t *xfer)
{
    usb_string_result = xfer->result;
    usb_string_pending = false;
}

// Pumps main_task() while spinning, like msc_scsi_sync.
static const void *usb_string_fetch(uint8_t daddr, uint8_t index)
{
    if (usb_string_pending)
        return NULL;
    memset(usb_string_buf, 0, sizeof(usb_string_buf));
    if (!index)
        return usb_string_buf; // device has no such string
    usb_string_pending = true;
    if (!tuh_descriptor_get_string(daddr, index, 0x0409, usb_string_buf,
                                   sizeof(usb_string_buf), usb_string_fetch_cb, 0))
    {
        usb_string_pending = false;
        return NULL;
    }
    uint32_t start_ms = tusb_time_millis_api();
    while (usb_string_pending)
    {
        if (tusb_time_millis_api() - start_ms >= 250)
            return NULL; // the callback still owns the buffer
        main_task();
    }
    if (usb_string_result != XFER_RESULT_SUCCESS)
        memset(usb_string_buf, 0, sizeof(usb_string_buf));
    return usb_string_buf;
}

static const void *usb_string_fetch_dev_field(uint8_t daddr, size_t field_off)
{
    tusb_desc_device_t desc;
    if (!tuh_descriptor_get_device_local(daddr, &desc))
        return NULL;
    return usb_string_fetch(daddr, ((const uint8_t *)&desc)[field_off]);
}

const void *usb_string_fetch_manufacturer(uint8_t daddr)
{
    return usb_string_fetch_dev_field(daddr, offsetof(tusb_desc_device_t, iManufacturer));
}

const void *usb_string_fetch_product(uint8_t daddr)
{
    return usb_string_fetch_dev_field(daddr, offsetof(tusb_desc_device_t, iProduct));
}

const void *usb_string_fetch_serial(uint8_t daddr)
{
    return usb_string_fetch_dev_field(daddr, offsetof(tusb_desc_device_t, iSerialNumber));
}

// Convert a USB string descriptor to printable ASCII for hashing.
static void usb_desc_string_to_ascii(const void *desc_buf, char *dest, size_t dest_size)
{
    const tusb_desc_string_t *desc = desc_buf;
    uint16_t ulen = usb_desc_string_ulen(desc_buf, USB_DESC_STRING_BUF_SIZE);
    size_t pos = 0;
    for (uint16_t i = 0; i < ulen && pos + 1 < dest_size; i++)
    {
        uint16_t ch = desc->utf16le[i];
        dest[pos++] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '\x7F';
    }
    dest[pos] = '\0';
}

typedef const void *(*usb_id_fetch_fn)(uint8_t daddr);
__in_flash("usb_id_fetchers") static const usb_id_fetch_fn usb_id_fetchers[] = {
    usb_string_fetch_manufacturer,
    usb_string_fetch_product,
    usb_string_fetch_serial,
};

bool usb_device_id_hash(uint8_t daddr, char *buf, size_t buf_size)
{
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    tusb_desc_device_t dev_desc;
    uint16_t bcd = 0;
    if (tuh_descriptor_get_device_local(daddr, &dev_desc))
        bcd = dev_desc.bcdDevice;
    int n = snprintf(buf, buf_size, "%04X:%04X:%04X:", vid, pid, bcd);
    if (n < 0 || n >= (int)buf_size)
        return false;
    for (size_t f = 0; f < TU_ARRAY_SIZE(usb_id_fetchers); f++)
    {
        if (f && n < (int)buf_size - 1)
            buf[n++] = ':';
        const void *desc = usb_id_fetchers[f](daddr);
        if (!desc)
            return false;
        usb_desc_string_to_ascii(desc, buf + n, buf_size - n);
        n += strlen(buf + n);
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

/* Two transports here; core/hid/keyboard.c asks for one. */
void hid_set_leds(uint8_t leds)
{
    usb_set_hid_leds(leds);
    ble_set_hid_leds(leds);
}

bool hid_boot_enumerating(void)
{
    return usb_boot_enumerating();
}

/* Devices report on their own schedule and a remapped block refills from the
 * next one, so there is nothing held here to send again. */
void hid_remapped(void)
{
}
