/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cmd.h"
#include "mon.h"
#include "dev/msc.h"
#include "mem/mbuf.h"
#include "ria.h"
#include "rom.h"
#include "str.h"
#include "act.h"
#include "dev/dev.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "fatfs/ff.h"

#define MON_BINARY_TIMEOUT_MS 200
static uint32_t rw_addr;
static uint32_t rw_len;
static uint32_t rw_crc;
static absolute_time_t binary_timer;
static void (*binary_cb)(void) = 0;
static void (*action_cb)(int32_t) = 0;
static FIL fat_fil;
static bool is_upload_mode = false;

// TODO make friendly error messages for filesystem errors

static void cmd_action_error_callback(int32_t result)
{
    switch (result)
    {
    case -1:
        // OK
        break;
    case -2:
        printf("?action watchdog timeout\n");
        break;
    default: // TODO can this happen?
        printf("?undefined action error at $%04lX\n", result);
        break;
    }
}

void cmd_read_cb(int32_t result)
{
    if (result != -1)
        return cmd_action_error_callback(result);
    printf("%04lX", rw_addr);
    for (size_t i = 0; i < mbuf_len; i++)
        printf(" %02X", mbuf[i]);
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
    act_ram_verify(rw_addr);
}

// Commands that start with a hex address. Read or write memory.
static void cmd_address(const char *args, size_t len)
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
        action_cb = cmd_read_cb;
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
    if (mbuf_crc32() == rw_crc)
    {
        act_ram_write(rw_addr);
        action_cb = cmd_write_cb;
    }
    else
        puts("?CRC does not match");
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
        if (!rw_len || rw_len > MBUF_SIZE || rw_addr + rw_len > 0x10000)
        {
            printf("?invalid length\n");
            return;
        }
        mbuf_len = 0;
        binary_cb = cmd_binary_callback;
        binary_timer = delayed_by_us(get_absolute_time(),
                                     MON_BINARY_TIMEOUT_MS * 1000);

        return;
    }
    printf("?invalid argument\n");
}

static void cmd_upload_callback()
{
    FRESULT result = FR_OK;

    if (mbuf_crc32() != rw_crc)
    {
        result = FR_INT_ERR; // any error to abort
        puts("?CRC does not match");
    }

    // This will let us leave the file unchanged until
    // the first chunk is received successfully.
    if (result == FR_OK && f_tell(&fat_fil) == 0)
    {
        result = f_truncate(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to truncate file (%d)\n", result);
    }

    if (result == FR_OK)
    {
        UINT bytes_written;
        result = f_write(&fat_fil, mbuf, mbuf_len, &bytes_written);
        if (result != FR_OK)
            printf("?Unable to write file (%d)\n", result);
    }

    if (result != FR_OK)
    {
        is_upload_mode = false;
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
}

static void cmd_upload(const char *args, size_t len)
{
    if (is_upload_mode)
    {
        if (len == 0 || (len == 3 && !strnicmp("END", args, 3)))
        {
            is_upload_mode = false;
            FRESULT result = f_close(&fat_fil);
            if (result != FR_OK)
                printf("?Unable to close file (%d)\n", result);
            return;
        }

        if (parse_uint32(&args, &len, &rw_len) &&
            parse_uint32(&args, &len, &rw_crc) &&
            parse_end(args, len))
        {
            if (!rw_len || rw_len > MBUF_SIZE)
            {
                printf("?invalid length\n");
                return;
            }
            mbuf_len = 0;
            binary_cb = cmd_upload_callback;
            binary_timer = delayed_by_us(get_absolute_time(),
                                         MON_BINARY_TIMEOUT_MS * 1000);

            return;
        }
        printf("?invalid argument\n");
        return;
    }

    if (len == 0)
    {
        printf("?missing filename\n");
        return;
    }

    FRESULT result = f_open(&fat_fil, args, FA_READ | FA_WRITE);
    if (result == FR_NO_FILE)
        result = f_open(&fat_fil, args, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK)
    {
        printf("?Unable to open file (%d)\n", result);
        return;
    }
    is_upload_mode = true;
    binary_timer = delayed_by_us(get_absolute_time(),
                                 MON_BINARY_TIMEOUT_MS * 1000);
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
        "CAPS (0|1|2)        - Invert or force caps while 6502 is running.\n"
        "PHI2 (kHz)          - Query or set PHI2 speed. This is the 6502 clock.\n"
        "RESB (ms)           - Query or set RESB hold time. Set to 0 for auto.\n"
        "LS (DIR|DRIVE)      - List contents of directory.\n"
        "CD (DIR|DRIVE)      - Change current directory.\n"
        "LOAD file           - Load ROM file. Start if contains reset vector.\n"
        "INSTALL file        - Install ROM file on RIA.\n"
        "REMOVE rom          - Remove ROM from RIA.\n"
        "BOOT rom            - Select ROM to boot from cold start.\n"
        "REBOOT              - Load and start selected boot ROM.\n"
        "rom                 - Load and start an installed ROM.\n"
        "UPLOAD file         - Write file. Binary chunks follow.\n"
        "RESET               - Start 6502 at current reset vector ($FFFC).\n"
        "BINARY addr len crc - Write memory. Binary data follows.\n"
        "F000 01 02 ...      - Write memory.\n"
        "F000                - Read memory.";
    puts(cmdhelp);
}

typedef void (*cmd_function)(const char *, size_t);
static struct
{
    size_t cmd_len;
    const char *const cmd;
    cmd_function func;
} const COMMANDS[] = {
    {4, "help", cmd_help},
    {1, "h", cmd_help},
    {1, "?", cmd_help},
    {6, "status", cmd_status},
    {4, "caps", cmd_caps},
    {4, "phi2", cmd_phi2},
    {4, "resb", cmd_resb},
    {2, "ls", cmd_ls},
    {2, "cd", cmd_cd},
    {4, "load", rom_load},
    // {7, "install", rom_install},
    // {6, "remove", rom_remove},
    // {4, "boot", rom_boot},
    // {6, "reboot", rom_reboot},
    {5, "reset", cmd_start},
    {6, "upload", cmd_upload},
    {6, "binary", cmd_binary},
};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

// Returns 0 if not found. Advances buf to start of args.
static cmd_function cmd_lookup(const char **buf, uint8_t buflen)
{
    size_t i;
    for (i = 0; i < buflen; i++)
    {
        if ((*buf)[i] != ' ')
            break;
    }
    const char *cmd = (*buf) + i;

    bool is_maybe_addr = false;
    bool is_not_addr = false;
    for (; i < buflen; i++)
    {
        uint8_t ch = (*buf)[i];
        if (char_is_hex(ch))
            is_maybe_addr = true;
        else if (ch == ' ')
            break;
        else
            is_not_addr = true;
    }
    size_t cmd_len = (*buf) + i - cmd;
    for (; i < buflen; i++)
    {
        if ((*buf)[i] != ' ')
            break;
    }

    // cd for chdir, 00cd for r/w address
    if (cmd_len == 2 && !strnicmp(cmd, "cd", cmd_len))
        is_not_addr = true;

    // address command
    if (is_maybe_addr && !is_not_addr)
    {
        *buf = cmd;
        return cmd_address;
    }

    *buf += i;
    for (i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func;
    }
    return 0;
}

void cmd_dispatch(const char *buf, uint8_t buflen)
{
    if (is_upload_mode)
        return cmd_upload(buf, buflen);

    const char *args = buf;
    cmd_function func = cmd_lookup(&args, buflen);

    if (!func)
    {
        for (; buf < args; buf++)
            if (buf[0] != ' ')
            {
                printf("?unknown command\n");
                break;
            }
        return;
    }

    size_t args_len = buflen - (args - buf);
    func(args, args_len);
}

void cmd_task()
{
    if (ria_is_active())
        return;

    if (action_cb)
    {
        void (*current_cb)(int32_t) = action_cb;
        action_cb = 0;
        current_cb(act_result());
    }

    if (binary_cb)
    {
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT)
        {
            while (ch != PICO_ERROR_TIMEOUT)
            {
                mbuf[mbuf_len++] = ch;
                if (mbuf_len >= rw_len)
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
    }

    if (binary_cb || is_upload_mode)
    {
        {
            absolute_time_t now = get_absolute_time();
            if (absolute_time_diff_us(now, binary_timer) < 0)
            {
                if (!binary_cb)
                    puts("");
                cmd_reset();
                mon_reset();
                printf("?timeout\n");
            }
        }
    }
}

char cmd_prompt()
{
    return is_upload_mode ? '}' : ']';
}

bool cmd_is_active()
{
    return action_cb != 0 || binary_cb != 0;
}

void cmd_reset()
{
    if (is_upload_mode)
    {
        is_upload_mode = false;
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
    action_cb = 0;
    binary_cb = 0;
}
