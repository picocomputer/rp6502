/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's syscall op registry — the counterpart to ria/main.c's
 * main_api switch. main.c holds the op table (api_ops) that main_api
 * dispatches through; the shared ria/api/api.c reaches main_api via "main.h".
 */

#ifndef _EMU_MAIN_H_
#define _EMU_MAIN_H_

#include <stdbool.h>

#include "ria/main.h" /* the main_api contract the shared api.c dispatches through */

#ifdef __cplusplus
extern "C"
{
#endif

/* Point the op table's dir slots at the firmware FatFs handlers (fat, over the
 * RAM disk) or the emu's host handlers. */
void main_dir_ops_set(bool fat);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MAIN_H_ */
