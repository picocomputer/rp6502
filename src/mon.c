/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "mon.h"
#include "ansi.h"
#include "ria.h"
#include "basic.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#define MON_BUF_SIZE 80
#define MON_RW_SIZE 1024
static uint8_t mon_buf[MON_BUF_SIZE];
static uint8_t mon_buflen = 0;
static uint8_t mon_bufpos = 0;
static ansi_state_t mon_ansi_state = ansi_state_C0;
static int mon_ansi_param;
volatile enum state {
    idle,
    read,
    write,
    verify,
    basic_load,
    basic_verify
} mon_state = idle;
static uint8_t mon_readwrite[MON_RW_SIZE];
static uint8_t mon_verify[MON_RW_SIZE];
uint32_t mon_rw_addr;
size_t mon_rw_len;

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
        mon_buflen = mon_bufpos = 0;
        return;
    }
    if (!len)
    {
        mon_rw_addr = addr;
        mon_rw_len = (addr | 0xF) - addr + 1;
        mon_state = read;
        ria_ram_read(mon_rw_addr, mon_readwrite, mon_rw_len);
        return;
    }
    uint32_t data = 0x80000000;
    mon_rw_len = 0;
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
                mon_readwrite[mon_rw_len++] = data;
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
    mon_rw_addr = addr;
    mon_state = write;
    ria_ram_write(mon_rw_addr, mon_readwrite, mon_rw_len);
    return;
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
        if (i > 8000 || !ria_set_phi2_khz(i))
        {
            printf("?invalid frequency\n");
            return;
        }
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
    ria_jmp(addr);
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

static void cmd_basic(const uint8_t *args, size_t len)
{
    // assert allows simpler code, this load code will be extended later
    assert(!(BASIC_ROM_SIZE % MON_RW_SIZE));
    mon_state = basic_load;
    mon_rw_addr = BASIC_ROM_START;
    mon_rw_len = MON_RW_SIZE;
    uint8_t *rompos = &basicrom[mon_rw_addr - BASIC_ROM_START];
    for (size_t i = 0; i < MON_RW_SIZE; i++)
        mon_readwrite[i] = rompos[i];
    ria_ram_write(mon_rw_addr, mon_readwrite, MON_RW_SIZE);
}

static void cmd_help(const uint8_t *args, size_t len)
{
    printf(
        "Commands:\n"
        "HELP         - This help.\n"
        "BASIC        - Start BASIC programming language.\n"
        "STATUS       - Show all settings.\n"
        "CAPS (0|1|2) - Invert or force caps while 6502 is running.\n"
        "SPEED (kHz)  - Query or set PHI2 speed. This is the 6502 clock.\n"
        "RESET (ms)   - Query or set RESB hold time. Set to 0 for auto.\n"
        "JMP address  - Start the 6502. Begin execution at address.\n"
        "F000         - Read memory.\n"
        "F000: 01 02  - Write memory. Colon optional.\n");
}

struct
{
    size_t cmd_len;
    const char *const cmd;
    void (*func)(const uint8_t *, size_t);
} const COMMANDS[] = {
    {5, "basic", cmd_basic},
    {5, "speed", cmd_speed},
    {5, "reset", cmd_reset},
    {4, "caps", cmd_caps},
    {3, "jmp", cmd_jmp},
    {6, "status", cmd_status},
    {4, "help", cmd_help},
    {1, "h", cmd_help},
    {1, "?", cmd_help},
};
const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

static void mon_enter()
{
    // find the cmd and args
    size_t i;
    for (i = 0; i < mon_buflen; i++)
    {
        if (mon_buf[i] != ' ')
            break;
    }
    uint8_t *cmd = mon_buf + i;
    uint32_t addr = 0;
    bool is_maybe_addr = false;
    bool is_not_addr = false;
    for (; i < mon_buflen; i++)
    {
        uint8_t ch = mon_buf[i];
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
    size_t cmd_len = mon_buf + i - cmd;
    for (; i < mon_buflen; i++)
    {
        if (mon_buf[i] != ' ')
            break;
    }
    char *args = mon_buf + i;
    size_t args_len = mon_buflen - i;

    // dispatch command
    if (is_maybe_addr && !is_not_addr)
        return cmd_address(addr, args, args_len);
    for (i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func(args, args_len);
    }
    if (cmd_len)
        printf("?unknown command\n");
}

static void mon_forward(int count)
{
    if (count > mon_buflen - mon_bufpos)
        count = mon_buflen - mon_bufpos;
    if (!count)
        return;
    mon_bufpos += count;
    // clang-format off
    printf(ANSI_FORWARD(%d), count);
    // clang-format on
}

static void mon_backward(int count)
{
    if (count > mon_bufpos)
        count = mon_bufpos;
    if (!count)
        return;
    mon_bufpos -= count;
    // clang-format off
    printf(ANSI_BACKWARD(%d), count);
    // clang-format on
}

static void mon_delete()
{
    if (!mon_buflen || mon_bufpos == mon_buflen)
        return;
    printf(ANSI_DELETE(1));
    mon_buflen--;
    for (uint8_t i = mon_bufpos; i < mon_buflen; i++)
        mon_buf[i] = mon_buf[i + 1];
}

static void mon_backspace()
{
    if (!mon_bufpos)
        return;
    printf("\b" ANSI_DELETE(1));
    mon_buflen--;
    for (uint8_t i = --mon_bufpos; i < mon_buflen; i++)
        mon_buf[i] = mon_buf[i + 1];
}

static void mon_state_C0(char ch)
{
    if (ch == '\33')
        mon_ansi_state = ansi_state_Fe;
    else if (ch == '\b' || ch == 127)
        mon_backspace();
    else if (ch == '\r')
    {
        printf("\n");
        mon_enter();
        mon_buflen = mon_bufpos = 0;
    }
    else if (ch >= 32 && ch < 127 && mon_bufpos < MON_BUF_SIZE - 1)
    {
        putchar(ch);
        mon_buf[mon_bufpos] = ch;
        if (++mon_bufpos > mon_buflen)
            mon_buflen = mon_bufpos;
    }
}

static void mon_state_Fe(char ch)
{
    if (ch == '[')
    {
        mon_ansi_state = ansi_state_CSI;
        mon_ansi_param = -1;
    }
    else if (ch == 'O')
    {
        mon_ansi_state = ansi_state_SS3;
    }
    else
    {
        mon_ansi_state = ansi_state_C0;
        mon_delete();
    }
}

static void mon_state_CSI(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        if (mon_ansi_param < 0)
        {
            mon_ansi_param = ch - '0';
        }
        else
        {
            mon_ansi_param *= 10;
            mon_ansi_param += ch - '0';
        }
        return;
    }
    if (ch == ';')
        return;
    mon_ansi_state = ansi_state_C0;
    if (mon_ansi_param < 0)
        mon_ansi_param = -mon_ansi_param;
    if (ch == 'C')
        mon_forward(mon_ansi_param);
    else if (ch == 'D')
        mon_backward(mon_ansi_param);
    else if (ch == '~' && mon_ansi_param == 3)
        mon_delete();
}

void mon_task()
{
    if (ria_is_active())
        return;
    if (mon_state == idle)
    {
        int ch = getchar_timeout_us(0);
        if (ch == ANSI_CANCEL)
            mon_ansi_state = ansi_state_C0;
        else if (ch != PICO_ERROR_TIMEOUT)
            switch (mon_ansi_state)
            {
            case ansi_state_C0:
                mon_state_C0(ch);
                break;
            case ansi_state_Fe:
                mon_state_Fe(ch);
                break;
            case ansi_state_SS3:
                // all SS3 is nop
                mon_ansi_state = ansi_state_C0;
                break;
            case ansi_state_CSI:
                mon_state_CSI(ch);
                break;
            }
        return;
    }
    else if (mon_state == read)
    {
        mon_state = idle;
        printf("%04X:", mon_rw_addr);
        for (size_t i = 0; i < mon_rw_len; i++)
        {
            printf(" %02X", mon_readwrite[i]);
        }
        printf("\n");
    }
    else if (mon_state == write)
    {
        mon_state = verify;
        ria_ram_read(mon_rw_addr, mon_verify, mon_rw_len);
    }
    else if (mon_state == verify)
    {
        mon_state = idle;
        for (size_t i = 0; i < mon_rw_len; i++)
        {
            if (mon_rw_addr + i < 0xFF00 || mon_rw_addr + i >= 0xFFFA)
            {
                if (mon_readwrite[i] != mon_verify[i])
                {
                    printf("%04X:", mon_rw_addr);
                    for (size_t i = 0; i < mon_rw_len; i++)
                    {
                        printf(" %02X", mon_verify[i]);
                    }
                    printf(" ERROR ERROR ERROR\n");
                    break;
                }
            }
        }
    }
    else if (mon_state == basic_load)
    {
        mon_state = basic_verify;
        ria_ram_read(mon_rw_addr, mon_verify, mon_rw_len);
    }
    else if (mon_state == basic_verify)
    {
        mon_state = idle;
        for (size_t i = 0; i < mon_rw_len; i++)
        {
            if (mon_rw_addr + i < 0xFF00)
            {
                if (mon_readwrite[i] != mon_verify[i])
                {
                    printf("Load error at %04X\n", mon_rw_addr + i);
                    return;
                }
            }
        }

        mon_rw_addr += MON_RW_SIZE;
        if (mon_rw_addr >= BASIC_ROM_START + BASIC_ROM_SIZE)
        {
            ria_jmp(BASIC_ROM_JMP);
            return;
        }
        mon_rw_len = MON_RW_SIZE;
        mon_state = basic_load;
        uint8_t *rompos = &basicrom[mon_rw_addr - BASIC_ROM_START];
        for (size_t i = 0; i < mon_rw_len; i++)
            mon_readwrite[i] = rompos[i];
        ria_ram_write(mon_rw_addr, mon_readwrite, mon_rw_len);
    }
}

// User requested stop by UART break or CTRL-ALT-DEL
void mon_halt()
{
    ria_halt();
    mon_state == idle;
    mon_ansi_state = ansi_state_C0;
    mon_buflen = 0;
    mon_bufpos = 0;
    puts("\30\33[0m\n" RP6502_NAME);
}
