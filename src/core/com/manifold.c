/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold.h"
#include "core/com/com.h"
#include "core/com/manifold_tx.h"
#include "core/aud/bel.h"
#include "core/sys/ria.h"
#include "core/sys/ring.h"
#include "core/sys/timer.h"
#include "host/host.h"
#include "machine.h"

#include <stdint.h>
#include <unistd.h>

#define MANIFOLD_ETX 0x03

typedef RING(uint8_t, MANIFOLD_RING_SIZE) manifold_ring_t;

static manifold_ring_t manifold_keyboard_ring;
static manifold_ring_t manifold_uart_ring;

static char manifold_crlf_last;

static int (*manifold_term_out)(const char *buf, int len, bool force);
static void (*manifold_tx_tap)(const char *buf, int len);
static void (*manifold_std_tap)(int fd, const char *buf, int len);
static void (*manifold_stderr_sink)(const char *buf, int len);

/* The console's line runs as the Pico's UART does, 115200 baud and 8N1,
 * 11520 bytes a second each way, on the machine's own clock, so a savestate
 * carries it and a machine that falls behind real time falls behind with
 * it. A tick is a 11520th of a microsecond, so a byte is a million ticks
 * and none of it rounds. Each direction keeps the tick its line is free
 * from. */
#define MANIFOLD_BYTES_PER_S 11520u
#define MANIFOLD_BYTE_TICKS 1000000u
static bool manifold_paced = true;
static uint64_t manifold_tx_free;
static uint64_t manifold_rx_free;

static void (*manifold_wire_tx)(const char *buf, int len);
static size_t (*manifold_wire_rx)(char *buf, size_t max);
static bool manifold_wire_stream;
static bool manifold_wire_held;
static timer_mach_t manifold_wire_hold;

/* A ring's counters may be anything that holds no more than the ring. */
static bool manifold_sst_valid(sst_cursor_t *c)
{
    if (RING_LEN(&manifold_keyboard_ring) > MANIFOLD_RING_SIZE ||
        RING_LEN(&manifold_uart_ring) > MANIFOLD_RING_SIZE)
        return sst_refuse(c, "a ring holding more than it can");
    return true;
}

#define MANIFOLD_RING_ROWS(Y, r) \
    Y(U32, r.idx.wr) Y(U32, r.idx.rd) Y(BYTES, r.buf)
#define MANIFOLD_SST_ROWS(X)                                   \
    X(STRUCT, manifold_keyboard_ring, MANIFOLD_RING_ROWS)      \
    X(STRUCT, manifold_uart_ring, MANIFOLD_RING_ROWS)          \
    X(VALIDATE, manifold_sst_valid)                            \
    X(U8, manifold_crlf_last)                                  \
    X(U64, manifold_tx_free)                                   \
    X(U64, manifold_rx_free)                                   \
    X(SKIP, manifold_paced, "host wiring")                     \
    X(SKIP, manifold_term_out, "host wiring")                  \
    X(SKIP, manifold_tx_tap, "host wiring")                    \
    X(SKIP, manifold_std_tap, "host wiring")                   \
    X(SKIP, manifold_stderr_sink, "host wiring")               \
    X(SKIP, manifold_wire_tx, "host wiring")                   \
    X(SKIP, manifold_wire_rx, "host wiring")                   \
    X(SKIP, manifold_wire_stream, "host wiring")               \
    X(SKIP, manifold_wire_held, "host wiring")                 \
    X(SKIP, manifold_wire_hold, "host wiring")
SST_TABLE(manifold, MANIFOLD_SST_ROWS)

static void manifold_line_idle(uint64_t *free);

/* A failed load can leave a ring's counters holding more than the ring. The
 * input waiting is dropped, as COM's RESET drops it, and both lines start
 * idle. */
void manifold_reset(void)
{
    RING_RESET(&manifold_keyboard_ring);
    RING_RESET(&manifold_uart_ring);
    manifold_line_idle(&manifold_tx_free);
    manifold_line_idle(&manifold_rx_free);
}

/* A line starts paced from now, as an idle one. */
void manifold_set_paced(bool paced)
{
    manifold_paced = paced;
    manifold_line_idle(&manifold_tx_free);
    manifold_line_idle(&manifold_rx_free);
}

/* How many bytes the line can have started by now. An unpaced line takes
 * all of them. */
static size_t manifold_line_room(uint64_t free)
{
    if (!manifold_paced)
        return SIZE_MAX;
    const uint64_t now = host_clock_us() * MANIFOLD_BYTES_PER_S;
    if (free > now)
        return 0;
    const uint64_t n = (now - free) / MANIFOLD_BYTE_TICKS + 1;
    return n < SIZE_MAX ? (size_t)n : SIZE_MAX;
}

/* A line with nothing to send is idle, and its next byte starts no earlier
 * than now. */
static void manifold_line_idle(uint64_t *free)
{
    const uint64_t now = host_clock_us() * MANIFOLD_BYTES_PER_S;
    if (*free < now)
        *free = now;
}

size_t manifold_keyboard_read(char *buf, size_t length)
{
    return RING_OUT(&manifold_keyboard_ring, buf, (uint32_t)length);
}

int manifold_keyboard_peek(size_t offset)
{
    return offset < RING_LEN(&manifold_keyboard_ring)
               ? *RING_AT(&manifold_keyboard_ring, offset)
               : -1;
}

void manifold_keyboard_clear(void)
{
    RING_SKIP(&manifold_keyboard_ring, RING_LEN(&manifold_keyboard_ring));
}

size_t manifold_uart_read(char *buf, size_t length)
{
    return RING_OUT(&manifold_uart_ring, buf, (uint32_t)length);
}

int manifold_uart_peek(size_t offset)
{
    return offset < RING_LEN(&manifold_uart_ring)
               ? *RING_AT(&manifold_uart_ring, offset)
               : -1;
}

void manifold_uart_clear(void)
{
    RING_SKIP(&manifold_uart_ring, RING_LEN(&manifold_uart_ring));
}

void manifold_set_term_out(int (*out_chars)(const char *buf, int len, bool force))
{
    manifold_term_out = out_chars;
}

void manifold_set_tx_tap(void (*tap)(const char *buf, int len))
{
    manifold_tx_tap = tap;
}

void manifold_set_std_tap(void (*tap)(int fd, const char *buf, int len))
{
    manifold_std_tap = tap;
}

void manifold_set_stderr_sink(void (*sink)(const char *buf, int len))
{
    manifold_stderr_sink = sink;
}

void manifold_set_wire(void (*tx)(const char *buf, int len),
                       size_t (*rx)(char *buf, size_t max), bool stream)
{
    manifold_wire_tx = tx;
    manifold_wire_rx = rx;
    manifold_wire_stream = stream;
    manifold_wire_held = false;
    /* A wire just attached has sent nothing yet. */
    manifold_line_idle(&manifold_rx_free);
}

/* The UART is the only terminal here besides term.c. The UART's sinks are the
 * tx tap, the host wire and term.c; COM_TERM_SCREEN's is term.c alone. The
 * terminal takes bytes until one has to wait for a smooth scroll still
 * moving, unless force, and the other sinks get only what it took, so every
 * terminal shows the same output at the same time. Returns how many of buf
 * went out. */
static size_t manifold_emit(uint8_t to, const char *buf, size_t len, bool force)
{
    size_t took = len;
    if ((to & (COM_TERM_UART | COM_TERM_SCREEN)) && manifold_term_out)
        took = (size_t)manifold_term_out(buf, (int)len, force);
    if ((to & COM_TERM_UART) && took)
    {
        if (manifold_tx_tap)
            manifold_tx_tap(buf, (int)took);
        if (manifold_wire_tx)
            manifold_wire_tx(buf, (int)took);
    }
    return took;
}

/* What waits goes out, oldest first and in runs for the same terminals, as
 * far as the line and the terminal take it. With force, it all goes out at
 * once, line or no line, and a byte the terminal holds goes out all the
 * same, which ends the scroll it waits for. */
static void manifold_drain(bool force)
{
    size_t room = force ? SIZE_MAX : manifold_line_room(manifold_tx_free);
    size_t len;
    while ((len = manifold_tx_len()) && room)
    {
        char run[MANIFOLD_TX_SIZE];
        const uint8_t to = manifold_tx_at(0).to;
        size_t n = 0;
        for (; n < len && n < room; n++)
        {
            manifold_tx_entry_t e = manifold_tx_at(n);
            if (e.to != to)
                break;
            run[n] = (char)e.ch;
        }
        size_t sent = manifold_emit(to, run, n, force);
        manifold_tx_skip(sent);
        if (!force && manifold_paced)
        {
            manifold_tx_free += sent * MANIFOLD_BYTE_TICKS;
            room -= sent;
        }
        if (sent < n)
        {
            /* A line held for a smooth scroll is busy, not free: its next
             * byte goes a byte's time after the scroll lets it, as a UART
             * waiting on the terminal sends it. */
            manifold_line_idle(&manifold_tx_free);
            return;
        }
    }
    if (!manifold_tx_len())
        manifold_line_idle(&manifold_tx_free);
}

/* A write from inside a task cannot wait for frames, so a byte that must go
 * in ends a smooth scroll it would wait for. */
void manifold_tx_make_room(void)
{
    manifold_drain(true);
}

/* Output is written as it is made, as far as the terminal takes it. */
void manifold_tx_wrote(void)
{
    manifold_drain(false);
}

/* A newline that does not follow a CR goes in as CR LF, and what waits goes
 * out at once as far as it can. */
static void manifold_crlf_put(uint8_t to, char c)
{
    if (c == '\n' && manifold_crlf_last != '\r')
        manifold_tx_put(to, '\r');
    manifold_tx_put(to, c);
    manifold_crlf_last = c;
}

void manifold_putchar_raw(char c)
{
    if (!manifold_tx_try_put(COM_TERM_ALL, c))
        return;
    if (manifold_std_tap)
        manifold_std_tap(STDOUT_FILENO, &c, 1);
    manifold_tx_wrote();
}

int manifold_putchar(int c)
{
    manifold_crlf_put(COM_TERM_ALL, (char)c);
    manifold_tx_wrote();
    return (int)(unsigned char)c;
}

/* Writes what fits and returns the count, as a Pico does. */
size_t manifold_write(int fd, const char *buf, size_t n)
{
    size_t i = 0;
    for (; i < n && manifold_cts(); i++)
        manifold_crlf_put(COM_TERM_ALL, buf[i]);
    manifold_tx_wrote();
    if (manifold_std_tap)
        manifold_std_tap(fd, buf, (int)i);
    if (fd == STDERR_FILENO && manifold_stderr_sink)
        manifold_stderr_sink(buf, (int)i);
    return i;
}

/* What waits goes out now, ending a smooth scroll it waits for: the machine
 * cannot run frames until the scroll is over while a task waits here. */
void manifold_flush(void)
{
    manifold_drain(true);
}

bool manifold_drained(void)
{
    return !manifold_tx_len();
}

/* The emulated terminal is the console's own, so com takes its answers. */
void manifold_reply(const char *s, size_t n)
{
    com_reply(s, n);
}

void manifold_bell(void)
{
    if (com_get_bel())
        bel_add(&bel_teletype);
}

/* A Ctrl-C is scanned for before the bytes are pushed, so a break is seen
 * even when the ring is full and the bytes are dropped. */
static void manifold_scan_break(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == MANIFOLD_ETX)
            ria_trigger_sigint();
}

void manifold_keyboard_push(const char *s, size_t n)
{
    manifold_scan_break(s, n);
    if (n <= RING_AVAIL(&manifold_keyboard_ring))
        RING_IN(&manifold_keyboard_ring, s, (uint32_t)n);
}

void manifold_uart_push(const char *s, size_t n)
{
    manifold_scan_break(s, n);
    RING_IN(&manifold_uart_ring, s, (uint32_t)n);
}

size_t manifold_keyboard_free(void)
{
    return RING_AVAIL(&manifold_keyboard_ring);
}

/* A stream wire is not read while the ring is full. A console wire is left for
 * COM_HOLD_MS and then read into the full ring, where the bytes are dropped,
 * because a Ctrl-C typed behind the type-ahead has to be seen even though the
 * rest of what was typed is lost. */
void manifold_task(void)
{
    manifold_drain(false);
    if (!manifold_wire_rx)
        return;
    char buf[MANIFOLD_RING_SIZE];
    size_t room = RING_AVAIL(&manifold_uart_ring);
    if (room)
    {
        manifold_wire_held = false;
        size_t line = manifold_line_room(manifold_rx_free);
        if (room > line)
            room = line;
        if (!room)
            return;
        size_t n = manifold_wire_rx(buf, room < sizeof buf ? room : sizeof buf);
        if (!n)
        {
            manifold_line_idle(&manifold_rx_free);
            return;
        }
        if (manifold_paced)
            manifold_rx_free += n * MANIFOLD_BYTE_TICKS;
        if (manifold_wire_stream)
            RING_IN(&manifold_uart_ring, buf, (uint32_t)n);
        else
            manifold_uart_push(buf, n);
        return;
    }
    /* A full ring holds the sender up, so its line saves up no time. */
    manifold_line_idle(&manifold_rx_free);
    if (manifold_wire_stream)
        return;
    if (!manifold_wire_held)
    {
        manifold_wire_held = true;
        manifold_wire_hold = timer_mach_in_ms(COM_HOLD_MS);
    }
    if (!timer_mach_passed(manifold_wire_hold))
        return;
    size_t n = manifold_wire_rx(buf, sizeof buf);
    if (n)
        manifold_uart_push(buf, n);
}
