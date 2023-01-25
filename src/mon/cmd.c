/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmd.h"
#include "mon.h"
#include "mem/mbuf.h"
#include "ria/ria.h"
#include "str.h"
#include "ria/act.h"
#include "dev/dev.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#define TIMEOUT_MS 200

static enum {
    CMD_IDLE,
    CMD_READ,
    CMD_WRITE,
    CMD_VERIFY,
    CMD_BINARY,
} cmd_state;

static uint32_t rw_addr;
static uint32_t rw_len;
static uint32_t rw_crc;
static absolute_time_t watchdog;

static bool cmd_ria_action_error()
{
    int32_t result = act_result();
    switch (result)
    {
    case -1: // OK
        return false;
        break;
    case -2:
        printf("?action watchdog timeout\n");
        break;
    default:
        printf("?verify failed at $%04lX\n", result);
        break;
    }
    return true;
}

static void cmd_ria_read()
{
    cmd_state = CMD_IDLE;
    if (cmd_ria_action_error())
        return;
    printf("%04lX", rw_addr);
    for (size_t i = 0; i < mbuf_len; i++)
        printf(" %02X", mbuf[i]);
    printf("\n");
}

static void cmd_ria_write()
{
    cmd_state = CMD_IDLE;
    if (cmd_ria_action_error())
        return;
    cmd_state = CMD_VERIFY;
    act_ram_verify(rw_addr);
}

static void cmd_ria_verify()
{
    cmd_state = CMD_IDLE;
    cmd_ria_action_error();
}

// Commands that start with a hex address. Read or write memory.
void cmd_address(const char *args, size_t len)
{
    // addr syntax is already validated by dispatch
    rw_addr = 0;
    size_t i = 0;
    for (; i < len; i++)
    {
        char ch = args[i];
        if (char_is_hex(ch))
            rw_addr = rw_addr * 16 + char_to_int(ch);
        else
            break;
    }
    for (; i < len; i++)
        if (args[i] != ' ')
            break;
    if (rw_addr > 0xFFFF)
    {
        printf("?invalid address\n");
        return;
    }
    if (i == len)
    {
        mbuf_len = (rw_addr | 0xF) - rw_addr + 1;
        act_ram_read(rw_addr);
        cmd_state = CMD_READ;
        return;
    }
    uint32_t data = 0x80000000;
    mbuf_len = 0;
    for (; i < len; i++)
    {
        char ch = args[i];
        if (char_is_hex(ch))
            data = data * 16 + char_to_int(ch);
        else if (ch != ' ')
        {
            printf("?invalid data character\n");
            return;
        }
        if (ch == ' ' || i == len - 1)
        {
            if (data < 0x100)
            {
                mbuf[mbuf_len++] = data;
                data = 0x80000000;
            }
            else
            {
                printf("?invalid data value\n");
                return;
            }
            for (; i + 1 < len; i++)
                if (args[i + 1] != ' ')
                    break;
        }
    }
    act_ram_write(rw_addr);
    cmd_state = CMD_WRITE;
}

static void status_phi2()
{
    printf("PHI2: %ld kHz\n", ria_get_phi2_khz());
}

void cmd_phi2(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            if (val > 8000)
            {
                printf("?invalid frequency\n");
                return;
            }
            ria_set_phi2_khz(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    status_phi2();
}

static void status_resb()
{
    uint8_t reset_ms = ria_get_reset_ms();
    float reset_us = ria_get_reset_us();
    if (!reset_ms)
        printf("RESB: %.3f ms (auto)\n", reset_us / 1000.f);
    else if (reset_ms * 1000 == reset_us)
        printf("RESB: %d ms\n", reset_ms);
    else
        printf("RESB: %.0f ms (%d ms requested)\n", reset_us / 1000.f, reset_ms);
}

void cmd_resb(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            if (val > 255)
            {
                printf("?invalid duration\n");
                return;
            }
            ria_set_reset_ms(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    status_resb();
}

void cmd_start(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    ria_reset();
}

static void status_caps()
{
    const char *const caps_labels[] = {"normal", "inverted", "forced"};
    printf("CAPS: %s\n", caps_labels[ria_get_caps()]);
}

void cmd_caps(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            ria_set_caps(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    status_caps();
}

void cmd_status(const char *args, size_t len)
{
    (void)(args);
    (void)(len);

    status_phi2();
    status_resb();
    printf("RIA : %.1f MHz\n", clock_get_hz(clk_sys) / 1000 / 1000.f);
    status_caps();
    dev_print_all();
}

void cmd_binary(const char *args, size_t len)
{
    if (parse_uint32(&args, &len, &rw_addr) &&
        parse_uint32(&args, &len, &rw_len) &&
        parse_uint32(&args, &len, &rw_crc) &&
        parse_end(args, len))
    {
        if (rw_addr > 0xFFFF)
        {
            printf("?invalid address\n");
            return;
        }
        if (!rw_len || rw_len > MBUF_SIZE || rw_addr + rw_len > 0x10000)
        {
            printf("?invalid length\n");
            return;
        }
        mbuf_len = 0;
        cmd_state = CMD_BINARY;
        watchdog = delayed_by_us(get_absolute_time(),
                                 TIMEOUT_MS * 1000);
        return;
    }
    printf("?invalid argument\n");
}

bool cmd_rx_handler()
{
    if (mbuf_len < rw_len)
        return false;
    if (mbuf_crc32() == rw_crc)
    {
        cmd_state = CMD_WRITE;
        act_ram_write(rw_addr);
    }
    else
    {
        cmd_state = CMD_IDLE;
        puts("?CRC does not match");
    }
    return true;
}

void cmd_task()
{
    if (ria_is_active())
        return;
    switch (cmd_state)
    {
    case CMD_IDLE:
        break;
    case CMD_READ:
        cmd_ria_read();
        break;
    case CMD_WRITE:
        cmd_ria_write();
        break;
    case CMD_VERIFY:
        cmd_ria_verify();
        break;
    case CMD_BINARY:
        if (absolute_time_diff_us(get_absolute_time(), watchdog) < 0)
        {
            printf("?timeout\n");
            cmd_state = CMD_IDLE;
            mon_reset();
        }
        break;
    }
}

void cmd_keep_alive()
{
    watchdog = delayed_by_us(get_absolute_time(),
                             TIMEOUT_MS * 1000);
}

bool cmd_is_active()
{
    return cmd_state != CMD_IDLE && cmd_state != CMD_BINARY;
}

bool cmd_is_rx_binary()
{
    return cmd_state == CMD_BINARY;
}

void cmd_reset()
{
    cmd_state = CMD_IDLE;
}
