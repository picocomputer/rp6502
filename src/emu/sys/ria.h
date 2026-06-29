/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RP6502 RIA modeled as a bus-interface chip, in the floooh/chips style — a
 * peripheral on the W65C02S bus, exactly like the 6522 VIA next to it. It decodes
 * the $FFE0-$FFFF register window, drives data on reads, asserts IRQB, and exposes
 * the register file, the XSTACK, and the RW0/RW1 XRAM portals to the CPU.
 *
 * The OS services its registers trigger (stdio/file I/O, exec, the VGA/PSG/OPL and
 * USB-HID devices, the clock) live on the FAR SIDE — separate subsystems on other
 * buses (XRAM, PIX) reached THROUGH the RIA. They are not part of this chip; an OP
 * write ($FFEF) hands off to them via ria.c's dispatch.
 *
 * Like via.c wraps m6522_t, ria.c keeps a single ria_t instance and ticks it as
 * `pins = ria_tick(pins)`. The RIA shares the 6502 bus, so it ticks on the m6502
 * pin mask (M6502_* in w65c02.h) directly — no separate pin layout. The register
 * file (regs[]) and XSTACK stay as the chip's memory-mapped storage: dual-ported,
 * since the RIA's own firmware addresses them directly (the REGS() shim), so they
 * are shared backing rather than embedded here.
 */

#ifndef _EMU_RIA_H_
#define _EMU_RIA_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    uint64_t PINS;       /* last bus pin state (do NOT modify; for the debug UI) */
    uint8_t irq_enabled; /* $FFF0 enable mask (VSYNC/SIGINT) */
    uint8_t irq_pending; /* latched pending sources, ORed onto IRQB while enabled */
    uint8_t pending_op;  /* in-flight syscall op being polled (0 = idle) */
} ria_t;

/* ------------------------------------------------------------------ */
/* RIA register window + syscall dispatch (ria.c)                      */
/* ------------------------------------------------------------------ */

void ria_reset(void);
uint8_t ria_reg_read(uint16_t addr);
void ria_reg_write(uint16_t addr, uint8_t data);

/* One PHI2 tick of the RIA's 6502-bus interface, mirroring via_tick so sys.c
 * drives both bus peripherals uniformly: when the CPU addresses the RIA window
 * ($FFE0-$FFF9) the register access is performed on the pins, and the RIA's IRQB
 * (VSYNC/SIGINT) is ORed onto the shared IRQ line (additive, after the VIA). */
uint64_t ria_tick(uint64_t pins);
void *ria_chip(void); /* ria_t* — the live chip instance, for the debugger UI */

/* RIA $FFF0 interrupt. ria_trigger_vsync latches the per-frame VSYNC flag (sys.c
 * at the vsync boundary); ria_irq_asserted is the tick-loop predicate sys.c ORs
 * into M6502_IRQ after via_tick. (SIGINT is latched via ria_trigger_sigint in
 * sys/ria.h.) */
void ria_trigger_vsync(void);
bool ria_irq_asserted(void);

/* Re-dispatch the in-flight (pending) syscall — the I/O polling step. Cheap
 * no-op when nothing is pending. ria_task_pump runs every scanline (the
 * emulator's analog of the RIA super-loop); ria_task is the per-frame entry
 * (after the line editor is pumped). */
void ria_task_pump(void);
void ria_task(void);

/* RIA-side firmware ABI reached by the vendored rln.c/atr.c through the firmware
 * path "sys/ria.h" (the shim there forwards to this header); ria.c implements
 * them. ria_active is always false in the emulator (no mbuf transfers). */
bool ria_active(void);         /* true while mid mbuf transfer; never here */
void ria_trigger_sigint(void); /* latch a Ctrl-C SIGINT (raises $FFF0 if enabled) */
bool ria_get_sigint(void);     /* consume the latched SIGINT for the attribute (true once) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_RIA_H_ */
