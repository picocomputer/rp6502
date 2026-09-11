/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_RIA_RIA_H_
#define _CORE_RIA_RIA_H_

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* host/sokol/dbg/ui_ria.h includes this from C++ before opening its own extern
 * "C" block, so this header declares its linkage. */
#ifdef __cplusplus
extern "C"
{
#endif

#include "core/sys/ria.h"

void ria_run(void);
void ria_trigger_vsync(void);

/* The 6502 sees the RIA as 32 bytes at $FFE0-$FFFF, the last six of which are
 * its own NMI, RESET and IRQ vectors. */
#define RIA_MMAP_LO 0xFFE0
#define RIA_MMAP_HI 0xFFFF

/* These pins do not follow the CPU's layout. The five address lines select a
 * register within the RIA's window. Two of the bits are produced rather than
 * read: ria_tick sets CS from its own decode of the address, and the debug
 * overlay sets RES from resb_running(). */
#define RIA_PIN_A0 (1ULL << 0) /* A0-A4 at bits 0-4 */
#define RIA_PIN_D0 (1ULL << 8) /* D0-D7 at bits 8-15 */
#define RIA_PIN_RW (1ULL << 16)
#define RIA_PIN_IRQ (1ULL << 17)
#define RIA_PIN_CS (1ULL << 18)
#define RIA_PIN_RES (1ULL << 19)

typedef struct
{
    uint64_t PINS;       /* the last bus cycle, published for the debug UI */
    uint8_t irq_enabled; /* $FFF0 enable mask (VSYNC/SIGINT) */
    uint8_t irq_pending; /* latched sources, asserting IRQB while also enabled */
} ria_t;

uint8_t ria_reg_read(uint16_t addr);
void ria_reg_write(uint16_t addr, uint8_t data);

/* One PHI2 tick. data is in/out: the RIA drives it on a read of its window and
 * takes it on a write. Returns the RIA's own IRQB, which the board ORs with
 * every other device's. */
bool ria_tick(uint16_t addr, bool read, uint8_t *data);
void *ria_chip(void); /* ria_t *, for the debugger UI */

bool ria_irq_asserted(void);

/* Drops the byte the $FFE2 latch is holding. The console clears its own queues
 * on a break, but this byte has already left them, so nothing else can drop it
 * and the next program would read what was typed at the one just stopped. */
void ria_break(void);

#ifdef __cplusplus
}
#endif

/* PINS, the two interrupt bytes and the latched RX source, then the register
 * file, the xstack and its pointer, then the write queue's page, head and tail
 * and the whole queue behind them:
 * 8 + 1 + 1 + 1 + 32 + 513 + 2 + 1 + 1 + 1 + 512 */
#define RIA_SST_SIZE 1073
void ria_sst_save(sst_cursor_t *c, unsigned flags);
bool ria_sst_load(sst_cursor_t *c, unsigned flags);

#define RIA_DRIVER DRIVER(nul_init, nul_task, nul_task, ria_run, nul_stop, ria_break, \
    nul_config, nul_config, SST(RIA_, 1, RIA_SST_SIZE, ria_sst_save, ria_sst_load))

#endif /* _CORE_RIA_RIA_H_ */
