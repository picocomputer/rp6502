/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/std.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "sys/rln.h"
#include "net/mdm.h"
#include "fatfs/ff.h"
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_STD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define STD_FIL_MAX 16
FIL std_fil[STD_FIL_MAX];
#define STD_FIL_STDIN 0
#define STD_FIL_STDOUT 1
#define STD_FIL_STDERR 2
#define STD_FIL_MODEM 3
#define STD_FIL_OFFS 4
static_assert(STD_FIL_MAX + STD_FIL_OFFS < 128);

static int32_t std_count_xram;
static int32_t std_count_std;
static int32_t std_count_mdm;
static int32_t std_count_moved;
static char *std_buf_ptr;

// TODO simplify this once we drop const buf
static bool std_rln_active;
static const char *std_rln_buf;
static bool std_rln_needs_nl;
static size_t std_rln_pos;
static size_t std_rln_length;
static size_t std_api_rln_str_length;
static uint32_t std_api_rln_ctrl_bits;

bool std_api_open(void)
{
    // These match CC65 which is closer to POSIX than FatFs.
    const unsigned char RDWR = 0x03;
    const unsigned char CREAT = 0x10;
    const unsigned char TRUNC = 0x20;
    const unsigned char APPEND = 0x40;
    const unsigned char EXCL = 0x80;

    TCHAR *path = (TCHAR *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (mdm_open(path))
        return api_return_ax(STD_FIL_MODEM);
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
    int fd = 0;
    for (; fd < STD_FIL_MAX; fd++)
        if (!std_fil[fd].obj.fs)
            break;
    if (fd == STD_FIL_MAX)
        return api_return_errno(API_EMFILE);
    FIL *fp = &std_fil[fd];
    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(fd + STD_FIL_OFFS);
}

bool std_api_close(void)
{
    int fd = API_A;
    if (fd == STD_FIL_MODEM)
    {
        if (mdm_close())
            return api_return_ax(0);
        else
            return api_return_fresult(FR_INVALID_OBJECT);
    }
    if (fd < STD_FIL_OFFS || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    FRESULT fresult = f_close(fp);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

static void std_rln_callback(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    std_rln_active = false;
    std_rln_buf = buf;
    std_rln_pos = 0;
    std_rln_length = length;
    std_rln_needs_nl = true;
}

static bool std_rln_ready(void)
{
    if (std_rln_needs_nl || std_rln_pos < std_rln_length)
        return true;
    if (!std_rln_active)
    {
        std_rln_active = true;
        rln_read_line(0, std_rln_callback, std_api_rln_str_length + 1, std_api_rln_ctrl_bits);
    }
    return false;
}

static size_t std_rln_read(uint8_t *buf, size_t count)
{
    size_t i;
    for (i = 0; i < count && std_rln_pos < std_rln_length; i++)
        buf[i] = std_rln_buf[std_rln_pos++];
    if (i < count && std_rln_needs_nl)
    {
        buf[i++] = '\n';
        std_rln_needs_nl = false;
    }
    return i;
}

bool std_api_stdin_opt(void)
{
    uint8_t str_length = API_A;
    uint32_t ctrl_bits;
    if (!api_pop_uint32_end(&ctrl_bits))
        return api_return_errno(API_EINVAL);
    std_api_rln_str_length = str_length;
    std_api_rln_ctrl_bits = ctrl_bits;
    return api_return_ax(0);
}

bool std_api_read_xstack(void)
{
    uint8_t *buf;
    uint16_t count;
    if (std_count_std >= 0)
    {
        if (!std_rln_ready())
            return api_working();
        count = std_count_std;
        buf = &xstack[XSTACK_SIZE - count];
        std_count_moved = std_rln_read(buf, count);
        std_count_std = -1;
    }
    else if (std_count_mdm >= 0)
    {
        count = std_count_mdm;
        buf = &xstack[XSTACK_SIZE - count];
        if (std_count_moved < count)
            switch (mdm_rx((char *)&buf[std_count_moved]))
            {
            case -1:
                std_count_mdm = -1;
                return api_return_fresult(FR_INVALID_OBJECT);
            case 1:
                std_count_moved++;
                return api_working();
            case 0:
                break;
            }
        std_count_mdm = -1;
    }
    else
    {
        int16_t fd = API_A;
        if (!api_pop_uint16_end(&count) ||
            (fd && fd < STD_FIL_MODEM) ||
            fd >= STD_FIL_MAX + STD_FIL_OFFS ||
            count > XSTACK_SIZE)
            return api_return_errno(API_EINVAL);
        buf = &xstack[XSTACK_SIZE - count];
        if (fd == STD_FIL_STDIN)
        {
            std_count_std = count;
            return api_working();
        }
        if (fd == STD_FIL_MODEM)
        {
            std_count_mdm = count;
            std_count_moved = 0;
            return api_working();
        }
        FIL *fp = &std_fil[fd - STD_FIL_OFFS];
        UINT br;
        FRESULT fresult = f_read(fp, buf, count, &br);
        std_count_moved = br;
        if (fresult != FR_OK)
            return api_return_fresult(fresult);
    }
    xstack_ptr = XSTACK_SIZE;
    if (std_count_moved == count)
        xstack_ptr -= count;
    else // relocate short read
        for (UINT i = std_count_moved; i;)
            xstack[--xstack_ptr] = buf[--i];
    return api_return_ax(std_count_moved);
}

bool std_api_read_xram(void)
{
    if (std_count_std >= 0)
    {
        if (!std_rln_ready())
            return api_working();
        std_count_xram = std_rln_read((uint8_t *)std_buf_ptr, std_count_std);
        api_set_ax(std_count_xram);
        std_count_std = -1;
        return api_working();
    }
    if (std_count_mdm >= 0)
    {
        if (std_count_moved < std_count_mdm)
            switch (mdm_rx(&std_buf_ptr[std_count_moved]))
            {
            case -1:
                std_count_mdm = -1;
                return api_return_fresult(FR_INVALID_OBJECT);
            case 1:
                std_count_moved++;
                return api_working();
            case 0:
                break;
            }
        std_count_xram = std_count_moved;
        api_set_ax(std_count_xram);
        std_count_mdm = -1;
        return api_working();
    }
    if (std_count_xram >= 0)
    {
        uint16_t xram_addr = std_buf_ptr - (char *)xram;
        for (; std_count_xram && pix_ready(); --std_count_xram, ++xram_addr, ++std_buf_ptr)
            pix_send(PIX_DEVICE_XRAM, 0, xram[xram_addr], xram_addr);
        if (!std_count_xram)
        {
            std_count_xram = -1;
            return api_return();
        }
        return api_working();
    }
    uint16_t count;
    uint16_t xram_addr;
    int16_t fd = API_A;
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr) ||
        (fd && fd < STD_FIL_MODEM) ||
        fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    std_buf_ptr = (char *)&xram[xram_addr];
    if (fd == STD_FIL_STDIN)
    {
        std_count_std = count;
        return api_working();
    }
    if (fd == STD_FIL_MODEM)
    {
        std_count_moved = 0;
        std_count_mdm = count;
        return api_working();
    }
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (std_buf_ptr + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT br;
    FRESULT fresult = f_read(fp, std_buf_ptr, count, &br);
    if (fresult == FR_OK)
        api_set_ax(br);
    else
    {
        API_ERRNO = fresult;
        api_set_ax(-1);
    }
    std_count_xram = br;
    return api_working();
}

static bool std_out_write(void)
{
    if (std_count_moved < std_count_std && com_tx_printable())
        putchar(std_buf_ptr[std_count_moved++]);
    if (std_count_moved >= std_count_std)
    {
        std_count_std = -1;
        return api_return_ax(std_count_moved);
    }
    return api_working();
}

static bool std_mdm_write(void)
{
    while (std_count_moved < std_count_mdm)
    {
        int tx = mdm_tx(std_buf_ptr[std_count_moved]);
        if (tx == -1)
        {
            std_count_mdm = -1;
            return api_return_fresult(FR_INVALID_OBJECT);
        }
        if (tx == 0)
            break;
        std_count_moved++;
        return api_working();
    }
    std_count_mdm = -1;
    return api_return_ax(std_count_moved);
}

bool std_api_write_xstack(void)
{
    if (std_count_std >= 0)
        return std_out_write();
    if (std_count_mdm >= 0)
        return std_mdm_write();
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    count = XSTACK_SIZE - xstack_ptr;
    std_count_moved = 0;
    std_buf_ptr = (char *)&xstack[xstack_ptr];
    xstack_ptr = XSTACK_SIZE;
    if (fd == STD_FIL_MODEM)
    {
        std_count_mdm = count;
        return api_working();
    }
    if (fd < STD_FIL_OFFS) // stdout stderr
    {
        std_count_std = count;
        return api_working();
    }
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf_ptr, count, &bw);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(bw);
}

bool std_api_write_xram(void)
{
    if (std_count_std >= 0)
        return std_out_write();
    if (std_count_mdm >= 0)
        return std_mdm_write();
    uint16_t xram_addr;
    uint16_t count;
    int fd = API_A;
    if (fd == STD_FIL_STDIN || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    if (!api_pop_uint16(&count) ||
        !api_pop_uint16_end(&xram_addr))
        return api_return_errno(API_EINVAL);
    std_count_moved = 0;
    std_buf_ptr = (char *)&xram[xram_addr];
    if (std_buf_ptr + count > (char *)xram + 0x10000)
        return api_return_errno(API_EINVAL);
    if (count > 0x7FFF)
        count = 0x7FFF;
    if (fd == STD_FIL_MODEM)
    {
        std_count_mdm = count;
        return api_working();
    }
    if (fd < STD_FIL_OFFS) // stdout stderr
    {
        std_count_std = count;
        return api_working();
    }
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    UINT bw;
    FRESULT fresult = f_write(fp, std_buf_ptr, count, &bw);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(bw);
}

// long f_lseek(long ofs, char whence, int fildes);
static bool std_api_lseek(int set, int cur, int end)
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
    if (whence == set)
        ; /* noop */
    else if (whence == cur)
        ofs += f_tell(fp);
    else if (whence == end)
        ofs += f_size(fp);
    else
        return api_return_errno(API_EINVAL);
    FRESULT fresult = f_lseek(fp, ofs);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    FSIZE_t pos = f_tell(fp);
    // Beyond 2GB is darkness.
    if (pos > 0x7FFFFFFF)
        pos = 0x7FFFFFFF;
    return api_return_axsreg(pos);
}

bool std_api_lseek_cc65(void)
{
    return std_api_lseek(2, 0, 1);
}

bool std_api_lseek_llvm(void)
{
    return std_api_lseek(0, 1, 2);
}

// int syncfs (int fd);
bool std_api_syncfs(void)
{
    int fd = API_A;
    if (fd < STD_FIL_OFFS || fd >= STD_FIL_MAX + STD_FIL_OFFS)
        return api_return_errno(API_EINVAL);
    FIL *fp = &std_fil[fd - STD_FIL_OFFS];
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
        return api_return_fresult(fresult);
    return api_return_ax(0);
}

void std_run(void)
{
    std_count_xram = -1;
    std_count_std = -1;
    std_count_mdm = -1;
    std_rln_active = false;
    std_rln_needs_nl = false;
    std_rln_pos = 0;
    std_rln_length = 0;
    std_api_rln_str_length = 254;
    std_api_rln_ctrl_bits = 0;
}

void std_stop(void)
{
    for (int i = 0; i < STD_FIL_MAX; i++)
        if (std_fil[i].obj.fs)
            f_close(&std_fil[i]);
}
