/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What a key spells on the wire. Two machines answer this question -- the
 * firmware's layout engine, from a keycode, and a desktop host's, from text
 * its OS already resolved -- and both were writing out the same VT100 and
 * VT220 forms, including the numbering that says F5 is 15 and F6 is 17.
 * That numbering is a table nobody can check by eye, so it lives once.
 *
 * These write into the caller's buffer; who the bytes go to is the caller's,
 * because one queues them for the 6502 and the other pushes a console ring. */

#ifndef _CORE_HID_VT_H_
#define _CORE_HID_VT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ANSI modifier parameter: 1 with nothing held, +1 shift, +2 alt,
 * +4 ctrl, +8 gui. A host whose window manager owns the gui key passes
 * false for it. */
int vt_ansi_mod(bool shift, bool alt, bool ctrl, bool gui);

/* ESC[1;{mod}{c1} when modified, else the bare ESC{c0}{c1} -- ESC[A for an
 * arrow, ESC O P for F1. Returns the length written. */
size_t vt_vt100(char *out, size_t cap, char c0, char c1, int ansi_mod);

/* The numbered form: ESC[{num}~, or ESC[{num};{mod}~ when modified. */
size_t vt_vt220(char *out, size_t cap, int num, int ansi_mod);

/* The escape sequence a key with no character of its own spells, chosen by HID
 * usage: the twelve function keys and the ten navigation keys. Zero for any
 * other usage, including Enter, Tab, Escape and Backspace -- those have
 * characters, and which character is the machine's to say. Both machines
 * reach here holding a usage already, so neither needs a key enum. */
size_t vt_key(char *out, size_t cap, uint8_t hid_usage, int ansi_mod);

/* C0 promotion of a printable byte: Ctrl-A is 0x01 through Ctrl-Z 0x1A, and
 * the punctuation range with it. 0 for a byte outside both. Backspace is a
 * keycode question, so a machine that has one answers that itself. */
char vt_ctrl_promote(char ch);

#endif /* _CORE_HID_VT_H_ */
