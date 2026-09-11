/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/api/proc.h"
#include "core/com/com.h"
#include "core/wdc/cpu.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include "core/sys/driver.h"
#include "core/api/api.h"
#include "core/ria/ria.h"
#include <stdatomic.h>
#include <string.h>

static ria_t ria;

#define RIA_IRQ_VSYNC 0x80
#define RIA_IRQ_SIGINT 0x40

/* A read of $FFF0 on the bus is answered from ria.irq_pending and never from
 * the register file, but the debugger's register panel peeks regs[] directly,
 * so regs[0x10] is kept in step with the flags the RIA actually holds. */
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

bool ria_irq_asserted(void)
{
    return (ria.irq_pending & ria.irq_enabled) != 0;
}

/* api_task refuses to latch op 0x00 and op 0xFF, so those two are serviced here,
 * inside the write that requested them. Every other op is latched and dispatched
 * by api_task, called once here so a quick one finishes without waiting for the
 * machine's next task pass. */
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
        /* Read first, because api_return_ax overwrites A and X with its own
         * return value. */
        int16_t code = (int16_t)API_AX;
        (void)api_return_ax(0);
        proc_exit(code);
        return;
    }
    default:
        api_task();
        return;
    }
}

bool ria_active(void)
{
    return false;
}

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
    if (xram_queue_page == (uint8_t)(addr >> 8))
    {
        uint8_t next = (uint8_t)(xram_queue_head + 1);
        if (next != xram_queue_tail)
        {
            xram_queue[next][0] = (uint8_t)addr;
            xram_queue[next][1] = data;
            /* The drain pairs an acquire fence with this one, so the entry
             * has to be written before the head that publishes it. */
            atomic_thread_fence(memory_order_release);
            xram_queue_head = next;
        }
    }
    addr = (uint16_t)(addr + step);
    if (which)
        REGSW(0xFFEA) = addr;
    else
        REGSW(0xFFE6) = addr;
}

#define RIA_UART_RX_READY 0x40
#define RIA_UART_TX_READY 0x80

/* com_read_source (core/com/pick.c) offers the staged byte to every source in
 * turn, so com_rx_reclaim and com_rx_peek compare this against the caller and
 * hand the byte back only to the source it came from. Without the comparison,
 * the keyboard would be handed a byte typed at the UART. */
static com_source_t ria_uart_rx_src;

static void ria_uart_rx_latch(void)
{
    com_source_t src = COM_SOURCE_ANY;
    int ch = com_getchar(&src);
    if (ch < 0)
        return;
    regs[0x02] = (uint8_t)ch;
    regs[0x00] |= RIA_UART_RX_READY;
    ria_uart_rx_src = src;
}

size_t com_rx_reclaim(char *buf, size_t length, com_source_t src)
{
    if (!length || !(regs[0x00] & RIA_UART_RX_READY) || ria_uart_rx_src != src)
        return 0;
    buf[0] = (char)regs[0x02];
    regs[0x00] &= ~RIA_UART_RX_READY;
    regs[0x02] = 0;
    return 1;
}

int com_rx_peek(com_source_t src)
{
    if (!(regs[0x00] & RIA_UART_RX_READY) || ria_uart_rx_src != src)
        return -1;
    return regs[0x02];
}

void ria_break(void)
{
    regs[0x00] &= ~RIA_UART_RX_READY;
    regs[0x02] = 0;
}

uint8_t ria_reg_read(uint16_t addr)
{
    switch (addr & 0x1F)
    {
    case 0x00: /* READY */
    {
        if (!(regs[0x00] & RIA_UART_RX_READY))
            ria_uart_rx_latch();
        regs[0x00] |= RIA_UART_TX_READY;
        return regs[0x00];
    }
    case 0x02: /* RX */
    {
        uint8_t v = regs[0x02];
        /* The refill reads the console through com_getchar, which offers the
         * staged byte back before it reads any source. Drop the staged byte
         * first, or the refill hands back the one being returned here. */
        regs[0x02] = 0;
        regs[0x00] &= ~RIA_UART_RX_READY;
        ria_uart_rx_latch();
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
    case 0x10: /* IRQ */
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

/* regs and the queue are declared volatile, so they are copied a byte at a time
 * through the volatile lvalue. A memcpy would read them through a plain pointer,
 * which is undefined. */
void ria_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u64(c, ria.PINS);
    sst_put_u8(c, ria.irq_enabled);
    sst_put_u8(c, ria.irq_pending);
    sst_put_u8(c, (uint8_t)ria_uart_rx_src);
    for (int i = 0; i < 0x20; i++)
        sst_put_u8(c, regs[i]);
    sst_put(c, xstack, XSTACK_SIZE + 1);
    sst_put_u16(c, (uint16_t)xstack_ptr);
    sst_put_u8(c, xram_queue_page);
    sst_put_u8(c, xram_queue_head);
    sst_put_u8(c, xram_queue_tail);
    for (int i = 0; i < 256; i++)
    {
        sst_put_u8(c, xram_queue[i][0]);
        sst_put_u8(c, xram_queue[i][1]);
    }
}

bool ria_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint64_t pins = sst_get_u64(c);
    uint8_t enabled = sst_get_u8(c);
    uint8_t pending = sst_get_u8(c);
    uint8_t src = sst_get_u8(c);
    uint8_t cells[0x20];
    for (int i = 0; i < 0x20; i++)
        cells[i] = sst_get_u8(c);
    static uint8_t stack[XSTACK_SIZE + 1];
    sst_get(c, stack, sizeof stack);
    uint16_t ptr = sst_get_u16(c);
    uint8_t page = sst_get_u8(c), head = sst_get_u8(c), tail = sst_get_u8(c);
    static uint8_t queue[256][2];
    for (int i = 0; i < 256; i++)
    {
        queue[i][0] = sst_get_u8(c);
        queue[i][1] = sst_get_u8(c);
    }
    if (!sst_ok(c))
        return false;
    if (ptr > XSTACK_SIZE || src >= COM_SOURCE_COUNT)
        return false;
    ria.PINS = pins;
    ria.irq_enabled = enabled;
    ria.irq_pending = pending;
    ria_uart_rx_src = (com_source_t)src;
    for (int i = 0; i < 0x20; i++)
        regs[i] = cells[i];
    memcpy(xstack, stack, sizeof stack);
    xstack_ptr = ptr;
    xram_queue_page = page;
    xram_queue_head = head;
    xram_queue_tail = tail;
    for (int i = 0; i < 256; i++)
    {
        xram_queue[i][0] = queue[i][0];
        xram_queue[i][1] = queue[i][1];
    }
    return true;
}

/* IRQB is sampled before the register access, because a read of $FFF0
 * acknowledges the pending flags and the line must still show asserted for the
 * cycle that read it. */
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

    ria.PINS = (addr & 0x1F) * RIA_PIN_A0 | (uint64_t)*data * RIA_PIN_D0;
    if (read)
        ria.PINS |= RIA_PIN_RW;
    if (irq)
        ria.PINS |= RIA_PIN_IRQ;
    if (selected)
        ria.PINS |= RIA_PIN_CS;

    return irq;
}

void *ria_chip(void) { return &ria; }

void ria_reg_write(uint16_t addr, uint8_t data)
{
    switch (addr & 0x1F)
    {
    case 0x01: /* TX */
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
    case 0x0F: /* OP */
        regs[0x0F] = data;
        ria_syscall(data);
        return;
    case 0x10: /* IRQ */
        ria.irq_enabled = data;
        /* The write acknowledges every source it enables, so a source left
         * disabled keeps its pending bit. */
        ria.irq_pending &= ~data;
        ria_irq_publish();
        return;
    default:
        regs[addr & 0x1F] = data;
        return;
    }
}

/* The SIGINT attribute (core/api/attr.c) reads the same latch the $FFF0
 * interrupt does, so a program can poll for Ctrl-C without enabling the
 * interrupt at all. */
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
    ria.irq_enabled = 0;
    ria.irq_pending = 0;
    regs[0x10] = 0;
}
