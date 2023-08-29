/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys.pio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "littlefs/lfs_util.h"

// This is the smallest value that will
// allow 1k read/write operations at 50 kHz.
#define RIA_WATCHDOG_MS 250

#define RIA_WRITE_PIO pio0
#define RIA_WRITE_SM 0
#define RIA_READ_PIO pio0
#define RIA_READ_SM 1
#define RIA_ACT_PIO pio1
#define RIA_ACT_SM 0

static enum state {
    action_state_idle = 0,
    action_state_read,
    action_state_write,
    action_state_verify,
} volatile action_state = action_state_idle;
static absolute_time_t action_watchdog_timer;
static volatile int32_t action_result = -1;
static int32_t saved_reset_vec = -1;
static uint16_t rw_addr;
static volatile int32_t rw_pos;
static volatile int32_t rw_end;

uint8_t ria_buf[MBUF_SIZE] __attribute__((aligned(4)));
size_t ria_buf_len;

uint32_t ria_buf_crc32()
{
    // use littlefs library
    return ~lfs_crc(~0, ria_buf, ria_buf_len);
}

// The PIO will notify the action loop of all register writes.
// Only every fourth register (0, 4, 8, ...) is watched for
// read access. This additional read address to be watched
// is varied based on the state of the RIA.
static void ria_set_watch_address(uint32_t addr)
{
    pio_sm_put(RIA_ACT_PIO, RIA_ACT_SM, addr & 0x1F);
}

void ria_run()
{
    ria_set_watch_address(0xFFE2);
    if (action_state == action_state_idle)
        return;
    action_result = -1;
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    action_watchdog_timer = delayed_by_us(get_absolute_time(),
                                          cpu_get_reset_us() +
                                              RIA_WATCHDOG_MS * 1000);
    switch (action_state)
    {
    case action_state_write:
        // Self-modifying fast load
        // FFF0  A9 00     LDA #$00
        // FFF2  8D 00 00  STA $0000
        // FFF5  80 F9     BRA $FFF0
        // FFF7  80 FE     BRA $FFF7
        ria_set_watch_address(0xFFF6);
        REGS(0xFFF0) = 0xA9;
        REGS(0xFFF1) = ria_buf[0];
        REGS(0xFFF2) = 0x8D;
        REGS(0xFFF3) = rw_addr & 0xFF;
        REGS(0xFFF4) = rw_addr >> 8;
        REGS(0xFFF5) = 0x80;
        REGS(0xFFF6) = 0xF9;
        REGS(0xFFF7) = 0x80;
        REGS(0xFFF8) = 0xFE;
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

void ria_stop()
{
    action_state = action_state_idle;
    if (saved_reset_vec >= 0)
    {
        REGSW(0xFFFC) = saved_reset_vec;
        saved_reset_vec = -1;
    }
}

bool ria_active()
{
    return action_state != action_state_idle;
}

void ria_task()
{
    // check on watchdog
    if (ria_active())
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, action_watchdog_timer) < 0)
        {
            action_result = -2;
            main_stop();
        }
    }
}

bool ria_print_error_message()
{
    switch (action_result)
    {
    case -1: // OK
        return false;
        break;
    case -2:
        printf("?watchdog timeout\n");
        break;
    default:
        printf("?verify failed at $%04lX\n", action_result);
        break;
    }
    return true;
}

void ria_read_buf(uint16_t addr)
{
    assert(!cpu_active());
    // avoid forbidden areas
    uint16_t len = ria_buf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF)
            ria_buf[len] = REGS(addr + len);
        else
            ria_buf[len] = 0;
    while (len && (addr + len > 0xFF00))
        if (addr + --len <= 0xFFFF)
            ria_buf[len] = 0;
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
    // avoid forbidden areas
    action_result = -1;
    uint16_t len = ria_buf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF && ria_buf[len] != REGS(addr + len))
            action_result = addr + len;
    while (len && (addr + len > 0xFF00))
        --len;
    if (!len || action_result != -1)
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
    // avoid forbidden area
    uint16_t len = ria_buf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = ria_buf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    if (!len)
        return;
    rw_addr = addr;
    rw_end = len;
    // Evil hack. Because otherwise this fails:
    // set phi2 100; 0000 0 0 0 0; set phi2 4000; 0000 5 5 5 5
    // It used to be that writes <10kHz would fail.
    rw_pos = -2;
    action_state = action_state_write;
    main_run();
}

#define CASE_READ(addr) (addr & 0x1F)
#define CASE_WRITE(addr) (0x20 | (addr & 0x1F))
static __attribute__((optimize("O1"))) void act_loop()
{
    // In here we bypass the usual SDK calls as needed for performance.
    while (true)
    {
        if (!(RIA_ACT_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + RIA_ACT_SM))))
        {
            uint32_t rw_addr_data = RIA_ACT_PIO->rxf[RIA_ACT_SM];
            if (((1u << CPU_RESB_PIN) & sio_hw->gpio_in))
            {
                uint32_t data = rw_addr_data & 0xFF;
                switch (rw_addr_data >> 8)
                {
                case CASE_READ(0xFFF6): // action write
                    if (rw_pos < rw_end)
                    {
                        if (rw_pos > 0)
                        {
                            REGS(0xFFF1) = ria_buf[rw_pos];
                            REGSW(0xFFF3) += 1;
                        }
                        if (++rw_pos == rw_end)
                            REGS(0xFFF6) = 0x00;
                    }
                    else
                    {
                        gpio_put(CPU_RESB_PIN, false);
                        main_stop();
                    }
                    break;
                case CASE_WRITE(0xFFFD): // action read
                    if (rw_pos < rw_end)
                    {
                        REGSW(0xFFF1) += 1;
                        ria_buf[rw_pos] = data;
                        if (++rw_pos == rw_end)
                        {
                            gpio_put(CPU_RESB_PIN, false);
                            main_stop();
                        }
                    }
                    break;
                case CASE_WRITE(0xFFFC): // action verify
                    if (rw_pos < rw_end)
                    {
                        REGSW(0xFFF1) += 1;
                        if (ria_buf[rw_pos] != data && action_result < 0)
                            action_result = REGSW(0xFFF1) - 1;
                        if (++rw_pos == rw_end)
                        {
                            gpio_put(CPU_RESB_PIN, false);
                            main_stop();
                        }
                    }
                    break;
                case CASE_WRITE(0xFFEF): // OS function call
                    api_return_blocked();
                    if (data == 0x00) // zxstack()
                    {
                        API_STACK = 0;
                        xstack_ptr = XSTACK_SIZE;
                        api_return_ax(0);
                    }
                    else if (data == 0xFF) // exit()
                    {
                        gpio_put(CPU_RESB_PIN, false);
                        main_stop();
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
                case CASE_WRITE(0xFFEB): // Set XRAM >ADDR1
                    REGS(0xFFEB) = data;
                    API_RW1 = xram[API_ADDR1];
                    break;
                case CASE_WRITE(0xFFEA): // Set XRAM <ADDR1
                    REGS(0xFFEA) = data;
                    API_RW1 = xram[API_ADDR1];
                    break;
                case CASE_WRITE(0xFFE8): // W XRAM1
                    xram[API_ADDR1] = data;
                    PIX_SEND_XRAM(API_ADDR1, data);
                    API_RW0 = xram[API_ADDR0];
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE8): // R XRAM1
                    API_ADDR1 += API_STEP1;
                    API_RW1 = xram[API_ADDR1];
                    break;
                case CASE_WRITE(0xFFE7): // Set XRAM >ADDR0
                    REGS(0xFFE7) = data;
                    API_RW0 = xram[API_ADDR0];
                    break;
                case CASE_WRITE(0xFFE6): // Set XRAM <ADDR0
                    REGS(0xFFE6) = data;
                    API_RW0 = xram[API_ADDR0];
                    break;
                case CASE_WRITE(0xFFE4): // W XRAM0
                    xram[API_ADDR0] = data;
                    PIX_SEND_XRAM(API_ADDR0, data);
                    API_RW1 = xram[API_ADDR1];
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE4): // R XRAM0
                    API_ADDR0 += API_STEP0;
                    API_RW0 = xram[API_ADDR0];
                    break;
                case CASE_READ(0xFFE2): // UART Rx
                {
                    int ch = cpu_rx_char;
                    if (ch >= 0)
                    {
                        REGS(0xFFE2) = ch;
                        REGS(0xFFE0) |= 0b01000000;
                        cpu_rx_char = -1;
                    }
                    else
                    {
                        REGS(0xFFE0) &= ~0b01000000;
                        REGS(0xFFE2) = 0;
                    }
                    break;
                }
                case CASE_WRITE(0xFFE1): // UART Tx
                    if (com_tx_writable())
                        com_tx_write(data);
                    if (com_tx_writable())
                        REGS(0xFFE0) |= 0b10000000;
                    else
                        REGS(0xFFE0) &= ~0b10000000;
                    break;
                case CASE_READ(0xFFE0): // UART Tx/Rx flow control
                {
                    int ch = cpu_rx_char;
                    if (!(REGS(0xFFE0) & 0b01000000) && ch >= 0)
                    {
                        REGS(0xFFE2) = ch;
                        REGS(0xFFE0) |= 0b01000000;
                        cpu_rx_char = -1;
                    }
                    if (com_tx_writable())
                        REGS(0xFFE0) |= 0b10000000;
                    else
                        REGS(0xFFE0) &= ~0b10000000;
                    break;
                }
                }
            }
        }
    }
}

static void ria_write_pio_init()
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
    pio_sm_put(RIA_WRITE_PIO, RIA_WRITE_SM, (uintptr_t)ria_regs >> 5);
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
        ria_regs,                          // dst
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

static void ria_read_pio_init()
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
    pio_sm_set_consecutive_pindirs(RIA_READ_PIO, RIA_READ_SM, RIA_DATA_PIN_BASE, 8, true);
    pio_sm_init(RIA_READ_PIO, RIA_READ_SM, offset, &config);
    pio_sm_put(RIA_READ_PIO, RIA_READ_SM, (uintptr_t)ria_regs >> 5);
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
        ria_regs,                        // src
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

static void ria_act_pio_init()
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(RIA_ACT_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, true, true, 32);
    pio_sm_init(RIA_ACT_PIO, RIA_ACT_SM, offset, &config);
    ria_set_watch_address(0);
    pio_sm_set_enabled(RIA_ACT_PIO, RIA_ACT_SM, true);
    multicore_launch_core1(act_loop);
}

void ria_init()
{
    // safety check for compiler alignment
    assert(!((uintptr_t)ria_regs & 0x1F));

    // Adjustments for GPIO performance. Important!
    for (int i = RIA_PIN_BASE; i < RIA_PIN_BASE + 15; i++)
    {
        gpio_set_pulls(i, true, true);
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&pio0->input_sync_bypass, 1u << i);
        hw_set_bits(&pio1->input_sync_bypass, 1u << i);
    }

    // Raise DMA above CPU on crossbar
    bus_ctrl_hw->priority |=
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    // the inits
    ria_write_pio_init();
    ria_read_pio_init();
    ria_act_pio_init();
}

void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    pio_sm_set_clkdiv_int_frac(RIA_WRITE_PIO, RIA_WRITE_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_READ_PIO, RIA_READ_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_ACT_PIO, RIA_ACT_SM, clkdiv_int, clkdiv_frac);
}
