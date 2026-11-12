/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "fatfs/ff.h"
#include "core/hid/parse.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/hid/gamepad.h"
#include "host/hcd.h"
#include "core/str/str.h"
#include "ria/usb/msc.h"
#include "ria-w/ble/ble.h"
#include "ria/usb/usb.h"
#include "ria/usb/xin.h"
#include "ria/sys/rp2350.h"
#include "core/sys/debug_log.h"
#include "machine.h"
#include <pico/time.h>
#include <stdio.h>
#include <string.h>
#include <tusb.h>

extern int hcd_free_ep_count(void);

_Static_assert(CFG_TUH_HID <= 8, "usb_gamepad_led_pending bitmask is 8 bits");

static uint8_t usb_gamepad_led_pending;
static uint8_t usb_gamepad_led_dev[CFG_TUH_HID];

/* TinyUSB numbers HID interfaces with one index across all devices, so
 * usb_hid_slot is indexed by that index alone. */
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
enum
{
    USB_APP_DRIVER_XIN,
    USB_APP_DRIVER_MSC,
};
static const usbh_class_driver_t usb_app_drivers[] = {
    [USB_APP_DRIVER_XIN] = {
        .name = "XInput",
        .init = xin_class_driver_init,
        .deinit = NULL,
        .open = xin_class_driver_open,
        .set_config = xin_class_driver_set_config,
        .xfer_cb = xin_class_driver_xfer_cb,
        .close = xin_class_driver_close,
    },
    [USB_APP_DRIVER_MSC] = {
        .name = "MSC",
        .init = msc_class_driver_init,
        .deinit = NULL,
        .open = msc_class_driver_open,
        .set_config = msc_class_driver_set_config,
        .xfer_cb = msc_class_driver_xfer_cb,
        .close = msc_class_driver_close,
    },
};

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = TU_ARRAY_SIZE(usb_app_drivers);
    return usb_app_drivers;
}

extern bool tuh_keep_only_driver(usbh_class_driver_t const *driver);

/* Flashing reads the image from a drive, so USB keeps only the drives. The
 * rest of TinyUSB's class drivers are in flash, and their devices are torn
 * down now, while flash is intact. */
void HOST_IN_FLASH_FUNC usb_flashing(void)
{
    tuh_keep_only_driver(&usb_app_drivers[USB_APP_DRIVER_MSC]);
}

void HOST_IN_FLASH_FUNC usb_init(void)
{
    for (int i = 0; i < CFG_TUH_HID; i++)
        usb_hid_slot[i] = -1;
    tusb_rhport_init_t rh_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(TUH_OPT_RHPORT, &rh_init);
    irq_set_priority(USBCTRL_IRQ, RP2350_IRQ_PRIORITY_USB);
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
    usb_enum_kick();
    RP6502_LOG(usb, INFO, "init");
}

void HOST_IN_RAM_FUNC usb_task(void)
{
    tuh_task();
}

/* The keyboard and gamepad LED reports reach the HID and gamepad code, which
 * is in flash. */
void HOST_IN_RAM_FUNC usb_report_task(void)
{
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

int HOST_IN_FLASH_FUNC usb_status_response(char *buf, size_t buf_size,
                                                                  int state, unsigned)
{
    (void)state;
    int count_gamepad = usb_count_hid_gamepad + xin_status_count();
    int count_ep_free = hcd_free_ep_count();
    oem_snprintf(buf, buf_size, STR_STATUS_USB,
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

// usbh clears the halt before it reports STALLED
void tuh_hid_report_failed_cb(uint8_t dev_addr, uint8_t idx, xfer_result_t result)
{
    if (result != XFER_RESULT_ABORTED)
        tuh_hid_receive_report(dev_addr, idx);
}

void HOST_IN_FLASH_FUNC tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *desc_report, uint16_t desc_len)
{
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, idx);

    uint16_t vendor_id;
    uint16_t product_id;
    tuh_vid_pid_get(dev_addr, &vendor_id, &product_id);

    RP6502_LOG(usb, INFO, "HID dev=%d idx=%d protocol=%d desc_len=%d",
               dev_addr, idx, itf_protocol, desc_len);

    hid_parsed_t parsed;
    hid_parse(desc_report, desc_len, &parsed);

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

        usb_gamepad_led_dev[idx] = dev_addr;
        usb_gamepad_led_pending |= (1u << idx);
    }

    tuh_hid_receive_report(dev_addr, idx);
}

void HOST_IN_FLASH_FUNC tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
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

bool HOST_IN_RAM_FUNC usb_boot_enumerating(void)
{
    if (usb_boot_enum_finished)
        return false;
    if (time_reached(usb_enum_timeout))
    {
        usb_boot_enum_finished = true;
        RP6502_LOG(usb, INFO, "boot enumeration done");
        return false;
    }
    return true;
}

uint16_t HOST_IN_FLASH_FUNC usb_desc_string_ulen(const void *desc_buf, size_t desc_buf_size)
{
    const tusb_desc_string_t *desc = desc_buf;
    if (desc->bDescriptorType != TUSB_DESC_STRING || desc->bLength < 2)
        return 0;
    uint16_t ulen = (desc->bLength - 2) / 2;
    uint16_t max_ulen = (desc_buf_size - sizeof(tusb_desc_string_t)) / sizeof(uint16_t);
    if (ulen > max_ulen)
        ulen = max_ulen;
    // Some devices report a bLength longer than the string and pad the rest
    // with NUL characters.
    while (ulen > 0 && desc->utf16le[ulen - 1] == 0)
        ulen--;
    return ulen;
}

void HOST_IN_FLASH_FUNC usb_desc_string_to_oem(const void *desc_buf,
                                                                         size_t desc_buf_size,
                                                                         char *dest,
                                                                         size_t dest_size)
{
    uint16_t ulen = usb_desc_string_ulen(desc_buf, desc_buf_size);
    if (ulen > USB_DESC_STRING_MAX_CHAR_LEN)
        ulen = USB_DESC_STRING_MAX_CHAR_LEN;
    // The utf16le member of the packed descriptor may be unaligned, so the
    // characters are copied to an aligned array before a uint16_t pointer to
    // them is passed on.
    uint16_t w[USB_DESC_STRING_MAX_CHAR_LEN];
    memcpy(w, (const uint8_t *)desc_buf + offsetof(tusb_desc_string_t, utf16le),
           ulen * sizeof(uint16_t));
    oem_from_wide_n(w, ulen, dest, dest_size);
}

// The strings of one device, each fetched when first asked for. A fetch for
// another device replaces them once no transfer is in flight, and so does a
// new device at the same address. Only usb_string_fetch_cb clears
// usb_string_busy, so a buffer is not reused while a transfer can still write
// to it.
enum
{
    USB_STRING_MANUFACTURER,
    USB_STRING_PRODUCT,
    USB_STRING_SERIAL,
    USB_STRING_COUNT,
};
static uint8_t usb_string_buf[USB_STRING_COUNT][USB_DESC_STRING_BUF_SIZE];
static uint8_t usb_string_daddr; // 0 for none
static uint8_t usb_string_done;  // a bit for each buffer that holds its string
static bool usb_string_busy;

static void usb_string_fetch_cb(tuh_xfer_t *xfer)
{
    uint8_t s = (uint8_t)xfer->user_data;
    if (xfer->result != XFER_RESULT_SUCCESS)
        memset(usb_string_buf[s], 0, USB_DESC_STRING_BUF_SIZE);
    usb_string_done |= 1u << s;
    usb_string_busy = false;
}

static const void *HOST_IN_FLASH_FUNC usb_string_fetch(uint8_t daddr, uint8_t s)
{
    if (daddr == usb_string_daddr && (usb_string_done & (1u << s)))
        return usb_string_buf[s];
    if (usb_string_busy)
        return NULL;
    if (daddr != usb_string_daddr)
    {
        usb_string_daddr = daddr;
        usb_string_done = 0;
    }
    memset(usb_string_buf[s], 0, USB_DESC_STRING_BUF_SIZE);
    tusb_desc_device_t desc;
    uint8_t index = 0;
    if (tuh_descriptor_get_device_local(daddr, &desc))
    {
        const uint8_t indexes[USB_STRING_COUNT] = {
            desc.iManufacturer, desc.iProduct, desc.iSerialNumber};
        index = indexes[s];
    }
    if (!index)
    {
        usb_string_done |= 1u << s; // no such string, or no such device
        return usb_string_buf[s];
    }
    usb_string_busy = true;
    if (!tuh_descriptor_get_string(daddr, index, 0x0409, usb_string_buf[s],
                                   USB_DESC_STRING_BUF_SIZE, usb_string_fetch_cb, s))
        usb_string_busy = false;
    return NULL;
}

const void *HOST_IN_FLASH_FUNC usb_string_fetch_manufacturer(uint8_t daddr)
{
    return usb_string_fetch(daddr, USB_STRING_MANUFACTURER);
}

const void *HOST_IN_FLASH_FUNC usb_string_fetch_product(uint8_t daddr)
{
    return usb_string_fetch(daddr, USB_STRING_PRODUCT);
}

const void *HOST_IN_FLASH_FUNC usb_string_fetch_serial(uint8_t daddr)
{
    return usb_string_fetch(daddr, USB_STRING_SERIAL);
}

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

bool HOST_IN_FLASH_FUNC usb_device_id_hash(uint8_t daddr, char *buf, size_t buf_size)
{
    const void *desc[USB_STRING_COUNT];
    for (uint8_t s = 0; s < USB_STRING_COUNT; s++)
        if (!(desc[s] = usb_string_fetch(daddr, s)))
            return false;
    uint16_t vid, pid;
    tuh_vid_pid_get(daddr, &vid, &pid);
    tusb_desc_device_t dev_desc;
    uint16_t bcd = 0;
    if (tuh_descriptor_get_device_local(daddr, &dev_desc))
        bcd = dev_desc.bcdDevice;
    int n = snprintf(buf, buf_size, "%04X:%04X:%04X:", vid, pid, bcd);
    if (n < 0 || n >= (int)buf_size)
        return false;
    for (uint8_t s = 0; s < USB_STRING_COUNT; s++)
    {
        if (s && n < (int)buf_size - 1)
            buf[n++] = ':';
        usb_desc_string_to_ascii(desc[s], buf + n, buf_size - n);
        n += strlen(buf + n);
    }
    return true;
}

void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void)rhport;
    (void)in_isr;
    if (eventid == HCD_EVENT_DEVICE_ATTACH)
        usb_enum_kick();
}

void tuh_mount_cb(uint8_t daddr)
{
    tuh_bus_info_t bi;
    tuh_bus_info_get(daddr, &bi);
    usb_enum_kick();
    RP6502_LOG(usb, INFO, "mount dev=%u hub=%u port=%u", daddr, bi.hub_addr, bi.hub_port);
}

void tuh_enum_descriptor_device_cb(uint8_t daddr, const tusb_desc_device_t *desc_device)
{
    (void)desc_device;
    // A new device has the address. The old device's transfers have all
    // ended, because usbh fails them when it closes a device.
    if (daddr == usb_string_daddr)
        usb_string_daddr = 0;
    usb_enum_kick();
    RP6502_LOG(usb, DEBUG, "device descriptor");
}

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index,
                                          const tusb_desc_configuration_t *desc_config)
{
    (void)daddr;
    (void)cfg_index;
    (void)desc_config;
    usb_enum_kick();
    RP6502_LOG(usb, DEBUG, "configuration descriptor");
    return true;
}

void hid_set_leds(uint8_t leds)
{
    usb_set_hid_leds(leds);
#ifdef RP6502_RIA_W
    ble_set_hid_leds(leds);
#endif
}

bool hid_boot_enumerating(void)
{
    return usb_boot_enumerating();
}

void hid_remapped(void)
{
}
