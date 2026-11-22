/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_AUD_H_
#define _HOST_POCKET_SW_AUD_H_

#include "core/aud/bel.h"
#include "core/aud/mix.h"
#include "mmio.h"

#include "core/sys/sst.h"

/* The PSG, the OPL and PCM are the fabric's, so this machine's AUD row
 * carries the engines' state through AUD_SST, which it reads and writes
 * with the machine held, and the firmware's own: the open PCM: device's
 * control word and its place in a FIFO word. It loads after the STD row,
 * whose reopen of PCM: opens the device and so empties the FIFO that this
 * row then puts back. */
extern const sst_table_t aud_fabric_sst;

void aud_reset(void);

#define AUD_FABRIC_DRIVER DRIVER(                                    \
    INIT(aud_init)                                                   \
    TASK_ALWAYS(aud_task)                                            \
    STOP(aud_stop)                                                   \
    END(aud_end)                                                     \
    RESET(aud_reset)                                                 \
    SST("AUD_", aud_fabric))

#endif /* _HOST_POCKET_SW_AUD_H_ */
