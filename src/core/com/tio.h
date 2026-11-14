/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The key reader between com and the line editor. tio reads each source as
 * items, an item being one character or one whole escape sequence, so a
 * sequence still arriving stays in its source and no reader takes half of
 * one. */

#ifndef _CORE_COM_TIO_H_
#define _CORE_COM_TIO_H_

#include "core/com/com.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest item. A sequence still open at this many bytes, or at its
 * source's queue size if that is lower, is overlong: tio takes it and the
 * rest of it as it arrives, and drops it. */
#define TIO_ITEM_MAX 32

/* What one byte does to the item it joins. Any byte but ESC is an item on its
 * own. ESC then '[' opens a CSI: parameter bytes 0x30-0x3F, then
 * intermediates 0x20-0x2F, keep it open, and a final byte 0x40-0x7E ends it.
 * Three CSIs go on past that: rxvt's shifted keys end at a '$' after digits,
 * an X10 mouse report takes three bytes after CSI 'M', and a Linux console
 * function key one after CSI '['. ESC 'O' (SS3) takes digits and ';', then a
 * final byte. ESC 'N' (SS2) takes one byte. ESC then any other byte below
 * 0x80 is a two-byte Alt key, Alt-Backspace's ESC DEL among them.
 *
 * Inside a sequence, the lone ESC included, CAN and SUB drop it and
 * themselves. ESC and a byte of 0x80 and up end it with nothing, and so do
 * any other control and DEL past the lone ESC; the byte is then framed again
 * as the start of the next item: TIO_AGAIN. The mouse report's three bytes
 * take DEL and 0x80 and up. Outside a sequence CAN and SUB are keys. */
typedef enum
{
    TIO_OPEN,
    TIO_END,
    TIO_DROP,
    TIO_AGAIN,
} tio_step_t;

/* Steps the framing state *frame by one byte. The state starts at 0 and is 0
 * again after TIO_END, TIO_DROP or TIO_AGAIN. After TIO_AGAIN the bytes before
 * this one are dropped and this one is stepped again, from 0. */
tio_step_t tio_frame(uint8_t *frame, uint8_t ch);

/* A lone ESC is the Escape key once its source has been quiet this long, and
 * any other unfinished sequence is dropped. */
#define TIO_ESC_MS 100

/* A CSI item's first two parameters, how many parameters it has, its private
 * marker ('<' '=' '>' '?', or 0), its last intermediate byte (or 0), and its
 * final byte. Subparameters, after ':', are skipped. */
typedef struct
{
    uint16_t param[2];
    uint8_t count;
    uint8_t priv;
    uint8_t inter;
    uint8_t final;
} tio_csi_t;

/* Reads a CSI item, ESC '[', a private marker only as the first parameter
 * byte, parameters, intermediates and a final byte, or one of the three
 * longer forms tio_frame takes. False for any other item. */
bool tio_csi_parse(const char *item, size_t len, tio_csi_t *csi);

/* The next whole item, from the first source that has one, keyboard first.
 * Returns its length, 0 when no source has one, and names its source. buf
 * holds TIO_ITEM_MAX bytes. */
size_t tio_read(com_source_t *src, char *buf);

/* True when no whole item is waiting in any source. */
bool tio_idle(void);

/* The terminal on src went away, and the next one there is a different
 * terminal: what is left of an overlong sequence from it is forgotten. */
void tio_forget(com_source_t src);

/* A line read is over. What has arrived of an overlong sequence is dropped,
 * and whatever comes after is read as new items. */
void tio_read_end(void);

void tio_init(void);

/* Each source's overlong framing state, and the length and quiet deadline of
 * the sequence in front of it (10 each). */
#define TIO_SST_SIZE (10 * COM_SOURCE_COUNT)
void tio_sst_save(sst_cursor_t *c, unsigned flags);
bool tio_sst_load(sst_cursor_t *c, unsigned flags);

#define TIO_DRIVER DRIVER(        \
    INIT(tio_init)                \
    SST(TIO_, 10, TIO_SST_SIZE, tio_sst_save, tio_sst_load))

#endif /* _CORE_COM_TIO_H_ */
