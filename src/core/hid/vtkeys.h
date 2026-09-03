/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The ANSI keyboard stream on a machine whose OS already decoded the key:
 * everything a host types enters here, and nothing else pushes the com
 * keyboard ring (see core/com/com.h).
 *
 * Which keys are down is a separate question with a separate answer, and this
 * declares none of it. A caller wanting both names both headers -- that is the
 * whole of what keeps vtkeys_key and keyboard_key_down apart.
 */

#ifndef _CORE_HID_VTKEYS_H_
#define _CORE_HID_VTKEYS_H_

#include "core/sys/driver.h"
#include <stdbool.h>
#include <stdint.h>

/* Queue printable UTF-8 input as OEM bytes, converting each sequence to the
 * active code page. */
void vtkeys_text(const char *utf8);

/* Queue one Unicode code point as its OEM byte. A host that already holds a
 * code point calls this; encoding it to UTF-8 for vtkeys_text above only to
 * have that decode it back is the same journey with two more steps. */
void vtkeys_char(uint32_t codepoint);

/* Queue a non-character key as its xterm sequence, annotating shift/alt/ctrl
 * into the modifier number the way xterm/the firmware do. */
bool vtkeys_key(uint8_t hid_usage, bool ctrl, bool shift, bool alt);

/* Queue a Ctrl+<letter> chord as its C0 control byte. A byte outside the
 * promotable @.._ / `..~ range is not a chord and queues nothing. */
void vtkeys_ctrl_letter(char letter);

/* Queue Alt+<char> as the xterm Meta form: ESC then the byte, ctrl-promoted
 * first when both are held (the firmware's order). */
void vtkeys_alt_char(char ch, bool ctrl);

/* Type a block of UTF-8 as keystrokes: printable runs as text, CR/LF/CRLF as one
 * Enter, tab as Tab, other control bytes stripped. Delivery is paced by vtkeys_task
 * against the ring's headroom, so text longer than the ring cannot lose bytes.
 * A new call replaces whatever is still dripping. */
void vtkeys_paste(const char *utf8);
void vtkeys_paste_cancel(void);

/* True while a paste is still being handed to the ring. */
bool vtkeys_paste_busy(void);

/* The paste drip, walked every pass. Ring space is what regulates it, so the window,
 * the headless batch and a script all pace a paste identically. */
void vtkeys_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. No stop hook:
 * type-ahead deliberately survives an exec. */
#define VTKEYS_DRIVER DRIVER(nul_init, vtkeys_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_HID_VTKEYS_H_ */
