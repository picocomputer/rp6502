/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_MACHINE_H_
#define _HOST_MACHINE_H_

#define HOST_IN_FLASH(group)
#define HOST_NOT_IN_FLASH(group)
#define HOST_UNINITIALIZED_RAM(name) name

/* APF has four controller slots, and apf.c mounts at most one HID slot
 * for each of them. */
#define HID_MAX_SLOTS 4
#define TERM_MAX_HEIGHT 30
/* Terminal replies are the only bytes pushed into the console rings on
 * this machine. They come from reply_buf in core/com/com.c, which holds 32
 * bytes, and a ring holds one byte less than its size, so 64 is the
 * smallest power of two that takes a full reply_buf. */
#define COM_RING_SIZE 64


#endif /* _HOST_MACHINE_H_ */
