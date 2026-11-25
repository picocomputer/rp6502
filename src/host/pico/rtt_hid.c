/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtt_hid.h"
#include "core/hid/parse.h"
#include "machine.h"
#include <stdint.h>

#define RTT_HID_DATA_SIZE 512
#define RTT_HID_SEATS 8

enum
{
    RTT_HID_MOUNT = 1,
    RTT_HID_REPORT,
    RTT_HID_UMOUNT,
};

/* The debugger writes the data, the ids and the head, and the chip clears
 * the head and writes the seats. A message's bytes end where the ids begin,
 * so the debugger sends all three in one write of rising addresses, which
 * leaves the head last. It is in .bss, so a reboot empties every seat. */
static struct
{
    uint8_t data[RTT_HID_DATA_SIZE];
    volatile uint16_t vendor_id;
    volatile uint16_t product_id;
    volatile uint32_t head;
    volatile uint8_t slot[RTT_HID_SEATS];
    volatile uint8_t claims[RTT_HID_SEATS];
} rtt_hid_box;

/* The debugger finds this in RAM by its id, which is why the id is here in
 * .data and written nowhere else. */
static struct
{
    char id[16];
    void *box;
    uint16_t size;
    uint16_t seats;
} rtt_hid __attribute__((used)) = {
    .id = "RP6502 RTT HID",
    .box = &rtt_hid_box,
    .size = RTT_HID_DATA_SIZE,
    .seats = RTT_HID_SEATS,
};

/* The debugger is the only writer, so a seat and a length are taken as
 * they come. A seat with nothing mounted has slot -1, which hid_report and
 * hid_umount ignore, as hid_slot_claims does a mount that failed. */
static void HOST_IN_FLASH_FUNC rtt_hid_message(uint32_t head)
{
    const uint8_t seat = (uint8_t)(head >> 8);
    const uint16_t len = (uint16_t)(head >> 16);
    const uint8_t *data = rtt_hid_box.data + RTT_HID_DATA_SIZE - len;
    switch ((uint8_t)head)
    {
    case RTT_HID_MOUNT:
    {
        hid_parsed_t parsed;
        hid_parse(data, len, &parsed);
        int slot = hid_mount(&parsed.keyboard, &parsed.mouse, &parsed.tablet, &parsed.gamepad,
                             rtt_hid_box.vendor_id, rtt_hid_box.product_id, GAMEPAD_TYPE_UNKNOWN);
        rtt_hid_box.slot[seat] = (uint8_t)(slot + 1);
        rtt_hid_box.claims[seat] = hid_slot_claims(slot);
        break;
    }
    case RTT_HID_REPORT:
        hid_report(rtt_hid_box.slot[seat] - 1, data, len);
        break;
    case RTT_HID_UMOUNT:
        hid_umount(rtt_hid_box.slot[seat] - 1);
        rtt_hid_box.slot[seat] = 0;
        rtt_hid_box.claims[seat] = 0;
        break;
    }
    rtt_hid_box.head = 0;
}

void HOST_IN_RAM_FUNC rtt_hid_task(void)
{
    uint32_t head = rtt_hid_box.head;
    if (head)
        rtt_hid_message(head);
}
