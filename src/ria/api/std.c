/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/std.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "fatfs/ff.h"
#include <stdio.h>

#define STD_FIL_MAX 16
FIL std_fil[STD_FIL_MAX];
#define STD_FIL_STDIN 0
#define STD_FIL_STDOUT 1
#define STD_FIL_STDERR 2
#define STD_FIL_OFFS 3
static_assert(STD_FIL_MAX + STD_FIL_OFFS < 128);

static int32_t std_count = -1;

static volatile size_t std_out_tail;
static volatile size_t std_out_head;
static volatile uint8_t std_out_buf[32];
#define STD_OUT_BUF(pos) std_out_buf[(pos) & 0x1F]

static inline bool com_stdout_writable()
{
    return (((std_out_head + 1) & 0x1F) != (std_out_tail & 0x1F));
}

static inline void com_stdout_write(char ch)
{
    STD_OUT_BUF(++std_out_head) = ch;
}

void std_task(void)
{
    // 6502 applications write to std_out_buf which we route
    // through the Pi Pico STDIO driver for CR/LR translation.
    if ((&STD_OUT_BUF(std_out_head) != &STD_OUT_BUF(std_out_tail)) &&
        (&COM_TX_BUF(com_tx_head + 1) != &COM_TX_BUF(com_tx_tail)) &&
        (&COM_TX_BUF(com_tx_head + 2) != &COM_TX_BUF(com_tx_tail)))
        putchar(STD_OUT_BUF(++std_out_tail));
}

void std_api_open(void)
{
    // These match CC65 which is closer to POSIX than FatFs.
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    uint8_t flags = API_A;
    uint8_t mode = flags & RDWR; // RDWR are same bits
    if (flags & CREAT)
    {
        if (flags & EXCL)
            mode |= FA_CREATE_NEW;
        else
        {
            if (flags & TRUNC)
                mode |= FA_CREATE_ALWAYS;
            else if (flags & APPEND)
                mode |= FA_OPEN_APPEND;
            else
                mode |= FA_OPEN_ALWAYS;
        }
    }
    uint8_t *path = &xstack[xstack_ptr];
    api_zxstack();
    int fd = 0;
    for (; fd < STD_FIL_MAX; fd++)
        if (!std_fil[fd].obj.fs)
            break;
    if (fd == STD_FIL_MAX)
        return api_return_errno(API_EMFILE);
    FIL *fp = &std_fil[fd];
    FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(fd + STD_FIL_OFFS);
}

void std_api_close(void)
{
    int fd = API_A;
    if (fd < STD_FIL_OFFS || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    FRESULT fresult = f_close(fp);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(0);
}

void std_api_read_xstack(void)
{
    uint8_t *buf;
    uint16_t count;
    int16_t fd = API_A;
    // TODO support fd==0 as STDIN
    if (!api_pop_uint16_end(&count) ||
        fd < STD_FIL_OFFS || fd >= STD_FIL_MAX + STD_FIL_OFFS ||
        count > 0x100)
        return api_return_errno(API_EINVAL);
    buf = &xstack[XSTACK_SIZE - count];
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    if (fresult == FR_OK)
        api_set_ax(br);
    else
    {
        API_ERRNO = fresult;
        api_set_ax(-1);
    }
    if (br == count)
        xstack_ptr = XSTACK_SIZE - count;
    else // short reads need to be moved
        for (UINT i = br; i;)
            xstack[--xstack_ptr] = buf[--i];
    api_sync_xstack();
    api_return_released();
}

void std_api_read_xram(void)
{
    static uint16_t xram_addr;
    if (std_count >= 0)
    {
        for (; std_count && pix_ready(); --std_count, ++xram_addr)
            pix_send(0, 0, xram[xram_addr], xram_addr);
        if (!std_count)
        {
            std_count = -1;
            api_return_released();
        }
        return;
    }
    uint8_t *buf;
    uint16_t count;
    int16_t fd = API_A;
    // TODO support fd==0 as STDIN
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr) ||
        fd < STD_FIL_OFFS || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    buf = &xram[xram_addr];
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (buf + count > xram + 0x10000)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    if (fresult == FR_OK)
        api_set_ax(br);
    else
    {
        API_ERRNO = fresult;
        api_set_ax(-1);
    }
    std_count = br;
}

// Non-blocking write
static void std_out_write(char *ptr)
{
    static void *std_out_ptr;
    if (ptr)
        std_out_ptr = ptr;
    for (; std_count && com_stdout_writable(); --std_count)
        com_stdout_write(*(uint8_t *)std_out_ptr++);
    if (!std_count)
    {
        std_count = -1;
        api_return_released();
    }
}

void std_api_write_xstack(void)
{
    if (std_count >= 0)
        return std_out_write(NULL);
    uint8_t *buf;
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    count = XSTACK_SIZE - xstack_ptr;
    buf = &xstack[xstack_ptr];
    api_zxstack();
    if (fd < STD_FIL_OFFS)
    {
        api_set_ax(count);
        std_count = count;
        std_out_write((char *)buf);
        return;
    }
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(bw);
}

void std_api_write_xram(void)
{
    if (std_count >= 0)
        return std_out_write(NULL);
    uint8_t *buf;
    uint16_t xram_addr;
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    buf = &xram[xram_addr];
    if (buf + count > xram + 0x10000)
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (fd < STD_FIL_OFFS)
    {
        api_set_ax(count);
        std_count = count;
        std_out_write((char *)buf);
        return;
    }
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    return api_return_ax(bw);
}

void std_api_lseek(void)
{
    int8_t whence;
    int32_t ofs;
    int fd = API_A;
    if (fd < STD_FIL_OFFS ||
        fd >= STD_FIL_MAX + STD_FIL_OFFS ||
        !api_pop_int8(&whence) ||
        !api_pop_int32_end(&ofs))
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    switch (whence) // CC65
    {
    case 0: // SEEK_CUR
        ofs += f_tell(fp);
        break;
    case 1: // SEEK_END
        ofs += f_size(fp);
        break;
    case 2: // SEEK_SET
        break;
    default:
        return api_return_errno(API_EINVAL);
    }
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
        return api_return_errno(API_EFATFS(fresult));
    FSIZE_t pos = f_tell(fp);
    // Beyond 2GB is darkness.
    if (pos > 0x7FFFFFFF)
        pos = 0x7FFFFFFF;
    return api_return_axsreg(pos);
}

void std_stop(void)
{
    std_count = -1;
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (std_fil[i].obj.fs)
            f_close(&std_fil[i]);
}
