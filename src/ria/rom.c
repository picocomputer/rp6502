/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rom.h"
#include "str.h"
#include "mem/mbuf.h"
#include "ria/ria.h"
#include "ria/act.h"
#include "fatfs/ff.h"
#include <stdio.h>
#include <string.h>

static enum {
    ROM_IDLE,
    ROM_LOADING,
    ROM_RIA_WRITING,
    ROM_RIA_VERIFYING,
} rom_state;
static FIL fat_fil;

static uint32_t rom_addr;
// static uint32_t rom_len;
// static uint32_t rom_crc;

static void rom_binary(const char *args, size_t len)
{
    uint32_t rom_len;
    uint32_t rom_crc;
    if (parse_uint32(&args, &len, &rom_addr) &&
        parse_uint32(&args, &len, &rom_len) &&
        parse_uint32(&args, &len, &rom_crc) &&
        parse_end(args, len))
    {
        if (rom_addr > 0xFFFF)
        {
            printf("?invalid address\n");
            rom_reset();
            return;
        }
        if (!rom_len || rom_len > MBUF_SIZE || rom_addr + rom_len > 0x10000)
        {
            printf("?invalid length\n");
            rom_reset();
            return;
        }

        FRESULT result = f_read(&fat_fil, mbuf, rom_len, &mbuf_len);
        if (result != FR_OK)
        {
            printf("?Unable to read file (%d)\n", result);
            rom_reset();
            return;
        }
        if (rom_len != mbuf_len)
        {
            printf("?Unable to read binary data\n");
            rom_reset();
            return;
        }
        if (mbuf_crc32() != rom_crc)
        {
            printf("?CRC failed\n");
            rom_reset();
            return;
        }

        printf("load %04lX\n", rom_addr);

        rom_state = ROM_RIA_WRITING;
        act_ram_write(rom_addr);
        return;
    }
    printf("?invalid argument in binary command\n");
    rom_reset();
}

static struct
{
    size_t cmd_len;
    const char *const cmd;
    void (*func)(const char *, size_t);
} const COMMANDS[] = {
    {6, "BINARY", rom_binary},
};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

static void rom_loading()
{
    if (!f_gets((char *)mbuf, MBUF_SIZE, &fat_fil))
    {
        if (f_error(&fat_fil))
            printf("?Unknown FatFs error.\n");
        rom_reset();
        return;
    }

    size_t buflen = strlen((char *)mbuf);
    if (mbuf[buflen - 1] == '\n')
        buflen--;
    if (mbuf[buflen - 1] == '\r')
        buflen--;
    size_t i;
    for (i = 0; i < buflen; i++)
    {
        if (mbuf[i] != ' ')
            break;
    }
    const char *cmd = (char *)mbuf + i;
    for (; i < buflen; i++)
    {
        uint8_t ch = mbuf[i];
        if (ch == ' ')
            break;
    }
    size_t cmd_len = (char *)mbuf + i - cmd;
    for (; i < buflen; i++)
    {
        if (mbuf[i] != ' ')
            break;
    }
    const char *args = (char *)mbuf + i;
    size_t args_len = buflen - i;

    for (size_t i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func(args, args_len);
    }

    rom_reset();
    printf("?unknown command in rom file\n");
}

void rom_load(const char *args, size_t len)
{
    (void)(len);
    FRESULT result = f_open(&fat_fil, args, FA_READ | FA_WRITE);
    if (result != FR_OK)
    {
        printf("?Unable to open file (%d)\n", result);
        return;
    }
    rom_state = ROM_LOADING;
}

static bool rom_action_is_finished()
{
    if (ria_is_active())
        return false;
    int32_t result = act_result();
    switch (result)
    {
    case -1:
        return true;
        break;
    case -2:
        rom_reset();
        printf("?action watchdog timeout\n");
        break;
    default:
        rom_reset();
        printf("?verify error at $%04lX\n", result);
        break;
    }
    return false;
}

void rom_task()
{
    switch (rom_state)
    {
    case ROM_IDLE:
        return;
    case ROM_LOADING:
        rom_loading();
        break;
    case ROM_RIA_WRITING:
        if (rom_action_is_finished())
        {
            rom_state = ROM_RIA_VERIFYING;
            act_ram_verify(rom_addr);
        }
        break;
    case ROM_RIA_VERIFYING:
        if (rom_action_is_finished())
            rom_state = ROM_LOADING;
        break;
    }
}

bool rom_is_active()
{
    return rom_state != ROM_IDLE;
}

void rom_reset()
{
    if (rom_state != ROM_IDLE)
    {
        rom_state = ROM_IDLE;
        FRESULT result = f_close(&fat_fil);
        if (result != FR_OK)
            printf("?Unable to close file (%d)\n", result);
    }
}
