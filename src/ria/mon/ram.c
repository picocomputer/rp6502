/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "str/rln.h"
#include "str/str.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include <stdio.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_RAM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define RAM_TIMEOUT_MS 200

static enum {
    RAM_IDLE,
    RAM_READ,
    RAM_WRITE,
    RAM_VERIFY,
    RAM_BINARY,
    RAM_XRAM,
} ram_state;

static uint32_t ram_rw_addr;
static uint32_t ram_rw_end;
static uint32_t ram_rw_size;
static uint32_t ram_rw_crc;
static uint32_t ram_intel_hex_base;

static bool ram_start_read_chunk(void);

static int ram_print_response(char *buf, size_t buf_size, int state)
{
    (void)buf_size;
    assert(mbuf_len <= 16);
    if (state < 0)
        return state;
    sprintf(buf, "%04lX ", ram_rw_addr);
    buf += strlen(buf);
    for (size_t i = 0; i < mbuf_len; i++)
    {
        if (i == 8)
            *buf++ = ' ';
        uint8_t c = ram_rw_addr + i < 0x10000 ? mbuf[i] : xram[ram_rw_addr + i - 0x10000];
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
        uint8_t c = ram_rw_addr + i < 0x10000 ? mbuf[i] : xram[ram_rw_addr + i - 0x10000];
        if (c < 32 || c >= 127)
            c = '.';
        *buf++ = c;
    }
    *buf++ = '|';
    *buf++ = '\n';
    *buf = '\0';
    ram_rw_addr += mbuf_len;
    if (ram_rw_addr > ram_rw_end)
        return -1;
    if (ram_start_read_chunk())
        return -1;
    return 0;
}

// Sets mbuf_len for the next chunk. For RIA-bus addresses, kicks off a RIA
// read and returns true (caller should wait). For XRAM, returns false (data
// is already resident; caller can print without a fetch).
static bool ram_start_read_chunk(void)
{
    mbuf_len = ram_rw_end - ram_rw_addr + 1;
    if (mbuf_len > 16)
        mbuf_len = 16;
    if (ram_rw_addr >= 0x10000)
        return false;
    ria_read_buf(ram_rw_addr);
    ram_state = RAM_READ;
    return true;
}

static void ram_ria_read(void)
{
    ram_state = RAM_IDLE;
    if (ria_handle_error())
        return;
    mon_add_response_fn(ram_print_response);
}

static void ram_ria_write(void)
{
    ram_state = RAM_IDLE;
    if (ria_handle_error())
        return;
    ram_state = RAM_VERIFY;
    ria_verify_buf(ram_rw_addr);
}

static void ram_ria_verify(void)
{
    ram_state = RAM_IDLE;
    ria_handle_error();
}

static void ram_begin_write(void)
{
    if (ram_rw_addr > 0xFFFF)
    {
        ram_rw_addr -= 0x10000;
        for (size_t i = 0; i < mbuf_len; i++)
        {
            xram[ram_rw_addr + i] = mbuf[i];
            while (!pix_ready())
                tight_loop_contents();
            PIX_SEND_XRAM(ram_rw_addr + i, mbuf[i]);
        }
        return;
    }
    ria_write_buf(ram_rw_addr);
    ram_state = RAM_WRITE;
}

static void ram_intel_hex(const char *args)
{
    args += 1; // eat colon
    size_t len = strlen(args);
    while (len && args[len - 1] == ' ')
        len--;
    if (len < 10 || len % 2)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    uint8_t ichecksum = 0;
    uint8_t icount = 0;
    uint8_t itype = 0;
    ram_rw_addr = 0;
    mbuf_len = 0;
    for (size_t i = 0; i < len; i += 2)
    {
        if (!isxdigit((unsigned char)args[i]) ||
            !isxdigit((unsigned char)args[i + 1]))
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        uint8_t val = str_xdigit_to_int(args[i]) * 16 +
                      str_xdigit_to_int(args[i + 1]);
        ichecksum += val;
        if (i == 0)
            icount = val;
        else if (i == 2 || i == 4)
            ram_rw_addr = ram_rw_addr * 0x100 + val;
        else if (i == 6)
            itype = val;
        else
            mbuf[mbuf_len++] = val;
    }
    if (icount != --mbuf_len || ichecksum)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    switch (itype)
    {
    case 0: // Data
        ram_rw_addr += ram_intel_hex_base;
        ram_begin_write();
        break;
    case 1: // End of file
        ram_intel_hex_base = 0;
        break;
    case 2: // Extended segment address
        if (icount == 2)
            ram_intel_hex_base = mbuf[0] * 0x1000 + mbuf[1] * 0x10;
        break;
    case 4: // Extended linear address
        if (icount == 2)
            ram_intel_hex_base = mbuf[0] * 0x1000000 + mbuf[1] * 0x10000;
        break;
    case 3: // Start segment address
    case 5: // Start linear address
        if (icount == 4)
        {
            mbuf[0] = mbuf[2];
            mbuf[1] = mbuf[3];
            ram_rw_addr = 0xFFFC;
            mbuf_len = 2;
            ram_begin_write();
        }
        break;
    default:
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        break;
    }
}

// Hex-address commands and Intel HEX records. Read or write memory.
void ram_mon_address(const char *args)
{
    if (*args == ':')
        return ram_intel_hex(args);
    ram_rw_addr = 0;
    ram_rw_end = 0;
    bool second_found = false;
    bool second_selected = false;
    size_t i = 0;
    for (; args[i]; i++)
    {
        char ch = args[i];
        if (isxdigit(ch))
        {
            if (!second_selected)
                ram_rw_addr = ram_rw_addr * 16 + str_xdigit_to_int(ch);
            else
            {
                second_found = true;
                ram_rw_end = ram_rw_end * 16 + str_xdigit_to_int(ch);
            }
        }
        else if (ch == '-' && second_selected == true)
            break;
        else if (ch == '-')
            second_selected = true;
        else
            break;
    }
    if (!second_selected)
    {
        ram_rw_end = ram_rw_addr + 15;
        if (ram_rw_end >= 0x20000)
            ram_rw_end = 0x1FFFF;
    }
    else if (!second_found)
    {
        ram_rw_end = 0x1FFFF;
    }
    if (args[i] == ':')
        i++;
    for (; args[i] == ' '; i++)
        ;
    if (args[i] == ':')
        i++;
    if (ram_rw_addr > 0x1FFFF || ram_rw_end > 0x1FFFF || ram_rw_addr > ram_rw_end)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    if (!args[i])
    {
        if (!ram_start_read_chunk())
            mon_add_response_fn(ram_print_response);
        return;
    }
    if (second_selected)
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    uint32_t data = 0x80000000;
    mbuf_len = 0;
    for (; args[i]; i++)
    {
        char ch = args[i];
        if (ch == '|')
            break;
        else if (isxdigit(ch))
            data = data * 16 + str_xdigit_to_int(ch);
        else if (ch != ' ')
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        if (ch == ' ' || !args[i + 1])
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
            for (; args[i + 1] == ' '; i++)
                ;
        }
    }
    ram_begin_write();
}

static void ram_rx_mbuf(bool timeout)
{
    ram_state = RAM_IDLE;
    if (timeout)
    {
        mon_add_response_str(STR_ERR_RX_TIMEOUT);
        return;
    }
    if (ria_buf_crc32() != ram_rw_crc)
    {
        mon_add_response_str(STR_ERR_CRC);
        return;
    }
    if (ram_rw_addr >= 0x10000)
    {
        ram_state = RAM_XRAM;
        for (size_t i = 0; i < ram_rw_size; i++)
            xram[ram_rw_addr + i - 0x10000] = mbuf[i];
    }
    else
    {
        ram_state = RAM_WRITE;
        ria_write_buf(ram_rw_addr);
    }
}

static void ram_xram(void)
{
    while (ram_rw_size)
    {
        if (!pix_ready())
            return;
        uint32_t addr = ram_rw_addr + --ram_rw_size - 0x10000;
        PIX_SEND_XRAM(addr, xram[addr]);
    }
    ram_state = RAM_IDLE;
}

void ram_mon_binary(const char *args)
{
    if (str_parse_uint32(&args, &ram_rw_addr) &&
        str_parse_uint32(&args, &ram_rw_size) &&
        str_parse_uint32(&args, &ram_rw_crc) &&
        str_parse_end(args))
    {
        if (ram_rw_addr > 0x1FFFF)
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        if (!ram_rw_size || ram_rw_size > MBUF_SIZE ||
            (ram_rw_addr < 0x10000 && ram_rw_addr + ram_rw_size > 0x10000) ||
            ram_rw_addr + ram_rw_size > 0x20000)
        {
            mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
            return;
        }
        mem_read_mbuf(RAM_TIMEOUT_MS, ram_rx_mbuf, ram_rw_size);
        ram_state = RAM_BINARY;
        return;
    }
    mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
}

void ram_task(void)
{
    if (main_active())
        return;
    switch (ram_state)
    {
    case RAM_IDLE:
    case RAM_BINARY:
        break;
    case RAM_READ:
        ram_ria_read();
        break;
    case RAM_WRITE:
        ram_ria_write();
        break;
    case RAM_VERIFY:
        ram_ria_verify();
        break;
    case RAM_XRAM:
        ram_xram();
        break;
    }
}

bool ram_active(void)
{
    return ram_state != RAM_IDLE;
}

void ram_break(void)
{
    ram_intel_hex_base = 0;
    ram_state = RAM_IDLE;
}
