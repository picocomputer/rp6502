/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api.h"
#include "com.h"
#include "pix.h"
#include "std.h"
#include "fatfs/ff.h"
#include "pico/stdlib.h"

#define FIL_MAX 16
FIL fil_pool[FIL_MAX];
#define FIL_STDIN 0
#define FIL_STDOUT 1
#define FIL_STDERR 2
#define FIL_OFFS 3
static_assert(FIL_MAX + FIL_OFFS < 128);

static enum {
    API_IDLE,
    API_READ_XRAM,
    // API_READ_STDIN, //TODO
    API_WRITE_STDOUT,
} api_state;

static void *api_io_ptr;
static uint16_t api_xaddr;
static unsigned api_count;

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
    xstack_ptr = XSTACK_SIZE;
    int fd = 0;
    for (; fd < FIL_MAX; fd++)
        if (!fil_pool[fd].obj.fs)
            break;
    if (fd == FIL_MAX)
        return api_return_errno_ax(FR_TOO_MANY_OPEN_FILES, -1);
    FIL *fp = &fil_pool[fd];
    FRESULT fresult = f_open(fp, (TCHAR *)path, mode);
    if (fresult != FR_OK)
        return api_return_errno_ax(fresult, -1);
    return api_return_ax(fd + FIL_OFFS);
}

void std_api_close(void)
{
    int fd = API_A;
    if (fd < FIL_OFFS || fd >= FIL_MAX + FIL_OFFS)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    FRESULT fresult = f_close(fp);
    if (fresult != FR_OK)
        return api_return_errno_ax(fresult, -1);
    return api_return_ax(0);
}

static void api_read_impl(bool is_xram)
{
    uint8_t *buf;
    UINT count;
    int fd = API_A;
    // TODO support fd==0 as STDIN
    if (fd < FIL_OFFS || fd >= FIL_MAX + FIL_OFFS)
        goto err_param;
    if (is_xram)
    {
        if (XSTACK_SIZE - xstack_ptr < 2)
            goto err_param;
        api_xaddr = *(uint16_t *)&xstack[xstack_ptr];
        xstack_ptr += 2;
        buf = &xram[api_xaddr];
        count = api_sstack_uint16();
        if (buf + count > xstack + 0x10000)
            goto err_param;
    }
    else
    {
        count = api_sstack_uint16();
        if (count > 0x100)
            goto err_param;
        buf = &xstack[XSTACK_SIZE - count];
    }
    if (xstack_ptr != XSTACK_SIZE)
        goto err_param;
    if (count > 0x7FFF)
        count = 0x7FFF;
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    if (fresult == FR_OK)
        api_set_ax(br);
    else
    {
        API_ERRNO = fresult;
        api_set_ax(-1);
    }
    if (is_xram)
    {
        api_sync_xram();
        api_state = API_READ_XRAM;
        api_count = br;
    }
    else
    {
        if (br == count)
            xstack_ptr = XSTACK_SIZE - count;
        else // short reads need to be moved
            for (UINT i = br; i;)
                xstack[--xstack_ptr] = buf[--i];
        api_sync_xstack();
        api_return_released();
    }
    return;

err_param:
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(FR_INVALID_PARAMETER, -1);
}

void std_api_read_(void)
{
    api_read_impl(false);
}

void std_api_readx(void)
{
    if (api_state == API_READ_XRAM)
    {
        for (; api_count && pix_ready(); --api_count, ++api_xaddr)
            pix_send(0, 0, xstack[api_xaddr], api_xaddr);
        if (!api_count)
        {
            api_state = API_IDLE;
            api_return_released();
        }
        return;
    }
    api_read_impl(true);
}

static void api_write_impl(bool is_xram)
{
    if (api_state == API_WRITE_STDOUT)
    {
        for (; api_count && uart_is_writable(RIA_UART); --api_count)
        {
            uint8_t ch = *(uint8_t *)api_io_ptr++;
            if (ch == '\n')
            {
                uart_putc_raw(RIA_UART, '\r');
                uart_putc_raw(RIA_UART, ch);
            }
            else
                uart_get_hw(RIA_UART)->dr = ch;
        }
        if (!api_count)
        {
            api_state = API_IDLE;
            api_return_released();
        }
        return;
    }
    uint8_t *buf;
    uint16_t count;
    int fd = API_A;
    // TODO support fd==1,2 as STDOUT
    if (fd == FIL_STDIN || fd >= FIL_MAX + FIL_OFFS)
        goto err_param;
    if (is_xram)
    {
        if (XSTACK_SIZE - xstack_ptr < 2)
            goto err_param;
        buf = &xram[*(uint16_t *)&xstack[xstack_ptr]];
        xstack_ptr += 2;
        count = api_sstack_uint16();
        if (buf + count > xstack + 0x10000)
            goto err_param;
    }
    else
    {
        count = XSTACK_SIZE - xstack_ptr;
        buf = &xstack[xstack_ptr];
        xstack_ptr = XSTACK_SIZE;
    }
    if (xstack_ptr != XSTACK_SIZE)
        goto err_param;
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (fd < FIL_OFFS)
    {
        api_state = API_WRITE_STDOUT;
        api_io_ptr = buf;
        api_count = count;
        api_set_ax(count);
        return;
    }
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    if (fresult != FR_OK)
        return api_return_errno_ax(fresult, -1);
    return api_return_ax(bw);

err_param:
    xstack_ptr = XSTACK_SIZE;
    api_return_errno_axsreg_zxstack(FR_INVALID_PARAMETER, -1);
}

void std_api_write_(void)
{
    api_write_impl(false);
}

void std_api_writex(void)
{
    api_write_impl(true);
}

void std_api_lseek(void)
{
    // These are identical to unistd.h but we don't want to depend on that.
    const unsigned SET = 0x00;
    const unsigned CUR = 0x01;
    const unsigned END = 0x02;
    int fd = API_A;
    if (xstack_ptr < XSTACK_SIZE - 9 || xstack_ptr > XSTACK_SIZE - 1 ||
        fd < FIL_OFFS || fd >= FIL_MAX + FIL_OFFS)
        return api_return_errno_axsreg_zxstack(FR_INVALID_PARAMETER, -1);
    unsigned whence = xstack[xstack_ptr++];
    int64_t ofs = api_sstack_int64();
    FIL *fp = &fil_pool[fd - FIL_OFFS];
    switch (whence)
    {
    case SET:
        (void)(SET);
        break;
    case CUR:
        (void)(CUR);
        ofs += f_tell(fp);
        break;
    case END:
        (void)(END);
        ofs += f_size(fp);
        break;
    }
    if (ofs < 0 || whence > END)
        return api_return_errno_axsreg_zxstack(FR_INVALID_PARAMETER, -1);
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
        return api_return_errno_axsreg(fresult, -1);
    FSIZE_t pos = f_tell(fp);
    // Anyone seeking around a file beyond
    // this size will have to do so blind.
    if (pos > 0x7FFFFFFF)
        pos = 0x7FFFFFFF;
    return api_return_axsreg(pos);
}

void std_stop(void)
{
    api_state = API_IDLE;
    for (int i = 0; i < FIL_MAX; i++)
        if (fil_pool[i].obj.fs)
            f_close(&fil_pool[i]);
}