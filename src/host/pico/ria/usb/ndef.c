/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/usb/ndef.h"
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

bool ndef_parse_text(const uint8_t *tag_data, size_t len, char *buf, size_t buf_size)
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
            uint8_t status = payload[0];
            uint8_t lang_len = status & 0x3F;
            if (payload_len <= (uint32_t)(1 + lang_len))
                continue;
            const uint8_t *txt = payload + 1 + lang_len;
            size_t txt_len = payload_len - 1 - lang_len;
            if (txt_len >= buf_size)
                txt_len = buf_size - 1;
            memcpy(buf, txt, txt_len);
            buf[txt_len] = '\0';
            return true;
        }
    }
    return false;
}
