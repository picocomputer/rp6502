/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rom.h"
#include "str.h"
#include "mem/mbuf.h"
#include "ria.h"
#include "act.h"
#include "cmd.h"
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
static bool rom_FFFC;
static bool rom_FFFD;

static size_t rom_gets()
{
    if (!f_gets((char *)mbuf, MBUF_SIZE, &fat_fil))
        return 0;
    size_t len = strlen((char *)mbuf);
    if (mbuf[len - 1] == '\n')
        len--;
    if (mbuf[len - 1] == '\r')
        len--;
    return len;
}

static bool rom_open(const char *name)
{
    FRESULT result = f_open(&fat_fil, name, FA_READ | FA_WRITE);
    if (result != FR_OK)
    {
        printf("?Unable to open file (%d)\n", result);
        return false;
    }
    if (rom_gets() != 6 || strnicmp("RP6502", (char *)mbuf, 6))
    {
        printf("?Missing RP6502 ROM header\n");
        rom_reset();
        return false;
    }
    rom_FFFC = false;
    rom_FFFD = false;
    return true;
}

static bool rom_eof()
{
    return !!f_eof(&fat_fil);
}

static bool rom_read(uint32_t len, uint32_t crc)
{

    FRESULT result = f_read(&fat_fil, mbuf, len, &mbuf_len);
    if (result != FR_OK)
    {
        printf("?Unable to read file (%d)\n", result);
        return false;
    }
    if (len != mbuf_len)
    {
        printf("?Unable to read binary data\n");
        return false;
    }
    if (mbuf_crc32() != crc)
    {
        printf("?CRC failed\n");
        return false;
    }
    // printf("load %04lX %d %d\n", rom_addr, rom_FFFC, rom_FFFD); // TODO remove
    return true;
}

static bool rom_next_chunk()
{
    size_t len = rom_gets();
    uint32_t rom_len;
    uint32_t rom_crc;
    const char *args = (char *)mbuf;
    if (parse_uint32(&args, &len, &rom_addr) &&
        parse_uint32(&args, &len, &rom_len) &&
        parse_uint32(&args, &len, &rom_crc) &&
        parse_end(args, len))
    {
        if (rom_addr > 0xFFFF)
        {
            printf("?invalid address\n");
            return false;
        }
        if (!rom_len || rom_len > MBUF_SIZE || rom_addr + rom_len > 0x10000)
        {
            printf("?invalid length\n");
            return false;
        }
        if (rom_addr <= 0xFFFC && rom_addr + rom_len > 0xFFFC)
            rom_FFFC = true;
        if (rom_addr <= 0xFFFD && rom_addr + rom_len > 0xFFFD)
            rom_FFFD = true;
        return rom_read(rom_len, rom_crc);
    }
    printf("?Corrupt ROM file\n");
    return false;
}

static void rom_loading()
{
    if (rom_eof())
    {
        rom_reset();
        if (rom_FFFC && rom_FFFD)
            ria_reset();
        else
            printf("Loaded. No reset vector.\n");
        return;
    }
    if (!rom_next_chunk())
    {
        rom_reset();
        return;
    }
    rom_state = ROM_RIA_WRITING;
    act_ram_write(rom_addr);
}

void rom_install(const char *args, size_t len)
{
    if (cmd_exists(args, len))
    {
        printf("?Invalid ROM name.\n");
        return;
    }
    if (!rom_open(args))
        return;
    while (!rom_eof())
        if (!rom_next_chunk())
        {
            rom_reset();
            return;
        }
    printf("Passed.\n"); // TODO
    rom_reset(); //TODO
}

void rom_load(const char *args, size_t len)
{
    (void)(len);
    if (rom_open(args))
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
        printf("?action watchdog timeout\n");
        break;
    default:
        printf("?verify error at $%04lX\n", result);
        break;
    }
    rom_reset();
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
