/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The clipboard. libretro gives a core none, so this keeps the bytes the
 * machine copies, for it to paste and to save, runahead and netplay
 * included (core/hid/clip_codepage.c). */

#include "core/hid/clip.h"
#include "core/sys/sst.h"

uint8_t host_clipboard_bytes[CLIP_SIZE];
uint16_t host_clipboard_len;

#define HOST_CLIP_SST(X)                                                     \
    X(SKIP, host_clipboard_bytes, "core/hid/clip_codepage.c's chunk saves it") \
    X(SKIP, host_clipboard_len, "core/hid/clip_codepage.c's chunk saves it")
SST_TABLE(host_clip, HOST_CLIP_SST)
