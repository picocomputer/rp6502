/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The terminals between com and the line editor. tio reads each source as
 * items, an item being one character or one whole escape sequence, so a
 * sequence still arriving stays in its source and no reader takes half of
 * one. It asks the terminal in charge what a line read needs, takes the
 * answers out of the input, keeps the screen's geometry, and sends cursor
 * shapes to the terminals that take them. No other terminal is asked
 * anything. */

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

/* What one byte does to the item it joins. ESC then '[' opens a CSI, which
 * digits, ';', ':' and '<' '=' '>' '?' keep open and any other byte ends. ESC
 * 'N' and ESC 'O' take one more byte. ESC then any other byte is two bytes.
 * Any other byte on its own is an item. CAN drops the sequence it lands in,
 * and itself. */
typedef enum
{
    TIO_OPEN,
    TIO_END,
    TIO_DROP,
} tio_step_t;

/* Steps the framing state *frame by one byte. The state starts at 0 and is 0
 * again after TIO_END or TIO_DROP. */
tio_step_t tio_frame(uint8_t *frame, uint8_t ch);

/* A CSI item's first two parameters, how many parameters it has, its private
 * marker ('<' '=' '>' '?', or 0), and its final byte. */
typedef struct
{
    uint16_t param[2];
    uint8_t count;
    uint8_t priv;
    uint8_t final;
} tio_csi_t;

/* Reads a CSI item, ESC '[' then parameters and a final byte. False for any
 * other item. */
bool tio_csi_parse(const char *item, size_t len, tio_csi_t *csi);

/* The next whole item, from the first source that has one, keyboard first.
 * Returns its length, 0 when no source has one, and names its source. buf
 * holds TIO_ITEM_MAX bytes. A terminal's answer to a cursor position report
 * (CPR) or to secondary device attributes (DA2) is taken here and never
 * returned, and so is one that comes from the keyboard. While a line end
 * waits for the terminal in charge (tio_line_end), only answers are taken,
 * and every other item stays in its source for the next reader. */
size_t tio_read(com_source_t *src, char *buf);

/* True when no whole item is waiting in any source. */
bool tio_idle(void);

/* Geometry is ready this long after the terminal in charge has answered and
 * no item has come, or at the deadline. */
#define TIO_QUIET_MS 5
#define TIO_DEADLINE_MS 250

/* A line read starts, the line editor owning length cells from the cursor.
 * Everything already printed goes out first. Then the terminal in charge, and
 * no other, is asked for the cursor's column:
 * - A telnet session that sent its window size (NAWS) is also asked DA2, with
 *   a length above 0, until its first DA2 answer.
 * - term.c is asked through COM_TERM_SCREEN, so nothing that hears the UART
 *   sees the question.
 * - A plain terminal on the UART is also asked DA2 with a length above 0, and
 *   for the screen's size unless both sizes are pinned.
 * With no terminal in charge nothing is asked, and geometry is ready at once
 * with no prompt column. */
void tio_read_start(uint8_t length);

/* True once geometry is ready for the read. */
bool tio_ready(void);

/* Geometry is ready now, with no prompt column. */
void tio_ready_now(void);

/* A line end was typed before geometry was ready. A plain terminal on the
 * UART, or a telnet session that has never answered a CPR, makes geometry
 * ready now with no prompt column. term.c, a telnet session that has
 * answered a CPR, and a telnet session at its first read after logging in
 * are waited for, until the read is ready as tio_ready says, or until
 * tio_read finds an item typed after the line end at the front of the source
 * the terminal in charge answers on, which an answer still to come would be
 * behind. Then tio_read returns nothing, so the held line is taken before
 * anything typed after it. */
void tio_line_end(void);

/* The column the read started at, 1-based, from the terminal in charge. 0
 * while geometry is not ready, or when it is ready without one, so the line
 * is laid out without wrapping. A later answer in the same read can change
 * it. */
uint16_t tio_prompt_col(void);

/* Terminal width. Priority: the pinned width, then the NAWS width of the
 * telnet session in charge, then the canvas's columns while term.c is in
 * charge, then the width the plain UART terminal in charge reported in this
 * read, then 80. */
uint16_t tio_get_term_width(void);

/* Terminal height, in the same order as the width: pinned, NAWS, the
 * canvas's rows, the plain UART terminal's report, then 24. */
uint16_t tio_get_term_height(void);

/* A non-zero value pins the size; 0 goes back to detection. With both pinned,
 * a read does not ask for the size. Both return to 0 when a program stops. */
void tio_set_term_width(uint16_t v);
void tio_set_term_height(uint16_t v);

/* The window size of the telnet session in charge. (0,0) clears it. */
void tio_set_naws_size(uint16_t w, uint16_t h);

/* The terminal on src went away, and the next one there is a different
 * terminal: what this read heard from it and what is left of an overlong
 * sequence from it are forgotten, and so is whether it ever answered a CPR or
 * DA2 or had a read ready. A read still waiting on it becomes ready, as it
 * would at its deadline. */
void tio_forget(com_source_t src);

/* The terminals taken to support cursor shapes:
 * - term.c always.
 * - A telnet session once it has answered DA2, at any time.
 * - The plain UART terminal until a read asks it DA2 and it answers the size
 *   CPR without answering DA2 first, or, with both sizes pinned, has answered
 *   its CPR and not DA2 when geometry is ready. A DA2 answer takes it as
 *   supporting them again. A read of no cells asks no DA2 and changes
 *   nothing. */

/* The cursor shape for the line being edited, as the one-digit parameter of
 * DECSCUSR (CSI Ps SP q). It goes to each connected terminal taken to support
 * shapes, except that the plain UART terminal in charge gets it only once it
 * has answered DA2 in this read. A telnet session or the plain UART terminal
 * whose DA2 answer comes after geometry is ready gets it when that answer
 * comes. term.c gets it through COM_TERM_SCREEN. */
void tio_cursor_shape(uint8_t ps);

/* The default cursor shape, to each connected terminal taken to support
 * shapes, so a shape a program set is put back. */
void tio_cursor_default(void);

/* A line read is over. What has arrived of an overlong sequence is dropped,
 * and whatever comes after is read as new items. */
void tio_read_end(void);

/* A program stopped, or a break: the read is over and the pinned sizes go
 * back to 0. */
void tio_stop(void);

void tio_init(void);

/* Each source's overlong framing state, its CPR count and its DA2 answer (3
 * each); the read's state (1), deadline (8), quiet deadline (8) and terminal
 * in charge (1); whether it asked DA2 (1) and for the size (1); the prompt
 * column, width and height (6); the pinned sizes (4); the NAWS size (4); the
 * cursor shape (1); and the terminals taken to support shapes (1), that have
 * answered a CPR (1) and that have had a read ready while in charge (1). */
#define TIO_SST_SIZE (3 * COM_SOURCE_COUNT + 38)
void tio_sst_save(sst_cursor_t *c, unsigned flags);
bool tio_sst_load(sst_cursor_t *c, unsigned flags);

#define TIO_DRIVER DRIVER(        \
    INIT(tio_init)                \
    STOP(tio_stop)                \
    BREAK(tio_stop)               \
    SST(TIO_, 4, TIO_SST_SIZE, tio_sst_save, tio_sst_load))

#endif /* _CORE_COM_TIO_H_ */
