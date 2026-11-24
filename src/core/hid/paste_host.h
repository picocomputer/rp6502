/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The host's text, typed through the paste stage (paste.h) on a machine
 * whose host can hand some over. The text is not copied: it is read where
 * the host keeps it. Text the host's clipboard gave stays there until the
 * clipboard is asked for or set again (host.h), so that ends its paste
 * first (paste_host_clipboard_changed). Text a web page's paste event gave
 * stays until the next one, which pastes in its turn. Text typed as written
 * stays where its caller keeps it until it has all been staged
 * (paste_staging). */

#ifndef _CORE_HID_PASTE_HOST_H_
#define _CORE_HID_PASTE_HOST_H_

/* A paste of the host's text: its clipboard's, or what a web page's paste
 * event handed over. Every control character but tab and the line ends is
 * left out, so the text cannot end its own brackets, and each line end,
 * CRLF, CR or LF, is pressed as Enter, CR. */
void paste_start(const char *utf8);

/* Text typed exactly as written, as a script types it: never bracketed,
 * and every character kept. */
void typed_text(const char *utf8);

/* The host's clipboard is about to be asked for or set, which can write
 * over the text it gave: a paste of that text ends here, with its closing
 * bracket. Text typed as written goes on. */
void paste_host_clipboard_changed(void);

#endif /* _CORE_HID_PASTE_HOST_H_ */
