/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The DMA that streams XRAM over PIX. test_vga.c never starts a stream, so
 * the channel is always idle. */

#ifndef _TEST_HARDWARE_DMA_H_
#define _TEST_HARDWARE_DMA_H_

#include <hardware/pio.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    int unused;
} dma_channel_config;
typedef struct
{
    struct
    {
        volatile uint32_t read_addr;
    } ch[16];
} dma_hw_t;
extern dma_hw_t test_dma;
#define dma_hw (&test_dma)

static inline uint dma_claim_unused_channel(bool required) { (void)required; return 0; }
static inline dma_channel_config dma_channel_get_default_config(uint ch) { (void)ch; return (dma_channel_config){0}; }
static inline void channel_config_set_dreq(dma_channel_config *c, uint d) { (void)c; (void)d; }
static inline void channel_config_set_ring(dma_channel_config *c, bool w, uint b) { (void)c; (void)w; (void)b; }
static inline void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *w,
                                         const volatile void *r, uint n, bool t)
{
    (void)ch; (void)c; (void)w; (void)r; (void)n; (void)t;
}
static inline bool dma_channel_is_busy(uint ch) { (void)ch; return false; }
static inline uint32_t dma_encode_transfer_count(uint32_t n) { return n; }
static inline void dma_channel_set_transfer_count(uint ch, uint32_t n, bool t) { (void)ch; (void)n; (void)t; }

#endif /* _TEST_HARDWARE_DMA_H_ */
