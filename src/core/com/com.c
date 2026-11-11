/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/com.h"
#include "core/str/oem.h"
#include "core/str/str.h"
#include "core/sys/ria.h"
#include "core/sys/timer.h"
#include "core/com/manifold.h"
#include "machine.h"
#include "drivers.h"

#include <stdarg.h>
#include <stdio.h>

static const com_source_driver_t
    com_sources[COM_SOURCE_COUNT] = {RP6502_COM_SOURCES};

static com_source_t com_rx_held = COM_SOURCE_ANY;
static timer_mach_t com_rx_deadline;

/* The source of the byte staged in the 6502's receive register. Every
 * source's read starts by taking that byte back, and only the source it came
 * from gets it; otherwise the keyboard would be handed a byte typed at the
 * UART. */
static com_source_t com_rx_staged;

static bool com_bel_enabled = true;

#define COM_LINE_CR 1
#define COM_LINE_LF 2

/* Which of CR and LF have gone out since the last other byte. A terminal
 * starts at the start of a line. */
static uint8_t com_line = COM_LINE_CR | COM_LINE_LF;

size_t HOST_IN_RAM_FUNC com_read(com_source_t src, char *buf, size_t n)
{
    size_t got = com_rx_reclaim(buf, n, src);
    if (got < n && com_sources[src].read)
        got += com_sources[src].read(&buf[got], n - got);
    return got;
}

int HOST_IN_RAM_FUNC com_peek(com_source_t src, size_t offset)
{
    int c = com_rx_peek(src);
    if (c >= 0)
    {
        if (!offset)
            return c;
        offset--;
    }
    return com_sources[src].peek ? com_sources[src].peek(offset) : -1;
}

size_t HOST_IN_RAM_FUNC com_queue_size(com_source_t src)
{
    return com_sources[src].size;
}

/* A source that read something holds the reader for the dwell it declares, so a
 * byte from another source cannot land between two chunks of a burst. A source
 * that declares no dwell is never held, because a deadline of zero microseconds
 * has already passed. */
static size_t HOST_IN_RAM_FUNC com_rx_pick(char *buf, size_t length, com_source_t *src_out)
{
    if (com_rx_held != COM_SOURCE_ANY && timer_mach_passed(com_rx_deadline))
        com_rx_held = COM_SOURCE_ANY;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        if (com_rx_held != COM_SOURCE_ANY && com_rx_held != s)
            continue;
        size_t n = com_read(s, buf, length);
        if (n)
        {
            com_rx_held = s;
            com_rx_deadline = timer_mach_in_us(com_sources[s].dwell_us);
            if (src_out)
                *src_out = s;
            return n;
        }
        if (!com_sources[s].dwell_us)
            com_rx_held = COM_SOURCE_ANY;
    }
    return 0;
}

int HOST_IN_RAM_FUNC com_getchar(com_source_t *src)
{
    char ch;
    size_t n = *src == COM_SOURCE_ANY ? com_rx_pick(&ch, 1, src)
                                      : com_read(*src, &ch, 1);
    if (n)
        return (unsigned char)ch;
    *src = COM_SOURCE_ANY;
    return -1;
}

size_t com_stdin_read(char *buf, size_t count)
{
    return com_rx_pick(buf, count, NULL);
}

bool HOST_IN_RAM_FUNC com_rx_stage(void)
{
    com_source_t src = COM_SOURCE_ANY;
    int ch = com_getchar(&src);
    if (ch < 0)
        return false;
    com_rx_staged = src;
    ria_uart_rx_offer((uint8_t)ch);
    return true;
}

size_t HOST_IN_RAM_FUNC com_rx_reclaim(char *buf, size_t length, com_source_t src)
{
    uint8_t ch;
    if (!length || com_rx_staged != src || !ria_uart_rx_reclaim(&ch))
        return 0;
    buf[0] = (char)ch;
    return 1;
}

int HOST_IN_RAM_FUNC com_rx_peek(com_source_t src)
{
    return com_rx_staged == src ? ria_uart_rx_peek() : -1;
}

/* A source's unread input, with the byte it has waiting in the RIA's RX
 * latch, which is the head of that source's queue and goes with it. */
void HOST_IN_FLASH_FUNC com_rx_clear_source(com_source_t s)
{
    if (com_rx_held == s)
        com_rx_held = COM_SOURCE_ANY;
    char waiting;
    com_rx_reclaim(&waiting, 1, s);
    if (com_sources[s].clear)
        com_sources[s].clear();
}

void HOST_IN_FLASH_FUNC com_rx_clear(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        com_rx_clear_source(s);
}

bool HOST_IN_RAM_FUNC com_cts(void)
{
    return manifold_cts();
}

int HOST_IN_RAM_FUNC com_putchar(int c)
{
    return manifold_putchar(c);
}

void HOST_IN_RAM_FUNC com_putchar_raw(char c)
{
    manifold_putchar_raw(c);
}

size_t HOST_IN_RAM_FUNC com_write(int fd, const char *buf, size_t n)
{
    return manifold_write(fd, buf, n);
}

void HOST_IN_RAM_FUNC com_flush(void)
{
    manifold_flush();
}

/* Where COM_LIBC_STDOUT says the C library's stdout is the console, this
 * formats straight to it. Elsewhere it formats into a 1 KB buffer and cuts off
 * what does not fit. */
int HOST_IN_FLASH_FUNC com_printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
#if COM_LIBC_STDOUT
    int n = vprintf(fmt, va);
#else
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    int w = n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1;
    for (int i = 0; i < w; i++)
        com_putchar(buf[i]);
#endif
    va_end(va);
    return n;
}

#define COM_PRINTF_UTF8_SIZE 128

int HOST_IN_FLASH_FUNC com_printf_utf8(const char *utf8_fmt, ...)
{
    char buf[COM_PRINTF_UTF8_SIZE];
    va_list va;
    va_start(va, utf8_fmt);
    oem_vsnprintf(buf, sizeof(buf), utf8_fmt, va);
    va_end(va);
    int n = 0;
    for (const char *p = buf; *p; p++, n++)
        com_putchar(*p);
    return n;
}

bool HOST_IN_RAM_FUNC com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}

void com_line_track(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        com_line = buf[i] == '\r'   ? com_line | COM_LINE_CR
                   : buf[i] == '\n' ? com_line | COM_LINE_LF
                                    : 0;
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

/* Flushed first, so bytes already on their way cannot count after it. */
void com_line_set_clean(void)
{
    com_flush();
    com_line = COM_LINE_CR | COM_LINE_LF;
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
    com_flush();
    bool clean = com_line_clean();
    com_line_puts(STR_TERM_SOFT_RESET);
    if (!clean)
    {
        com_putchar('\r');
        com_putchar('\n');
    }
}

void com_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u8(c, (uint8_t)com_rx_held);
    sst_put_u64(c, com_rx_deadline);
    sst_put_bool(c, com_bel_enabled);
    sst_put_u8(c, (uint8_t)com_rx_staged);
    sst_put_u8(c, com_line);
}

bool com_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t held = sst_get_u8(c);
    uint64_t deadline = sst_get_u64(c);
    bool bel = sst_get_bool(c);
    uint8_t staged = sst_get_u8(c);
    uint8_t line = sst_get_u8(c);
    if (!sst_ok(c) || held > COM_SOURCE_ANY || staged >= COM_SOURCE_COUNT ||
        line > (COM_LINE_CR | COM_LINE_LF))
        return false;
    com_rx_held = (com_source_t)held;
    com_rx_deadline = deadline;
    com_bel_enabled = bel;
    com_rx_staged = (com_source_t)staged;
    com_line = line;
    return true;
}
