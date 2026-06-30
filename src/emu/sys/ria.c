/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RIA register window ($FFE0-$FFF9) and the 6502 syscall ABI.
 *
 * The hardware uses a self-modifying BRA-spin trampoline at $FFF1 plus a
 * RIA-side polling loop. Most syscalls finish synchronously inside the $FFEF
 * (API_OP) write: by the time the 6502 fetches its next instruction the
 * trampoline is released and the return value (A/X/SREG) is patched in.
 *
 * I/O follows a polling model: a source returns api_working() until its bytes
 * are ready (stdin until the user types a line; a file read completes on the
 * first poll). The trampoline stays blocked, the 6502 spins on it, and
 * ria_task_pump() re-dispatches the op every scanline until it completes —
 * mirroring how the real RIA polls the operation to completion in its loop.
 *
 * The return mechanics live in ria/api/api.h, shared with std.c/dir.c and the
 * vendored rln.c.
 */

#include "emu/api/api.h"
#include "emu/api/clk.h"
#include "emu/api/oem.h"
#include "emu/api/pro.h"
#include "emu/api/std.h"
#include "emu/aud/snd.h"
#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/hid/pad.h"
#include "emu/host/host.h"
#include "emu/sys/com.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/via.h"
#include "emu/sys/xreg.h"
#include "emu/sys/ria.h" /* ria_t — the RIA modeled as a bus-interface chip */
#include "api/api.h"
#include "api/atr.h"
#include "api/std.h"
#include "api/dir.h"
#include "api/clk.h"
#include "str/rln.h"
#include "sys/ria.h"
#include "emu/sys/w65c02.h" /* M6502_* bus pin macros for ria_tick (impl is in w65c02.c) */
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

/* True while an enabled RIA source is pending. sys.c ORs M6502_IRQ from this
 * after via_tick — purely additive, so the RIA and the VIA share the IRQ line
 * (the VIA owns clearing it; the RIA only ever adds its own assertion). */
bool ria_irq_asserted(void)
{
    return (ria.irq_pending & ria.irq_enabled) != 0;
}

/* ------------------------------------------------------------------ */
/* xreg (op 0x01): marshal device/channel/address + words off the xstack */
/* ------------------------------------------------------------------ */

/* The i-th xreg data word (target address+i) sits at xstack[SIZE-5-2i]. */
static uint16_t word_at(int i)
{
    uint16_t word;
    memcpy(&word, &xstack[XSTACK_SIZE - 5 - 2 * i], sizeof(word));
    return word;
}

static bool std_xreg(void)
{
    uint8_t device = xstack[XSTACK_SIZE - 1];
    uint8_t channel = xstack[XSTACK_SIZE - 2];
    uint8_t address = xstack[XSTACK_SIZE - 3];
    int count = (int)((XSTACK_SIZE - xstack_ptr - 3) / 2);
    if ((xstack_ptr & 1) == 0 || count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_EINVAL);
    }
    /* word[i] lives at xstack[SIZE-5-2i] and targets address+i. Hardware
     * dispatch order: a VGA channel-0 multi-word call starting at address 0
     * sends the canvas word (address 0) first so it can't clear later mode
     * programming; every other word follows high address -> low. This makes
     * a register that consumes earlier ones (e.g. the term mode word at
     * address 1) land after its parameters. */
    bool canvas_first = (device == 1 && channel == 0 && address == 0 && count > 1);
    if (canvas_first && !emu_xreg(device, channel, address, word_at(0)))
    {
        xstack_ptr = XSTACK_SIZE;
        return api_return_errno(API_EINVAL);
    }
    for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
    {
        /* PIX_DEVICE_RIA (device 0) holds the address constant (last-wins);
         * only the VGA/non-RIA path walks address+i. */
        uint8_t reg = device ? (uint8_t)(address + i) : address;
        if (!emu_xreg(device, channel, reg, word_at(i)))
        {
            xstack_ptr = XSTACK_SIZE;
            return api_return_errno(API_EINVAL);
        }
    }
    xstack_ptr = XSTACK_SIZE;
    return api_return_ax(0);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Returns true if the op has more work (api_working) and should be
 * re-dispatched, false once it has returned to the 6502. */
static bool ria_op_dispatch(uint8_t op)
{
    switch (op)
    {
    case 0x01:
        return std_xreg();
    case 0x02:
        return atr_api_phi2();
    case 0x03:
        return atr_api_code_page();
    case 0x04:
        return atr_api_lrand();
    case 0x06:
        return atr_api_errno_opt();
    case 0x08:
        return pro_api_argv();
    case 0x09:
        return pro_api_exec();
    case 0x0A:
        return atr_api_get();
    case 0x0B:
        return atr_api_set();
    case 0x14:
        return std_api_open();
    case 0x15:
        return std_api_close();
    case 0x16:
        return std_api_read_xstack();
    case 0x17:
        return std_api_read_xram();
    case 0x18:
        return std_api_write_xstack();
    case 0x19:
        return std_api_write_xram();
    case 0x1A:
        return std_api_lseek_cc65();
    case 0x1B:
        return dir_api_unlink();
    case 0x1C:
        return dir_api_rename();
    case 0x1D:
        return std_api_lseek_llvm();
    case 0x1E:
        return std_api_syncfs();
    case 0x1F:
        return dir_api_stat();
    case 0x20:
        return dir_api_opendir();
    case 0x21:
        return dir_api_readdir();
    case 0x22:
        return dir_api_closedir();
    case 0x23:
        return dir_api_telldir();
    case 0x24:
        return dir_api_seekdir();
    case 0x25:
        return dir_api_rewinddir();
    case 0x26:
        return dir_api_chmod();
    case 0x27:
        return dir_api_utime();
    case 0x28:
        return dir_api_mkdir();
    case 0x29:
        return dir_api_chdir();
    case 0x2A:
        return dir_api_chdrive();
    case 0x2B:
        return dir_api_getcwd();
    case 0x2C:
        return dir_api_setlabel();
    case 0x2D:
        return dir_api_getlabel();
    case 0x2E:
        return dir_api_getfree();
    case 0x30:
        return rln_api_lastkey();
    case 0x31:
        return rln_api_peek();
    case 0x32:
        return rln_api_poke();
    case 0x3A:
        return clk_api_gmtime();
    case 0x3B:
        return clk_api_localtime();
    case 0x3C:
        return clk_api_mktime();
    case 0x3D:
        return clk_api_strftime();
    case 0x3E:
        return clk_api_time_set();
    case 0x3F:
        return clk_api_time_get();
    default:
        return api_return_errno(API_ENOSYS);
    }
}

/* The op currently blocked mid-flight (ria.pending_op, 0 = none). Only a stdin
 * read ever lingers; everything else completes on the first dispatch. */
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
        emu_exit_code = (uint8_t)code;
        (void)api_return_ax(0);
        /* If a launcher is armed, pro_exit re-execs it (machine keeps running);
         * otherwise the chain has ended, so halt. */
        if (!pro_exit(code))
            emu_cpu_halted = true;
        return;
    }
    default:
        /* Dispatch once. If the handler is still working (blocking stdin),
         * leave it pending and let the 6502 spin until ria_task finishes it. */
        ria.pending_op = ria_op_dispatch(op) ? op : 0;
        return;
    }
}

/* Re-dispatch the in-flight op (the I/O poll). A cheap no-op when nothing is
 * pending. Called every scanline (the emulator's analog of the RIA super-loop)
 * so a pending source completes within ~32 us of virtual time, not a whole
 * frame — nothing tolerates per-frame I/O latency. */
void ria_task_pump(void)
{
    if (!ria.pending_op)
        return;
    if (!ria_op_dispatch(ria.pending_op))
        ria.pending_op = 0;
}

/* Per-frame entry, run after the line editor (std_task) so a stdin line that
 * just arrived is dispatched at the frame boundary. */
void ria_task(void)
{
    ria_task_pump();
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

/* The emulator has no physical UART; the bare-UART RX pins read the same typed
 * input as stdin (the keyboard com source). The com ring stands in for the
 * firmware's com_rx_char upstream byte. Using the direct UART regs while a stdio
 * call is in flight is undefined per the docs, so sharing the source is faithful. */
static int ria_uart_rx_next(void)
{
    com_source_t src = COM_SOURCE_KBD;
    return com_getchar(&src);
}

uint8_t ria_reg_read(uint16_t addr)
{
    switch (addr & 0x1F)
    {
    case 0x00: /* UART flow control: bit7 TX always ok; bit6 set once a byte is
                * pulled into the $FFE2 latch (mirrors ria/sys/ria.c $FFE0). */
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
    case 0x02: /* UART RX (mirrors ria/sys/ria.c $FFE2): pull the next byte,
                * flag bit6 and return it, else clear bit6 and return 0. */
    {
        int ch = ria_uart_rx_next();
        if (ch >= 0)
        {
            regs[0x02] = (uint8_t)ch;
            regs[0x00] |= RIA_UART_RX_READY;
            return regs[0x02];
        }
        regs[0x00] &= ~RIA_UART_RX_READY;
        regs[0x02] = 0;
        return 0;
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

/* The RIA's 6502-bus interface, mirroring via_tick (via.c) so sys.c can drive
 * both peripherals as `pins = peripheral_tick(pins)`. The IRQB OR runs BEFORE the
 * register access so that a read of $FFF0 (which acks/clears the pending flags)
 * still shows IRQB asserted on its own cycle — preserving the prior bus ordering
 * (via_tick -> RIA IRQ OR -> RIA register access -> RAM) exactly, so timing is
 * byte-identical. ria.PINS is stashed for the debug overlay's pin view. */
uint64_t ria_tick(uint64_t pins)
{
    if (ria_irq_asserted())
        pins |= M6502_IRQ;
    uint16_t addr = M6502_GET_ADDR(pins);
    if (addr >= RIA_WINDOW_LO && addr <= RIA_WINDOW_HI)
    {
        if (pins & M6502_RW)
        {
            M6502_SET_DATA(pins, ria_reg_read(addr)); /* braces: the macro is a {block} */
        }
        else
            ria_reg_write(addr, M6502_GET_DATA(pins));
    }
    ria.PINS = pins;
    return pins;
}

/* The live chip instance, for the debugger UI (the RIA overlay reads ria.PINS),
 * mirroring via_chip()/sys_cpu(). */
void *ria_chip(void) { return &ria; }

void ria_reg_write(uint16_t addr, uint8_t data)
{
    switch (addr & 0x1F)
    {
    case 0x01: /* UART TX: emit the byte; TX is always ready (bit7). */
    {
        char c = (char)data;
        emu_stdout_write(&c, 1);
        regs[0x00] |= RIA_UART_TX_READY;
        return;
    }
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
/* Reset (mirrors api_run startup)                                     */
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

void ria_reset(void)
{
    for (int a = 0xFFE0; a <= 0xFFEF; a++)
        if (a != 0xFFE3) /* leave VSYNC counter */
            REGS(a) = 0;
    ria.irq_enabled = 0; /* $FFF0: IRQ disabled, no pending sources, line idle */
    ria.irq_pending = 0;
    regs[0x10] = 0;
    xstack_ptr = XSTACK_SIZE;
    xstack[XSTACK_SIZE] = 0; /* cstring guard */
    REGS(0xFFE5) = 1;        /* STEP0 */
    REGS(0xFFE9) = 1;        /* STEP1 */
    API_ERRNO = 0xFFFF;
    api_set_axsreg(0xFFFFFFFFu);
    api_set_regs_released();
    ria.pending_op = 0;
    emu_cpu_halted = false;
    emu_exit_code = 0;
    std_reset();
    kbd_reset();
    pad_reset();
    mou_reset();
    clk_reset();
    snd_reset();
    /* NOTE: the OEM code page is deliberately NOT reset here. ria_reset also runs
     * on exec, and an exec'd program inherits the current code page (it can read
     * it back via the CODE_PAGE attribute). oem_reset() is a cold-boot default,
     * applied by emu_init instead — keeping the font, code page, and attribute
     * in sync across an exec. */
}
