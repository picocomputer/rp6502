/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "str/str.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/rln.h"
#include <stdio.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_RAM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define RAM_TIMEOUT_MS 200

static enum {
    SYS_IDLE,
    SYS_READ,
    SYS_WRITE,
    SYS_VERIFY,
    SYS_BINARY,
    SYS_XRAM,
} cmd_state;

static uint32_t rw_addr;
static uint32_t rw_len;
static uint32_t rw_crc;

static int ram_print_response(char *buf, size_t buf_size, int state)
{
    (void)buf_size;
    (void)state;
    assert(mbuf_len <= 16);
    sprintf(buf, "%04lX ", rw_addr);
    buf += strlen(buf);
    for (size_t i = 0; i < mbuf_len; i++)
    {
        if (i == 8)
            *buf++ = ' ';
        uint8_t c = rw_addr < 0x10000 ? mbuf[i] : xram[rw_addr + i - 0x10000];
        sprintf(buf, " %02X", c);
        buf += 3;
    }
    size_t spaces = (16 - mbuf_len) * 3;
    if (mbuf_len <= 8)
        ++spaces;
    for (size_t s = 0; s < spaces; s++)
        *buf++ = ' ';

    *buf++ = ' ';
    *buf++ = ' ';
    *buf++ = '|';
    for (size_t i = 0; i < mbuf_len; i++)
    {
        uint8_t c = rw_addr < 0x10000 ? mbuf[i] : xram[rw_addr + i - 0x10000];
        if (c < 32 || c >= 127)
            c = '.';
        *buf++ = c;
    }
    *buf++ = '|';
    *buf++ = '\n';
    *buf = '\0';
    return -1;
}

static void cmd_ria_read(void)
{
    cmd_state = SYS_IDLE;
    if (ria_handle_error())
        return;
    mon_add_response_fn(ram_print_response);
}

static void cmd_ria_write(void)
{
    cmd_state = SYS_IDLE;
    if (ria_handle_error())
        return;
    cmd_state = SYS_VERIFY;
    ria_verify_buf(rw_addr);
}

static void cmd_ria_verify(void)
{
    cmd_state = SYS_IDLE;
    ria_handle_error();
}

// Commands that start with a hex address. Read or write memory.
void ram_mon_address(const char *args, size_t len)
{
    // addr syntax is already validated by dispatch
    rw_addr = 0;
    size_t i = 0;
    for (; i < len; i++)
    {
        char ch = args[i];
        if (isxdigit(ch))
            rw_addr = rw_addr * 16 + str_xdigit_to_int(ch);
        else
            break;
    }
    for (; i < len; i++)
        if (args[i] != ' ')
            break;
    if (rw_addr > 0x1FFFF)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    if (i == len)
    {
        mbuf_len = (rw_addr | 0xF) - rw_addr + 1;
        if (rw_addr > 0xFFFF)
        {
            mon_add_response_fn(ram_print_response);
            return;
        }
        ria_read_buf(rw_addr);
        cmd_state = SYS_READ;
        return;
    }
    uint32_t data = 0x80000000;
    mbuf_len = 0;
    for (; i < len; i++)
    {
        char ch = args[i];
        if (isxdigit(ch))
            data = data * 16 + str_xdigit_to_int(ch);
        else if (ch != ' ')
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
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
                mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
                return;
            }
            for (; i + 1 < len; i++)
                if (args[i + 1] != ' ')
                    break;
        }
    }
    if (rw_addr > 0xFFFF)
    {
        rw_addr -= 0x10000;
        for (size_t i = 0; i < mbuf_len; i++)
        {
            xram[rw_addr + i] = mbuf[i];
            while (!pix_ready())
                tight_loop_contents();
            PIX_SEND_XRAM(rw_addr + i, mbuf[i]);
        }
        return;
    }
    ria_write_buf(rw_addr);
    cmd_state = SYS_WRITE;
}

static void sys_com_rx_mbuf(bool timeout, const char *buf, size_t length)
{
    (void)buf;
    mbuf_len = length;
    cmd_state = SYS_IDLE;
    if (timeout)
    {
        mon_add_response_str(STR_ERR_RX_TIMEOUT);
        return;
    }
    if (ria_buf_crc32() != rw_crc)
    {
        mon_add_response_str(STR_ERR_CRC);
        return;
    }
    if (rw_addr >= 0x10000)
    {
        cmd_state = SYS_XRAM;
        for (size_t i = 0; i < rw_len; i++)
            xram[rw_addr + i - 0x10000] = buf[i];
    }
    else
    {
        cmd_state = SYS_WRITE;
        ria_write_buf(rw_addr);
    }
}

static void cmd_xram()
{
    while (rw_len)
    {
        if (!pix_ready())
            return;
        uint32_t addr = rw_addr + --rw_len - 0x10000;
        PIX_SEND_XRAM(addr, xram[addr]);
    }
    cmd_state = SYS_IDLE;
}

void ram_mon_binary(const char *args, size_t len)
{
    if (str_parse_uint32(&args, &len, &rw_addr) &&
        str_parse_uint32(&args, &len, &rw_len) &&
        str_parse_uint32(&args, &len, &rw_crc) &&
        str_parse_end(args, len))
    {
        if (rw_addr > 0x1FFFF)
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        if (!rw_len || rw_len > MBUF_SIZE ||
            (rw_addr < 0x10000 && rw_addr + rw_len > 0x10000) ||
            rw_addr + rw_len > 0x20000)
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        rln_read_binary(RAM_TIMEOUT_MS, sys_com_rx_mbuf, mbuf, rw_len);
        cmd_state = SYS_BINARY;
        return;
    }
    mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
}

void ram_task(void)
{
    if (main_active())
        return;
    switch (cmd_state)
    {
    case SYS_IDLE:
    case SYS_BINARY:
        break;
    case SYS_READ:
        cmd_ria_read();
        break;
    case SYS_WRITE:
        cmd_ria_write();
        break;
    case SYS_VERIFY:
        cmd_ria_verify();
        break;
    case SYS_XRAM:
        cmd_xram();
        break;
    }
}

bool ram_active(void)
{
    return cmd_state != SYS_IDLE;
}

void ram_break(void)
{
    cmd_state = SYS_IDLE;
}
