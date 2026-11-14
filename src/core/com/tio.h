/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The terminals between com and the line editor. tio reads each source as
 * items, an item being one character or one whole escape sequence, so a
 * sequence still arriving stays in its source and no reader takes half of
 * one. A line read takes its geometry from the console's terminal, cells_term,
 * and cursor shapes go to the screen. No terminal is asked anything. */

#ifndef _CORE_COM_TIO_H_
#define _CORE_COM_TIO_H_

#include "core/com/com.h"
#include "core/term/term.h"
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

/* Where a line read starts, from the console's terminal. */
typedef struct
{
    uint8_t col;    // 1-based column of the cursor on its row
    uint8_t cols;   // that row's columns, half the width on a double-size row
    uint8_t width;  // the shown grid's width
    uint8_t height; // the shown grid's rows
} tio_line_t;

/* A line read starts, or starts over at a new width. Everything already
 * printed reaches the terminals and cells_term first, then insert mode goes
 * off on every terminal, because the line editor overwrites the line as it
 * draws it again. A cursor in a pending wrap gets a CR LF, so the line
 * starts in column 1 of the next row. Then line is read from cells_term's
 * shown grid. */
void tio_read_start(tio_line_t *line);

/* The console's size: a program's own while it pins one, otherwise the
 * screen's showing grid. */
uint16_t tio_get_term_width(void);
uint16_t tio_get_term_height(void);

/* A non-zero value pins the size; 0 goes back to the screen's. Both return
 * to 0 when a program stops. */
void tio_set_term_width(uint16_t v);
void tio_set_term_height(uint16_t v);

/* The terminal on src went away, and the next one there is a different
 * terminal: what is left of an overlong sequence from it is forgotten. */
void tio_forget(com_source_t src);

/* The cursor shape for the line being edited, as the one-digit parameter of
 * DECSCUSR (CSI Ps SP q). It goes to term.c alone, through COM_TERM_SCREEN,
 * because a terminal on the UART or telnet may print it. */
void tio_cursor_shape(uint8_t ps);

/* The default cursor shape, to term.c, so a shape a program set is put
 * back. */
void tio_cursor_default(void);

/* A line read is over. What has arrived of an overlong sequence is dropped,
 * and whatever comes after is read as new items. */
void tio_read_end(void);

/* A program stopped, or a break: the read is over and the pinned sizes go
 * back to 0. */
void tio_stop(void);

void tio_init(void);

/* Each source's overlong framing state, and the length and quiet deadline of
 * the sequence in front of it (10 each); the pinned sizes (4); whether a
 * program's own size has a grid (1), and that grid. */
#define TIO_GRID_ROWS 255
#define TIO_SST_SIZE (10 * COM_SOURCE_COUNT + 5 + TERM_GRID_SST_SIZE(TIO_GRID_ROWS))
void tio_sst_save(sst_cursor_t *c, unsigned flags);
bool tio_sst_load(sst_cursor_t *c, unsigned flags);

#define TIO_DRIVER DRIVER(        \
    INIT(tio_init)                \
    STOP(tio_stop)                \
    BREAK(tio_stop)               \
    SST(TIO_, 9, TIO_SST_SIZE, tio_sst_save, tio_sst_load))

#endif /* _CORE_COM_TIO_H_ */
