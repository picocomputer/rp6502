/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmd.h"
#include "msc.h"
#include "ria.h"
#include "ria_action.h"
#include "usb/dev.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#define MON_RW_SIZE 1024
#define MON_BINARY_TIMEOUT_MS 200
static uint8_t rw_buf[MON_RW_SIZE];
static uint32_t rw_buf_len;
static uint32_t rw_addr;
static uint32_t rw_len;
static uint32_t rw_crc;
static absolute_time_t binary_timer;
static void (*binary_cb)(void) = 0;
static void (*action_cb)(int32_t) = 0;

static bool is_hex(char ch)
{
    return ((ch >= '0') && (ch <= '9')) ||
           ((ch >= 'A') && (ch <= 'F')) ||
           ((ch >= 'a') && (ch <= 'f'));
}

static uint32_t to_int(char ch)
{
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    if (ch - 'A' < 6)
        return ch - 'A' + 10;
    if (ch - 'a' < 6)
        return ch - 'a' + 10;
    return 0xFF;
}

// A single argument in hex or decimal. e.g. 0x0, $0, 0
static bool parse_end(const char *args, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (args[i] != ' ')
            return false;
    }
    return true;
}

// A single argument in hex or decimal. e.g. 0x0, $0, 0
static bool parse_uint32(const char **args, size_t *len, uint32_t *result)
{
    size_t i;
    for (i = 0; i < *len; i++)
    {
        if ((*args)[i] != ' ')
            break;
    }
    uint32_t base = 10;
    uint32_t value = 0;
    uint32_t prefix = 0;
    if (i < (*len) && (*args)[i] == '$')
    {
        base = 16;
        prefix = 1;
    }
    else if (i + 1 < *len && (*args)[i] == '0' &&
             ((*args)[i + 1] == 'x' || (*args)[i + 1] == 'X'))
    {
        base = 16;
        prefix = 2;
    }
    i = prefix;
    if (i == *len)
        return false;
    for (; i < *len; i++)
    {
        char ch = (*args)[i];
        if (!is_hex(ch))
            break;
        uint32_t i = to_int(ch);
        if (i >= base)
            return false;
        value = value * base + i;
    }
    if (i == prefix)
        return false;
    for (; i < *len; i++)
        if ((*args)[i] != ' ')
            break;
    *len -= i;
    *args += i;
    *result = value;
    return true;
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

void cmd_action_error_callback(int32_t result)
{
    switch (result)
    {
    case -1:
        // OK
        break;
    case -2:
        printf("?action watchdog timeout\n");
        break;
    default:
        printf("?undefined action error at $%04lX\n", result);
        break;
    }
}

void cmd_read_cb(int32_t result)
{
    if (result != -1)
        return cmd_action_error_callback(result);
    printf("%04lX:", rw_addr);
    for (size_t i = 0; i < rw_len; i++)
        printf(" %02X", rw_buf[i]);
    printf("\n");
}

void cmd_verify_cb(int32_t result)
{
    if (result < 0)
        return cmd_action_error_callback(result);
    printf("?verify failed at $%04lX\n", result);
}

void cmd_write_cb(int32_t result)
{
    if (result != -1)
        return cmd_action_error_callback(result);
    action_cb = cmd_verify_cb;
    ria_action_ram_verify(rw_addr, rw_buf, rw_len);
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
    rw_addr = addr;
    if (!len)
    {
        rw_len = (addr | 0xF) - addr + 1;
        ria_action_ram_read(addr, rw_buf, rw_len);
        action_cb = cmd_read_cb;
        return;
    }
    uint32_t data = 0x80000000;
    rw_len = 0;
    for (size_t i = 0; i < len; i++)
    {
        char ch = args[i];
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
    ria_action_ram_write(addr, rw_buf, rw_len);
    action_cb = cmd_write_cb;
}

static void status_phi2()
{
    printf("PHI2: %ld kHz\n", ria_get_phi2_khz());
}

static void cmd_phi2(const char *args, size_t len)
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

static void cmd_resb(const char *args, size_t len)
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

static void cmd_start(const char *args, size_t len)
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

static void cmd_caps(const char *args, size_t len)
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

static void cmd_status(const char *args, size_t len)
{
    (void)(args);
    (void)(len);

    status_phi2();
    status_resb();
    printf("RIA : %.1f MHz\n", clock_get_hz(clk_sys) / 1000 / 1000.f);
    status_caps();
    dev_print_all();
}

static void cmd_binary_callback()
{
    // TODO check crc
    ria_action_ram_write(rw_addr, rw_buf, rw_len);
    action_cb = cmd_write_cb;
}

static void cmd_binary(const char *args, size_t len)
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
        if (rw_len > 1024 || rw_addr + rw_len > 0x10000)
        {
            printf("?invalid length\n");
            return;
        }
        rw_buf_len = 0;
        binary_cb = cmd_binary_callback;
        binary_timer = delayed_by_us(get_absolute_time(),
                                     MON_BINARY_TIMEOUT_MS * 1000);

        return;
    }
    printf("?invalid argument\n");
}

static void cmd_ls(const char *args, size_t len)
{
    (void)(len);
    msc_ls(args);
}

static void cmd_cd(const char *args, size_t len)
{
    (void)(len);
    msc_cd(args);
}

static void cmd_help(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    static const char *__in_flash("cmdhelp") cmdhelp =
        "Commands:\n"
        "HELP (COMMAND)      - This help or expanded help for command.\n"
        "STATUS              - Show all settings and USB devices.\n"
        "LS                  - List contents of current directory.\n"
        "CD (DIR|DRIVE)      - Change to directory or drive.\n"
        "CAPS (0|1|2)        - Invert or force caps while 6502 is running.\n"
        "PHI2 (kHz)          - Query or set PHI2 speed. This is the 6502 clock.\n"
        "RESB (ms)           - Query or set RESB hold time. Set to 0 for auto.\n"
        "LOAD file           - Load file.\n"
        "RUN (file)          - Optionally load file. Begin execution at ($FFFC).\n"
        "INSTALL file        - Install file on RIA to be used as a ROM.\n"
        "BOOT (rom)          - Select ROM to run at boot or run current ROM.\n"
        "REMOVE rom          - Remove ROM from RIA.\n"
        "UPLOAD file len crc - Write file.\n"
        "BINARY addr len crc - Write memory.\n"
        "F000                - Read memory.\n"
        "F000: 01 02         - Write memory. Colon optional.";
    puts(cmdhelp);
}

struct
{
    size_t cmd_len;
    const char *const cmd;
    void (*func)(const char *, size_t);
} const COMMANDS[] = {
    {2, "ls", cmd_ls},
    {2, "cd", cmd_cd},
    {4, "phi2", cmd_phi2},
    {4, "resb", cmd_resb},
    {4, "caps", cmd_caps},
    {5, "start", cmd_start},
    {6, "status", cmd_status},
    {6, "binary", cmd_binary},
    {4, "help", cmd_help},
    {1, "h", cmd_help},
    {1, "?", cmd_help},
};
const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

void cmd_dispatch(const char *buf, uint8_t buflen)
{
    // find the cmd and args
    size_t i;
    for (i = 0; i < buflen; i++)
    {
        if (buf[i] != ' ')
            break;
    }
    const char *cmd = buf + i;
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

    // cd for chdir, 0cd or cd: for r/w address
    if (cmd_len == 2 && addr == 0xCD)
        is_not_addr = true;

    // address command
    if (is_maybe_addr && !is_not_addr)
    {
        cmd_address(addr, args, args_len);
        return;
    }

    for (size_t i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func(args, args_len);
    }
    if (cmd_len)
        printf("?unknown command\n");
}

void cmd_task()
{
    if (ria_is_active())
        return;

    if (action_cb)
    {
        void (*current_cb)(int32_t) = action_cb;
        action_cb = 0;
        current_cb(ria_action_result());
    }

    if (binary_cb)
    {
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT)
        {
            while (ch != PICO_ERROR_TIMEOUT)
            {
                rw_buf[rw_buf_len++] = ch;
                if (rw_buf_len >= rw_len)
                {
                    void (*current_cb)(void) = binary_cb;
                    binary_cb = 0;
                    current_cb();
                    return;
                }
                ch = getchar_timeout_us(0);
            }
            binary_timer = delayed_by_us(get_absolute_time(),
                                         MON_BINARY_TIMEOUT_MS * 1000);
        }
        else
        {
            absolute_time_t now = get_absolute_time();
            if (absolute_time_diff_us(now, binary_timer) < 0)
            {
                printf("?timeout\n");
                binary_cb = 0;
            }
        }
    }
}

bool cmd_is_active()
{
    return action_cb != 0 || binary_cb != 0;
}

void cmd_reset()
{
    action_cb = 0;
}
