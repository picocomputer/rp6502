/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_PICO_RTT_HID_H_
#define _HOST_PICO_RTT_HID_H_

/* Human input devices over a debug probe, in a RIA built with
 * RP6502_RTT_HID. A debugger on the SWD port finds a small structure in RAM
 * and leaves messages through it while the chip runs, so the bench plugs in
 * keyboards, mice and gamepads and sends their reports with nothing on the
 * USB port. A device is its HID report descriptor, and it goes where a USB
 * device's goes: hid_parse and hid_mount, then hid_report, then hid_umount.
 * Nothing here knows what a descriptor describes. An XInput gamepad has no
 * descriptor, so it cannot be one of these.
 *
 * The debugger sends one message at a time. It writes the message's bytes
 * so that they end where the vendor id begins, then the two ids, then the
 * head:
 *
 *   head = verb | seat << 8 | length << 16
 *
 *   1 mount    the bytes are a report descriptor, and the ids are the
 *              device's
 *   2 report   the bytes are a report, as the device sends it
 *   3 unmount  no bytes
 *
 * The task acts and clears the head, which is the acknowledgment. Any other
 * verb is acknowledged and does nothing. A seat is one of the devices the
 * debugger has mounted, and the debugger picks it. After the head comes a
 * byte for each seat, its hid slot plus one, or 0 while nothing is mounted
 * there, which is also what a mount that failed leaves. Then comes a byte
 * for each seat with the HID_CLAIM_ bits of the drivers that took it. All
 * of it is 0 after a reboot.
 *
 * Nothing is timed here: a key held is a report, a wait in the debugger,
 * and another report. rtt_hid.c lays the structure out, and RttHid in
 * src/core/build.py is the debugger's side.
 *
 * It needs nothing of rtt.h, the console on a debug probe. Without
 * RP6502_RTT_HID none of it is built and its row of the driver list is
 * empty. */

#if RP6502_RTT_HID

void rtt_hid_task(void);

/* RTT_HID_DRIVER follows USB_DRIVER in the driver list, so a message is
 * acted on where a USB report is. A mount runs code that is in flash, so
 * the task is not one that runs while flashing. */
#define RTT_HID_DRIVER DRIVER(TASK_PUMPED(rtt_hid_task))

#else

#define RTT_HID_DRIVER DRIVER()

#endif

#endif /* _HOST_PICO_RTT_HID_H_ */
