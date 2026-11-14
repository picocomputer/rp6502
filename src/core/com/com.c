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
#include "core/term/cells.h"
#include "core/com/manifold.h"
#include "machine.h"
#include "drivers.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

/* The console's terminal's answers, read from the UART's source. The first
 * com_reply_at bytes have been read, and while any have, the rest are read
 * before the UART's queue. */
static uint8_t com_reply_buf[32];
static uint8_t com_reply_len;
static uint8_t com_reply_at;
static bool com_reply_quiet;
static bool com_reply_default;

static size_t HOST_IN_RAM_FUNC com_reply_read(char *buf, size_t n)
{
    size_t got = (size_t)(com_reply_len - com_reply_at);
    if (got > n)
        got = n;
    memcpy(buf, &com_reply_buf[com_reply_at], got);
    com_reply_at += (uint8_t)got;
    if (com_reply_at == com_reply_len)
        com_reply_len = com_reply_at = 0;
    return got;
}

/* The UART's queue peeked empty at offset, so it holds offset bytes or
 * fewer, and an answer not yet started follows its last one. */
static int HOST_IN_RAM_FUNC com_reply_peek_past(size_t offset)
{
    size_t end = offset;
    while (end && offset - end < com_reply_len &&
           com_sources[COM_SOURCE_UART].peek(end - 1) < 0)
        end--;
    return offset - end < com_reply_len ? com_reply_buf[offset - end] : -1;
}

/* A source's read that comes back short has emptied its queue, so a held
 * answer that has not started is read next. */
size_t HOST_IN_RAM_FUNC com_read(com_source_t src, char *buf, size_t n)
{
    size_t got = com_rx_reclaim(buf, n, src);
    bool uart = src == COM_SOURCE_UART;
    if (uart && com_reply_at)
        got += com_reply_read(&buf[got], n - got);
    if (got < n && com_sources[src].read)
        got += com_sources[src].read(&buf[got], n - got);
    if (uart && got < n)
        got += com_reply_read(&buf[got], n - got);
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
    bool uart = src == COM_SOURCE_UART;
    if (uart && com_reply_at)
    {
        size_t left = (size_t)(com_reply_len - com_reply_at);
        if (offset < left)
            return com_reply_buf[com_reply_at + offset];
        offset -= left;
    }
    c = com_sources[src].peek ? com_sources[src].peek(offset) : -1;
    if (c < 0 && uart && !com_reply_at)
        return com_reply_peek_past(offset);
    return c;
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
    if (s == COM_SOURCE_UART)
        com_reply_len = com_reply_at = 0;
}

void HOST_IN_FLASH_FUNC com_rx_clear(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        com_rx_clear_source(s);
}

/* An answer that does not fit is dropped whole rather than cut short,
 * because half of an escape sequence is a sequence the reader parses as
 * something else. */
void HOST_IN_FLASH_FUNC com_reply(const char *s, size_t n)
{
    if (com_reply_quiet || n > sizeof com_reply_buf - com_reply_len)
        return;
    memcpy(&com_reply_buf[com_reply_len], s, n);
    com_reply_len += (uint8_t)n;
}

bool HOST_IN_FLASH_FUNC com_get_reply_quiet(void)
{
    return com_reply_quiet;
}

void HOST_IN_FLASH_FUNC com_set_reply_quiet(bool quiet)
{
    com_reply_quiet = quiet;
}

void HOST_IN_FLASH_FUNC com_set_reply_default(bool quiet)
{
    com_reply_default = quiet;
    com_reply_quiet = quiet;
}

void HOST_IN_FLASH_FUNC com_run(void)
{
    com_reply_quiet = com_reply_default;
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
        manifold_putchar(buf[i]);
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
        manifold_putchar(*p);
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

/* The console's terminal has had every byte sent, so its cursor tells. At
 * the last column, a pending wrap or not, CR LF is right. */
static bool com_line_clean(void)
{
    manifold_flush();
    return term_pos(term_shown(&cells_term))->x == 0;
}

void com_clean_line(void)
{
    if (!com_line_clean())
    {
        manifold_putchar('\r');
        manifold_putchar('\n');
    }
}

/* The soft reset starts with CAN, which ends any escape sequence left half
 * sent, and leaves the alternate screen, so the cursor read after it is the
 * one the CR LF lands on. The default cursor shape goes to the screen alone,
 * because a terminal on the UART or telnet may print it. */
void com_end_reset(void)
{
    static const char shape[] = "\33[0 q";
    manifold_flush();
    for (const char *s = STR_TERM_SOFT_RESET; *s; s++)
        manifold_putchar((unsigned char)*s);
    manifold_write_to(COM_TERM_SCREEN, shape, sizeof shape - 1);
    com_clean_line();
}

void com_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u8(c, (uint8_t)com_rx_held);
    sst_put_u64(c, com_rx_deadline);
    sst_put_bool(c, com_bel_enabled);
    sst_put_u8(c, (uint8_t)com_rx_staged);
    sst_put_u8(c, com_reply_len);
    sst_put_u8(c, com_reply_at);
    sst_put(c, com_reply_buf, sizeof com_reply_buf);
    sst_put_bool(c, com_reply_quiet);
}

bool com_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t held = sst_get_u8(c);
    uint64_t deadline = sst_get_u64(c);
    bool bel = sst_get_bool(c);
    uint8_t staged = sst_get_u8(c);
    uint8_t reply_len = sst_get_u8(c);
    uint8_t reply_at = sst_get_u8(c);
    uint8_t reply[sizeof com_reply_buf];
    sst_get(c, reply, sizeof reply);
    bool quiet = sst_get_bool(c);
    if (!sst_ok(c) || held > COM_SOURCE_ANY || staged >= COM_SOURCE_COUNT ||
        reply_len > sizeof com_reply_buf || (reply_at && reply_at >= reply_len))
        return false;
    com_rx_held = (com_source_t)held;
    com_rx_deadline = deadline;
    com_bel_enabled = bel;
    com_rx_staged = (com_source_t)staged;
    com_reply_len = reply_len;
    com_reply_at = reply_at;
    memcpy(com_reply_buf, reply, sizeof com_reply_buf);
    com_reply_quiet = quiet;
    return true;
}
