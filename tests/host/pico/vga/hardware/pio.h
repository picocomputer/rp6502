/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The Pico SDK's PIO API as the RIA's pix.c and vga.c use it, and the VGA's
 * ria.c. The FIFOs that carry messages are the test's: for test_vga.c,
 * PIX_SM's TX FIFO, where each word put is a message to the VGA, and the
 * backchannel's RX FIFO, which the fake VGA fills; for test_vga_reply.c,
 * the VGA's backchannel TX FIFO. The rest configures hardware the host has
 * not got, and does nothing. */

#ifndef _TEST_HARDWARE_PIO_H_
#define _TEST_HARDWARE_PIO_H_

#include <stdbool.h>
#include <stdint.h>

typedef unsigned int uint;

/* pico.h's, which every SDK header brings. A busy wait moves the test's clock
 * on (test_vga.c), as time passes on the chip. */
#define __force_inline inline __attribute__((always_inline))
void tight_loop_contents(void);
typedef struct pio_hw pio_hw_t;
typedef pio_hw_t *PIO;
struct pio_hw
{
    volatile uint32_t txf[4];
};
extern pio_hw_t test_pio[2];
#define pio0 (&test_pio[0])
#define pio1 (&test_pio[1])

typedef struct
{
    int unused;
} pio_program_t;

typedef struct
{
    int unused;
} pio_sm_config;

enum pio_fifo_join
{
    PIO_FIFO_JOIN_NONE,
    PIO_FIFO_JOIN_TX,
    PIO_FIFO_JOIN_RX,
};

enum pio_src_dest
{
    pio_x,
    pio_osr,
};

typedef enum
{
    pis_interrupt0,
    pis_sm3_rx_fifo_not_empty,
} pio_interrupt_source_t;

void pio_sm_put(PIO pio, uint sm, uint32_t data);
uint32_t pio_sm_get(PIO pio, uint sm);
bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm);
uint pio_sm_get_tx_fifo_level(PIO pio, uint sm);
void pio_set_irq1_source_enabled(PIO pio, pio_interrupt_source_t source, bool enabled);

static inline bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm) { return !pio_sm_get_tx_fifo_level(pio, sm); }
static inline void pio_set_irq0_source_enabled(PIO p, pio_interrupt_source_t s, bool e) { (void)p; (void)s; (void)e; }
static inline void pio_interrupt_clear(PIO p, uint i) { (void)p; (void)i; }
static inline uint pio_add_program(PIO p, const pio_program_t *prog) { (void)p; (void)prog; return 0; }
static inline void pio_gpio_init(PIO p, uint pin) { (void)p; (void)pin; }
static inline void pio_sm_init(PIO p, uint sm, uint off, const pio_sm_config *c) { (void)p; (void)sm; (void)off; (void)c; }
static inline void pio_sm_set_enabled(PIO p, uint sm, bool e) { (void)p; (void)sm; (void)e; }
static inline void pio_enable_sm_mask_in_sync(PIO p, uint32_t m) { (void)p; (void)m; }
static inline void pio_sm_set_consecutive_pindirs(PIO p, uint sm, uint b, uint n, bool o) { (void)p; (void)sm; (void)b; (void)n; (void)o; }
static inline void pio_sm_set_clkdiv_int_frac(PIO p, uint sm, uint16_t i, uint8_t f) { (void)p; (void)sm; (void)i; (void)f; }
static inline void pio_clkdiv_restart_sm_mask(PIO p, uint32_t m) { (void)p; (void)m; }
static inline void pio_sm_exec_wait_blocking(PIO p, uint sm, uint instr) { (void)p; (void)sm; (void)instr; }
static inline uint pio_encode_pull(bool a, bool b) { (void)a; (void)b; return 0; }
static inline uint pio_encode_mov(enum pio_src_dest d, enum pio_src_dest s) { (void)d; (void)s; return 0; }
static inline uint pio_get_dreq(PIO p, uint sm, bool tx) { (void)p; (void)sm; (void)tx; return 0; }
static inline void sm_config_set_out_pins(pio_sm_config *c, uint b, uint n) { (void)c; (void)b; (void)n; }
static inline void sm_config_set_in_pins(pio_sm_config *c, uint b) { (void)c; (void)b; }
static inline void sm_config_set_out_shift(pio_sm_config *c, bool r, bool a, uint t) { (void)c; (void)r; (void)a; (void)t; }
static inline void sm_config_set_in_shift(pio_sm_config *c, bool r, bool a, uint t) { (void)c; (void)r; (void)a; (void)t; }
static inline void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join j) { (void)c; (void)j; }
static inline void sm_config_set_wrap(pio_sm_config *c, uint t, uint w) { (void)c; (void)t; (void)w; }
static inline void sm_config_set_clkdiv(pio_sm_config *c, float d) { (void)c; (void)d; }
static inline void sm_config_set_sideset_pins(pio_sm_config *c, uint b) { (void)c; (void)b; }
static inline void pio_sm_set_clkdiv(PIO p, uint sm, float d) { (void)p; (void)sm; (void)d; }
static inline void pio_sm_set_pins_with_mask(PIO p, uint sm, uint32_t v, uint32_t m) { (void)p; (void)sm; (void)v; (void)m; }
static inline void pio_sm_set_pindirs_with_mask(PIO p, uint sm, uint32_t v, uint32_t m) { (void)p; (void)sm; (void)v; (void)m; }

#endif /* _TEST_HARDWARE_PIO_H_ */
