/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _HOST_HOST_H_
#define _HOST_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    uint32_t host_seed(void);
    uint64_t host_clock_us(void);
    uint32_t host_crc32(uint32_t crc, const void *buf, size_t len);

    /* The SAVE: folder, as a host path in UTF-8, or NULL for a host that has
     * none, where SAVE: is the working directory when the ROM starts. */
    const char *host_save_dir(void);

    /* Whether the screen is in a window that is minimized, for the
     * console's answer to CSI 11 t. A machine with a display of its own
     * says no. */
    bool host_minimized(void);

    /* Whether the screen is in a window that gains and loses focus, which
     * the console tells a program that asks (?1004). A machine with a
     * display of its own has no focus to lose. */
    bool host_focusable(void);

    /* The host's clipboard, in UTF-8. get gives its text, or NULL for a
     * host that has no clipboard. The text stays where it is until the
     * clipboard is asked for or set again, and either may write over it or
     * free it. set puts text there, and is false for a host whose clipboard
     * the machine cannot set. The machine reaches both only through
     * clip_host_get and clip_host_set (clip.h), which first end a paste of
     * the text get gave. A script's check asks get itself, as a script runs
     * with no window, where asking writes over nothing (sokol's app.c). */
    const char *host_clipboard_get(void);
    bool host_clipboard_set(const char *utf8);

    /* Whether the user is at a Mac, where Cmd-C and Cmd-V copy and paste
     * (core/hid/key.h). */
    bool host_mac_keys(void);

#ifdef __cplusplus
}
#endif

#endif /* _HOST_HOST_H_ */
