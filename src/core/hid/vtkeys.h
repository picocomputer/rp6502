/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard stream on a machine whose OS has already decoded the key.
 * Everything a host types enters through one of the calls below, and nothing
 * else pushes the com keyboard ring; see core/com/com.h.
 *
 * Which keys are down is core/hid/keyboard.h.
 */

#ifndef _CORE_HID_VTKEYS_H_
#define _CORE_HID_VTKEYS_H_

#include "core/sys/driver.h"
#include <stdbool.h>
#include <stdint.h>

/* Queue printable UTF-8 as OEM bytes in the active code page. */
void vtkeys_text(const char *utf8);

/* Queue one Unicode code point as its OEM byte, for a host that holds a code
 * point rather than UTF-8. */
void vtkeys_char(uint32_t codepoint);

/* Queue a key that has no character of its own as its xterm sequence. */
bool vtkeys_key(uint8_t hid_usage, bool ctrl, bool shift, bool alt);

/* Queue a Ctrl and letter chord as its C0 control byte. A byte that is
 * neither promotable nor a C0 control already is not a chord and queues
 * nothing. */
void vtkeys_ctrl_letter(char letter);

/* Queue Alt and a character as the xterm Meta form, which is ESC then the
 * byte, promoted by Ctrl first when both are held. */
void vtkeys_alt_char(char ch, bool ctrl);

/* Type a block of UTF-8 as keystrokes. CR, LF and CRLF each become one Enter;
 * every other control byte passes through unchanged. vtkeys_task paces the
 * delivery against the room left in the ring, so text longer than the ring
 * cannot lose bytes. A new call replaces whatever is still being delivered. */
void vtkeys_paste(const char *utf8);
void vtkeys_paste_cancel(void);

bool vtkeys_paste_busy(void);

void vtkeys_task(void);

/* There is no stop hook because type-ahead survives an exec on purpose. A
 * break cancels the paste, because the console's break clears the ring and a
 * paste left running would refill it with the rest of what the user just
 * interrupted. */
#define VTKEYS_DRIVER DRIVER(nul_init, vtkeys_task, nul_task, nul_run, nul_stop, vtkeys_paste_cancel, nul_config, nul_config, nul_sst)

#endif /* _CORE_HID_VTKEYS_H_ */
