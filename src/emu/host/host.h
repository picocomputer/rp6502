/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host stdout sink (host.c): feed bytes from a stdout/console write syscall to
 * the terminal (with the firmware's CRLF translation), and an optional raw tap
 * the tests use to assert program output without rendering a frame.
 */

#ifndef _EMU_HOST_H_
#define _EMU_HOST_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* Feed bytes from a stdout/console write syscall to the terminal,
 * applying the firmware's CRLF translation. */
void emu_stdout_write(const char *buf, int len);

/* Same sink without the CRLF translation (com_write / TTY: raw path). */
void emu_stdout_write_raw(const char *buf, int len);

/* Tap the raw terminal byte stream (NULL to clear). Used by tests to assert
 * program output without rendering a frame. */
void emu_set_stdout_tap(void (*tap)(const char *buf, int len));

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_H_ */
