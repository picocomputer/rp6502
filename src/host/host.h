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

    /* The clipboard, which every host keeps in a clip.c of its own. A host
     * with a clipboard of its own, which builds core/hid/clip_utf8.c, is
     * given each copy in UTF-8 by set, which may put it there later, as a
     * desktop window does at its next frame. get gives its text as it is
     * now, which the machine copies before it returns, so the host may
     * write over it or free it afterwards. A window's paste key never asks:
     * the window hands its text over itself (event_paste). A host with
     * none builds core/hid/clip_codepage.c, and keeps the clipboard's
     * bytes for the machine instead (clip.h). */
    const char *host_clipboard_get(void);
    void host_clipboard_set(const char *utf8);

    /* Whether the user is at a Mac, where Cmd-C and Cmd-V copy and paste
     * (core/hid/key.h). */
    bool host_mac_keys(void);

#ifdef __cplusplus
}
#endif

#endif /* _HOST_HOST_H_ */
