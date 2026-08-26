/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_RIA_RIA_H_
#define _CORE_RIA_RIA_H_

/* Pulled in ahead of the extern "C" block so the firmware header's own includes
 * are already-guarded no-ops by the time it is reached. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* host/sokol/ui_ria.h includes this from a C++ TU outside any extern "C" wrapper, so
 * unlike its sibling core/sys headers this one must declare its own linkage. */
#ifdef __cplusplus
extern "C"
{
#endif

/* The bus this RIA sits on: ria_active, ria_trigger_sigint, ria_get_sigint. The
 * PIO/UART/mbuf half is firmware-only and has no implementation here; ria_active
 * is always false (no mbuf transfers). */
#include "core/main.h"

/* Program start, per-frame service, and the vsync the video half raises. */
void ria_run(void);
void ria_task(void);
void ria_trigger_vsync(void);

/* The RIA decodes the RIA_MMAP_* register window, drives data on reads and asserts
 * IRQB. The OS services its registers trigger — stdio/file I/O, exec, the VGA/PSG/OPL
 * and USB-HID devices, the clock — are NOT part of this interface; an OP write
 * ($FFEF) hands off to them from ria.c's dispatch. The register file (regs[]) and the
 * XSTACK are dual-ported shared backing rather than state held here, because the
 * RIA's own firmware addresses them directly through REGS(). */

/* 6502 memory map: 32 registers, the last six being the vectors (ria.rst).
 * A5-A15 are decoded off-chip into CS. */
#define RIA_MMAP_LO 0xFFE0
#define RIA_MMAP_HI 0xFFFF

/* The RIA's pins. It wires only CS, RW, D0-D7 and the low five address lines that
 * select its register window, so it has its own compact layout rather than borrowing
 * the CPU's. RES is not a RIA input; the debug overlay lights it from cpu_halted(). */
#define RIA_PIN_A0 (1ULL << 0) /* A0-A4 at bits 0-4 */
#define RIA_PIN_D0 (1ULL << 8) /* D0-D7 at bits 8-15 */
#define RIA_PIN_RW (1ULL << 16)
#define RIA_PIN_IRQ (1ULL << 17)
#define RIA_PIN_CS (1ULL << 18)
#define RIA_PIN_RES (1ULL << 19)

typedef struct
{
    uint64_t PINS;       /* last bus state in RIA pins (do NOT modify; for the debug UI) */
    uint8_t irq_enabled; /* $FFF0 enable mask (VSYNC/SIGINT) */
    uint8_t irq_pending; /* latched pending sources, ORed onto IRQB while enabled */
} ria_t;

/* Take back a byte staged in the $FFE2 latch, false when none is. The
 * console's raw read uses it so a byte the ready bit committed is not
 * stranded there. Not the firmware's ria_uart_rx_reclaim, which takes
 * from the slot a byte reaches the latch through. */
bool ria_reg_rx_reclaim(char *ch);

uint8_t ria_reg_read(uint16_t addr);
void ria_reg_write(uint16_t addr, uint8_t data);

/* One PHI2 tick, mirroring via_tick: services the register access when the address
 * is in the RIA's window, publishes PINS for the debug UI, and returns the RIA's
 * IRQB (VSYNC/SIGINT). data is in/out. */
bool ria_tick(uint16_t addr, bool read, uint8_t *data);
void *ria_chip(void); /* ria_t* — the live chip instance, for the debugger UI */

/* True while an enabled $FFF0 source is pending. ria_trigger_vsync (firmware
 * contract) latches the VSYNC source, raising IRQB only while it is enabled. */
bool ria_irq_asserted(void);

#ifdef __cplusplus
}
#endif

#endif /* _CORE_RIA_RIA_H_ */
