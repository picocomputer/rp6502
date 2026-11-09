/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/com.h"
#include "core/str/str.h"

#define COM_LINE_CR 1
#define COM_LINE_LF 2

/* Which of CR and LF have gone out since the last other byte. A terminal
 * starts at the start of a line. */
static uint8_t com_line = COM_LINE_CR | COM_LINE_LF;

void com_line_track(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        com_line = buf[i] == '\r'   ? com_line | COM_LINE_CR
                   : buf[i] == '\n' ? com_line | COM_LINE_LF
                                    : 0;
}

void com_line_save(sst_cursor_t *c)
{
    sst_put_u8(c, com_line);
}

bool com_line_load(sst_cursor_t *c)
{
    uint8_t line = sst_get_u8(c);
    if (!sst_ok(c) || line > (COM_LINE_CR | COM_LINE_LF))
        return false;
    com_line = line;
    return true;
}

bool com_line_clean(void)
{
    return com_line == (COM_LINE_CR | COM_LINE_LF);
}

static void com_line_puts(const char *s)
{
    while (*s)
        com_putchar((unsigned char)*s++);
}

void com_write_control(const char *s)
{
    com_flush();
    uint8_t line = com_line;
    com_line_puts(s);
    com_flush();
    com_line = line;
}

/* Byte by byte, because a string would be data in flash, and this runs while
 * the RIA is flashing. */
void com_clean_line(void)
{
    com_flush();
    if (!com_line_clean())
    {
        com_putchar('\r');
        com_putchar('\n');
    }
}

/* The soft reset starts with CAN, which ends any escape sequence left half
 * sent, so the CR LF after it lands as CR LF. */
void com_end_reset(void)
{
    com_write_control(STR_TERM_SOFT_RESET);
    com_clean_line();
}
