/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2021 Federico Zuccardi Merli
 * Copyright (c) 2026 Rumbledethumps
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <tusb.h>
#include <pico/unique_id.h>

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 *   [MSB]         HID | MSC | CDC          [LSB]
 */
#define _PID_MAP(itf, n) ((CFG_TUD_##itf) << (n))
#define USB_PID (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
                 _PID_MAP(MIDI, 3) | _PID_MAP(VENDOR, 4))

static tusb_desc_device_t const desc_device =
    {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0110,        // USB Specification version 1.1
        .bDeviceClass = 0x00,    // Each interface specifies its own
        .bDeviceSubClass = 0x00, // Each interface specifies its own
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

        .idVendor = 0x2E8A,   // Pi
        .idProduct = USB_PID, // Auto
        .bcdDevice = 0x0100,  // Version 01.00
        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = 0x03,
        .bNumConfigurations = 0x01};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum
{
    ITF_NUM_CDC_COM,  // Interface 0
    ITF_NUM_CDC_DATA, // Interface 1
    ITF_NUM_TOTAL     // Compute total
};

#define CDC_NOTIFICATION_EP_NUM 0x81
#define CDC_DATA_OUT_EP_NUM 0x02
#define CDC_DATA_IN_EP_NUM 0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_COM, 0, CDC_NOTIFICATION_EP_NUM, 64, CDC_DATA_OUT_EP_NUM, CDC_DATA_IN_EP_NUM, 64)};

// Invoked when received GET CONFIGURATION DESCRIPTOR
// Application return pointer to descriptor
// Descriptor contents must exist long enough for transfer to complete
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptor
//--------------------------------------------------------------------+

#define DESC_STR(...) \
    {(TUSB_DESC_STRING << 8) | (sizeof((uint16_t[]){__VA_ARGS__}) + 2), __VA_ARGS__}

static const uint16_t desc_str_lang[] = DESC_STR(0x0409);
static const uint16_t desc_str_manuf[] = DESC_STR('R', 'a', 's', 'p', 'b', 'e', 'r', 'r', 'y', ' ', 'P', 'i');
static const uint16_t desc_str_prod[] = DESC_STR('R', 'P', '6', '5', '0', '2', ' ', 'C', 'o', 'n', 's', 'o', 'l', 'e');
static uint16_t desc_str_serno[1 + PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2];

static const uint16_t *const desc_str_table[] = {
    desc_str_lang,
    desc_str_manuf,
    desc_str_prod,
    desc_str_serno,
};

// Invoked when received GET STRING DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    if (index >= TU_ARRAY_SIZE(desc_str_table))
        return NULL;

    if (index == 3 && desc_str_serno[0] == 0)
    {
        /* Init once */
        pico_unique_board_id_t uID;
        pico_get_unique_board_id(&uID);
        for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2; i++)
        {
            /* Byte index inside the uid array */
            int bi = i / 2;
            /* Use high nibble first to keep memory order (just cosmetics) */
            uint8_t nibble = (uID.id[bi] >> 4) & 0x0F;
            uID.id[bi] <<= 4;
            /* Binary to hex digit */
            desc_str_serno[1 + i] = nibble < 10 ? nibble + '0' : nibble + 'A' - 10;
        }
        desc_str_serno[0] = (TUSB_DESC_STRING << 8) | sizeof(desc_str_serno);
    }

    return desc_str_table[index];
}
