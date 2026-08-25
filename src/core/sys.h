/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The rate the machine's host CPU runs at. It is named for the part because
 * that is what it is -- a Picocomputer's RIA is an RP2350 at 256 MHz -- and
 * every other machine answers to the same number: the emulator counts its
 * ticks in it, and the PHI2 rates a program can ask for are the ones this
 * clock can divide to. A machine that ran it slower would not be running the
 * same PHI2. */

#ifndef _CORE_SYS_H_
#define _CORE_SYS_H_

// We run the RP2350 at 256MHz with 0.05V boost.
// One user tested up to 280 MHz on the default 1.10V.
// https://forums.raspberrypi.com/viewtopic.php?t=375975
#define SYS_RP2350_KHZ 256000

#endif /* _CORE_SYS_H_ */
