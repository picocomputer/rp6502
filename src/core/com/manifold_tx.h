/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_COM_MANIFOLD_TX_H_
#define _CORE_COM_MANIFOLD_TX_H_

/* The console's output that waits to go out: what the firmware writes, a
 * program's stdout among it, each byte with the COM_TERM_ bitmap of the
 * terminals it is for. manifold_cts, manifold_putchar_raw and
 * manifold_write_to (manifold.h) put bytes in; the machine's manifold takes
 * them out, as fast as its terminals take them. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MANIFOLD_TX_SIZE 32

typedef struct
{
    uint8_t ch;
    uint8_t to;
} manifold_tx_entry_t;

/* One byte for the terminals in to, waiting for room as manifold_write_to
 * does. */
void manifold_tx_put(uint8_t to, char c);

/* The same without waiting: false, and the byte dropped, when there is no
 * room. manifold_putchar_raw's. */
bool manifold_tx_try_put(uint8_t to, char c);

/* What waits, oldest first. */
size_t manifold_tx_len(void);
manifold_tx_entry_t manifold_tx_at(size_t i);
void manifold_tx_skip(size_t n);

/* The machine's manifold makes room when a byte must go in and none is
 * left: it sends what it can, waiting on its terminals as it must. */
void manifold_tx_make_room(void);

/* After bytes go in: a machine whose terminals take output at once sends it
 * now, and one that drains in a task leaves it for the task. */
void manifold_tx_wrote(void);

/* A 16-bit count, then MANIFOLD_TX_SIZE entries of two bytes. */
#define MANIFOLD_TX_SST_SIZE (2 + 2 * MANIFOLD_TX_SIZE)
void manifold_tx_sst_save(sst_cursor_t *c);
bool manifold_tx_sst_load(sst_cursor_t *c);

#endif /* _CORE_COM_MANIFOLD_TX_H_ */
