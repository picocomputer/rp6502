/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "ria.pio.h"
#include <pico/stdio.h>
#include <pico/multicore.h>
#include <hardware/dma.h>
#include <hardware/sync.h>
#include <littlefs/lfs_util.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_RIA)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define RIA_WATCHDOG_MS 250

#define RIA_ACTION_RESULT_NONE (-1)
#define RIA_ACTION_RESULT_FINISHED (-2)
#define RIA_ACTION_RESULT_TIMEOUT (-3)

static enum action_state {
    action_state_idle = 0,
    action_state_read,
    action_state_write,
    action_state_verify,
} volatile action_state = action_state_idle;
static absolute_time_t action_watchdog_timer;
static volatile int32_t action_result = RIA_ACTION_RESULT_NONE;
static int32_t saved_reset_vec = -1;
static uint16_t rw_addr;
static volatile int16_t rw_pos;
static volatile int16_t rw_end;

#define RIA_IRQ_VSYNC 0x80
#define RIA_IRQ_SIGINT 0x40

static volatile uint8_t irq_enabled;    // bit7=vsync, bit6=sigint mask
static volatile uint8_t vsync_pending;  // 0 or RIA_IRQ_VSYNC; owner: core0 IRQ
static volatile uint8_t sigint_pending; // 0 or RIA_IRQ_SIGINT; owner: core0 task

void ria_trigger_vsync(void)
{
    if (!ria_active())
    {
        vsync_pending = RIA_IRQ_VSYNC;
        __dmb();
        REGS(0xFFF0) = vsync_pending | sigint_pending;
        if (irq_enabled & RIA_IRQ_VSYNC)
            gpio_put(CPU_IRQB_PIN, false);
    }
}

void ria_trigger_sigint(void)
{
    if (!ria_active())
    {
        sigint_pending = RIA_IRQ_SIGINT;
        __dmb();
        REGS(0xFFF0) = vsync_pending | sigint_pending;
        if (irq_enabled & RIA_IRQ_SIGINT)
            gpio_put(CPU_IRQB_PIN, false);
    }
}

bool ria_get_sigint(void)
{
    if (!sigint_pending)
        return false;
    sigint_pending = 0;
    return true;
}

uint32_t ria_buf_crc32(void)
{
    return ~lfs_crc(~0, mbuf, mbuf_len);
}

void ria_run(void)
{
    irq_enabled = 0;
    vsync_pending = 0;
    sigint_pending = 0;
    REGS(0xFFF0) = 0;
    if (action_state == action_state_idle)
        return;
    action_result = RIA_ACTION_RESULT_NONE;
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    action_watchdog_timer = make_timeout_time_us(cpu_get_reset_us() +
                                                 RIA_WATCHDOG_MS * 1000);
    switch (action_state)
    {
    case action_state_write:
        // Self-modifying fast load
        // FFF0  A9 00     LDA #$00
        // FFF2  8D 00 00  STA $0000
        // FFF5  80 F9     BRA $FFF0
        REGS(0xFFF0) = 0xA9;
        REGS(0xFFF1) = mbuf[0];
        REGS(0xFFF2) = 0x8D;
        REGS(0xFFF3) = rw_addr & 0xFF;
        REGS(0xFFF4) = rw_addr >> 8;
        REGS(0xFFF5) = 0x80;
        REGS(0xFFF6) = 0xF9;
        break;
    case action_state_read:
    case action_state_verify:
        // Self-modifying fast load
        // FFF0  AD 00 00  LDA $0000
        // FFF3  8D FC FF  STA $FFFC/$FFFD
        // FFF6  80 F8     BRA $FFF0
        REGS(0xFFF0) = 0xAD;
        REGS(0xFFF1) = rw_addr & 0xFF;
        REGS(0xFFF2) = rw_addr >> 8;
        REGS(0xFFF3) = 0x8D;
        REGS(0xFFF4) = (action_state == action_state_verify) ? 0xFC : 0xFD;
        REGS(0xFFF5) = 0xFF;
        REGS(0xFFF6) = 0x80;
        REGS(0xFFF7) = 0xF8;
        break;
    default:
        break;
    }
}

void ria_stop(void)
{
    irq_enabled = 0;
    gpio_put(CPU_IRQB_PIN, true);
    action_state = action_state_idle;
    if (saved_reset_vec >= 0)
    {
        REGSW(0xFFFC) = saved_reset_vec;
        saved_reset_vec = -1;
    }
    ria_uart_rx_clear(); // discard input queued for the now-stopped 6502 UART
}

bool ria_active(void)
{
    return action_state != action_state_idle;
}

void ria_task(void)
{
    // check on watchdog unless we explicitly ended or errored
    if (ria_active() && action_result == RIA_ACTION_RESULT_NONE)
    {
        if (time_reached(action_watchdog_timer))
        {
            action_result = RIA_ACTION_RESULT_TIMEOUT;
            main_stop();
        }
    }

    // Resync REGS(0xFFF0) and /IRQ with the pending flags. Heals any
    // benign cross-core race between core0 triggers and core1's clear.
    if (!ria_active())
    {
        uint8_t live = vsync_pending | sigint_pending;
        REGS(0xFFF0) = live;
        gpio_put(CPU_IRQB_PIN, (live & irq_enabled) == 0);
    }
}

static int ria_verify_error_response(char *buf, size_t buf_size, int state, unsigned)
{
    snprintf_utf8(buf, buf_size, S(STR_ERR_RIA_VERIFY), state);
    return -1;
}

bool ria_handle_error(void)
{
    switch (action_result)
    {
    case RIA_ACTION_RESULT_NONE:     // Ok, default at start
    case RIA_ACTION_RESULT_FINISHED: // OK, explicitly ended
        return false;
    case RIA_ACTION_RESULT_TIMEOUT:
        mon_add_response_utf8(S(STR_ERR_RIA_TIMEOUT));
        break;
    default:
        mon_add_response_fn_state(ria_verify_error_response, action_result);
        break;
    }
    return true;
}

void ria_read_buf(uint16_t addr)
{
    assert(!cpu_active());
    action_result = RIA_ACTION_RESULT_NONE;
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF)
            mbuf[len] = REGS(addr + len);
        else
            mbuf[len] = 0;
    while (len && (addr + len > 0xFF00))
        mbuf[--len] = 0;
    if (!len)
        return;
    rw_addr = addr;
    rw_end = len;
    rw_pos = 0;
    action_state = action_state_read;
    main_run();
}

void ria_verify_buf(uint16_t addr)
{
    assert(!cpu_active());
    action_result = RIA_ACTION_RESULT_NONE;
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF && mbuf[len] != REGS(addr + len))
            action_result = addr + len;
    while (len && (addr + len > 0xFF00))
        --len;
    if (!len || action_result != RIA_ACTION_RESULT_NONE)
        return;
    rw_addr = addr;
    rw_end = len;
    rw_pos = 0;
    action_state = action_state_verify;
    main_run();
}

void ria_write_buf(uint16_t addr)
{
    assert(!cpu_active());
    action_result = RIA_ACTION_RESULT_NONE;
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = mbuf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    if (!len)
        return;
    rw_addr = addr;
    rw_end = len;
    // First write doesn't always write because ???
    rw_pos = -1; // force a second write
    action_state = action_state_write;
    main_run();
}

// 6502 memory-mapped UART (0xFFE0-0xFFE2) <-> console bridge. act_loop (core 1)
// produces TX / consumes RX directly (these live here, in its translation unit,
// so its hot path stays a plain memory access); com.c (core 0) drains TX and
// feeds RX through the ria_uart_* accessors. Keeping the cross-core rings a ria
// concern is what lets com.h stay platform-neutral.

// act_loop's 6502 UART-TX ring: single producer (act_loop, core 1), single
// consumer (com_tx_fanout via ria_uart_tx_dequeue, core 0).
#define RIA_UART_TX_BUF_SIZE 32
static volatile uint8_t ria_uart_tx_buf[RIA_UART_TX_BUF_SIZE];
static volatile size_t ria_uart_tx_head;
static volatile size_t ria_uart_tx_tail;

static inline bool ria_uart_tx_writable(void)
{
    return (((ria_uart_tx_head + 1) % RIA_UART_TX_BUF_SIZE) != ria_uart_tx_tail);
}

// Caller (act_loop) must have checked ria_uart_tx_writable() first. __dmb()
// publishes the slot before the head so the core-0 reader can't observe a new
// head with a stale slot.
static inline void ria_uart_tx_write(uint8_t ch)
{
    size_t next = (ria_uart_tx_head + 1) % RIA_UART_TX_BUF_SIZE;
    ria_uart_tx_buf[next] = ch;
    __dmb();
    ria_uart_tx_head = next;
}

// core-0 (com_tx_fanout) pops one byte. The __dmb() finishes reading the slot
// before publishing the tail advance, pairing with the producer DMB.
bool ria_uart_tx_dequeue(uint8_t *ch)
{
    if (ria_uart_tx_head == ria_uart_tx_tail)
        return false;
    size_t next = (ria_uart_tx_tail + 1) % RIA_UART_TX_BUF_SIZE;
    *ch = ria_uart_tx_buf[next];
    __dmb();
    ria_uart_tx_tail = next;
    return true;
}

bool ria_uart_tx_empty(void) { return ria_uart_tx_head == ria_uart_tx_tail; }

// 6502 UART-RX single-byte handoff: com_task (core 0) offers, act_loop (core 1)
// consumes for 0xFFE0/0xFFE2 reads. -1 => empty. The source tag and the
// ordering barrier stay on core 0 in com.c (act_loop never needs the tag).
static volatile int ria_uart_rx_slot = -1;

bool ria_uart_rx_offer_ready(void) { return ria_uart_rx_slot < 0; }
void ria_uart_rx_offer(uint8_t ch) { ria_uart_rx_slot = ch; }
int ria_uart_rx_peek(void) { return ria_uart_rx_slot; }

bool ria_uart_rx_reclaim(uint8_t *ch)
{
    int c = ria_uart_rx_slot;
    if (c < 0)
        return false;
    *ch = (uint8_t)c;
    ria_uart_rx_slot = -1;
    return true;
}

void ria_uart_rx_clear(void) { ria_uart_rx_slot = -1; }

#define CASE_READ(addr) (addr & 0x1F)
#define CASE_WRITE(addr) (0x20 | (addr & 0x1F))
#define RIA_RW0 REGS(0xFFE4)
#define RIA_STEP0 *(int8_t *)&REGS(0xFFE5)
#define RIA_ADDR0 REGSW(0xFFE6)
#define RIA_RW1 REGS(0xFFE8)
#define RIA_STEP1 *(int8_t *)&REGS(0xFFE9)
#define RIA_ADDR1 REGSW(0xFFEA)
// 6502 writes to regs can arrive later than these events.
// Make sure to use the FIFO event data in here instead.
__attribute__((optimize("O3"))) static void __no_inline_not_in_flash_func(act_loop)(void)
{
    while (true)
    {
        RIA_RW0 = xram[RIA_ADDR0];
        RIA_RW1 = xram[RIA_ADDR1];
        if (!(RIA_ACT_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + RIA_ACT_SM))))
        {
            uint32_t rw_addr_data = RIA_ACT_PIO->rxf[RIA_ACT_SM];
            if (((1u << CPU_RESB_PIN) & sio_hw->gpio_in))
            {
                uint32_t data = rw_addr_data & 0xFF;
                switch (rw_addr_data >> 8)
                {
                case CASE_READ(0xFFF4): // action write
                    if (action_state == action_state_write)
                    {
                        if (rw_pos == rw_end)
                        {
                            action_result = RIA_ACTION_RESULT_FINISHED;
                            main_stop();
                        }
                        else if (++rw_pos > 0 && rw_pos < rw_end)
                        {
                            REGS(0xFFF1) = mbuf[rw_pos];
                            REGSW(0xFFF3) += 1;
                        }
                    }
                    break;
                case CASE_WRITE(0xFFFD): // action read
                    if (action_state == action_state_read)
                    {
                        REGSW(0xFFF1) += 1;
                        mbuf[rw_pos] = data;
                        if (++rw_pos == rw_end)
                        {
                            action_result = RIA_ACTION_RESULT_FINISHED;
                            main_stop();
                        }
                    }
                    break;
                case CASE_WRITE(0xFFFC): // action verify
                    if (action_state == action_state_verify)
                    {
                        REGSW(0xFFF1) += 1;
                        if (mbuf[rw_pos] != data && action_result < 0)
                            action_result = REGSW(0xFFF1) - 1;
                        if (++rw_pos == rw_end)
                        {
                            if (action_result < 0)
                                action_result = RIA_ACTION_RESULT_FINISHED;
                            main_stop();
                        }
                    }
                    break;
                case CASE_WRITE(0xFFF0): // IRQ enable mask
                    irq_enabled = data;
                    __attribute__((fallthrough));
                case CASE_READ(0xFFF0): // IRQ event read + selective clear
                    if (action_state == action_state_idle)
                    {
                        if (data & RIA_IRQ_VSYNC)
                            vsync_pending = 0;
                        if (data & RIA_IRQ_SIGINT)
                            sigint_pending = 0;
                        uint8_t live = vsync_pending | sigint_pending;
                        REGS(0xFFF0) = live;
                        gpio_put(CPU_IRQB_PIN, (live & irq_enabled) == 0);
                    }
                    break;
                case CASE_WRITE(0xFFEF): // OS function call
                    API_OP = data;       // get ahead of DMA
                    api_set_regs_blocked();
                    if (data == 0x00) // zxstack()
                    {
                        API_STACK = 0;
                        xstack_ptr = XSTACK_SIZE;
                        api_return_ax(0);
                    }
                    else if (data == 0xFF) // exit()
                        main_stop();
                    break;
                case CASE_WRITE(0xFFEC): // xstack
                    if (xstack_ptr)
                        xstack[--xstack_ptr] = data;
                    API_STACK = xstack[xstack_ptr];
                    break;
                case CASE_READ(0xFFEC): // xstack
                    if (xstack_ptr < XSTACK_SIZE)
                        ++xstack_ptr;
                    API_STACK = xstack[xstack_ptr];
                    break;
                case CASE_WRITE(0xFFE8): // W XRAM1
                    xram[RIA_ADDR1] = data;
                    PIX_SEND_XRAM(RIA_ADDR1, data);
                    if (xram_queue_page == REGS(0xFFEB))
                    {
                        uint8_t next = xram_queue_head + 1;
                        if (next != xram_queue_tail)
                        {
                            xram_queue[next][0] = REGS(0xFFEA);
                            xram_queue[next][1] = data;
                            xram_queue_head = next;
                        }
                    }
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE8): // R XRAM1
                    RIA_ADDR1 += RIA_STEP1;
                    break;
                case CASE_WRITE(0xFFE4): // W XRAM0
                    xram[RIA_ADDR0] = data;
                    PIX_SEND_XRAM(RIA_ADDR0, data);
                    if (xram_queue_page == REGS(0xFFE7))
                    {
                        uint8_t next = xram_queue_head + 1;
                        if (next != xram_queue_tail)
                        {
                            xram_queue[next][0] = REGS(0xFFE6);
                            xram_queue[next][1] = data;
                            xram_queue_head = next;
                        }
                    }
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE4): // R XRAM0
                    RIA_ADDR0 += RIA_STEP0;
                    break;
                case CASE_READ(0xFFE2): // UART Rx
                {
                    int ch = ria_uart_rx_slot;
                    if (ch >= 0)
                    {
                        REGS(0xFFE2) = (uint8_t)ch;
                        REGS(0xFFE0) |= 0b01000000;
                        ria_uart_rx_slot = -1;
                    }
                    else
                    {
                        REGS(0xFFE0) &= ~0b01000000;
                        REGS(0xFFE2) = 0;
                    }
                    break;
                }
                case CASE_WRITE(0xFFE1): // UART Tx
                    if (ria_uart_tx_writable())
                        ria_uart_tx_write(data);
                    if (ria_uart_tx_writable())
                        REGS(0xFFE0) |= 0b10000000;
                    else
                        REGS(0xFFE0) &= ~0b10000000;
                    break;
                case CASE_READ(0xFFE0): // UART Tx/Rx flow control
                {
                    uint8_t flags = REGS(0xFFE0);
                    if (!(flags & 0b01000000))
                    {
                        int ch = ria_uart_rx_slot;
                        if (ch >= 0)
                        {
                            REGS(0xFFE2) = (uint8_t)ch;
                            flags |= 0b01000000;
                            ria_uart_rx_slot = -1;
                        }
                    }
                    if (ria_uart_tx_writable())
                        flags |= 0b10000000;
                    else
                        flags &= ~0b10000000;
                    REGS(0xFFE0) = flags;
                    break;
                }
                }
            }
        }
    }
}

static void __in_flash("ria_cs_rwb_pio_init") ria_cs_rwb_pio_init(void)
{
    uint offset = pio_add_program(RIA_CS_RWB_PIO, &ria_cs_rwb_program);
    pio_sm_config config = ria_cs_rwb_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    sm_config_set_in_pin_count(&config, 2);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_out_shift(&config, true, false, 0);
    sm_config_set_out_pin_count(&config, 8);
    sm_config_set_jmp_pin(&config, CPU_PHI2_PIN);
    pio_sm_init(RIA_CS_RWB_PIO, RIA_CS_RWB_SM, offset, &config);
    pio_sm_set_enabled(RIA_CS_RWB_PIO, RIA_CS_RWB_SM, true);
}

static void __in_flash("ria_write_pio_init") ria_write_pio_init(void)
{
    // PIO to manage PHI2 clock and 6502 writes
    uint offset = pio_add_program(RIA_WRITE_PIO, &ria_write_program);
    pio_sm_config config = ria_write_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_sideset_pins(&config, CPU_PHI2_PIN);
    pio_gpio_init(RIA_WRITE_PIO, CPU_PHI2_PIN);
    pio_sm_set_consecutive_pindirs(RIA_WRITE_PIO, RIA_WRITE_SM, CPU_PHI2_PIN, 1, true);
    pio_sm_init(RIA_WRITE_PIO, RIA_WRITE_SM, offset, &config);
    pio_sm_put(RIA_WRITE_PIO, RIA_WRITE_SM, (uintptr_t)regs >> 5);
    pio_sm_exec_wait_blocking(RIA_WRITE_PIO, RIA_WRITE_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_WRITE_PIO, RIA_WRITE_SM, pio_encode_mov(pio_y, pio_osr));
    pio_sm_set_enabled(RIA_WRITE_PIO, RIA_WRITE_SM, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_WRITE_PIO, RIA_WRITE_SM, false));
    channel_config_set_read_increment(&data_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        regs,                              // dst
        &RIA_WRITE_PIO->rxf[RIA_WRITE_SM], // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_WRITE_PIO, RIA_WRITE_SM, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->write_addr, // dst
        &RIA_WRITE_PIO->rxf[RIA_WRITE_SM],           // src
        1,
        true);
}

static void __in_flash("ria_read_pio_init") ria_read_pio_init(void)
{
    // PIO for 6502 reads
    uint offset = pio_add_program(RIA_READ_PIO, &ria_read_program);
    pio_sm_config config = ria_read_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_ADDR_PIN_BASE);
    sm_config_set_in_shift(&config, false, true, 5);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_out_shift(&config, true, true, 8);
    for (int i = RIA_DATA_PIN_BASE; i < RIA_DATA_PIN_BASE + 8; i++)
        pio_gpio_init(RIA_READ_PIO, i);
    pio_sm_init(RIA_READ_PIO, RIA_READ_SM, offset, &config);
    pio_sm_put(RIA_READ_PIO, RIA_READ_SM, (uintptr_t)regs >> 5);
    pio_sm_exec_wait_blocking(RIA_READ_PIO, RIA_READ_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_READ_PIO, RIA_READ_SM, pio_encode_mov(pio_y, pio_osr));
    pio_sm_set_enabled(RIA_READ_PIO, RIA_READ_SM, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_READ_PIO, RIA_READ_SM, true));
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        &RIA_READ_PIO->txf[RIA_READ_SM], // dst
        regs,                            // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_READ_PIO, RIA_READ_SM, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->read_addr, // dst
        &RIA_READ_PIO->rxf[RIA_READ_SM],            // src
        1,
        true);
}

static void __in_flash("ria_act_pio_init") ria_act_pio_init(void)
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(RIA_ACT_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, true, true, 32);
    pio_sm_init(RIA_ACT_PIO, RIA_ACT_SM, offset, &config);
    // The CS/RWB PIO triggers read events only on offsets where
    // (addr & 0x1F) % 4 == 0. Register one extra watched read offset.
    pio_sm_put(RIA_ACT_PIO, RIA_ACT_SM, 0xFFE2 & 0x1F); // UART Rx
    pio_sm_set_enabled(RIA_ACT_PIO, RIA_ACT_SM, true);
    multicore_launch_core1(act_loop);
}

void __in_flash("ria_init") ria_init(void)
{
    // drive irq pin
    gpio_init(CPU_IRQB_PIN);
    gpio_put(CPU_IRQB_PIN, true);
    gpio_set_dir(CPU_IRQB_PIN, true);

    // safety check for compiler alignment
    assert(!((uintptr_t)regs & 0x1F));

    // Adjustments for GPIO performance. Important!
    for (int i = RIA_PIN_BASE; i < RIA_PIN_BASE + 15; i++)
    {
        pio_gpio_init(pio0, i); // any pio
        gpio_set_pulls(i, false, false);
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&pio0->input_sync_bypass, 1u << i);
        hw_set_bits(&pio1->input_sync_bypass, 1u << i);
        hw_set_bits(&pio2->input_sync_bypass, 1u << i);
    }

    // the inits
    ria_cs_rwb_pio_init();
    ria_write_pio_init();
    ria_read_pio_init();
    ria_act_pio_init();
}

void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    // chip_select doesn't reclock
    pio_sm_set_clkdiv_int_frac(RIA_WRITE_PIO, RIA_WRITE_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_READ_PIO, RIA_READ_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_ACT_PIO, RIA_ACT_SM, clkdiv_int, clkdiv_frac);
}
