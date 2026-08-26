/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_HOST_H_
#define _HOST_POCKET_HOST_H_

/* The launcher chain's path buffer, before the contract's default. */
#define PROC_PATH_MAX 128

/* Static RAM, and the keyboard ring is fed by a 16-entry key queue. */
#define COM_RING_SIZE 128

#include "host/os.h"

/* The dock's four ports are every device this machine can have. */
#define HID_MAX_SLOTS 4

#endif /* _HOST_POCKET_HOST_H_ */
