/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The RIA's side of the messages the VGA answers (ria/sys/vga.c), the gate
 * every message of the RIA's passes on PIX, and a 6502 program's xreg burst
 * (ria/sys/pix.c), against a fake VGA on PIX and the backchannel. The fake
 * answers as src/host/pico/vga/sys/pix.c does: an ack for CANVAS, MODE,
 * DISPLAY, CODE_PAGE and FLASH_PROGRAM, or, as the firmware before them did,
 * nothing for DISPLAY and CODE_PAGE. As the VGA's ria.c does, it answers
 * once the work a message made is done: a case gives it work, and its
 * answer waits until the case ends the work. Its link is
 * src/host/pico/vga/sys/ria.c's: "VGA1" on the UART for a request, and its
 * version on the backchannel when the backchannel is turned on. An answer
 * reaches the RIA only when a test delivers it, which runs the backchannel's
 * interrupt as the chip does when a byte arrives. */

#include "ria/sys/pix.h"
#include "ria/sys/vga.h"
#include "ria/mon/mon.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/ria.h"
#include "core/api/api.h"
#include "core/api/xreg.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/str/oem.h"
#include "core/str/str.h"
#include "core/sys/xram.h"
#include "core/term/draw.h"
#include "core/term/vterm.h"
#include "utest.h"
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/timer.h>
#include <pico/time.h>
#include <string.h>

/* ---- the clock --------------------------------------------------------- */

/* The test's clock starts past the VGA's boot time (VGA_BOOT_MS), and a busy
 * wait moves it on a microsecond, as time passes on the chip. */
static uint64_t now_us = 1000000;
absolute_time_t get_absolute_time(void) { return now_us; }
void tight_loop_contents(void) { now_us++; }
static void advance_ms(uint32_t ms) { now_us += (uint64_t)ms * 1000; }

/* ---- the fake VGA ------------------------------------------------------ */

static struct
{
    bool present; // answers the request for the backchannel
    bool old;     // the firmware before: answers neither DISPLAY nor CODE_PAGE
    bool nak;     // refuses CANVAS and MODE
    bool linked;  // its backchannel is on
    bool working; // at work on what a message made, which holds its answer
    uint8_t owed; // the answer the work holds, 0 for none
} fake;

/* Every message PIX_SM sent, decoded, in order. A case sends far fewer. */
typedef struct
{
    uint8_t dev, ch, byte;
    uint16_t word;
} msg_t;
#define SENT_SIZE 1024
static msg_t sent[SENT_SIZE];
static unsigned sent_count;

/* The backchannel's RX FIFO, which the fake VGA fills. */
static uint8_t rx[256];
static unsigned rx_head, rx_tail;
static void rx_put(uint8_t b) { rx[rx_tail++ % sizeof rx] = b; }
static void rx_puts(const char *s)
{
    while (*s)
        rx_put((uint8_t)*s++);
}

static bool request_answered; // "VGA1" waits on the UART for mbuf_read
static bool backchannel_irq_on;
static irq_handler_t backchannel_handler;

pio_hw_t test_pio[2];
timer_hw_t test_timer;
dma_hw_t test_dma;
const pio_program_t pix_tx_program;
const pio_program_t vga_backchannel_rx_program;

static void fake_answer(bool ack)
{
    if (!fake.linked)
        return;
    if (fake.working)
        fake.owed = ack ? 0x90 : 0xA0;
    else
        rx_put(ack ? 0x90 : 0xA0);
}

/* The fake VGA's work ends, and the answer it held goes. */
static void work_done(void)
{
    fake.working = false;
    if (fake.owed)
        rx_put(fake.owed);
    fake.owed = 0;
}

void pio_sm_put(PIO pio, uint sm, uint32_t data)
{
    if (pio != PIX_PIO || sm != PIX_SM)
        return;
    msg_t m = {(uint8_t)((data >> 29) & 7), (uint8_t)((data >> 24) & 0xF),
               (uint8_t)(data >> 16), (uint16_t)data};
    sent[sent_count++ % SENT_SIZE] = m;
    if (m.dev != PIX_DEVICE_VGA)
        return;
    if (m.ch == 0 && m.byte <= 1)
        fake_answer(!fake.nak);
    if (m.ch != 0xF)
        return;
    switch (m.byte)
    {
    case 0x00: // DISPLAY
    case 0x01: // CODE_PAGE
        if (!fake.old)
            fake_answer(true);
        break;
    case 0x04: // BACKCHAN
        if (m.word == 2)
            request_answered = fake.present;
        else if (m.word == 1 && fake.present)
        {
            fake.linked = true;
            rx_puts("VGA test\r");
        }
        else if (m.word == 0)
            fake.linked = false;
        break;
    case 0x07: // FLASH_PROGRAM
        fake_answer(true);
        break;
    }
}

uint32_t pio_sm_get(PIO pio, uint sm)
{
    (void)pio;
    (void)sm;
    return (uint32_t)rx[rx_head++ % sizeof rx] << 24;
}

bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm)
{
    (void)pio;
    (void)sm;
    return rx_head == rx_tail;
}

/* PIX always has room. On the chip the FIFO drains at PIX's pace, which only
 * delays a message, and no case here is about that. */
uint pio_sm_get_tx_fifo_level(PIO pio, uint sm)
{
    (void)pio;
    (void)sm;
    return 0;
}

void pio_set_irq1_source_enabled(PIO pio, pio_interrupt_source_t source, bool enabled)
{
    (void)pio;
    if (source == pis_sm3_rx_fifo_not_empty)
        backchannel_irq_on = enabled;
}

void irq_set_exclusive_handler(unsigned int num, irq_handler_t handler)
{
    if (num == VGA_BACKCHANNEL_IRQ)
        backchannel_handler = handler;
}

/* What the fake VGA sent arrives, and the backchannel's interrupt takes it
 * while it is enabled. */
static void deliver(void)
{
    if (backchannel_irq_on && rx_head != rx_tail)
        backchannel_handler();
}

/* A VSYNC byte arrives with frame number 0, a millisecond on. */
static void vsync(void)
{
    advance_ms(1);
    rx_put(0x80);
    deliver();
}

static int sent_find(uint8_t ch, uint8_t byte, unsigned from)
{
    for (unsigned i = from; i < sent_count; i++)
        if (sent[i].dev == PIX_DEVICE_VGA && sent[i].ch == ch && sent[i].byte == byte)
            return (int)i;
    return -1;
}

/* ---- the RIA around vga.c and pix.c ----------------------------------- */

/* mbuf.c reads the UART: the fake VGA's "VGA1", or a timeout. */
size_t mbuf_len;
uint8_t buf1k[BUF1K_SIZE];
static mbuf_read_callback_t mbuf_callback;
void mbuf_read(com_source_t src, uint32_t timeout_ms, mbuf_read_callback_t callback,
               size_t length)
{
    (void)src;
    (void)timeout_ms;
    (void)length;
    mbuf_callback = callback;
}
void mbuf_task(void)
{
    mbuf_read_callback_t cb = mbuf_callback;
    mbuf_callback = NULL;
    if (!cb)
        return;
    mbuf_len = request_answered ? 4 : 0;
    memcpy(buf1k, "VGA1", 4);
    request_answered = false;
    cb(!mbuf_len);
}

/* The monitor's responses, as mon.c queues them. */
static const char *responses[8];
static unsigned response_count;
void mon_add_response_utf8(const char *utf8) { responses[response_count++ % 8] = utf8; }

/* core/str/str.c's locale table: each id names itself. */
const char *S(int id)
{
    static char names[STR_LOC_COUNT][8];
    snprintf(names[id], sizeof names[id], "S%d", id);
    return names[id];
}
#define X(name, value) const char name[] = value;
#define XR(name, value) X(name, value)
#include "core/def/str_sys.def"
#undef X
#undef XR

/* The console's terminal (vterm.c): the code pages it takes, in order. */
term_t vterm;
static uint16_t vterm_pages[8];
static unsigned vterm_page_count;
bool vterm_set_code_page(uint16_t cp)
{
    vterm_pages[vterm_page_count++ % 8] = cp;
    return true;
}
/* Its speed (term_speed) in bits a second: 'r', the final byte of DECSCS,
 * sets the one a case gives, and 'c', of RIS, makes it unlimited, as does a
 * RIS of the terminal. */
static uint32_t vterm_speed;
static uint32_t vterm_speed_set;
void vterm_init(cell_t *cells) { (void)cells; }
void vterm_RIS(void) { vterm_speed = 0; }
void vterm_RIS_no_clear(void) {}
void vterm_canvas(uint8_t cols, bool is_console) { (void)cols; (void)is_console; }
void vterm_place(uint8_t cols, uint8_t rows, int16_t begin, uint8_t row_px)
{
    (void)cols; (void)rows; (void)begin; (void)row_px;
}
void vterm_unplace(void) {}
/* The selection: a case makes a change of the user's by marking it unsent
 * and giving the words it encodes to. */
screen_selection_t vterm_selection;
static vterm_select_words_t select_words;
vterm_select_words_t vterm_select_words(void)
{
    return select_words;
}
static void user_selects(uint16_t start, uint16_t end)
{
    select_words = (vterm_select_words_t){start, (uint16_t)(start ^ end), end};
    vterm_selection.unsent = true;
}
void term_write(term_t *t, uint8_t ch)
{
    (void)t;
    if (ch == 'r')
        vterm_speed = vterm_speed_set;
    else if (ch == 'c')
        vterm_speed = 0;
}
uint32_t term_speed(const term_t *t)
{
    (void)t;
    return vterm_speed;
}

/* The console's output (manifold.c): with nothing queued, a flush is done.
 * It feeds the console's terminal through vga.c every byte it sends. */
static int (*term_out)(const char *buf, int len, bool force);
void manifold_write_to(uint8_t to, const char *buf, size_t n) { (void)to; (void)buf; (void)n; }
void manifold_flush(void) {}
void manifold_set_term_out(int (*out_chars)(const char *buf, int len, bool force)) { term_out = out_chars; }
void manifold_uart_drain_rx(void) {}
void com_rx_clear_source(com_source_t src) { (void)src; }
void com_set_reply_default(bool quiet) { (void)quiet; }

static uint16_t oem_page_run;
uint16_t oem_get_code_page_run(void) { return oem_page_run; }
uint8_t vga_get_display_type(void) { return 0; }
void ria_trigger_vsync(void) {}

/* The 6502's side of the API (regs.c, api.c, xram.c). */
volatile uint8_t regs[0x20];
uint8_t xstack[XSTACK_SIZE + 1];
volatile uint16_t xstack_ptr = XSTACK_SIZE;
volatile uint8_t xram[0x10000];
uint16_t api_platform_errno(api_errno num) { return (uint16_t)num; }
bool xreg0(uint8_t channel, uint8_t index, uint16_t word)
{
    (void)channel; (void)index; (void)word;
    return true;
}

/* ---- set up ------------------------------------------------------------ */

/* The RIA boots with the fake VGA as given, and the code page is asked for
 * as oem_init asks for it, after vga_init. Each case is a process of its own
 * (SPLIT), so each boots once from nothing. */
static void boot(bool present, bool old)
{
    fake.present = present;
    fake.old = old;
    vga_init();
    vga_set_code_page(437);
    oem_page_run = 437;
}

/* A connected VGA has taken the code page and DISPLAY of its connect. */
static void boot_settled(void)
{
    boot(true, false);
    vga_task(); // CODE_PAGE
    deliver();
    vga_task(); // DISPLAY
    deliver();
}

/* A burst of CANVAS and MODE on the xstack, as a 6502 program pushes it. */
static void push_canvas_mode(uint16_t canvas, uint16_t mode)
{
    xstack_ptr = XSTACK_SIZE - 7;
    memcpy(&xstack[XSTACK_SIZE - 7], &mode, 2);
    memcpy(&xstack[XSTACK_SIZE - 5], &canvas, 2);
    xstack[XSTACK_SIZE - 3] = 0; // index
    xstack[XSTACK_SIZE - 2] = 0; // channel
    xstack[XSTACK_SIZE - 1] = PIX_DEVICE_VGA;
}

/* ---- the code page and DISPLAY ----------------------------------------- */

UTEST(vga, code_page_goes_before_the_console)
{
    boot(true, false);
    /* The console is held for the page, and the page goes when it asks. */
    ASSERT_FALSE(vga_console_ready());
    int at = sent_find(0xF, 0x01, 0);
    ASSERT_GE(at, 0);
    ASSERT_EQ(437, sent[at].word);
    ASSERT_EQ(1u, vterm_page_count);
    ASSERT_EQ(437, vterm_pages[0]);
    /* It stays held until the VGA answers. */
    ASSERT_FALSE(vga_console_ready());
    deliver();
    ASSERT_TRUE(vga_console_ready());
}

UTEST(vga, display_follows_the_code_page)
{
    boot(true, false);
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x01, 0), 0);
    ASSERT_LT(sent_find(0xF, 0x00, 0), 0);
    /* No DISPLAY while the code page's answer is awaited, and no wait. */
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x00, 0), 0);
    deliver();
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x00, 0), 0);
    ASSERT_FALSE(vga_console_ready());
    deliver();
    ASSERT_TRUE(vga_console_ready());
    ASSERT_EQ(0u, response_count);
}

UTEST(vga, stop_sends_display_without_waiting)
{
    boot_settled();
    vga_stop();
    vga_set_code_page(850);
    unsigned from = sent_count;
    uint64_t at = now_us;
    vga_task(); // the code page, then DISPLAY after its answer
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x00, from), 0);
    ASSERT_EQ(at, now_us);
    deliver();
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x00, from), 0);
    ASSERT_GT(sent_find(0xF, 0x00, from), sent_find(0xF, 0x01, from));
}

UTEST(vga, pages_before_the_first_goes_are_one)
{
    boot_settled();
    unsigned from = sent_count;
    vga_set_code_page(850);
    vga_set_code_page(437);
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x01, from), 0);
    ASSERT_EQ(437, sent[sent_find(0xF, 0x01, from)].word);
    ASSERT_LT(sent_find(0xF, 0x01, (unsigned)sent_find(0xF, 0x01, from) + 1), 0);
}

/* ---- timeouts ---------------------------------------------------------- */

UTEST(vga, old_vga_is_lost_at_the_answers_time)
{
    boot(true, true);
    vga_task(); // CODE_PAGE, never answered
    advance_ms(VGA_ANSWER_MS - 1);
    ASSERT_FALSE(vga_console_ready());
    ASSERT_TRUE(vga_connected());
    /* With its time up, the console is still held until the wait is a
     * timeout, and that loses the VGA. */
    advance_ms(1);
    ASSERT_FALSE(vga_console_ready());
    ASSERT_FALSE(vga_connected());
    ASSERT_EQ(1u, response_count);
    ASSERT_STREQ(S(STR_ERR_VGA_CONNECTION_LOST), responses[0]);
    /* No DISPLAY goes to a lost VGA, and nothing is asked of it. */
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x00, 0), 0);
    ASSERT_TRUE(vga_ask_ready());
    ASSERT_FALSE(vga_ask(0, 0, 1, VGA_ANSWER_MS));
}

/* ---- the asker's answer ------------------------------------------------ */

UTEST(vga, the_askers_answer_outlives_vgas_own)
{
    boot_settled();
    ASSERT_TRUE(vga_ask_ready());
    ASSERT_TRUE(vga_ask(0, 0, 1, VGA_ANSWER_MS));
    ASSERT_EQ(VGA_ANSWER_WAITING, vga_answer());
    ASSERT_FALSE(vga_ask_ready());
    deliver();
    ASSERT_EQ(VGA_ANSWER_ACK, vga_answer());
    /* A code page goes after it, and its answer is vga.c's alone. */
    vga_set_code_page(850);
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x01, 0), 0);
    ASSERT_EQ(VGA_ANSWER_ACK, vga_answer());
    ASSERT_FALSE(vga_ask_ready());
    deliver();
    ASSERT_EQ(VGA_ANSWER_ACK, vga_answer());
    ASSERT_TRUE(vga_ask_ready());
}

UTEST(vga, ask_waits_for_display)
{
    boot_settled();
    vga_stop();
    ASSERT_FALSE(vga_ask_ready());
    vga_task();
    ASSERT_FALSE(vga_ask_ready());
    deliver();
    ASSERT_TRUE(vga_ask_ready());
}

/* ---- a 6502 program's xreg burst --------------------------------------- */

UTEST(vga, xreg_canvas_and_mode_wait_for_each_answer)
{
    boot_settled();
    push_canvas_mode(1, 3);
    unsigned from = sent_count;
    ASSERT_TRUE(pix_api_xreg()); // CANVAS goes
    ASSERT_EQ(1, sent[sent_find(0, 0, from)].word);
    ASSERT_TRUE(pix_api_xreg()); // its answer is awaited
    ASSERT_LT(sent_find(0, 1, from), 0);
    deliver();
    ASSERT_TRUE(pix_api_xreg()); // MODE goes: the answer came after the reclock
    ASSERT_EQ(3, sent[sent_find(0, 1, from)].word);
    ASSERT_TRUE(pix_api_xreg());
    deliver();
    ASSERT_FALSE(pix_api_xreg()); // done
    ASSERT_EQ(0, API_AX);
    ASSERT_EQ(XSTACK_SIZE, xstack_ptr);
}

UTEST(vga, xreg_refused_is_einval)
{
    boot_settled();
    fake.nak = true;
    push_canvas_mode(1, 3);
    ASSERT_TRUE(pix_api_xreg());
    deliver();
    ASSERT_FALSE(pix_api_xreg());
    ASSERT_EQ(API_EINVAL, API_ERRNO);
    ASSERT_TRUE(vga_connected());
}

UTEST(vga, xreg_unanswered_is_eio_and_loses_the_vga)
{
    boot_settled();
    push_canvas_mode(1, 3);
    ASSERT_TRUE(pix_api_xreg());
    advance_ms(VGA_ANSWER_MS);
    ASSERT_FALSE(pix_api_xreg());
    ASSERT_EQ(API_EIO, API_ERRNO);
    ASSERT_FALSE(vga_connected());
    ASSERT_EQ(1u, response_count);
}

UTEST(vga, xreg_waits_for_the_code_page)
{
    boot_settled();
    vga_set_code_page(850);
    push_canvas_mode(1, 3);
    unsigned from = sent_count;
    ASSERT_TRUE(pix_api_xreg()); // the code page goes first
    ASSERT_GE(sent_find(0xF, 0x01, from), 0);
    ASSERT_LT(sent_find(0, 0, from), 0);
    deliver();
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_GE(sent_find(0, 0, from), 0);
}

/* ---- the console's wait for the VGA ----------------------------------- */

/* A job the console's terminal started holds the console's bytes for the
 * VGA a millisecond (vga.c's VGA_JOB_HOLD_US), and a sync waits for it. */
UTEST(vga, a_job_holds_the_console_a_while)
{
    boot_settled();
    ASSERT_TRUE(vga_console_ready());
    ASSERT_FALSE(vga_console_busy());
    draw_job_started();
    ASSERT_FALSE(vga_console_ready());
    ASSERT_TRUE(vga_console_busy());
    now_us += 999;
    ASSERT_FALSE(vga_console_ready());
    ASSERT_TRUE(vga_console_busy());
    now_us += 1;
    ASSERT_TRUE(vga_console_ready());
    ASSERT_FALSE(vga_console_busy());
}

/* A sync waits for an answer, and for a code page still to go. */
UTEST(vga, the_console_is_busy_while_an_answer_is_awaited)
{
    boot(true, false);
    ASSERT_TRUE(vga_console_busy()); // the code page, then its answer
    deliver();
    vga_task(); // DISPLAY
    ASSERT_TRUE(vga_console_busy());
    deliver();
    ASSERT_FALSE(vga_console_busy());
    ASSERT_TRUE(vga_ask(0, 0, 1, VGA_ANSWER_MS));
    ASSERT_TRUE(vga_console_busy());
    deliver();
    ASSERT_FALSE(vga_console_busy());
}

/* With no VGA nothing waits: a job holds nothing. */
UTEST(vga, no_vga_no_wait)
{
    boot(false, false);
    ASSERT_FALSE(vga_console_busy());
    draw_job_started();
    ASSERT_FALSE(vga_console_busy());
    ASSERT_TRUE(vga_gate_open());
    ASSERT_TRUE(pix_ready());
}

/* A VGA lost to an answer it did not give holds nothing either. */
UTEST(vga, a_lost_vga_no_wait)
{
    boot(true, true);
    vga_task(); // CODE_PAGE, never answered
    ASSERT_TRUE(vga_console_busy());
    ASSERT_FALSE(pix_ready());
    advance_ms(VGA_ANSWER_MS);
    ASSERT_FALSE(vga_console_busy());
    ASSERT_TRUE(pix_ready());
    ASSERT_FALSE(vga_connected());
    draw_job_started();
    ASSERT_FALSE(vga_console_busy());
    ASSERT_TRUE(pix_ready());
}

/* A VGA lost to its VSYNCs stopping while a job's hold runs: the hold goes
 * with it. */
UTEST(vga, a_vga_lost_in_a_hold_no_wait)
{
    boot_settled();
    advance_ms(44); // the VSYNC watchdog's time, with no VSYNC
    draw_job_started();
    ASSERT_FALSE(pix_ready());
    vga_task();
    ASSERT_FALSE(vga_connected());
    ASSERT_TRUE(pix_ready());
    ASSERT_FALSE(vga_console_busy());
}

/* ---- the gate ---------------------------------------------------------- */

/* A 6502 program's xreg of one word to VGA channel 0, which the VGA does
 * not answer: a CANVAS or MODE argument. */
static void push_xreg(uint8_t index, uint16_t word)
{
    xstack_ptr = XSTACK_SIZE - 5;
    memcpy(&xstack[XSTACK_SIZE - 5], &word, 2);
    xstack[XSTACK_SIZE - 3] = index;
    xstack[XSTACK_SIZE - 2] = 0; // channel
    xstack[XSTACK_SIZE - 1] = PIX_DEVICE_VGA;
}

/* Every message of the RIA's that the pointer, the selection, a 6502
 * program's xreg and the RIA's own XRAM sends make waits out a job's hold,
 * and then goes. */
UTEST(vga, every_message_waits_out_a_jobs_hold)
{
    boot_settled();
    advance_ms(20); // a frame on, so the pointer and the selection may go
    unsigned from = sent_count;
    draw_job_started();
    ASSERT_FALSE(vga_gate_open());
    ASSERT_FALSE(pix_ready()); // the RIA's own XRAM sends wait on it
    ASSERT_FALSE(vga_console_ready());
    vga_set_pointer(1, 10, 20);
    user_selects(0x1234, 0x1300);
    vga_task();
    push_xreg(2, 0x5678);
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_EQ(from, sent_count);
    now_us += 999;
    vga_task();
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_EQ(from, sent_count);
    now_us += 1;
    ASSERT_TRUE(vga_gate_open());
    ASSERT_TRUE(vga_console_ready());
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x08, from), 0);
    ASSERT_GE(sent_find(0xF, 0x09, from), 0);
    ASSERT_GE(sent_find(0xF, 0x0A, from), 0);
    ASSERT_GE(sent_find(0xF, 0x0B, from), 0);
    ASSERT_GE(sent_find(0xF, 0x0C, from), 0);
    ASSERT_GE(sent_find(0xF, 0x0D, from), 0);
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_EQ(0x5678, sent[sent_find(0, 2, from)].word);
    ASSERT_FALSE(pix_api_xreg()); // done
    ASSERT_EQ(0, API_AX);
}

/* The code page, DISPLAY and a 6502 program's CANVAS wait out a job's hold
 * too: one the last of the console started before DISPLAY goes included. */
UTEST(vga, answered_messages_wait_out_a_jobs_hold)
{
    boot_settled();
    unsigned from = sent_count;
    draw_job_started();
    vga_set_code_page(850);
    vga_task();
    ASSERT_FALSE(vga_ask_ready());
    ASSERT_LT(sent_find(0xF, 0x01, from), 0);
    now_us += 1000;
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x01, from), 0);
    deliver();
    draw_job_started();
    vga_stop();
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x00, from), 0);
    now_us += 1000;
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x00, from), 0);
    deliver();
    draw_job_started();
    push_canvas_mode(1, 3);
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_LT(sent_find(0, 0, from), 0);
    now_us += 1000;
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_GE(sent_find(0, 0, from), 0);
}

/* While an answer is awaited, however long the VGA's work takes it, nothing
 * of the RIA's goes: not the console, the pointer, the selection, a 6502
 * program's xreg, a code page or the RIA's own XRAM sends. They go once it
 * comes. */
UTEST(vga, nothing_goes_while_an_answer_is_awaited)
{
    boot_settled();
    advance_ms(20);
    fake.working = true;
    ASSERT_TRUE(vga_ask(0, 0, 1, VGA_ANSWER_MS));
    unsigned from = sent_count;
    advance_ms(VGA_ANSWER_MS - 2);
    vsync(); // a millisecond on, and the VGA's VSYNCs go on
    ASSERT_EQ(VGA_ANSWER_WAITING, vga_answer());
    ASSERT_FALSE(pix_ready());
    ASSERT_FALSE(vga_console_ready());
    vga_set_pointer(1, 10, 20);
    user_selects(0x1234, 0x1300);
    vga_set_code_page(850);
    vga_task();
    push_xreg(2, 0x5678);
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_EQ(from, sent_count);
    ASSERT_TRUE(vga_connected());
    work_done();
    deliver();
    ASSERT_EQ(VGA_ANSWER_ACK, vga_answer());
    vga_task(); // the code page first, and its answer
    ASSERT_EQ(sent_find(0xF, 0x01, from), (int)from);
    ASSERT_LT(sent_find(0xF, 0x08, from), 0);
    deliver();
    advance_ms(40); // the frames go on by the clock
    vga_task();
    ASSERT_GE(sent_find(0xF, 0x08, from), 0);
    ASSERT_GE(sent_find(0xF, 0x0B, from), 0);
    ASSERT_TRUE(pix_api_xreg());
    ASSERT_GE(sent_find(0, 2, from), 0);
    ASSERT_TRUE(vga_console_ready());
}

/* The VGA answers DISPLAY once its work is done, and the console waits for
 * that, not a hold: the jobs the RIA's terminal starts as it follows the
 * message are the work the answer waits for. */
UTEST(vga, the_answer_is_the_wait_for_the_messages_work)
{
    boot_settled();
    fake.working = true;
    vga_stop();
    vga_task(); // DISPLAY
    advance_ms(10);
    vga_task();
    ASSERT_TRUE(vga_console_busy());
    ASSERT_TRUE(vga_connected());
    draw_job_started(); // the RIA's terminal takes the DISPLAY's work
    now_us += 999; // inside the hold the job would have raised
    work_done();
    deliver();
    ASSERT_FALSE(vga_console_busy());
    ASSERT_TRUE(vga_console_ready());
}

/* The 6502's writes to XRAM go round the gate: the stream is not held while
 * an answer is awaited. */
UTEST(vga, the_6502s_xram_goes_round_the_gate)
{
    boot_settled();
    ASSERT_TRUE(vga_ask(0, 0, 1, VGA_ANSWER_MS));
    ASSERT_FALSE(pix_ready());
    ASSERT_EQ(4u, pix_stream_xram(0, 4));
    ASSERT_TRUE(pix_stream_done());
    deliver();
    draw_job_started();
    ASSERT_FALSE(pix_ready());
    ASSERT_EQ(8u, pix_stream_xram(4, 8));
}

/* ---- the console's speed ---------------------------------------------- */

/* One byte of the console goes to the terminals, as manifold.c sends it. */
static void console_byte(char ch) { term_out(&ch, 1, true); }

/* The console's terminal takes a DECSCS for bits a second. */
static void console_speed(uint32_t bps)
{
    vterm_speed_set = bps;
    console_byte('r');
}

/* At 300 baud each byte holds the next 10 bits, 33333 us, and a sync does
 * not wait for it. The byte that sets the speed went unlimited, so the byte
 * after it goes at once. */
UTEST(vga, the_speed_spaces_the_console)
{
    boot_settled();
    console_speed(300);
    ASSERT_TRUE(vga_console_ready());
    console_byte('A');
    ASSERT_FALSE(vga_console_ready());
    ASSERT_FALSE(vga_console_busy());
    now_us += 33332;
    ASSERT_FALSE(vga_console_ready());
    now_us += 1;
    ASSERT_TRUE(vga_console_ready());
    console_byte('B');
    ASSERT_FALSE(vga_console_ready());
    now_us += 33333;
    ASSERT_TRUE(vga_console_ready());
}

/* Every speed's byte time, and the byte that changes the speed waits out its
 * time at the speed it went at, as SyncTERM's next character does. At
 * 115200 it is under the UART's own, which spaces the bytes then. */
UTEST(vga, each_speed_its_byte_time)
{
    static const uint32_t bps[] = {300, 600, 1200, 2400, 4800, 9600,
                                   19200, 38400, 57600, 76800, 115200};
    static const uint32_t us[] = {33333, 16666, 8333, 4166, 2083, 1041,
                                  520, 260, 173, 130, 86};
    boot_settled();
    for (unsigned i = 0; i < 11; i++)
    {
        console_speed(bps[i]);
        if (i)
        {
            now_us += us[i - 1] - 1;
            ASSERT_FALSE(vga_console_ready());
            now_us += 1;
        }
        ASSERT_TRUE(vga_console_ready());
        console_byte('A');
        now_us += us[i] - 1;
        ASSERT_FALSE(vga_console_ready());
        now_us += 1;
        ASSERT_TRUE(vga_console_ready());
    }
}

/* Unlimited holds nothing, and neither does the byte that makes it so,
 * DECSCS or RIS. */
UTEST(vga, unlimited_and_ris_end_the_wait)
{
    boot_settled();
    console_byte('A');
    ASSERT_TRUE(vga_console_ready());
    console_speed(300);
    console_byte('A');
    now_us += 33333;
    console_speed(0);
    ASSERT_TRUE(vga_console_ready());
    console_byte('A');
    ASSERT_TRUE(vga_console_ready());
    console_speed(300);
    console_byte('A');
    now_us += 33333;
    console_byte('c');
    ASSERT_TRUE(vga_console_ready());
    console_byte('A');
    ASSERT_TRUE(vga_console_ready());
}

/* A job and the speed both hold the console, each to its own end, and only
 * the job is the VGA at work. */
UTEST(vga, a_job_and_the_speed_hold_together)
{
    boot_settled();
    console_speed(9600);
    draw_job_started();
    console_byte('A');
    ASSERT_TRUE(vga_console_busy());
    now_us += 1000;
    ASSERT_FALSE(vga_console_busy());
    ASSERT_FALSE(vga_console_ready());
    now_us += 41;
    ASSERT_TRUE(vga_console_ready());
    console_speed(300);
    now_us += 1041;
    console_byte('A');
    draw_job_started();
    now_us += 1000;
    ASSERT_FALSE(vga_console_busy());
    ASSERT_FALSE(vga_console_ready());
    now_us += 32333;
    ASSERT_TRUE(vga_console_ready());
}

/* With no VGA the console's terminal keeps the speed and nothing waits for
 * it: the console goes to the UART's wire, which keeps its own pace. */
UTEST(vga, no_vga_no_speed)
{
    boot(false, false);
    console_speed(300);
    console_byte('A');
    ASSERT_EQ(300u, term_speed(&vterm));
    ASSERT_FALSE(vga_console_busy());
}

/* A lost VGA takes the console's terminal back to unlimited, and what the
 * console sends while it is lost holds nothing: the VGA connected again at
 * the next run is sent its bytes at once. */
UTEST(vga, a_lost_vga_no_speed)
{
    boot_settled();
    console_speed(300);
    fake.old = true;
    vga_set_code_page(850);
    vga_task(); // CODE_PAGE, never answered
    advance_ms(VGA_ANSWER_MS);
    vga_task();
    ASSERT_FALSE(vga_connected());
    ASSERT_EQ(0u, term_speed(&vterm));
    console_speed(300);
    console_byte('A');
    uint64_t went = now_us;
    fake.old = false;
    vga_run();
    ASSERT_TRUE(vga_connected());
    vga_task(); // CODE_PAGE
    deliver();
    vga_task(); // DISPLAY
    deliver();
    ASSERT_LT(now_us - went, (uint64_t)33333);
    ASSERT_TRUE(vga_console_ready());
}

/* ---- FLASH ------------------------------------------------------------- */

/* A new RIA with an old VGA: lost at boot, flashed all the same. */
UTEST(vga, flash_takes_a_vga_that_did_not_answer)
{
    boot(true, true);
    vga_task();
    advance_ms(VGA_ANSWER_MS);
    vga_task();
    ASSERT_FALSE(vga_connected());
    unsigned from = sent_count;
    ASSERT_EQ(VGA_ANSWER_WAITING, vga_flash_take()); // linked, no VSYNC yet
    ASSERT_TRUE(vga_connected());
    /* Nothing it may not answer goes while it is linked, either. */
    vga_stop();
    vga_set_code_page(850);
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x00, from), 0);
    ASSERT_LT(sent_find(0xF, 0x01, from), 0);
    ASSERT_EQ(VGA_ANSWER_WAITING, vga_flash_take());
    vsync();
    ASSERT_EQ(VGA_ANSWER_ACK, vga_flash_take());
    /* Linked again without the DISPLAY and CODE_PAGE it does not answer,
     * and they never go from now on. */
    vga_stop();
    vga_set_code_page(850);
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x00, from), 0);
    ASSERT_LT(sent_find(0xF, 0x01, from), 0);
    ASSERT_TRUE(vga_console_ready());
    /* FLASH_PROGRAM's own answer tells. */
    ASSERT_TRUE(vga_ask_ready());
    ASSERT_TRUE(vga_ask(0xF, 0x07, 0, 500));
    deliver();
    ASSERT_EQ(VGA_ANSWER_ACK, vga_answer());
    ASSERT_EQ(1u, response_count);
}

UTEST(vga, flash_finds_no_vga_without_vsync)
{
    boot(true, true);
    vga_task();
    advance_ms(VGA_ANSWER_MS);
    vga_task();
    ASSERT_EQ(VGA_ANSWER_WAITING, vga_flash_take());
    advance_ms(100);
    vga_task();
    ASSERT_EQ(VGA_ANSWER_TIMEOUT, vga_flash_take());
    /* The loss at boot is the only message: FLASH says the rest. */
    ASSERT_EQ(1u, response_count);
}

UTEST(vga, flash_finds_no_vga_where_none_was)
{
    boot(false, false);
    ASSERT_FALSE(vga_connected());
    ASSERT_EQ(VGA_ANSWER_TIMEOUT, vga_flash_take());
    ASSERT_EQ(0u, response_count);
}

UTEST(vga, flash_waits_out_an_answer)
{
    boot(true, false);
    vga_task(); // CODE_PAGE
    ASSERT_EQ(VGA_ANSWER_WAITING, vga_flash_take());
    deliver();
    ASSERT_EQ(VGA_ANSWER_ACK, vga_flash_take());
    ASSERT_TRUE(vga_connected());
}

/* The RIA's own flash: an answer is waited out, here to its timeout, and
 * nothing is awaited or still to go after, so the console, which runs from
 * RAM then, never calls into flash. */
UTEST(vga, ria_flashing_leaves_nothing_awaited)
{
    boot(true, true);
    vga_task(); // CODE_PAGE, never answered
    vga_flashing();
    ASSERT_FALSE(vga_connected());
    ASSERT_EQ(1u, response_count);
    vga_set_code_page(850);
    ASSERT_FALSE(vga_console_ready());
    ASSERT_EQ(1u, vterm_page_count);
}

UTEST(vga, ria_flashing_sends_nothing_awaited)
{
    boot_settled();
    vga_flashing();
    unsigned from = sent_count;
    vga_set_code_page(850);
    vga_stop();
    vga_task();
    ASSERT_TRUE(vga_console_ready());
    ASSERT_LT(sent_find(0xF, 0x01, from), 0);
    ASSERT_LT(sent_find(0xF, 0x00, from), 0);
}

/* ---- the selection ----------------------------------------------------- */

/* A change of the user's goes as SELECT_START, SELECT_SIDES and then
 * SELECT_END, the words of the selection as it is when it goes, once a
 * frame at most, and nothing goes without one. */
UTEST(vga, a_selection_goes_once_a_frame)
{
    boot_settled();
    unsigned from = sent_count;
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x0B, from), 0);
    user_selects(0x1234, 0x1300);
    vga_task();
    int at = sent_find(0xF, 0x0B, from);
    ASSERT_GE(at, 0);
    ASSERT_EQ(sent[at].word, 0x1234);
    ASSERT_EQ((int)sent_find(0xF, 0x0C, from), at + 1);
    ASSERT_EQ(sent[at + 1].word, 0x1234 ^ 0x1300);
    ASSERT_EQ((int)sent_find(0xF, 0x0D, from), at + 2);
    ASSERT_EQ(sent[at + 2].word, 0x1300);
    from = sent_count;
    user_selects(0x0001, 0x0002);
    user_selects(0x0003, 0x0004);
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x0B, from), 0);
    advance_ms(40); /* the frames go on by the clock */
    vga_task();
    at = sent_find(0xF, 0x0B, from);
    ASSERT_GE(at, 0);
    ASSERT_EQ(sent[at].word, 0x0003);
    ASSERT_EQ(sent[at + 2].word, 0x0004);
    ASSERT_LT(sent_find(0xF, 0x0B, (unsigned)at + 1), 0);
    from = sent_count;
    advance_ms(40);
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x0B, from), 0);
}

/* Nothing goes with no VGA, nor once a flash has begun, when nothing in
 * flash may run. */
UTEST(vga, no_selection_goes_while_flashing)
{
    boot_settled();
    vga_flashing();
    unsigned from = sent_count;
    user_selects(0x1234, 0x1300);
    advance_ms(40);
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x0B, from), 0);
}

UTEST(vga, no_selection_goes_with_no_vga)
{
    boot(false, false);
    unsigned from = sent_count;
    user_selects(0x1234, 0x1300);
    advance_ms(40);
    vga_task();
    ASSERT_LT(sent_find(0xF, 0x0B, from), 0);
}

UTEST_MAIN()
