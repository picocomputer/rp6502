/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "core/sys/ria.h"
#include "core/api/api.h"
#include "core/api/proc.h"
#include "ria/mon/mon.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "ria/sys/phi2.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/pix.h"
#include "ria/sys/resb.h"
#include "ria/sys/ria.h"
#include "ria.pio.h"
#include <pico/stdio.h>
#include <pico/multicore.h>
#include <hardware/dma.h>
#include <hardware/structs/bus_ctrl.h>
#include <hardware/sync.h>

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
static ria_callback_t action_callback;

#define RIA_IRQ_VSYNC 0x80
#define RIA_IRQ_SIGINT 0x40

static volatile uint8_t irq_enabled;
static volatile uint8_t vsync_pending;
static volatile uint8_t sigint_pending;

void ria_trigger_vsync(void)
{
    if (action_state == action_state_idle)
    {
        vsync_pending = RIA_IRQ_VSYNC;
        __dmb();
        REGS(0xFFF0) = vsync_pending | sigint_pending;
        if (irq_enabled & RIA_IRQ_VSYNC)
            gpio_put(CPU_IRQB_PIN, false);
    }
}

// sigint_pending is set even during a transfer, but $FFF0 is written only
// when no transfer is running, because the loop that the 6502 runs for a
// transfer is written at $FFF0. ria_task rewrites $FFF0 and IRQB from the
// pending flags once the transfer ends.
void ria_trigger_sigint(void)
{
    sigint_pending = RIA_IRQ_SIGINT;
    if (action_state == action_state_idle)
    {
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

void ria_run(void)
{
    irq_enabled = 0;
    vsync_pending = 0;
    sigint_pending = 0;
    REGS(0xFFF0) = 0;
}

void ria_stop(void)
{
    irq_enabled = 0;
    gpio_put(CPU_IRQB_PIN, true);
}

static int ria_verify_error_response(char *buf, size_t buf_size, int state, unsigned)
{
    oem_snprintf(buf, buf_size, S(STR_ERR_RIA_VERIFY), state);
    return -1;
}

/* The reset vector is saved here and restored by ria_action_close. ria_task
 * calls ria_action_close between the two halves of a write, so the verify half
 * compares the bytes above $FFF9 against what the write put there. */
static void ria_action_start(void)
{
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    if (action_state == action_state_write)
    {
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
    }
    else
    {
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
    }
    action_watchdog_timer = make_timeout_time_us(resb_get_reset_us() +
                                                 RIA_WATCHDOG_MS * 1000);
    resb_release();
}

static void ria_action_close(void)
{
    action_state = action_state_idle;
    if (saved_reset_vec >= 0)
    {
        REGSW(0xFFFC) = saved_reset_vec;
        saved_reset_vec = -1;
    }
}

/* A transfer is ended from either core. RESB goes low before the result is
 * published, so ria_task never rewrites the loop or the reset vector while
 * the 6502 can still fetch them. */
static void ria_action_end(int32_t result)
{
    resb_assert();
    __dmb();
    action_result = result;
}

static void ria_verify_start(void)
{
    action_state = action_state_verify;
    action_result = RIA_ACTION_RESULT_NONE;
    uint16_t addr = rw_addr;
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF && mbuf[len] != REGS(addr + len))
            action_result = addr + len;
    while (len && (addr + len > 0xFF00))
        --len;
    if (action_result != RIA_ACTION_RESULT_NONE)
        return;
    if (!len)
    {
        action_result = RIA_ACTION_RESULT_FINISHED;
        return;
    }
    rw_end = len;
    rw_pos = 0;
    ria_action_start();
}

void ria_task(void)
{
    if (action_state == action_state_idle)
    {
        // A trigger on core 0 can race the clear in act_loop on core 1 and
        // leave $FFF0 or IRQB out of step with the pending flags, so both are
        // rewritten from the flags on every idle pass.
        uint8_t live = vsync_pending | sigint_pending;
        REGS(0xFFF0) = live;
        gpio_put(CPU_IRQB_PIN, (live & irq_enabled) == 0);
        return;
    }
    if (action_result == RIA_ACTION_RESULT_NONE)
    {
        if (time_reached(action_watchdog_timer))
            ria_action_end(RIA_ACTION_RESULT_TIMEOUT);
        return;
    }
    if (action_state == action_state_write &&
        action_result == RIA_ACTION_RESULT_FINISHED)
    {
        ria_action_close();
        ria_verify_start();
        return;
    }
    bool ok = action_result == RIA_ACTION_RESULT_FINISHED;
    if (action_result == RIA_ACTION_RESULT_TIMEOUT)
        mon_add_response_utf8(S(STR_ERR_RIA_TIMEOUT));
    else if (!ok)
        mon_add_response_fn_state(ria_verify_error_response, action_result);
    ria_action_close();
    ria_callback_t callback = action_callback;
    action_callback = NULL;
    callback(ok);
}

void ria_read_buf(uint16_t addr, ria_callback_t callback)
{
    assert(!sys_active());
    action_callback = callback;
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
    action_state = action_state_read;
    if (!len)
    {
        action_result = RIA_ACTION_RESULT_FINISHED;
        return;
    }
    rw_addr = addr;
    rw_end = len;
    rw_pos = 0;
    ria_action_start();
}

void ria_write_buf(uint16_t addr, ria_callback_t callback)
{
    assert(!sys_active());
    action_callback = callback;
    action_result = RIA_ACTION_RESULT_NONE;
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = mbuf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    rw_addr = addr;
    if (!len)
    {
        ria_verify_start();
        return;
    }
    rw_end = len;
    // First write doesn't always write because ???
    rw_pos = -1; // force a second write
    action_state = action_state_write;
    ria_action_start();
}

// The UART TX ring has a single producer, act_loop on core 1, and a single
// consumer, com_tx_fanout on core 0 through ria_uart_tx_dequeue.
#define RIA_UART_TX_BUF_SIZE 32
static volatile uint8_t ria_uart_tx_buf[RIA_UART_TX_BUF_SIZE];
static volatile size_t ria_uart_tx_head;
static volatile size_t ria_uart_tx_tail;

static inline bool ria_uart_tx_writable(void)
{
    return (((ria_uart_tx_head + 1) % RIA_UART_TX_BUF_SIZE) != ria_uart_tx_tail);
}

// The barrier publishes the slot before the head, so core 0 never reads a new
// head while the slot still holds a stale byte.
static inline void ria_uart_tx_write(uint8_t ch)
{
    size_t next = (ria_uart_tx_head + 1) % RIA_UART_TX_BUF_SIZE;
    ria_uart_tx_buf[next] = ch;
    __dmb();
    ria_uart_tx_head = next;
}

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

static volatile int ria_uart_rx_slot = -1;

bool ria_uart_rx_offer_ready(void)
{
    return ria_uart_rx_slot < 0 && !(REGS(0xFFE0) & 0b01000000);
}

void ria_uart_rx_offer(uint8_t ch) { ria_uart_rx_slot = ch; }

int ria_uart_rx_peek(void)
{
    int c = ria_uart_rx_slot;
    if (c >= 0)
        return c;
    if (REGS(0xFFE0) & 0b01000000)
        return REGS(0xFFE2);
    return -1;
}

bool ria_uart_rx_reclaim(uint8_t *ch)
{
    int c = ria_uart_rx_slot;
    if (c >= 0)
    {
        *ch = (uint8_t)c;
        ria_uart_rx_slot = -1;
        return true;
    }
    if (REGS(0xFFE0) & 0b01000000)
    {
        *ch = REGS(0xFFE2);
        REGS(0xFFE0) &= ~0b01000000;
        REGS(0xFFE2) = 0;
        return true;
    }
    return false;
}

static void ria_uart_rx_clear(void)
{
    ria_uart_rx_slot = -1;
    REGS(0xFFE0) = 0;
    REGS(0xFFE2) = 0;
}

/* sys_commit calls sys_stop, which lowers RESB, before it calls the break
 * hooks, so the 6502 is held in reset when ria_break closes the transfer. */
void ria_break(void)
{
    action_callback = NULL;
    ria_action_close();
    ria_uart_rx_clear();
}

volatile uint8_t ria_aud_head;
volatile uint8_t ria_aud_tail;
volatile uint8_t ria_aud_ring[RIA_AUD_RING_SIZE][2];

static volatile uint32_t ria_aud_page = 0xFFFF;

void ria_aud_watch(uint16_t xaddr)
{
    ria_aud_page = 0xFFFF;
    ria_aud_tail = ria_aud_head;
    if (xaddr != 0xFFFF)
        ria_aud_page = (uint32_t)(xaddr & 0xFF00);
}

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
                            ria_action_end(RIA_ACTION_RESULT_FINISHED);
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
                            ria_action_end(RIA_ACTION_RESULT_FINISHED);
                    }
                    break;
                case CASE_WRITE(0xFFFC): // action verify
                    if (action_state == action_state_verify)
                    {
                        REGSW(0xFFF1) += 1;
                        if (mbuf[rw_pos] != data)
                            ria_action_end(REGSW(0xFFF1) - 1);
                        else if (++rw_pos == rw_end)
                            ria_action_end(RIA_ACTION_RESULT_FINISHED);
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
                    if (data == 0x00) // ria_drop()
                    {
                        API_STACK = 0;
                        xstack_ptr = XSTACK_SIZE;
                        api_return_ax(0);
                    }
                    else if (data == 0xFF) // exit()
                    {
                        proc_exit((int16_t)API_AX);
                    }
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
                    if ((RIA_ADDR1 & 0xFF00) == ria_aud_page)
                    {
                        uint8_t next = (ria_aud_head + 1) & (RIA_AUD_RING_SIZE - 1);
                        if (next != ria_aud_tail)
                        {
                            ria_aud_ring[next][0] = REGS(0xFFEA);
                            ria_aud_ring[next][1] = data;
                            /* The drain pairs an acquire barrier with this
                             * one, so the entry has to be written before the
                             * head that publishes it. */
                            __dmb();
                            ria_aud_head = next;
                        }
                    }
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE8): // R XRAM1
                    RIA_ADDR1 += RIA_STEP1;
                    break;
                case CASE_WRITE(0xFFE4): // W XRAM0
                    xram[RIA_ADDR0] = data;
                    PIX_SEND_XRAM(RIA_ADDR0, data);
                    if ((RIA_ADDR0 & 0xFF00) == ria_aud_page)
                    {
                        uint8_t next = (ria_aud_head + 1) & (RIA_AUD_RING_SIZE - 1);
                        if (next != ria_aud_tail)
                        {
                            ria_aud_ring[next][0] = REGS(0xFFE6);
                            ria_aud_ring[next][1] = data;
                            /* The drain pairs an acquire barrier with this
                             * one, so the entry has to be written before the
                             * head that publishes it. */
                            __dmb();
                            ria_aud_head = next;
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
    // The action program reports a read only at an offset that is a multiple
    // of four or that equals the last value written to its TX FIFO, so the
    // $FFE2 offset is written there.
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

    /* A 6502 read is served by two chained DMA transfers that must finish
     * within one PHI2 cycle, so the DMA is given bus priority over both
     * cores. */
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

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
