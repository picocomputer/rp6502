/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The host's text, typed through the keyboard's queue (typed.h) on a
 * machine whose host can hand some over. The text is copied as it is
 * handed over, as code page bytes, and kept until it has been typed, so
 * nothing the host does to its own copy afterwards changes it. No save
 * carries it, and every load ends it. */

#ifndef _CORE_HID_PASTE_HOST_H_
#define _CORE_HID_PASTE_HOST_H_

#include <stdbool.h>
#include <stddef.h>

/* A paste of text the host hands over now, as a web page's paste event
 * does: its turn comes after everything typed so far. Every control
 * character but tab and the line ends is left out, so the text cannot end
 * its own brackets, and each line end, CRLF, CR or LF, is pressed as
 * Enter, CR. A full queue drops it, as it drops a key, and no text queues
 * nothing. */
void paste_start(const char *utf8);

/* Text typed exactly as written, as a script types it: never bracketed,
 * and every character kept. A Ctrl-C in it raises the SIGINT now. False,
 * with nothing typed, when the queue has no room for it. */
bool typed_text(const char *utf8);

/* What clip_utf8.c asks for the keyboard's queue (clip.h). A paste's turn
 * has come: the text it types is utf8, the host's clipboard as it is now,
 * or with none given, the text handed over that has waited longest. Then
 * that text's length and its byte i. paste_host_end lets that text go.
 * paste_host_break lets go the pastes that wait and keeps the plain text,
 * as their entries go and stay in the queue, and is true when the text
 * being typed is no plain text. paste_host_drop lets all of it go. */
void paste_host_begin(const char *utf8);
size_t paste_host_size(void);
char paste_host_at(size_t i);
void paste_host_end(void);
bool paste_host_break(void);
void paste_host_drop(void);

#endif /* _CORE_HID_PASTE_HOST_H_ */
