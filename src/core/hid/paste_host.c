/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/clip.h"
#include "core/hid/paste_host.h"
#include "core/hid/typed.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/sys/ria.h"
#include "core/sys/sst.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A text the host handed over, as code page bytes: a paste, or plain text
 * typed as written. */
typedef struct paste_host_text
{
    struct paste_host_text *next;
    uint16_t len;
    bool plain;
    char bytes[];
} paste_host_text_t;

/* The text being typed, and the texts whose pastes wait in the keyboard's
 * queue, in the order of their entries there. */
static paste_host_text_t *paste_host_typing;
static paste_host_text_t *paste_host_first;
static paste_host_text_t **paste_host_last = &paste_host_first;

#define PASTE_HOST_SST(X)                                                 \
    X(SKIP, paste_host_typing, "the host's text, which every load ends")  \
    X(SKIP, paste_host_first, "the host's text, which every load ends")   \
    X(SKIP, paste_host_last, "the host's text, which every load ends")
SST_TABLE(paste_host, PASTE_HOST_SST)

/* The text as code page bytes, no more of them than a paste counts, or
 * NULL when there are none. A paste is filtered on characters, before they
 * become bytes: the C0 controls but tab and the line ends, DEL, and the C1
 * controls go. Typed text keeps every character. A character the code page
 * lacks, or a malformed sequence, becomes '?'.
 *
 * The bytes are in the keyboard's code page (oem_get_code_page_run), as a
 * key typing the same character gives them (hostkey.c): host text pasted is
 * typing, and a program reads it as it reads its keys. A program may set
 * the VGA's page apart from the keyboard's, and the console's page
 * (vterm_get_code_page) is the one its glyphs are read in: a copy of them
 * kept in the machine carries those bytes over as they are (cell.h). */
static paste_host_text_t *paste_host_text(const char *utf8, bool filtered)
{
    paste_host_text_t *text = malloc(sizeof *text + strlen(utf8));
    if (!text)
        return NULL;
    size_t n = 0;
    const char *p = utf8;
    while (n < UINT16_MAX && *p)
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
        text->bytes[n++] = (char)(cp != 0x7F && b == 0x7F ? '?' : b);
    }
    if (!n)
    {
        free(text);
        return NULL;
    }
    text->next = NULL;
    text->len = (uint16_t)n;
    text->plain = !filtered;
    return text;
}

/* The text waits behind those handed over before it, as its entry does
 * in the queue. The room is checked first, so the entry is taken. */
static bool paste_host_queue(const char *utf8, bool plain)
{
    if (!typed_free())
        return false;
    paste_host_text_t *text = paste_host_text(utf8, !plain);
    if (!text)
        return true;
    if (plain && memchr(text->bytes, 0x03, text->len))
        ria_trigger_sigint();
    *paste_host_last = text;
    paste_host_last = &text->next;
    typed_put(TYPED_ENTRY(TYPED_PASTE, CLIP_HOST | (plain ? TYPED_PASTE_PLAIN : 0)));
    return true;
}

void paste_start(const char *utf8)
{
    paste_host_queue(utf8, false);
}

bool typed_text(const char *utf8)
{
    return paste_host_queue(utf8, true);
}

void paste_host_begin(const char *utf8)
{
    if (utf8)
    {
        paste_host_typing = paste_host_text(utf8, true);
        return;
    }
    paste_host_typing = paste_host_first;
    if (paste_host_typing && !(paste_host_first = paste_host_typing->next))
        paste_host_last = &paste_host_first;
}

size_t paste_host_size(void)
{
    return paste_host_typing ? paste_host_typing->len : 0;
}

char paste_host_at(size_t i)
{
    return paste_host_typing->bytes[i];
}

void paste_host_end(void)
{
    free(paste_host_typing);
    paste_host_typing = NULL;
}

bool paste_host_break(void)
{
    paste_host_last = &paste_host_first;
    while (*paste_host_last)
    {
        paste_host_text_t *text = *paste_host_last;
        if (text->plain)
            paste_host_last = &text->next;
        else
        {
            *paste_host_last = text->next;
            free(text);
        }
    }
    return !paste_host_typing || !paste_host_typing->plain;
}

void paste_host_drop(void)
{
    paste_host_end();
    while (paste_host_first)
    {
        paste_host_text_t *text = paste_host_first;
        paste_host_first = text->next;
        free(text);
    }
    paste_host_last = &paste_host_first;
}
