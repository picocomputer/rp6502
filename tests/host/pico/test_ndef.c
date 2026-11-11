/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* An NTAG215 read the way the PN532 driver reads it, 16 bytes a READ, ending
 * where ndef_complete says the data ends or at the end of the area that the
 * CC gives. A 0xFE in the UID or in the data is not the Terminator TLV. */

#include "ria/usb/ndef.h"
#include "utest.h"
#include <string.h>

static uint8_t card[16 + 496];

/* Pages 0 to 3 with the given UID bytes, then the CC of an NTAG215. */
static void blank_card(uint8_t uid1, uint8_t bcc1)
{
    memset(card, 0, sizeof card);
    const uint8_t head[16] = {0x04, uid1, 0x34, 0x04 ^ uid1 ^ 0x34 ^ 0x88,
                              0x56, 0x78, 0x9A, bcc1,
                              0x48, 0x00, 0x00, 0x00,
                              0xE1, 0x10, 0x3E, 0x00};
    memcpy(card, head, sizeof head);
}

/* An NDEF message of one Text record at page 4, written with a one- or a
 * three-byte TLV length, then a Terminator TLV. Returns its end. */
static size_t put_text(size_t at, const char *text, size_t text_len, bool long_tlv)
{
    const size_t payload = 3 + text_len;
    const size_t msg = 4 + payload; /* header, type length, payload length, 'T' */
    card[at++] = 0x03;
    if (long_tlv || msg >= 0xFF)
    {
        card[at++] = 0xFF;
        card[at++] = (uint8_t)(msg >> 8);
        card[at++] = (uint8_t)msg;
    }
    else
        card[at++] = (uint8_t)msg;
    card[at++] = 0xD1; /* MB ME SR, Well Known */
    card[at++] = 0x01;
    card[at++] = (uint8_t)payload;
    card[at++] = 'T';
    card[at++] = 0x02; /* UTF-8, language "en" */
    card[at++] = 'e';
    card[at++] = 'n';
    memcpy(&card[at], text, text_len);
    at += text_len;
    card[at++] = 0xFE;
    return at;
}

/* The number of bytes the driver reads before it stops. */
static size_t read_len(void)
{
    const unsigned last_page = 4 + card[14] * 2 - 1;
    size_t len = 0;
    for (unsigned page = 0;; page += 4)
    {
        len += 16;
        if (ndef_complete(card, len) || len >= sizeof card || page + 4 > last_page)
            return len;
    }
}

static bool reads_as(const char *want)
{
    char text[256];
    size_t len = read_len();
    if (!ndef_parse_text(card, len, text, sizeof text))
        return false;
    return strcmp(text, want) == 0;
}

UTEST(ndef, a_plain_card_reads)
{
    blank_card(0x12, 0x00);
    put_text(16, "/game.rp6502", 12, false);
    ASSERT_EQ(read_len(), (size_t)48);
    ASSERT_TRUE(reads_as("/game.rp6502"));
}

UTEST(ndef, a_uid_with_fe_reads)
{
    blank_card(0xFE, 0x00);
    put_text(16, "/game.rp6502", 12, false);
    ASSERT_TRUE(reads_as("/game.rp6502"));
    blank_card(0x12, 0xFE);
    put_text(16, "/game.rp6502", 12, false);
    ASSERT_TRUE(reads_as("/game.rp6502"));
}

/* A text of 247 bytes makes a message of 254, 0xFE for its TLV length, and a
 * text of 251 bytes has 0xFE for its record's payload length. */
UTEST(ndef, an_fe_length_reads)
{
    char text[252];
    memset(text, 'a', sizeof text - 1);
    text[sizeof text - 1] = 0;
    blank_card(0x12, 0x00);
    put_text(16, text, 247, false);
    ASSERT_EQ(card[17], 0xFE);
    char want[248];
    memcpy(want, text, 247);
    want[247] = 0;
    ASSERT_TRUE(reads_as(want));
    blank_card(0x12, 0x00);
    put_text(16, text, 251, true);
    ASSERT_EQ(card[22], 0xFE);
    ASSERT_TRUE(reads_as(text));
}

/* A record before the text whose payload holds 0xFE and a run of zeros. */
UTEST(ndef, fe_and_zeros_in_the_data_read)
{
    blank_card(0x12, 0x00);
    size_t at = 16;
    card[at++] = 0x03;
    const size_t mime = 3 + 4 + 40;
    const size_t text = 4 + 3 + 12;
    card[at++] = (uint8_t)(mime + text);
    card[at++] = 0x92; /* MB SR, MIME */
    card[at++] = 4;
    card[at++] = 40;
    memcpy(&card[at], "a/bc", 4);
    at += 4;
    card[at] = 0xFE; /* then 39 zeros */
    at += 40;
    card[at++] = 0x51; /* ME SR, Well Known */
    card[at++] = 0x01;
    card[at++] = 3 + 12;
    card[at++] = 'T';
    card[at++] = 0x02;
    card[at++] = 'e';
    card[at++] = 'n';
    memcpy(&card[at], "/game.rp6502", 12);
    at += 12;
    card[at] = 0xFE;
    ASSERT_TRUE(reads_as("/game.rp6502"));
}

/* A factory card holds an empty NDEF TLV and stops at once. A card wiped to
 * zeros stops at the end of its area. */
UTEST(ndef, empty_cards_stop)
{
    blank_card(0x12, 0x00);
    card[16] = 0x03;
    card[17] = 0x00;
    card[18] = 0xFE;
    ASSERT_EQ(read_len(), (size_t)32);
    ASSERT_FALSE(reads_as(""));
    blank_card(0x12, 0x00);
    ASSERT_EQ(read_len(), sizeof card);
    ASSERT_FALSE(reads_as(""));
}

UTEST_MAIN();
