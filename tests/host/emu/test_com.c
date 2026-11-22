/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/aud/bel.h"
#include "core/com/manifold.h"
#include "core/hid/clip.h"
#include "core/hid/paste.h"
#include "core/hid/paste_host.h"
#include "core/hid/typed.h"
#include "core/ria/ria.h"
#include "core/com/com.h"
#include "core/com/tio.h"
#include "core/str/oem.h"
#include "core/str/str.h"
#include "core/sys/proc.h"
#include "core/sys/ria.h"
#include "core/sys/sys.h"
#include "core/sys/timer.h"
#include "core/term/draw.h"
#include "core/term/grid.h"
#include "core/term/vterm.h"
#include "host/host.h"
#include "core/vga/vga_emu.h"
#include "machine.h"
#include "utest.h"
#include <stdio.h>
#include <string.h>

/* The machine clock counts scanlines, so a wait on it has to advance the beam
 * with vga_task. */
static void machine_us(uint64_t us)
{
    timer_mach_t d = timer_mach_in_us(us);
    while (!timer_mach_passed(d))
        vga_task();
}

static void dwell(void)
{
    machine_us(2 * COM_DWELL_US);
}

/* A byte waits in some source, a held terminal answer included. */
static bool queued(void)
{
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (com_peek(s, 0) >= 0)
            return true;
    return false;
}

static void drain(void)
{
    char buf[MANIFOLD_RING_SIZE * 2];
    timer_mach_t give_up = timer_mach_in_ms(50);
    while (queued() && !timer_mach_passed(give_up))
    {
        com_stdin_read(buf, sizeof buf);
        vga_task();
    }
}

UTEST(com, a_source_keeps_the_reader_until_it_runs_dry_and_a_dwell_past)
{
    com_rx_clear();
    typed_push("abc", 3);
    char one[1];
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'a');
    manifold_uart_push("XY", 2);
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'b');
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'c');
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)0);
    dwell();
    ASSERT_EQ(com_stdin_read(one, 1), (size_t)1);
    ASSERT_EQ(one[0], 'X');
    drain();
}

UTEST(com, one_read_is_one_source_and_the_keyboard_goes_first)
{
    com_rx_clear();
    typed_push("ab", 2);
    manifold_uart_push("XY", 2);
    char buf[8];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "ab", 2), 0);
    dwell();
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "XY", 2), 0);
    drain();
}

UTEST(com, a_terminal_answer_never_jumps_ahead_of_the_wire)
{
    com_rx_clear();
    manifold_uart_push("hello", 5);
    com_reply("\33[1;1R", 6);
    char buf[32];
    size_t n = com_stdin_read(buf, sizeof buf);
    ASSERT_EQ(n, (size_t)11);
    ASSERT_EQ(memcmp(buf, "hello\33[1;1R", 11), 0);
    drain();
}

UTEST(com, a_held_answer_is_read_by_the_source_that_owes_it)
{
    com_rx_clear();
    com_reply("\33[0n", 4);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), 0x1b);
    com_source_t src = COM_SOURCE_UART;
    ASSERT_EQ(com_getchar(&src), 0x1b);
    ASSERT_EQ(src, COM_SOURCE_UART);
    drain();
}

UTEST(com, each_row_says_how_much_its_queue_holds)
{
    ASSERT_EQ(com_queue_size(COM_SOURCE_KEYBOARD), (size_t)TYPED_QUEUE_SIZE);
    ASSERT_EQ(com_queue_size(COM_SOURCE_UART), (size_t)MANIFOLD_RING_SIZE);
    ASSERT_EQ(com_queue_size(COM_SOURCE_TEL), (size_t)0);
}

UTEST(com, a_peek_at_an_offset_takes_nothing)
{
    com_rx_clear();
    typed_push("abc", 3);
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), 'a');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 2), 'c');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 3), -1);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    ASSERT_EQ(com_peek(COM_SOURCE_TEL, 0), -1);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "abc", 3), 0);
}

/* A held answer is peeked where the read delivers it, after the ring. */
UTEST(com, a_uart_peek_sees_a_held_answer_after_the_ring)
{
    com_rx_clear();
    manifold_uart_push("hi", 2);
    com_reply("\33[0n", 4);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 1), 'i');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 2), 0x1b);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 5), 'n');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 6), -1);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, sizeof buf), (size_t)6);
    ASSERT_EQ(memcmp(buf, "hi\33[0n", 6), 0);
    drain();
}

UTEST(com, a_staged_byte_is_offset_zero_of_its_own_source)
{
    com_rx_clear();
    typed_push("kl", 2);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), 'k');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 1), 'l');
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 2), -1);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), -1);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf), (size_t)2);
    ASSERT_EQ(memcmp(buf, "kl", 2), 0);
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
}

UTEST(com, a_named_read_leaves_the_other_sources_queued)
{
    com_rx_clear();
    typed_push("ab", 2);
    manifold_uart_push("XYZ", 3);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, 2), (size_t)2);
    ASSERT_EQ(memcmp(buf, "XY", 2), 0);
    ASSERT_EQ(com_read(COM_SOURCE_TEL, buf, sizeof buf), (size_t)0);
    ASSERT_EQ(com_peek(COM_SOURCE_KEYBOARD, 0), 'a');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), 'Z');
    drain();
}

/* Once an answer's first byte is read, UART bytes that arrive after wait
 * behind the rest of it, the byte staged in the window included. */
UTEST(com, an_answer_started_is_never_split_by_the_wire)
{
    com_rx_clear();
    com_reply("\33[0n", 4);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, 2), (size_t)2);
    ASSERT_EQ(memcmp(buf, "\33[", 2), 0);
    manifold_uart_push("xy", 2);
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 0), '0');
    ASSERT_EQ(com_peek(COM_SOURCE_UART, 2), 'x');
    com_source_t src = COM_SOURCE_UART;
    ASSERT_EQ(com_getchar(&src), '0');
    ASSERT_TRUE(com_rx_stage());
    ASSERT_EQ(com_rx_peek(COM_SOURCE_UART), 'n');
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "nxy", 3), 0);
    drain();
}

/* An answer waits behind the UART bytes that came before it. */
UTEST(com, an_answer_not_started_waits_behind_the_wire)
{
    com_rx_clear();
    manifold_uart_push("ab", 2);
    com_reply("\33[0n", 4);
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, 1), (size_t)1);
    manifold_uart_push("c", 1);
    ASSERT_EQ(com_read(COM_SOURCE_UART, buf, sizeof buf), (size_t)6);
    ASSERT_EQ(memcmp(buf, "bc\33[0n", 6), 0);
    drain();
}

UTEST(com, an_answer_too_big_to_hold_is_dropped_whole)
{
    com_rx_clear();
    char big[257];
    memset(big, 'x', sizeof big);
    com_reply(big, sizeof big);
    char buf[MANIFOLD_RING_SIZE];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)0);
    drain();
}

/* A lone ESC is no item, but the UART's peek looks past it into a terminal
 * answer held behind it, and the two make whole items. */
UTEST(com, tio_is_idle_with_a_lone_esc_until_an_answer_is_held_behind_it)
{
    com_rx_clear();
    ASSERT_TRUE(tio_idle());
    manifold_uart_push("\33", 1);
    ASSERT_TRUE(tio_idle());
    com_reply("\33[0n", 4);
    ASSERT_FALSE(tio_idle());
    com_rx_clear();
    ASSERT_TRUE(tio_idle());
}

UTEST(com, a_break_during_a_paste_cancels_the_drip_too)
{
    com_rx_clear();
    char big[TYPED_QUEUE_SIZE * 4];
    memset(big, 'p', sizeof big - 1);
    big[sizeof big - 1] = 0;
    paste_start(big);
    paste_task();
    ASSERT_TRUE(paste_busy());
    sys_break_request();
    sys_commit();
    ASSERT_FALSE(paste_busy());
    paste_task();
    ASSERT_FALSE(queued());
}

/* With bracketed paste set, a paste arrives between ESC [ 200 ~ and
 * ESC [ 201 ~, and an ESC in the text is left out. */
UTEST(com, a_bracketed_paste_is_marked_and_cannot_end_early)
{
    com_rx_clear();
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004h", 8);
    paste_start("a\33[201~b");
    paste_task();
    char buf[64] = {0};
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf - 1), (size_t)19);
    ASSERT_STREQ(buf, "\33[200~a[201~b\33[201~");
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
    com_rx_clear();
}

/* A script's typing is typed as written: under bracketed paste it is not
 * bracketed, an ESC in it stays, and a CR or LF stays what it is. */
UTEST(com, typed_text_is_never_bracketed)
{
    com_rx_clear();
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004h", 8);
    typed_text("a\33[Ab\rc\n");
    paste_task();
    char buf[64] = {0};
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf - 1), (size_t)8);
    ASSERT_STREQ(buf, "a\33[Ab\rc\n");
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
    com_rx_clear();
}

/* A paste presses Enter for each line end, CRLF, CR or LF. */
UTEST(com, a_pasted_line_end_is_enter)
{
    com_rx_clear();
    paste_start("a\r\nb\nc\rd");
    paste_task();
    char buf[64] = {0};
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf - 1), (size_t)7);
    ASSERT_STREQ(buf, "a\rb\rc\rd");
    com_rx_clear();
}

/* New text cuts the paste in progress short and follows it, and empty text
 * ends it. */
UTEST(com, empty_text_ends_the_paste_in_progress)
{
    com_rx_clear();
    paste_start("abc");
    paste_start("");
    ASSERT_FALSE(paste_busy());
    typed_text("abc");
    typed_text("");
    ASSERT_FALSE(paste_busy());
    typed_text("abc");
    paste_start(NULL);
    ASSERT_FALSE(paste_busy());
    paste_task();
    char buf[8];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf), (size_t)0);
    com_rx_clear();
}

/* All the keyboard row gives, the paste staged as the main loop stages it
 * between reads. */
static size_t keyboard_all(char *out, size_t max)
{
    size_t got = 0;
    for (int i = 0; i < 10000 && got < max; i++)
    {
        paste_task();
        size_t n = com_read(COM_SOURCE_KEYBOARD, out + got, max - got);
        got += n;
        if (!n && !paste_busy())
            break;
    }
    return got;
}

/* Host text with nothing but control characters is a paste with no text,
 * which sends nothing, not even brackets, as an empty buffer's does. */
UTEST(com, a_paste_of_only_controls_sends_no_brackets)
{
    com_rx_clear();
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004h", 8);
    typed_push("ab", 2);
    paste_start("\33\3\x7f");
    typed_push("cd", 2);
    char buf[16] = {0};
    ASSERT_EQ(keyboard_all(buf, sizeof buf - 1), (size_t)4);
    ASSERT_STREQ(buf, "abcd");
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
}

/* Every control character but tab and the line ends leaves a paste: the
 * C0 controls, DEL and the C1 controls, so ESC, Ctrl-C and CSI too, and the
 * Ctrl-C raises no SIGINT. */
UTEST(com, a_paste_leaves_out_control_characters)
{
    com_rx_clear();
    ria_get_sigint();
    paste_start("a\33[1mb\3c\xc2\x9b" "d\te\x7f" "f\r\n");
    char buf[64] = {0};
    ASSERT_EQ(keyboard_all(buf, sizeof buf - 1), (size_t)11);
    ASSERT_STREQ(buf, "a[1mbcd\tef\r");
    ASSERT_FALSE(ria_get_sigint());
}

/* The console and the keyboard in CP437, which this test runs without the
 * machine's start to choose. */
static void cp437(void)
{
    oem_set_code_page_run(437);
    vterm_load_code_page(437);
}

/* A pasted character becomes the code page's byte, and one the code page
 * lacks, or a malformed sequence, '?'. */
UTEST(com, a_paste_spells_what_the_code_page_cannot_as_a_question_mark)
{
    com_rx_clear();
    cp437();
    paste_start("\xc3\xa9\xe4\xb8\xad\xc3!");
    char buf[8] = {0};
    ASSERT_EQ(keyboard_all(buf, sizeof buf - 1), (size_t)4);
    ASSERT_EQ((unsigned char)buf[0], 0x82); /* CP437 e acute */
    ASSERT_STREQ(buf + 1, "?\?!");
}

/* Keys typed before a paste go first, then the paste, then the keys typed
 * while it went in. */
UTEST(com, keys_typed_during_a_paste_arrive_after_it)
{
    com_rx_clear();
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004h", 8);
    typed_push("ab", 2);
    paste_start("paste");
    typed_push("cd", 2);
    char buf[64] = {0};
    ASSERT_EQ(keyboard_all(buf, sizeof buf - 1), (size_t)21);
    ASSERT_STREQ(buf, "ab\33[200~paste\33[201~cd");
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
}

/* A paste longer than the typed queue arrives whole. Keys typed during it
 * fill the queue behind it, and past that are dropped. */
UTEST(com, a_paste_longer_than_the_queue_arrives_whole)
{
    com_rx_clear();
    static char big[TYPED_QUEUE_SIZE * 3 + 1];
    for (size_t i = 0; i < sizeof big - 1; i++)
        big[i] = (char)('a' + i % 26);
    paste_start(big);
    paste_task();
    char keys[TYPED_QUEUE_SIZE];
    memset(keys, 'k', sizeof keys);
    typed_push(keys, sizeof keys);
    typed_push("z", 1);
    static char out[sizeof big + TYPED_QUEUE_SIZE + 8];
    ASSERT_EQ(keyboard_all(out, sizeof out), sizeof big - 1 + TYPED_QUEUE_SIZE);
    ASSERT_EQ(memcmp(out, big, sizeof big - 1), 0);
    ASSERT_EQ(memcmp(out + sizeof big - 1, keys, sizeof keys), 0);
}

/* A Ctrl-C typed during a paste raises the SIGINT at once and ends the
 * paste: what was staged goes, then the closing bracket, then the Ctrl-C. */
UTEST(com, a_ctrl_c_ends_a_paste_with_its_closing_bracket)
{
    com_rx_clear();
    ria_get_sigint();
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004h", 8);
    static char big[TYPED_QUEUE_SIZE * 3 + 1];
    memset(big, 'p', sizeof big - 1);
    paste_start(big);
    paste_task();
    char head[7] = {0};
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, head, 6), (size_t)6);
    ASSERT_STREQ(head, "\33[200~");
    typed_push("\3", 1);
    ASSERT_TRUE(ria_get_sigint());
    static char out[TYPED_QUEUE_SIZE + 8];
    const size_t staged = TYPED_QUEUE_SIZE;
    ASSERT_EQ(keyboard_all(out, sizeof out), staged + 7);
    for (size_t i = 0; i < staged; i++)
        ASSERT_EQ(out[i], 'p');
    ASSERT_EQ(memcmp(out + staged, "\33[201~\3", 7), 0);
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
}

static void show(const char *s)
{
    manifold_write_to(COM_TERM_SCREEN, s, strlen(s));
}

/* The text of the screen showing, from row y0, column x0 up to row y1,
 * column x1, copied into buffer which. */
static void copy(uint8_t which, uint8_t y0, uint8_t x0, uint8_t y1, uint8_t x1)
{
    term_job_finish(&vterm);
    clip_copy(which, vterm_get_shown(&vterm)->shown, y0, x0, y1, x1);
}

/* The primary is the machine's: a paste of it types its text, a line end
 * as Enter. */
UTEST(com, the_primary_pastes_what_was_copied)
{
    com_rx_clear();
    show("\33[H\33[2Jone\r\ntwo\r\n");
    copy(CLIP_PRIMARY, 0, 0, 1, 3);
    clip_paste(CLIP_PRIMARY);
    char buf[16] = {0};
    ASSERT_EQ(keyboard_all(buf, sizeof buf - 1), (size_t)7);
    ASSERT_STREQ(buf, "one\rtwo");
}

/* The clipboard is the host's: a copy gives it each glyph's Unicode, and a
 * paste of it turns each character back into the code page's. */
UTEST(com, the_clipboard_is_the_host_s)
{
    com_rx_clear();
    cp437();
    show("\33[H\33[2Ja\33(0q\33(B\x82\r\nb\r\n");
    copy(CLIP_CLIPBOARD, 0, 0, 2, 0);
    ASSERT_STREQ(host_clipboard_get(), "a\xe2\x94\x80\xc3\xa9\nb\n");
    clip_paste(CLIP_CLIPBOARD);
    char buf[16] = {0};
    ASSERT_EQ(keyboard_all(buf, sizeof buf - 1), (size_t)6);
    ASSERT_STREQ(buf, "a\xc4\x82\rb\r");
}

/* Setting the host's clipboard can write over the text a paste of it
 * reads, so it ends that paste first: what was staged goes, then the
 * closing bracket, and nothing of the new text. */
UTEST(com, setting_the_host_clipboard_ends_a_paste_of_it)
{
    com_rx_clear();
    show("\33[?2004h");
    static char big[TYPED_QUEUE_SIZE * 3 + 1];
    memset(big, 'p', sizeof big - 1);
    ASSERT_TRUE(clip_host_set(big));
    clip_paste(CLIP_CLIPBOARD);
    paste_task();
    ASSERT_TRUE(clip_host_set("new"));
    static char out[TYPED_QUEUE_SIZE + 16];
    ASSERT_EQ(keyboard_all(out, sizeof out), (size_t)TYPED_QUEUE_SIZE + 12);
    ASSERT_EQ(memcmp(out, "\33[200~pppp", 10), 0);
    ASSERT_EQ(out[TYPED_QUEUE_SIZE + 5], 'p');
    ASSERT_EQ(memcmp(out + TYPED_QUEUE_SIZE + 6, "\33[201~", 6), 0);
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
}

/* Text typed as written is not the clipboard's, so it goes on whole. */
UTEST(com, setting_the_host_clipboard_leaves_typed_text_be)
{
    com_rx_clear();
    static char big[TYPED_QUEUE_SIZE * 3 + 1];
    memset(big, 't', sizeof big - 1);
    typed_text(big);
    paste_task();
    ASSERT_TRUE(clip_host_set("new"));
    static char out[sizeof big + 8];
    ASSERT_EQ(keyboard_all(out, sizeof out), sizeof big - 1);
    ASSERT_EQ(memcmp(out, big, sizeof big - 1), 0);
}

/* A copy into the buffer a paste is reading ends that paste, with its
 * closing bracket. */
UTEST(com, a_copy_into_the_buffer_ends_its_paste)
{
    com_rx_clear();
    show("\33[?2004h\33[H\33[2J");
    for (int y = 0; y < 10; y++)
    {
        static const char row[] = "0123456789012345678901234567890123456789\r\n";
        show(row);
    }
    copy(CLIP_PRIMARY, 0, 0, 10, 0);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    copy(CLIP_PRIMARY, 0, 0, 0, 2);
    static char out[TYPED_QUEUE_SIZE + 16];
    const size_t got = keyboard_all(out, sizeof out);
    ASSERT_EQ(got, (size_t)TYPED_QUEUE_SIZE + 12);
    ASSERT_EQ(memcmp(out, "\33[200~0123456789", 16), 0);
    ASSERT_EQ(memcmp(out + got - 6, "\33[201~", 6), 0);
    manifold_write_to(COM_TERM_SCREEN, "\33[?2004l", 8);
}

/* A key's sequence that does not fit is dropped whole. */
UTEST(com, a_full_typed_queue_never_cuts_a_push)
{
    com_rx_clear();
    char fill[TYPED_QUEUE_SIZE - 2];
    memset(fill, 'f', sizeof fill);
    typed_push(fill, sizeof fill);
    ASSERT_EQ(typed_free(), (size_t)2);
    typed_push("\33[A", 3);
    ASSERT_EQ(typed_free(), (size_t)2);
    typed_push("ab", 2);
    ASSERT_EQ(typed_free(), (size_t)0);
    com_rx_clear();
}

UTEST(com, a_byte_the_window_staged_comes_back_to_its_own_source)
{
    com_rx_clear();
    typed_push("k", 1);
    /* When bit 6 is clear and a console byte is waiting, reading $FFE0 stages
     * that byte in $FFE2 and sets bit 6. */
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_UART), -1);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), 'k');
    char buf[1];
    ASSERT_EQ(com_rx_reclaim(buf, 1, COM_SOURCE_UART), (size_t)0);
    ASSERT_EQ(com_rx_reclaim(buf, 0, COM_SOURCE_KEYBOARD), (size_t)0);
    ASSERT_EQ(com_rx_reclaim(buf, 1, COM_SOURCE_KEYBOARD), (size_t)1);
    ASSERT_EQ(buf[0], 'k');
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
}

UTEST(com, a_break_drops_what_the_window_staged)
{
    com_rx_clear();
    typed_push("k", 1);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ria_break();
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* An arrow pressed in a program that reads only the keyboard bitmap: its ESC
 * waits in the window and the rest in the keyboard's queue. A stop with no ROM
 * launching drops all of it. */
UTEST(com, a_stop_drops_what_the_program_never_read)
{
    com_rx_clear();
    typed_push("\33[A", 3);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    proc_stop();
    ASSERT_FALSE(proc_exec_inflight());
    /* The chain ended; its end goes out now, as com_task would send it. */
    com_end_now();
    ASSERT_FALSE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* A stop for an exec keeps the type-ahead, the byte in the window included. */
UTEST(com, an_exec_keeps_the_type_ahead)
{
    com_rx_clear();
    typed_push("ab", 2);
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    arg_clear();
    arg_append("next.rp6502");
    arg_push_xstack();
    proc_api_exec();
    proc_stop();
    ASSERT_TRUE(proc_exec_inflight());
    ASSERT_TRUE(ria_reg_read(0xFFE0) & 0x40);
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), 'a');
    proc_exec_init();
    arg_clear();
    proc_stop();
    ASSERT_FALSE(proc_exec_inflight());
    /* The chain ended; its end goes out now, as com_task would send it. */
    com_end_now();
    ASSERT_EQ(com_rx_peek(COM_SOURCE_KEYBOARD), -1);
}

/* A paste goes with the rest of the type-ahead. */
UTEST(com, a_paste_goes_only_where_the_type_ahead_goes)
{
    com_rx_clear();
    paste_start("for the program");
    arg_clear();
    arg_append("next.rp6502");
    arg_push_xstack();
    proc_api_exec();
    proc_stop();
    ASSERT_TRUE(proc_exec_inflight());
    ASSERT_TRUE(paste_busy());
    proc_exec_init();
    arg_clear();
    proc_stop();
    ASSERT_FALSE(proc_exec_inflight());
    /* The chain ended; its end goes out now, as com_task would send it. */
    com_end_now();
    ASSERT_FALSE(paste_busy());
}

static const char *wire_data;
static size_t wire_len, wire_pos, wire_asked_max;

static size_t wire_rx(char *buf, size_t max)
{
    if (max > wire_asked_max)
        wire_asked_max = max;
    size_t n = wire_len - wire_pos;
    if (n > max)
        n = max;
    memcpy(buf, wire_data + wire_pos, n);
    wire_pos += n;
    return n;
}

static void wire(const char *data, size_t len, bool stream)
{
    wire_data = data;
    wire_len = len;
    wire_pos = 0;
    wire_asked_max = 0;
    manifold_set_wire(NULL, wire_rx, stream);
}

static void unwire(void)
{
    manifold_set_wire(NULL, NULL, false);
}

static size_t read_all(char *out, size_t max)
{
    size_t got = 0;
    for (int pass = 0; pass < 4000 && (wire_pos < wire_len || queued()); pass++)
    {
        manifold_task();
        got += com_stdin_read(out + got, max - got);
    }
    return got;
}

UTEST(com, a_ctrl_c_on_a_console_wire_latches_at_the_fill)
{
    com_rx_clear();
    ria_get_sigint();
    wire("ab\3", 3, false);
    manifold_task();
    ASSERT_TRUE(ria_get_sigint());
    char buf[8];
    ASSERT_EQ(com_stdin_read(buf, sizeof buf), (size_t)3);
    ASSERT_EQ(memcmp(buf, "ab\3", 3), 0);
    unwire();
}

UTEST(com, a_full_console_wire_is_held_then_read_to_drop)
{
    com_rx_clear();
    ria_get_sigint();
    static char data[MANIFOLD_RING_SIZE + 100 + 1];
    memset(data, 'x', MANIFOLD_RING_SIZE);
    memset(data + MANIFOLD_RING_SIZE, 'y', 100);
    data[sizeof data - 1] = 3;
    wire(data, sizeof data, false);
    for (int i = 0; i < 10; i++)
        manifold_task();
    ASSERT_EQ(wire_pos, (size_t)MANIFOLD_RING_SIZE);
    ASSERT_FALSE(ria_get_sigint());
    machine_us((COM_HOLD_MS + 20) * 1000);
    manifold_task();
    ASSERT_EQ(wire_pos, sizeof data);
    ASSERT_TRUE(ria_get_sigint());
    static char out[MANIFOLD_RING_SIZE * 2];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, (size_t)MANIFOLD_RING_SIZE);
    for (size_t i = 0; i < got; i++)
        ASSERT_EQ(out[i], 'x');
    unwire();
}

UTEST(com, a_reader_that_keeps_up_loses_nothing_to_a_console_wire)
{
    com_rx_clear();
    ria_get_sigint();
    static char data[600];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    wire(data, sizeof data, false);
    static char out[sizeof data + 1];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, sizeof data);
    ASSERT_EQ(memcmp(out, data, sizeof data), 0);
    ASSERT_LE(wire_asked_max, (size_t)MANIFOLD_RING_SIZE);
    ASSERT_FALSE(ria_get_sigint());
    unwire();
}

UTEST(com, a_stream_never_signals_and_never_loses_a_byte)
{
    com_rx_clear();
    ria_get_sigint();
    static char data[5 + 600];
    memcpy(data, "ab\3cd", 5);
    for (size_t i = 5; i < sizeof data; i++)
        data[i] = (char)('a' + i % 26);
    wire(data, sizeof data, true);
    for (int i = 0; i < 10; i++)
        manifold_task();
    ASSERT_LE(wire_pos, (size_t)MANIFOLD_RING_SIZE);
    ASSERT_FALSE(ria_get_sigint());
    static char out[sizeof data + 1];
    size_t got = read_all(out, sizeof out);
    ASSERT_EQ(got, sizeof data);
    ASSERT_EQ(memcmp(out, data, sizeof data), 0);
    ASSERT_FALSE(ria_get_sigint());
    unwire();
}

static char sent[128];
static size_t sent_len;

static void sent_tap(const char *buf, int len)
{
    memcpy(sent + sent_len, buf, (size_t)len);
    sent_len += (size_t)len;
}

static void tap(void)
{
    sent_len = 0;
    manifold_set_tx_tap(sent_tap);
}

static void untap(void)
{
    manifold_set_tx_tap(NULL);
}

UTEST(com, a_write_to_the_uart_reaches_it_as_it_is)
{
    tap();
    manifold_write_to(COM_TERM_UART, "\33[6n\n", 5);
    ASSERT_EQ(sent_len, (size_t)5);
    ASSERT_EQ(memcmp(sent, "\33[6n\n", 5), 0);
    untap();
}

UTEST(com, a_write_to_telnet_never_reaches_the_uart)
{
    tap();
    manifold_write_to(COM_TERM_TEL, "\33[6n", 4);
    ASSERT_EQ(sent_len, (size_t)0);
    untap();
}

UTEST(com, a_clean_line_breaks_only_off_column_1)
{
    manifold_putchar('a');
    manifold_putchar('b');
    tap();
    com_clean_line();
    ASSERT_EQ(sent_len, (size_t)2);
    ASSERT_EQ(memcmp(sent, "\r\n", 2), 0);
    manifold_putchar('a');
    manifold_putchar('b');
    manifold_putchar('\r');
    manifold_putchar('\n');
    tap();
    com_clean_line();
    ASSERT_EQ(sent_len, (size_t)0);
    untap();
}

UTEST(com, a_clean_line_counts_a_write_to_the_screen_alone)
{
    com_clean_line();
    manifold_write_to(COM_TERM_SCREEN, "x", 1);
    tap();
    com_clean_line();
    ASSERT_EQ(sent_len, (size_t)2);
    ASSERT_EQ(memcmp(sent, "\r\n", 2), 0);
    untap();
}

/* The soft reset leaves the alternate screen, and the cursor comes back with
 * it, so where the alternate screen left it decides. */
static bool end_reset_breaks(const char *primary, const char *alternate)
{
    for (const char *p = "\33[2J\33[H"; *p; p++)
        manifold_putchar(*p);
    for (const char *p = primary; *p; p++)
        manifold_putchar(*p);
    for (const char *p = "\33[?47h"; *p; p++)
        manifold_putchar(*p);
    for (const char *p = alternate; *p; p++)
        manifold_putchar(*p);
    tap();
    com_end_reset();
    com_end_now();
    untap();
    return sent_len >= 2 && !memcmp(&sent[sent_len - 2], "\r\n", 2);
}

UTEST(com, the_end_reset_reads_the_cursor_after_leaving_the_alternate_screen)
{
    ASSERT_TRUE(end_reset_breaks("", "xy"));
    ASSERT_FALSE(end_reset_breaks("", "xy\r"));
    /* What the alternate screen had on its rows does not count. */
    ASSERT_FALSE(end_reset_breaks("", "\r\n\n\nlower\33[H"));
    /* The cursor's own row has something on it, wherever the cursor is. */
    ASSERT_TRUE(end_reset_breaks("xy", "\r"));
}

/* Where the cursor is after the reset, and what the reset sent after the
 * soft reset's own bytes. */
static const char *end_reset_after(const char *drawn, int *row)
{
    static char tail[80];
    for (const char *p = "\33[2J\33[H"; *p; p++)
        manifold_putchar(*p);
    for (const char *p = drawn; *p; p++)
        manifold_putchar(*p);
    tap();
    com_end_reset();
    com_end_now();
    untap();
    manifold_flush();
    const size_t reset = strlen(STR_TERM_SOFT_RESET);
    const size_t n = sent_len > reset ? sent_len - reset : 0;
    memcpy(tail, sent + reset, n < sizeof tail - 1 ? n : sizeof tail - 1);
    tail[n < sizeof tail - 1 ? n : sizeof tail - 1] = 0;
    /* A cursor not in the first column is a row past any there is. */
    *row = term_pos(vterm_get_shown(&vterm))->x ? 99 : term_pos(vterm_get_shown(&vterm))->y;
    return tail;
}

/* A program that drew below its cursor leaves the prompt a row of its own:
 * the first with nothing on it or below it. */
UTEST(com, the_end_reset_leaves_a_clear_line)
{
    int row;
    /* Nothing drawn: it stays, and sends nothing. */
    ASSERT_STREQ(end_reset_after("", &row), "");
    ASSERT_EQ(row, 0);
    /* Text on the cursor's row alone, as before: a CR LF. */
    ASSERT_STREQ(end_reset_after("text", &row), "\r\n");
    ASSERT_EQ(row, 1);
    /* Rows 2 to 5 drawn, and the cursor back on row 1, in column 4. */
    ASSERT_STREQ(end_reset_after("\33[3;1Ha\33[6;9Hb\33[2;5H", &row), "\r\n\r\n\r\n\r\n\r\n");
    ASSERT_EQ(row, 6);
    /* The cursor below everything drawn stays. */
    ASSERT_STREQ(end_reset_after("\33[3;1Ha\33[9;1H", &row), "");
    ASSERT_EQ(row, 8);
    /* A row erased to a color is not a clear one. */
    ASSERT_STREQ(end_reset_after("\33[4;1H\33[44m\33[K\33[0m\33[2;1H", &row), "\r\n\r\n\r\n");
    ASSERT_EQ(row, 4);
    /* Nor one with an underlined space on it. */
    ASSERT_STREQ(end_reset_after("\33[3;1H\33[4m \33[0m\33[H", &row), "\r\n\r\n\r\n");
    ASSERT_EQ(row, 3);
    /* Nor one erased in iCE colors with blink on, to the bright
     * background. */
    ASSERT_STREQ(end_reset_after("\33[?33h\33[4;1H\33[5m\33[K\33[0m\33[?33l\33[2;1H", &row),
                 "\r\n\r\n\r\n");
    ASSERT_EQ(row, 4);
    /* Nor one with a space whose colors are swapped, though both are the
     * default background: concealed, then reversed by DECCARA. */
    ASSERT_STREQ(end_reset_after("\33[3;1H\33[8m \33[0m\33[3;1;3;1;7$r\33[H", &row),
                 "\r\n\r\n\r\n");
    ASSERT_EQ(row, 3);
    /* A row drawn and erased again is clear. */
    ASSERT_STREQ(end_reset_after("\33[3;1Hgone\33[2K\33[H", &row), "");
    ASSERT_EQ(row, 0);
}

/* The last row in use is the bottom one, so the line feeds scroll. */
UTEST(com, the_end_reset_scrolls_for_a_clear_line)
{
    int row;
    const int bottom = vterm_get_shown(&vterm)->height - 1;
    char drawn[32];
    snprintf(drawn, sizeof drawn, "\33[%d;1Hlast\33[%d;1H", bottom + 1, bottom);
    ASSERT_STREQ(end_reset_after(drawn, &row), "\r\n\r\n");
    ASSERT_EQ(row, bottom);
    ASSERT_FALSE(screen_row_used(vterm_get_shown(&vterm)->shown, (uint8_t)bottom));
    ASSERT_TRUE(screen_row_used(vterm_get_shown(&vterm)->shown, (uint8_t)(bottom - 1)));
}

/* A program that ends with a left margin set leaves the prompt on the
 * first column of the screen, not on the margin. */
UTEST(com, the_end_reset_takes_the_left_and_right_margins_away)
{
    com_clean_line();
    for (const char *p = "\33[?69h\33[11;20s\33[5;15Htext"; *p; p++)
        manifold_putchar(*p);
    com_end_reset();
    com_end_now();
    manifold_flush();
    ASSERT_EQ(term_pos(vterm_get_shown(&vterm))->x, 0);
    for (const char *p = "\33[?69h\33[11;20s\33[5;11H"; *p; p++)
        manifold_putchar(*p);
    com_end_reset();
    com_end_now();
    manifold_flush();
    ASSERT_EQ(term_pos(vterm_get_shown(&vterm))->x, 0);
}

UTEST(com, a_write_to_one_terminal_lands_between_the_output_around_it)
{
    tap();
    manifold_putchar('a');
    manifold_write_to(COM_TERM_UART, "b", 1);
    manifold_putchar('c');
    ASSERT_EQ(sent_len, (size_t)3);
    ASSERT_EQ(memcmp(sent, "abc", 3), 0);
    untap();
}

static char screen[128];
static size_t screen_len;

static int screen_out(const char *buf, int len, bool force)
{
    (void)force;
    memcpy(screen + screen_len, buf, (size_t)len);
    screen_len += (size_t)len;
    return len;
}

static char wired[128];
static size_t wired_len;

static void wired_tx(const char *buf, int len)
{
    memcpy(wired + wired_len, buf, (size_t)len);
    wired_len += (size_t)len;
}

UTEST(com, a_write_to_the_screen_reaches_term_c_alone)
{
    tap();
    screen_len = 0;
    wired_len = 0;
    manifold_set_term_out(screen_out);
    manifold_set_wire(wired_tx, NULL, false);
    manifold_write_to(COM_TERM_SCREEN, "\33[6n\n", 5);
    ASSERT_EQ(screen_len, (size_t)5);
    ASSERT_EQ(memcmp(screen, "\33[6n\n", 5), 0);
    ASSERT_EQ(sent_len, (size_t)0);
    ASSERT_EQ(wired_len, (size_t)0);
    manifold_write_to(COM_TERM_UART, "u", 1);
    ASSERT_EQ(screen_len, (size_t)6);
    ASSERT_EQ(sent_len, (size_t)1);
    ASSERT_EQ(wired_len, (size_t)1);
    unwire();
    draw_init();
    untap();
}

/* A terminal on the UART may print a shape, so the default shape reaches
 * term.c alone. */
UTEST(com, the_end_reset_puts_the_default_shape_on_the_screen_alone)
{
    com_clean_line();
    tap();
    screen_len = 0;
    manifold_set_term_out(screen_out);
    com_end_reset();
    com_end_now();
    ASSERT_EQ(screen_len, sent_len + 5);
    ASSERT_EQ(memcmp(&screen[screen_len - 5], "\33[0 q", 5), 0);
    draw_init();
    untap();
}

UTEST(com, a_write_to_the_screen_lands_between_the_output_around_it)
{
    screen_len = 0;
    manifold_set_term_out(screen_out);
    manifold_putchar('a');
    manifold_write_to(COM_TERM_SCREEN, "b", 1);
    manifold_putchar('c');
    ASSERT_EQ(screen_len, (size_t)3);
    ASSERT_EQ(memcmp(screen, "abc", 3), 0);
    draw_init();
}

/* The console's terminal, which com_clean_line reads. */
/* Whether the bell is sounding: its voice's gate is up. */
static bool bell_rings(void)
{
    /* The BEL_ chunk starts with the queue's two counters and ends with
     * the three deadlines, 0 when none is set. */
    static uint8_t row[256], quiet[24];
    sst_cursor_t c = {row, row + bel_sst.size, false, NULL};
    bel_sst.save(&c, 0);
    return memcmp(row, row + 4, 4) != 0 ||
           memcmp(row + bel_sst.size - 24, quiet, 24) != 0;
}

static void say(const char *s)
{
    while (*s)
        manifold_putchar(*s++);
    manifold_flush();
}

/* A BEL that ends an OSC is the end of a string and no bell. */
UTEST(com, a_bell_inside_an_escape_sequence_does_not_ring)
{
    bel_set_term(true);
    bel_init();
    ASSERT_FALSE(bell_rings());
    say("\33]0;a title\a");
    say("\33]4;1;rgb:ff/80/00\a\33]104\a");
    ASSERT_FALSE(bell_rings());
    say("text\r\n");
    ASSERT_FALSE(bell_rings());
    say("\a");
    ASSERT_TRUE(bell_rings());
    /* One inside a CSI is a bell all the same, as on a VT102. */
    bel_init();
    say("\33[1\a;1H");
    ASSERT_TRUE(bell_rings());
    /* And none rings with the bell turned off. */
    bel_init();
    bel_set_term(false);
    say("\a");
    ASSERT_FALSE(bell_rings());
    bel_set_term(true);
    bel_init();
}

UTEST_STATE();
/* The console's line, paced, carries 11520 bytes in a second of the
 * machine's time each way, 115200 baud at ten bits a byte, and none of it
 * rounds away: the beam's second is 31500 lines. */
static size_t paced_count;
static void paced_tap(const char *buf, int len)
{
    (void)buf;
    paced_count += (size_t)len;
}

UTEST(com, the_line_carries_11520_bytes_a_second_out)
{
    /* A terminal with no smooth scroll left on by another case, which
     * would hold the line up. */
    manifold_flush();
    draw_init();
    manifold_set_paced(true);
    paced_count = 0;
    manifold_set_tx_tap(paced_tap);
    const uint64_t from = vga_beam_lines();
    for (int line = 0; line < 31500; line++)
    {
        while (manifold_cts())
            manifold_putchar_raw('x');
        manifold_task();
        vga_task();
    }
    const uint64_t lines = vga_beam_lines() - from;
    manifold_set_tx_tap(NULL);
    manifold_set_paced(false);
    manifold_flush();
    ASSERT_EQ(lines, (uint64_t)31500);
    ASSERT_GE(paced_count, (size_t)11519);
    ASSERT_LE(paced_count, (size_t)11521);
}

UTEST(com, the_line_carries_11520_bytes_a_second_in)
{
    static char data[12000];
    memset(data, 'y', sizeof data);
    com_rx_clear();
    manifold_set_paced(true);
    wire(data, sizeof data, true);
    size_t got = 0;
    char buf[64];
    for (int line = 0; line < 31500; line++)
    {
        manifold_task();
        got += manifold_uart_read(buf, sizeof buf);
        vga_task();
    }
    unwire();
    manifold_set_paced(false);
    ASSERT_GE(got, (size_t)11519);
    ASSERT_LE(got, (size_t)11521);
}

/* A speed the console's terminal keeps (DECSCS) slows the bytes going out to
 * one every 10 bits of it: at 300 baud a byte is a thirtieth of a second,
 * 1050 lines of the beam, where at the line's pace it is under 3. The byte
 * that sets a speed goes at the one before it, and the next byte at its own,
 * as SyncTERM's; but unlimited has no wait, so the byte after the one that
 * sets it goes at the line's pace at once. speed_send writes to a paced
 * line, idles a few lines first so its first byte goes at once, and runs the
 * beam until it has all gone out, keeping the line each byte was sent on.
 * The line is drained again only after late lines, when the bytes that
 * waited go together. */
static uint64_t speed_lines[16];
static size_t speed_count;

static void speed_tap(const char *buf, int len)
{
    (void)buf;
    for (int i = 0; i < len && speed_count < 16; i++)
        speed_lines[speed_count++] = vga_beam_lines();
}

static void speed_send(const char *s, int late)
{
    const size_t n = strlen(s);
    for (int line = 0; line < 4; line++)
    {
        manifold_task();
        vga_task();
    }
    speed_count = 0;
    manifold_set_tx_tap(speed_tap);
    for (size_t i = 0; i < n; i++)
        manifold_putchar_raw(s[i]);
    for (int line = 0; line < late; line++)
        vga_task();
    for (int line = 0; speed_count < n && line < 31500; line++)
    {
        manifold_task();
        vga_task();
    }
    manifold_set_tx_tap(NULL);
}

/* The lines from the byte at i to the one after it. */
static uint64_t speed_gap(size_t i)
{
    return speed_lines[i + 1] - speed_lines[i];
}

UTEST(com, a_speed_paces_the_bytes_after_the_one_that_sets_it)
{
    manifold_flush();
    draw_init();
    manifold_set_paced(true);
    speed_send("\33[0;1*rabc", 0);
    ASSERT_EQ(speed_count, (size_t)10);
    for (size_t i = 0; i < 7; i++)
        ASSERT_LE(speed_gap(i), (uint64_t)3);
    for (size_t i = 7; i < 9; i++)
    {
        ASSERT_GE(speed_gap(i), (uint64_t)1049);
        ASSERT_LE(speed_gap(i), (uint64_t)1051);
    }
    /* Unlimited is the line's pace again, from the byte that sets it. */
    speed_send("\33[0;0*rab", 0);
    ASSERT_EQ(speed_count, (size_t)9);
    for (size_t i = 0; i < 6; i++)
    {
        ASSERT_GE(speed_gap(i), (uint64_t)1049);
        ASSERT_LE(speed_gap(i), (uint64_t)1051);
    }
    for (size_t i = 6; i < 8; i++)
        ASSERT_LE(speed_gap(i), (uint64_t)3);
    /* And so is 115200, the fastest. */
    speed_send("\33[0;11*rab", 0);
    ASSERT_EQ(speed_count, (size_t)10);
    for (size_t i = 0; i < 9; i++)
        ASSERT_LE(speed_gap(i), (uint64_t)3);
    draw_init();
    manifold_set_paced(false);
}

/* Bytes that waited go together as far as their times have passed: the
 * ones up to the byte after the speed's, which started at the line's pace
 * 19 lines after the first, and the next one a thirtieth of a second
 * after that. */
UTEST(com, bytes_that_waited_together_keep_the_speed_s_pace)
{
    manifold_flush();
    draw_init();
    manifold_set_paced(true);
    speed_send("\33[0;1*rab", 40);
    ASSERT_EQ(speed_count, (size_t)9);
    ASSERT_EQ(speed_lines[7] - speed_lines[0], (uint64_t)40);
    ASSERT_GE(speed_lines[8] - speed_lines[0], (uint64_t)1068);
    ASSERT_LE(speed_lines[8] - speed_lines[0], (uint64_t)1070);
    manifold_flush();
    draw_init();
    manifold_set_paced(false);
}

/* RIS sets the speed back to unlimited, and the byte after it goes at the
 * line's pace. */
UTEST(com, a_reset_ends_the_speed)
{
    manifold_flush();
    draw_init();
    manifold_set_paced(true);
    manifold_write_to(COM_TERM_ALL, "\33[0;1*r", 7);
    manifold_flush();
    speed_send("\33cab", 0);
    ASSERT_EQ(speed_count, (size_t)4);
    ASSERT_GE(speed_gap(0), (uint64_t)1049);
    ASSERT_LE(speed_gap(0), (uint64_t)1051);
    for (size_t i = 1; i < 3; i++)
        ASSERT_LE(speed_gap(i), (uint64_t)3);
    manifold_set_paced(false);
}

/* A flush sends all that waits past a byte that changes the speed, 115200
 * from unlimited included, whose byte time is the line's either way. */
UTEST(com, a_flush_goes_past_a_new_speed)
{
    manifold_flush();
    draw_init();
    manifold_set_paced(true);
    manifold_write_to(COM_TERM_ALL, "\33[0;11*rab\33[0;1*rcd", 22);
    manifold_flush();
    ASSERT_TRUE(manifold_drained());
    ASSERT_EQ(term_speed(&vterm), (uint32_t)300);
    draw_init();
    manifold_set_paced(false);
}

/* A line with no pace to keep keeps no speed either. */
UTEST(com, an_unpaced_line_keeps_no_speed)
{
    manifold_flush();
    draw_init();
    manifold_set_paced(false);
    speed_send("\33[0;1*rabc", 0);
    ASSERT_EQ(speed_count, (size_t)10);
    ASSERT_EQ(speed_lines[9], speed_lines[0]);
    draw_init();
}

/* A session's end waits for what the program wrote to go out at the line's
 * pace, then goes out whole, behind it. */
UTEST(com, a_session_end_waits_for_the_output_then_follows_it)
{
    manifold_flush();
    draw_init();
    manifold_set_paced(true);
    tap();
    for (const char *p = "abcdefghij"; *p; p++)
        manifold_putchar_raw(*p);
    com_end_reset();
    ASSERT_TRUE(com_ending());
    int lines = 0;
    for (; com_ending() && lines < 2000; lines++)
    {
        manifold_task();
        com_task();
        vga_task();
    }
    untap();
    manifold_set_paced(false);
    ASSERT_FALSE(com_ending());
    /* Ten bytes at 115200 baud are 868 us, about 27 lines of the beam. */
    ASSERT_GE(lines, 25);
    ASSERT_GE(sent_len, (size_t)10 + strlen(STR_TERM_SOFT_RESET));
    ASSERT_EQ(memcmp(sent, "abcdefghij", 10), 0);
    ASSERT_EQ(memcmp(sent + 10, STR_TERM_SOFT_RESET, strlen(STR_TERM_SOFT_RESET)), 0);
}

/* A program starting ends the last session at once. */
UTEST(com, a_program_starting_ends_the_last_session)
{
    com_end_reset();
    ASSERT_TRUE(com_ending());
    com_start();
    ASSERT_FALSE(com_ending());
}

int main(int argc, const char *const argv[])
{
    draw_init();
    /* No frames run, so the console's line keeps no pace. */
    manifold_set_paced(false);
    return utest_main(argc, argv);
}
