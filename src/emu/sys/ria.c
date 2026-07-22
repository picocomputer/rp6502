/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/emu/pro.h"
#include "emu/sys/com.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/main.h"
#include "ria/api/api.h"
#include "emu/sys/ria.h"
#include <string.h>

/* The RIA chip instance. ria.c keeps a single ria_t and ticks it on the 6502 bus,
 * exactly as via.c wraps its m6522_t (`static m6522_t via;`). The memory-mapped
 * register file (regs[]) and the XSTACK are its dual-ported storage and stay
 * global; ria holds the bus pins + the non-memory-mapped internal latches. */
static ria_t ria;

/* ------------------------------------------------------------------ */
/* RIA interrupt ($FFF0): VSYNC (bit7) + SIGINT (bit6)                 */
/* ------------------------------------------------------------------ */

#define RIA_IRQ_VSYNC 0x80
#define RIA_IRQ_SIGINT 0x40

/* Mirror ria/sys/ria.c: an enable mask the 6502 writes to $FFF0 plus the two
 * latched pending flags. The IRQ line is asserted while a pending source is
 * also enabled; reading $FFF0 returns the flags and acknowledges them. $FFF0
 * (offset 0x10) sits just past the API trampoline window, so it is its own
 * register, not RAM. */
/* Keep $FFF0's readback byte in step with the live pending flags. */
static void ria_irq_publish(void)
{
    regs[0x10] = ria.irq_pending;
}

void ria_trigger_sigint(void)
{
    ria.irq_pending |= RIA_IRQ_SIGINT;
    ria_irq_publish();
}

void ria_trigger_vsync(void)
{
    ria.irq_pending |= RIA_IRQ_VSYNC;
    ria_irq_publish();
}

/* True while an enabled RIA source is pending. ria_tick returns this as the RIA's
 * IRQB; the board ORs every device's assertion onto the shared line, so the RIA and
 * the VIA can both raise it without either owning the clear. */
bool ria_irq_asserted(void)
{
    return (ria.irq_pending & ria.irq_enabled) != 0;
}

/* $FFEF write. ZXSTACK (0x00) and EXIT (0xFF) run inside the write, mirroring
 * act_loop in ria/sys/ria.c; every other op is latched by the shared api_task,
 * pumped once here so most syscalls still complete before the 6502's next
 * instruction. */
static void ria_syscall(uint8_t op)
{
    api_set_regs_blocked();
    switch (op)
    {
    case 0x00: /* ZXSTACK */
        xstack_ptr = XSTACK_SIZE;
        (void)api_return_ax(0);
        return;
    case 0xFF: /* EXIT */
    {
        int16_t code = (int16_t)API_AX; /* capture before api_return_ax clobbers A/X */
        (void)api_return_ax(0);
        /* If a launcher is armed, pro_exit re-execs it (machine keeps running);
         * otherwise the chain has ended, so halt. */
        if (!pro_exit(code))
            cpu_set_halted(true);
        return;
    }
    default:
        api_task();
        return;
    }
}

/* Per-frame entry, run after the line editor (std_task) so a stdin line that
 * just arrived is dispatched at the frame boundary. */
void ria_task(void)
{
    api_task();
}

/* rln consults this to suppress cursor-shape escapes while the RIA is busy
 * with an mbuf transfer; the emulator never is. */
bool ria_active(void)
{
    return false;
}

/* ------------------------------------------------------------------ */
/* XRAM windowed access (RW0/RW1 with signed auto-increment)           */
/* ------------------------------------------------------------------ */

static uint8_t rw_read(int which)
{
    uint16_t addr = which ? REGSW(0xFFEA) : REGSW(0xFFE6);
    int8_t step = (int8_t)(which ? regs[0x09] : regs[0x05]);
    uint8_t v = xram[addr];
    addr = (uint16_t)(addr + step);
    if (which)
        REGSW(0xFFEA) = addr;
    else
        REGSW(0xFFE6) = addr;
    return v;
}

static void rw_write(int which, uint8_t data)
{
    uint16_t addr = which ? REGSW(0xFFEA) : REGSW(0xFFE6);
    int8_t step = (int8_t)(which ? regs[0x09] : regs[0x05]);
    xram[addr] = data;
    /* Notify the active audio device of writes to its page (ria/sys/ria.c):
     * record (low byte, value) for its handler to drain. */
    if (xram_queue_page == (uint8_t)(addr >> 8))
    {
        uint8_t next = (uint8_t)(xram_queue_head + 1);
        if (next != xram_queue_tail)
        {
            xram_queue[next][0] = (uint8_t)addr;
            xram_queue[next][1] = data;
            xram_queue_head = next;
        }
    }
    addr = (uint16_t)(addr + step);
    if (which)
        REGSW(0xFFEA) = addr;
    else
        REGSW(0xFFE6) = addr;
}

/* ------------------------------------------------------------------ */
/* Register window read/write                                          */
/* ------------------------------------------------------------------ */

/* Bare UART flow-control bits in $FFE0 (ria/sys/ria.c). */
#define RIA_UART_RX_READY 0x40
#define RIA_UART_TX_READY 0x80

/* The emulator has no physical UART; the bare-UART RX pins read the merged input
 * stream (keyboard plus terminal replies), matching the firmware's com_rx_pick
 * across all sources — so a terminal-query reply (e.g. the CPR from ESC[6n) is
 * readable at $FFE2. Mixing the direct UART regs with an in-flight stdio call is
 * undefined per the docs, so sharing the stream is faithful. */
static int ria_uart_rx_next(void)
{
    com_source_t src = COM_SOURCE_ANY;
    return com_getchar(&src);
}

uint8_t ria_reg_read(uint16_t addr)
{
    switch (addr & 0x1F)
    {
    case 0x00: /* UART flow control: bit7 TX always ok; bit6 set once a byte is
                * pulled into the $FFE2 latch. */
    {
        uint8_t flags = regs[0x00];
        if (!(flags & RIA_UART_RX_READY))
        {
            int ch = ria_uart_rx_next();
            if (ch >= 0)
            {
                regs[0x02] = (uint8_t)ch;
                flags |= RIA_UART_RX_READY;
            }
        }
        flags |= RIA_UART_TX_READY;
        regs[0x00] = flags;
        return flags;
    }
    case 0x02: /* UART RX: return the latched byte, then refill it. */
    {
        uint8_t v = regs[0x02];
        int ch = ria_uart_rx_next();
        if (ch >= 0)
        {
            regs[0x02] = (uint8_t)ch;
            regs[0x00] |= RIA_UART_RX_READY;
        }
        else
        {
            regs[0x02] = 0;
            regs[0x00] &= ~RIA_UART_RX_READY;
        }
        return v;
    }
    case 0x04: /* RW0 */
        return rw_read(0);
    case 0x08: /* RW1 */
        return rw_read(1);
    case 0x0C: /* XSTACK pop */
    {
        uint8_t v = xstack[xstack_ptr];
        if (xstack_ptr < XSTACK_SIZE)
            xstack_ptr++;
        regs[0x0C] = xstack[xstack_ptr];
        return v;
    }
    case 0x10: /* $FFF0 IRQ flags: read the pending sources, then acknowledge
                * (clear) them — the read deasserts the line on the next tick. */
    {
        uint8_t live = ria.irq_pending;
        ria.irq_pending = 0;
        ria_irq_publish();
        return live;
    }
    default:
        return regs[addr & 0x1F];
    }
}

/* The RIA's 6502-bus interface, mirroring via_tick (via.c). The IRQB drive is read
 * BEFORE the register access so a read of $FFF0 (which acks/clears the pending
 * flags) still shows IRQB asserted on its own cycle. ria.PINS is published in the
 * RIA's own pin layout for the debug overlay. */
bool ria_tick(uint16_t addr, bool read, uint8_t *data)
{
    const bool selected = addr >= RIA_MMAP_LO && addr <= RIA_MMAP_HI;
    const bool irq = ria_irq_asserted();

    if (selected)
    {
        if (read)
            *data = ria_reg_read(addr);
        else
            ria_reg_write(addr, *data);
    }

    /* Only the five address lines the RIA wires actually reach it. */
    ria.PINS = (addr & 0x1F) * RIA_PIN_A0 | (uint64_t)*data * RIA_PIN_D0;
    if (read)
        ria.PINS |= RIA_PIN_RW;
    if (irq)
        ria.PINS |= RIA_PIN_IRQ;
    if (selected)
        ria.PINS |= RIA_PIN_CS;

    return irq;
}

/* The live chip instance, for the debugger UI (the RIA overlay reads ria.PINS),
 * mirroring via_chip()/cpu_chip(). */
void *ria_chip(void) { return &ria; }

void ria_reg_write(uint16_t addr, uint8_t data)
{
    switch (addr & 0x1F)
    {
    case 0x01: /* UART TX: emit the byte; TX is always ready (bit7). */
        /* Raw, like hardware: $FFE1 bypasses the SDK's CRLF translation, so a
         * bare '\n' stair-steps on the terminal. */
        com_write((char)data);
        regs[0x00] |= RIA_UART_TX_READY;
        return;
    case 0x04: /* RW0 */
        rw_write(0, data);
        return;
    case 0x08: /* RW1 */
        rw_write(1, data);
        return;
    case 0x0C: /* XSTACK push */
        if (xstack_ptr > 0)
            xstack[--xstack_ptr] = data;
        regs[0x0C] = xstack[xstack_ptr];
        return;
    case 0x0F: /* API_OP trigger */
        regs[0x0F] = data;
        ria_syscall(data);
        return;
    case 0x10: /* $FFF0 IRQ enable mask; bits set in the write are also acked */
        ria.irq_enabled = data;
        ria.irq_pending &= ~data;
        ria_irq_publish();
        return;
    default:
        regs[addr & 0x1F] = data;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* The SIGINT attribute (vendored atr.c) consumes the same latch the $FFF0 IRQ
 * uses: a program can poll for a Ctrl-C break without enabling the interrupt.
 * Returns true once per latched SIGINT, then clears it. */
bool ria_get_sigint(void)
{
    if (!(ria.irq_pending & RIA_IRQ_SIGINT))
        return false;
    ria.irq_pending &= ~RIA_IRQ_SIGINT;
    ria_irq_publish();
    return true;
}

void ria_run(void)
{
    ria.irq_enabled = 0; /* $FFF0: IRQ disabled, no pending sources, line idle */
    ria.irq_pending = 0;
    regs[0x10] = 0;
}
