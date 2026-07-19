/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/api/arg.h"
#include "ria/sys/mem.h"
#include <string.h>

// Layout: offset[0], offset[1], ..., offset[n-1], {0,0}, str[0], ..., str[n-1].
// Each offset is a little-endian uint16 into arg_buf pointing at its string. The
// {0,0} pair terminates the offset table; strings follow immediately, packed with
// no padding and in ascending offset order (so str[i] always precedes str[i+1]).
// A plain static, so it survives a machine reset — EXEC's new argv must reach the
// new program.
static uint8_t arg_buf[XSTACK_SIZE];

static uint16_t arg_count(void)
{
    for (uint16_t i = 0; i < XSTACK_SIZE / 2; i++)
        if (arg_buf[i * 2] == 0 && arg_buf[i * 2 + 1] == 0)
            return i;
    return 0;
}

void arg_clear(void)
{
    arg_buf[0] = arg_buf[1] = 0;
}

static uint16_t arg_offset_read(uint16_t i)
{
    return arg_buf[i * 2] | ((uint16_t)arg_buf[i * 2 + 1] << 8);
}

static void arg_offset_write(uint16_t i, uint16_t offset)
{
    arg_buf[i * 2] = offset & 0xFF;
    arg_buf[i * 2 + 1] = offset >> 8;
}

static uint16_t arg_size(void)
{
    uint16_t count = arg_count();
    if (count == 0)
        return 2;
    uint16_t offset = arg_offset_read(count - 1);
    return offset + (uint16_t)strlen((const char *)&arg_buf[offset]) + 1;
}

static bool arg_validate(void)
{
    uint16_t count = arg_count();
    uint16_t pos = (count + 1) * 2;
    if (pos >= XSTACK_SIZE)
        return false;
    for (uint16_t i = 0; i < count; i++)
    {
        if (arg_offset_read(i) != pos)
            return false;
        while (pos < XSTACK_SIZE && arg_buf[pos] != 0)
            pos++;
        if (pos >= XSTACK_SIZE)
            return false;
        pos++;
    }
    return true;
}

bool arg_append(const char *str)
{
    uint16_t count = arg_count();
    uint16_t old_strings_start = (count + 1) * 2;
    uint16_t old_size = arg_size();
    uint16_t strings_len = old_size - old_strings_start;
    uint16_t new_str_len = (uint16_t)strlen(str) + 1;
    if (old_size + 2 + new_str_len > XSTACK_SIZE)
        return false;
    memmove(&arg_buf[old_strings_start + 2], &arg_buf[old_strings_start], strings_len);
    for (uint16_t i = 0; i < count; i++)
        arg_offset_write(i, arg_offset_read(i) + 2);
    uint16_t new_offset = old_strings_start + 2 + strings_len;
    arg_offset_write(count, new_offset);
    arg_offset_write(count + 1, 0);
    memcpy(&arg_buf[new_offset], str, new_str_len);
    return true;
}

const char *arg_index(uint16_t idx)
{
    if (idx >= arg_count())
        return NULL;
    return (const char *)&arg_buf[arg_offset_read(idx)];
}

bool arg_replace(uint16_t idx, const char *str)
{
    uint16_t count = arg_count();
    if (idx >= count)
        return false;
    uint16_t old_offset = arg_offset_read(idx);
    uint16_t old_len = (uint16_t)strlen((const char *)&arg_buf[old_offset]) + 1;
    uint16_t new_len = (uint16_t)strlen(str) + 1;
    uint16_t old_size = arg_size();
    uint16_t tail_len = old_size - (old_offset + old_len);
    if (new_len != old_len)
    {
        if (new_len > old_len && old_size + (new_len - old_len) > XSTACK_SIZE)
            return false;
        memmove(&arg_buf[old_offset + new_len],
                &arg_buf[old_offset + old_len],
                tail_len);
        for (uint16_t i = 0; i < count; i++)
        {
            uint16_t offset = arg_offset_read(i);
            if (offset >= old_offset + old_len)
            {
                if (new_len > old_len)
                    offset += new_len - old_len;
                else
                    offset -= old_len - new_len;
                arg_offset_write(i, offset);
            }
        }
    }
    memcpy(&arg_buf[old_offset], str, new_len);
    return true;
}

uint16_t arg_push_xstack(void)
{
    uint16_t size = arg_size();
    xstack_ptr = XSTACK_SIZE - size;
    memcpy(&xstack[xstack_ptr], arg_buf, size);
    return size;
}

bool arg_pull_xstack(void)
{
    size_t ptr = xstack_ptr;
    uint16_t size = (uint16_t)(XSTACK_SIZE - ptr);
    memcpy(arg_buf, &xstack[ptr], size);
    memset(&arg_buf[size], 0, XSTACK_SIZE - size);
    xstack_ptr = XSTACK_SIZE;
    if (!arg_validate() || !arg_count())
    {
        arg_clear();
        return false;
    }
    return true;
}
