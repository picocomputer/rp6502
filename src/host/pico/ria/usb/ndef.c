/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/usb/ndef.h"
#include "core/str/unicode.h"
#include <string.h>

#define NDEF_TLV_NULL 0x00
#define NDEF_TLV_NDEF_MESSAGE 0x03
#define NDEF_TLV_TERMINATOR 0xFE

/* Walks the TLVs of a tag image from page 4, because pages 0 to 3 hold the
 * UID, lock and CC bytes. True once the walk reaches a Terminator TLV, false
 * when the image ends first, mid-TLV included. *msg is the value of the
 * first whole NDEF Message TLV, or NULL. */
static bool ndef_walk(const uint8_t *tag, size_t len, const uint8_t **msg, size_t *msg_len)
{
    *msg = NULL;
    *msg_len = 0;
    size_t pos = 16;
    while (pos < len)
    {
        uint8_t type = tag[pos++];
        if (type == NDEF_TLV_TERMINATOR)
            return true;
        if (type == NDEF_TLV_NULL)
            continue;
        if (pos >= len)
            return false;
        size_t tlv_len = tag[pos++];
        if (tlv_len == 0xFF) // three-byte length
        {
            if (pos + 2 > len)
                return false;
            tlv_len = ((size_t)tag[pos] << 8) | tag[pos + 1];
            pos += 2;
        }
        if (tlv_len > len - pos)
            return false;
        if (type == NDEF_TLV_NDEF_MESSAGE && !*msg)
        {
            *msg = tag + pos;
            *msg_len = tlv_len;
        }
        pos += tlv_len;
    }
    return false;
}

bool ndef_complete(const uint8_t *tag, size_t len)
{
    const uint8_t *msg;
    size_t msg_len;
    return ndef_walk(tag, len, &msg, &msg_len);
}

/* Text to the code page, from UTF-16 when bit 7 of the status byte is set,
 * big endian unless a byte order mark says otherwise, else from UTF-8. A
 * character the page lacks, a malformed sequence, a NUL, or more text than
 * the buffer holds refuses the card, rather than name some other file or cut
 * the arguments short. */
static bool ndef_text(uint8_t status, const uint8_t *txt, size_t txt_len,
                      uint16_t page, char *buf, size_t buf_size)
{
    size_t n = 0;
    if (status & 0x80)
    {
        const bool le = txt_len >= 2 && txt[0] == 0xFF && txt[1] == 0xFE;
        if (le || (txt_len >= 2 && txt[0] == 0xFE && txt[1] == 0xFF))
            txt += 2, txt_len -= 2;
        if (txt_len & 1)
            return false;
        for (size_t i = 0; i < txt_len; i += 2)
        {
            const uint16_t u = le ? (uint16_t)(txt[i] | txt[i + 1] << 8)
                                  : (uint16_t)(txt[i] << 8 | txt[i + 1]);
            const unsigned char b = u ? unicode_from_codepoint(u, page) : 0x7F;
            if (b == 0x7F || n + 1 >= buf_size)
                return false;
            buf[n++] = (char)b;
        }
    }
    else
    {
        /* The decoder ends at a NUL, so the text goes into the buffer first
         * and is converted in place, which only ever shortens it. */
        if (txt_len >= buf_size || memchr(txt, 0, txt_len))
            return false;
        memcpy(buf, txt, txt_len);
        buf[txt_len] = 0;
        const char *p = buf;
        unsigned char b;
        while ((b = unicode_from_utf8_next(&p, page)))
        {
            if (b == 0x7F)
                return false;
            buf[n++] = (char)b;
        }
    }
    buf[n] = 0;
    return true;
}

bool ndef_parse_text(const uint8_t *tag_data, size_t len, uint16_t page,
                     char *buf, size_t buf_size)
{
    const uint8_t *msg;
    size_t msg_len;
    ndef_walk(tag_data, len, &msg, &msg_len);
    if (!msg)
        return false;

    size_t rpos = 0;
    while (rpos < msg_len)
    {
        uint8_t hdr = msg[rpos++];
        uint8_t tnf = hdr & 0x07;
        bool cf = (bool)((hdr >> 5) & 1);
        bool sr = (bool)((hdr >> 4) & 1);
        bool il = (bool)((hdr >> 3) & 1);
        if (rpos >= msg_len)
            break;
        uint8_t type_len = msg[rpos++];
        uint32_t payload_len;
        if (sr)
        {
            if (rpos >= msg_len)
                break;
            payload_len = msg[rpos++];
        }
        else
        {
            if (rpos + 4 > msg_len)
                break;
            payload_len = ((uint32_t)msg[rpos] << 24) | ((uint32_t)msg[rpos + 1] << 16) |
                          ((uint32_t)msg[rpos + 2] << 8) | msg[rpos + 3];
            rpos += 4;
        }
        uint8_t id_len = 0;
        if (il)
        {
            if (rpos >= msg_len)
                break;
            id_len = msg[rpos++];
        }
        if (rpos + type_len + id_len > msg_len)
            break;
        const uint8_t *type_data = msg + rpos;
        rpos += type_len + id_len;
        if (payload_len > msg_len - rpos)
            break;
        const uint8_t *payload = msg + rpos;
        rpos += payload_len;
        if (tnf == 0x01 && type_len == 1 && type_data[0] == 'T' && payload_len >= 1)
        {
            if (cf)
                return false; // chunked text record not supported
            uint8_t lang_len = payload[0] & 0x3F;
            if (payload_len <= (uint32_t)(1 + lang_len))
                continue;
            return ndef_text(payload[0], payload + 1 + lang_len,
                             payload_len - 1 - lang_len, page, buf, buf_size);
        }
    }
    return false;
}
