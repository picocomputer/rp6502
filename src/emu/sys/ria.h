/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_RIA_H_
#define _EMU_SYS_RIA_H_

/* Pulled in ahead of the extern "C" block so the firmware header's own includes
 * are already-guarded no-ops by the time it is reached. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* chips/ui_ria.h includes this from a C++ TU outside any extern "C" wrapper, so
 * unlike its sibling emu/sys headers this one must declare its own linkage. */
#ifdef __cplusplus
extern "C"
{
#endif

/* The firmware contract ria.c implements: ria_run, ria_task, ria_active,
 * ria_trigger_vsync, ria_trigger_sigint, ria_get_sigint. The PIO/UART/mbuf half is
 * firmware-only and has no emulator implementation; ria_active is always false here
 * (no mbuf transfers). */
#include "ria/sys/ria.h"

/* The RIA decodes the $FFE0-$FFF9 register window, drives data on reads and asserts
 * IRQB. The OS services its registers trigger — stdio/file I/O, exec, the VGA/PSG/OPL
 * and USB-HID devices, the clock — are NOT part of this interface; an OP write
 * ($FFEF) hands off to them from ria.c's dispatch. The register file (regs[]) and the
 * XSTACK are dual-ported shared backing rather than state held here, because the
 * RIA's own firmware addresses them directly through REGS(). */

/* The RP6502 schematic's RIA-request select (active high), decoded off-chip from
 * A5-A15. It rides a free bit above M6502_PIN_MASK so it never collides with a real
 * CPU pin; the VIA's M6522_CS1 uses the same bit, which is harmless because each
 * chip is ticked on its own pin copy. */
#define RIA_PIN_RREQ (1ULL << 40)

typedef struct
{
    uint64_t PINS;       /* last bus pin state (do NOT modify; for the debug UI) */
    uint8_t irq_enabled; /* $FFF0 enable mask (VSYNC/SIGINT) */
    uint8_t irq_pending; /* latched pending sources, ORed onto IRQB while enabled */
} ria_t;

uint8_t ria_reg_read(uint16_t addr);
void ria_reg_write(uint16_t addr, uint8_t data);

/* One PHI2 tick of the RIA's 6502-bus interface, mirroring via_tick so the board
 * drives both bus peripherals uniformly: when selected (the board decodes the RIA
 * window into RREQ) the register access is performed on the pins, and the RIA's
 * IRQB (VSYNC/SIGINT) is driven onto M6502_IRQ. Returns the RIA's own pin mask;
 * the board merges the data and ORs the IRQ onto the bus. */
uint64_t ria_tick(uint64_t pins, bool selected);
void *ria_chip(void); /* ria_t* — the live chip instance, for the debugger UI */

/* True while an enabled $FFF0 source is pending. ria_trigger_vsync (firmware
 * contract) latches the VSYNC source, raising IRQB only while it is enabled. */
bool ria_irq_asserted(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SYS_RIA_H_ */
