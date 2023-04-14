/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api.h"
#include "com.h"
#include "ria.h"
#include "fatfs/ff.h"
#include "mem/regs.h"
#include "mem/xram.h"
#include "mem/xstack.h"
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

uint16_t api_sstack_uint16()
{

    if (xstack_ptr == XSTACK_SIZE - 1)
    {
        uint16_t val = *(uint8_t *)&xstack[xstack_ptr];
        xstack_ptr += 1;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 2)
    {
        uint16_t val = *(uint16_t *)&xstack[xstack_ptr];
        xstack_ptr += 2;
        return val;
    }
    return 0;
}

uint32_t api_sstack_uint32()
{
    if (xstack_ptr == XSTACK_SIZE - 3)
    {
        uint32_t val = *(uint32_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 3;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 4)
    {
        uint32_t val = *(uint32_t *)&xstack[xstack_ptr];
        xstack_ptr += 4;
        return val;
    }
    return api_sstack_uint16();
}

uint64_t api_sstack_uint64()
{
    if (xstack_ptr == XSTACK_SIZE - 5)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr - 3] >> 24;
        xstack_ptr += 5;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 6)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr - 2] >> 16;
        xstack_ptr += 6;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 7)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 7;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 8)
    {
        uint64_t val = *(uint64_t *)&xstack[xstack_ptr];
        xstack_ptr += 8;
        return val;
    }
    return api_sstack_uint32();
}

int16_t api_sstack_int16()
{

    if (xstack_ptr == XSTACK_SIZE - 1)
    {
        int16_t val = *(int8_t *)&xstack[xstack_ptr];
        xstack_ptr += 1;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 2)
    {
        int16_t val = *(int16_t *)&xstack[xstack_ptr];
        xstack_ptr += 2;
        return val;
    }
    return 0;
}

int32_t api_sstack_int32()
{
    if (xstack_ptr == XSTACK_SIZE - 3)
    {
        int32_t val = *(int32_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 3;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 4)
    {
        int32_t val = *(int32_t *)&xstack[xstack_ptr];
        xstack_ptr += 4;
        return val;
    }
    return api_sstack_int16();
}

int64_t api_sstack_int64()
{
    if (xstack_ptr == XSTACK_SIZE - 5)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr - 3] >> 24;
        xstack_ptr += 5;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 6)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr - 2] >> 16;
        xstack_ptr += 6;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 7)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr - 1] >> 8;
        xstack_ptr += 7;
        return val;
    }
    if (xstack_ptr == XSTACK_SIZE - 8)
    {
        int64_t val = *(int64_t *)&xstack[xstack_ptr];
        xstack_ptr += 8;
        return val;
    }
    return api_sstack_int32();
}

static void api_open(void)
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

static void api_close(void)
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

static void api_read(bool is_xram)
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

static void api_read_xram()
{
    for (; api_count && ria_pix_ready(); --api_count, ++api_xaddr)
        ria_pix_send(0, xstack[api_xaddr], api_xaddr);
    if (!api_count)
    {
        api_state = API_IDLE;
        return api_return_released();
    }
}

static void api_write(bool is_xram)
{
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

void api_write_stdout(void)
{
    for (; api_count && uart_is_writable(RIA_UART); --api_count)
    {
        uint8_t ch = *(uint8_t *)api_io_ptr++;
        if (ch == '\n') {
            uart_putc_raw(RIA_UART, '\r');
            uart_putc_raw(RIA_UART, ch);
        }
        else
            uart_get_hw(RIA_UART)->dr = ch;
    }
    if (!api_count)
    {
        api_state = API_IDLE;
        return api_return_released();
    }
}

static void api_lseek(void)
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

static void api_set_xreg()
{
    unsigned regno = API_A;
    uint16_t data = api_sstack_uint16();
    if (xstack_ptr != XSTACK_SIZE)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    RIA_PIX_PIO->txf[RIA_PIX_SM] = (regno << 16) | data | RIA_PIX_XREG(1);
    return api_return_ax(0);
}

void api_task()
{
    switch (api_state)
    {
    case API_READ_XRAM:
        return api_read_xram();
        break;
    case API_WRITE_STDOUT:
        return api_write_stdout();
        break;
    case API_IDLE:
        break;
    }

    if (API_BUSY)
        switch (API_OP) // 1-127 valid
        {
        case 0x00:
        case 0xFF:
            // action loop handles these
            break;
        case 0x01:
            api_open();
            break;
        case 0x04:
            api_close();
            break;
        case 0x05:
            api_read(0);
            break;
        case 0x06:
            api_read(1);
            break;
        case 0x08:
            api_write(0);
            break;
        case 0x09:
            api_write(1);
            break;
        case 0x0B:
            api_lseek();
            break;
        case 0x10:
            api_set_xreg();
            break;
        default:
            // TODO report an error
            //  API_ERRNO = EUNKNOWN;
            api_return_released();
            break;
        }
}

void api_stop()
{
    api_state = API_IDLE;
    for (unsigned i = 1; i < 7; i++)
    {
        while (!ria_pix_ready())
            ;
        // TODO send global config bits.
        ria_pix_send(i, 0xFFF, 0);
    }
    for (int i = 0; i < FIL_MAX; i++)
        if (fil_pool[i].obj.fs)
            f_close(&fil_pool[i]);
}

void api_reset()
{
    for (int i = 0; i < 16; i++)
        REGS(i) = 0;
    XRAM_STEP0 = 1;
    XRAM_STEP1 = 1;
    xstack_ptr = XSTACK_SIZE;
    // TODO this doesn't work here and isn't very important,
    //       but it'd be nice to have $FFF0-$FFFA initialized for programs.
    // api_return_errno_axsreg_zxstack(0, 0);
}
