/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RP6502 RIA modeled as a bus-interface chip, in the floooh/chips style — a
 * peripheral on the W65C02S bus, exactly like the 6522 VIA next to it. It decodes
 * the $FFE0-$FFFF register window, drives data on reads, asserts IRQB, and exposes
 * the register file, the XSTACK, and the RW0/RW1 XRAM portals to the CPU. The RIA
 * is the RP6502's only chip-level bus interface beyond the CPU/VIA, so this header
 * carries the system name; the API keeps the ria_* prefix.
 *
 * The OS services its registers trigger (stdio/file I/O, exec, the VGA/PSG/OPL and
 * USB-HID devices, the clock) live on the FAR SIDE — separate subsystems on other
 * buses (XRAM, PIX) reached THROUGH the RIA. They are not part of this chip; an OP
 * write ($FFEF) hands off to them via ria.c's dispatch.
 *
 * Unlike the single-header w65c02.h/m6522.h cores, the implementation stays in
 * sys/ria.c (the emulator's normal C convention); this header is the chip's
 * interface only. Like via.c wraps m6522_t, ria.c keeps a single ria_t instance
 * and ticks it as `pins = ria_tick(pins)`. The RIA shares the 6502 bus, so it
 * ticks on the m6502 pin mask (M6502_* in w65c02.h) directly — no separate pin
 * layout. The register file (the top of RAM at $FFE0) and XSTACK are the chip's
 * memory-mapped storage: dual-ported, since the RIA's own firmware addresses them
 * directly (the REGS() shim), so they are shared backing rather than embedded here.
 */

#ifndef _EMU_CHIPS_RP6502_H_
#define _EMU_CHIPS_RP6502_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

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

/* ------------------------------------------------------------------ */
/* RIA register window + syscall dispatch (ria.c)                      */
/* ------------------------------------------------------------------ */

void ria_run(void); /* program start: clear the $FFF0 IRQ latches */
uint8_t ria_reg_read(uint16_t addr);
void ria_reg_write(uint16_t addr, uint8_t data);

/* One PHI2 tick of the RIA's 6502-bus interface, mirroring via_tick so the board
 * drives both bus peripherals uniformly: when selected (the board decodes the RIA
 * window into RREQ) the register access is performed on the pins, and the RIA's
 * IRQB (VSYNC/SIGINT) is driven onto M6502_IRQ. Returns the RIA's own pin mask;
 * the board merges the data and ORs the IRQ onto the bus. */
uint64_t ria_tick(uint64_t pins, bool selected);
void *ria_chip(void); /* ria_t* — the live chip instance, for the debugger UI */

/* RIA $FFF0 interrupt. ria_trigger_vsync latches the VSYNC source (raising IRQB
 * only while the interrupt is enabled); ria_irq_asserted reports whether an
 * enabled source is pending. SIGINT is latched by ria_trigger_sigint, below. */
void ria_trigger_vsync(void);
bool ria_irq_asserted(void);

/* Per-frame entry (after the line editor is pumped). The scanline-rate I/O
 * poll is the shared firmware api_task (ria/api/api.h). */
void ria_task(void);

/* RIA-side firmware ABI the shared rln.c/atr.c/api.c reach through ria/sys/ria.h,
 * whose declarations match these; ria.c implements them here. ria_active is always
 * false in the emulator (no mbuf transfers). */
bool ria_active(void);         /* true while mid mbuf transfer; never here */
void ria_trigger_sigint(void); /* latch a Ctrl-C SIGINT (raises $FFF0 if enabled) */
bool ria_get_sigint(void);     /* consume the latched SIGINT for the attribute (true once) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_CHIPS_RP6502_H_ */
