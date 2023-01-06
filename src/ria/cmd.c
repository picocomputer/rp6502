/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmd.h"
#include "mon.h"
#include "msc.h"
#include "ria.h"
#include "ria_action.h"
#include <stdio.h>
#include "hardware/clocks.h"

#define MON_RW_SIZE 512
static uint8_t rw_buf[MON_RW_SIZE];

static bool is_hex(uint8_t ch)
{
    return ((ch >= '0') && (ch <= '9')) ||
           ((ch >= 'A') && (ch <= 'F')) ||
           ((ch >= 'a') && (ch <= 'f'));
}

static uint32_t to_int(uint8_t ch)
{
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    if (ch - 'A' < 6)
        return ch - 'A' + 10;
    if (ch - 'a' < 6)
        return ch - 'a' + 10;
}

// Expects a single argument in hex or decimal. e.g. 0x0, $0, 0
// Returns negative value on failure.
static int32_t arg_to_int32(const uint8_t *args, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
    {
        if (args[i] != ' ')
            break;
    }
    int32_t base = 10;
    int32_t value = 0;
    if (i < len && args[i] == '$')
    {
        base = 16;
        i++;
    }
    else if (i + 1 < len && args[i] == '0' &&
             (args[i + 1] == 'x' || args[i + 1] == 'X'))
    {
        base = 16;
        i += 2;
    }
    if (i == len)
        return -1;
    for (; i < len; i++)
    {
        uint8_t ch = args[i];
        if (is_hex(ch))
        {
            int32_t i = to_int(ch);
            if (i < base)
            {
                value = value * base + i;
                if (value >= 0)
                    continue;
            }
        }
        for (; i < len; i++)
        {
            if (args[i] != ' ')
                return -1;
        }
    }
    return value;
}

// case insensitive string with length limit
static int strnicmp(const char *string1, const char *string2, int n)
{
    while (n--)
    {
        if (!*string1 && !*string2)
            return 0;
        int ch1 = *string1;
        int ch2 = *string2;
        if (ch1 >= 'a' && ch1 <= 'z')
            ch1 -= 32;
        if (ch2 >= 'a' && ch2 <= 'z')
            ch2 -= 32;
        int rc = ch1 - ch2;
        if (rc)
            return rc;
        string1++;
        string2++;
    }
    return 0;
}

// Commands that start with a hex address. Read or write memory.
static void cmd_address(uint32_t addr, const char *args, size_t len)
{
    // TODO move address check to RIA
    if (addr > 0xFFFF)
    {
        printf("?invalid address\n");
        return;
    }
    if (!len)
    {
        mon_read(addr, rw_buf, (addr | 0xF) - addr + 1);
        return;
    }
    uint32_t data = 0x80000000;
    uint16_t rw_len = 0;
    for (size_t i = 0; i < len; i++)
    {
        uint8_t ch = args[i];
        if (is_hex(ch))
            data = data * 16 + to_int(ch);
        else if (ch != ' ')
        {
            printf("?invalid data character\n");
            return;
        }
        if (ch == ' ' || i == len - 1)
        {
            if (data < 0x100)
            {
                rw_buf[rw_len++] = data;
                data = 0x80000000;
            }
            else
            {
                printf("?invalid data value\n");
                return;
            }
            for (; i + 1 < len; i++)
            {
                if (args[i + 1] != ' ')
                    break;
            }
        }
    }
    mon_write(addr, rw_buf, rw_len);
}

static void status_speed()
{
    printf("PHI2: %ld kHz\n", ria_get_phi2_khz());
}

static void cmd_speed(const uint8_t *args, size_t len)
{
    if (len)
    {
        int32_t i = arg_to_int32(args, len);
        if (i < 0)
        {
            printf("?syntax error\n");
            return;
        }
        if (i > 8000)
        {
            printf("?invalid frequency\n");
            return;
        }
        ria_set_phi2_khz(i);
    }
    status_speed();
}

static void status_reset()
{
    uint8_t reset_ms = ria_get_reset_ms();
    float reset_us = ria_get_reset_us();
    if (!reset_ms)
        printf("RESB: %.3f ms (auto)\n", reset_us / 1000.f);
    else if (reset_ms * 1000 == reset_us)
        printf("RESB: %ld ms\n", reset_ms);
    else
        printf("RESB: %.0f ms (%ld ms requested)\n", reset_us / 1000.f, reset_ms);
}

static void cmd_reset(const uint8_t *args, size_t len)
{
    if (len)
    {
        int32_t i = arg_to_int32(args, len);
        if (i < 0)
        {
            printf("?syntax error\n");
            return;
        }
        if (i > 255)
        {
            printf("?invalid duration\n");
            return;
        }
        ria_set_reset_ms(i);
    }
    status_reset();
}

static void cmd_jmp(const uint8_t *args, size_t len)
{
    int32_t addr = arg_to_int32(args, len);
    if (!len || addr < 0 || addr > 0xFFFF)
    {
        printf("?invalid address\n");
        return;
    }
    ria_action_jmp(addr);
}

static void cmd_start(const uint8_t *args, size_t len)
{
    ria_reset();
}

static void status_caps()
{
    const char *const caps_labels[] = {"normal", "inverted", "forced"};
    printf("CAPS: %s\n", caps_labels[ria_get_caps()]);
}

static void cmd_caps(const uint8_t *args, size_t len)
{
    int32_t val = arg_to_int32(args, len);
    if (len)
    {
        if (val < 0 || val > 2)
        {
            printf("?invalid argument\n");
            return;
        }
        ria_set_caps(val);
    }
    status_caps();
}

static void cmd_status(const uint8_t *args, size_t len)
{
    status_speed();
    status_reset();
    printf("RIA : %.1f MHz\n", clock_get_hz(clk_sys) / 1000 / 1000.f);
    status_caps();
}

static void cmd_ls(const uint8_t *args, size_t len)
{
    msc_ls(args);
}

static void cmd_cd(const uint8_t *args, size_t len)
{
    msc_cd(args);
}

static void cmd_help(const uint8_t *args, size_t len)
{
    printf(
        "Commands:\n"
        "HELP         - This help.\n"
        "STATUS       - Show all settings.\n"
        "CAPS (0|1|2) - Invert or force caps while 6502 is running.\n"
        "SPEED (kHz)  - Query or set PHI2 speed. This is the 6502 clock.\n"
        "RESET (ms)   - Query or set RESB hold time. Set to 0 for auto.\n"
        "JMP address  - Start the 6502. Begin execution at address.\n"
        "START        - Start the 6502. Begin execution at ($FFFC).\n"
        "F000         - Read memory.\n"
        "F000: 01 02  - Write memory. Colon optional.\n");
}

struct
{
    size_t cmd_len;
    const char *const cmd;
    void (*func)(const uint8_t *, size_t);
} const COMMANDS[] = {
    {2, "ls", cmd_ls},
    {2, "cd", cmd_cd},
    {5, "speed", cmd_speed},
    {5, "reset", cmd_reset},
    {4, "caps", cmd_caps},
    {3, "jmp", cmd_jmp},
    {5, "start", cmd_start},
    {6, "status", cmd_status},
    {4, "help", cmd_help},
    {1, "h", cmd_help},
    {1, "?", cmd_help},
};
const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

void cmd_dispatch(const uint8_t *buf, uint8_t buflen)
{
    // find the cmd and args
    size_t i;
    for (i = 0; i < buflen; i++)
    {
        if (buf[i] != ' ')
            break;
    }
    const uint8_t *cmd = buf + i;
    uint32_t addr = 0;
    bool is_maybe_addr = false;
    bool is_not_addr = false;
    for (; i < buflen; i++)
    {
        uint8_t ch = buf[i];
        if (is_hex(ch))
        {
            is_maybe_addr = true;
            addr = addr * 16 + to_int(ch);
        }
        else if (is_maybe_addr && !is_not_addr && ch == ':')
        {
            // optional colon "0000: 00 00"
            i++;
            break;
        }
        else if (ch == ' ')
            break;
        else
            is_not_addr = true;
    }
    size_t cmd_len = buf + i - cmd;
    for (; i < buflen; i++)
    {
        if (buf[i] != ' ')
            break;
    }
    const char *args = buf + i;
    size_t args_len = buflen - i;

    // cd for chdir, cd: for r/w address
    if (cmd_len == 2 && addr == 0xCD)
        is_not_addr = true;

    // address command
    if (is_maybe_addr && !is_not_addr)
        return cmd_address(addr, args, args_len);

    for (size_t i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func(args, args_len);
    }
    if (cmd_len)
        printf("?unknown command\n");
}
