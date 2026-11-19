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

/* The PSG, the OPL and PCM are the fabric's, so this machine's AUD row
 * carries the engines' state through AUD_SST, which it reads and writes
 * with the machine held, and the firmware's own: the open PCM: device's
 * control word, its place in a FIFO word and the bell. It loads after the
 * STD row, whose reopen of PCM: opens the device and so empties the FIFO
 * that this row then puts back.
 *
 * 4 control, 1 place, the bell, then AUD_SST_WORDS words. */
#define AUD_FABRIC_SST_SIZE (4 + 1 + BEL_SST_SIZE + AUD_SST_WORDS * 4)

#define AUD_FABRIC_DRIVER DRIVER(                                    \
    INIT(aud_init)                                                   \
    TASK_ALWAYS(aud_task)                                            \
    STOP(aud_stop)                                                   \
    END(aud_end)                                                     \
    SST(.id = "AUD_",                                                \
        .version = 1,                                                \
        .size = AUD_FABRIC_SST_SIZE,                                 \
        .save = aud_sst_save,                                        \
        .load = aud_sst_load))

#endif /* _HOST_POCKET_SW_AUD_H_ */
