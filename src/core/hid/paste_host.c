/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/paste.h"
#include "core/hid/paste_host.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/sys/sst.h"
#include "core/term/vterm.h"
#include <stdbool.h>

/* The host's text still to come. */
static const char *paste_host_text;

#define PASTE_HOST_SST(X) \
    X(SKIP, paste_host_text, "the host's text, which every load ends")
SST_TABLE(paste_host, PASTE_HOST_SST)

/* A paste is filtered on characters, before they become code page bytes:
 * the C0 controls but tab and the line ends, DEL, and the C1 controls go.
 * Typed text keeps every character. A character the code page lacks, or a
 * malformed sequence, becomes '?'.
 *
 * The bytes are in the keyboard's code page (oem_get_code_page_run), as a
 * key typing the same character gives them (hostkey.c): host text pasted is
 * typing, and a program reads it as it reads its keys. A program may set
 * the VGA's page apart from the keyboard's, and the console's page
 * (vterm_get_code_page) is the one its glyphs are read in: a copy of them
 * kept in the machine carries those bytes over as they are (cell.h). */
static size_t paste_host_fill(char *dst, size_t room, bool filtered)
{
    size_t n = 0;
    const char *p = paste_host_text;
    while (n < room && *p)
    {
        uint32_t cp = unicode_utf8_next(&p);
        if (filtered)
        {
            if (cp == '\r' || cp == '\n')
            {
                if (cp == '\r' && *p == '\n')
                    p++;
                cp = '\r';
            }
            else if ((cp < 0x20 && cp != '\t') || (cp >= 0x7F && cp <= 0x9F))
                continue;
        }
        unsigned char b = cp == UNICODE_MALFORMED ? 0x7F : oem_from_codepoint(cp);
        dst[n++] = (char)(cp != 0x7F && b == 0x7F ? '?' : b);
    }
    paste_host_text = p;
    return n;
}

/* The clipboard's text, pasted, and text typed as written: the paste stage
 * tells them apart by which of these reads them. */
static size_t paste_host_clipboard_fill(char *dst, size_t room)
{
    return paste_host_fill(dst, room, true);
}

static size_t paste_host_typed_fill(char *dst, size_t room)
{
    return paste_host_fill(dst, room, false);
}

/* No text ends the paste going in and starts nothing, as a paste that
 * gives no text sends nothing (paste.h), but at once. */
static void paste_host_start(const char *utf8, paste_fill_t fill, bool bracketed)
{
    if (!utf8 || !*utf8)
    {
        paste_cut();
        return;
    }
    paste_host_text = utf8;
    paste_host(fill, bracketed);
}

void paste_start(const char *utf8)
{
    paste_host_start(utf8, paste_host_clipboard_fill, vterm.bracketed_paste);
}

void typed_text(const char *utf8)
{
    paste_host_start(utf8, paste_host_typed_fill, false);
}

void paste_host_clipboard_changed(void)
{
    paste_host_cut(paste_host_clipboard_fill);
}
