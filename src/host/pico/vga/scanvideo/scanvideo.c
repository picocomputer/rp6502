/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include "pico.h"

#pragma GCC push_options
#pragma GCC optimize("O3")

#include "pico/sync.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "scanvideo.h"
#include "scanvideo.pio.h"
#include "pico/binary_info.h"

#define SCANVIDEO_SCANLINE_BUFFER_COUNT 10
#define SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS 323
#define SCANVIDEO_COLOR_PIN_BASE 6
#define SCANVIDEO_COLOR_PIN_COUNT 16
#define SCANVIDEO_SYNC_PIN_BASE 26

// One lock for everything the cores and the line interrupt share.
#define SCANVIDEO_LOCK spin_lock_instance(2)

// ======================

// A plane's state machine and its DMA channel have the plane's number.
#define SCANVIDEO_TIMING_SM 3u
#define SCANVIDEO_SCANLINE_SM_MASK 0x7u
#define SCANVIDEO_ALL_SM_MASK 0xfu
#define SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK 0x7u
#define SCANVIDEO_TIMING_DMA_CHANNEL 3u
#define SCANVIDEO_TIMING_CB_DMA_CHANNEL 4u
#define SCANVIDEO_TIMING_DMA_CHANNELS_MASK 0x18u

#define video_pio pio0

// The composable program loads at 0: its offsets are in the scanline data.
#define PIO_OFFSET(x) composable_offset_##x
#define COMPOSABLE_COLOR_RUN ((unsigned)composable_offset_color_run)
#define COMPOSABLE_EOL_ALIGN ((unsigned)composable_offset_end_of_scanline_ALIGN)
#define COMPOSABLE_EOL_SKIP_ALIGN ((unsigned)composable_offset_end_of_scanline_skip_ALIGN)
#define COMPOSABLE_RAW_RUN ((unsigned)composable_offset_raw_run)
#define COMPOSABLE_RAW_1P ((unsigned)composable_offset_raw_1p)

#define PIO_WAIT_IRQ4 pio_encode_wait_irq(1, false, 4)
static uint8_t video_htiming_load_offset;

// --- video timing stuff

// These values index the instructions of video_htiming_states in scanvideo.pio.
enum
{
    SET_IRQ_0 = 0u,
    SET_IRQ_1 = 1u,
    SET_IRQ_SCANLINE = 2u,
    CLEAR_IRQ_SCANLINE = 3u,
};

// The timing state machine is fed by DMA, with no help from the CPU, so sync
// carries on whatever the cores are doing. A line is four words. The frame has
// four parts: the vblank lines before, during and after the vsync pulse, then
// the active lines. Every line in a part is the same, so a part is one aligned
// line that the timing channel reads round and round. The control channel
// loads the timing channel with each part in turn from timing_blocks. The last
// block has the timing channel point the control channel back at the first.
// The frame starts in vblank, so the scanline side starts with a whole frame.
enum
{
    TIMING_FRONT_PORCH,
    TIMING_PULSE,
    TIMING_BACK_PORCH,
    TIMING_ACTIVE,
    TIMING_PARTS
};
static uint32_t __attribute__((aligned(16))) timing_lines[TIMING_PARTS][4];

// Matches the timing channel's al3 registers, which the control channel writes.
typedef struct
{
    uint32_t ctrl;
    uint32_t write_addr;
    uint32_t transfer_count;
    uint32_t read_addr_trig;
} timing_block_t;
static timing_block_t timing_blocks[TIMING_PARTS + 1];
static timing_block_t *timing_blocks_start = timing_blocks;

// --- scanline stuff
// A scanline buffer with how many words of each plane the DMA sends, and the
// link of the one list it is in. A plane's words are a header, the pixels,
// and a trailer; the pixels start at the second word.
typedef struct full_scanline_buffer
{
    scanvideo_scanline_buffer_t core;
    uint16_t used[SCANVIDEO_PLANE_COUNT];
    struct full_scanline_buffer *next;
} full_scanline_buffer_t;

static full_scanline_buffer_t scanline_buffers[SCANVIDEO_SCANLINE_BUFFER_COUNT];

/* In the VGA's VIDEO region, away from the code; see vga/ld/. */
static uint32_t __attribute__((section(".video_data.scanline_data"))) scanline_data[SCANVIDEO_PLANE_COUNT][SCANVIDEO_SCANLINE_BUFFER_COUNT][SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS];

// This state is shared by both cores and the line interrupt on core 0. The
// cores take its lock with interrupts off; the line interrupt, which nothing
// preempts, takes it as it is. A buffer is on the free list, or with the core
// that is drawing it, or on the generated list in the order of its line, or
// with the line interrupt, which shows it and puts it back on the free list.
static struct
{
    uint32_t last_scanline_id;
    uint32_t next_scanline_id;
    full_scanline_buffer_t *generated_ascending_scanline_id_list;
    full_scanline_buffer_t *generated_ascending_scanline_id_list_tail;
    full_scanline_buffer_t *free_list;
} shared_state;

// What only the line interrupt touches: the scanline DMA; the buffer being
// shown and how many display lines it has been shown on; and the buffer whose
// last line the DMA is sending, which is free once that DMA has ended.
static struct
{
    full_scanline_buffer_t *current;
    full_scanline_buffer_t *retiring;
    uint8_t y_repeat_index;
    bool in_progress;
} scanline_dma;

// A line with nothing on it: one transparent pixel, which on the planes over
// the first clears their sticky alpha.
static uint32_t blank_scanline_data[] = {
    COMPOSABLE_RAW_1P | (0 << 16),
    COMPOSABLE_EOL_SKIP_ALIGN,
};

// The line shown when a line's buffer is not ready: blue, the width of the
// view, which a mode set writes into its second word.
static uint32_t missing_scanline_data[] = {
    COMPOSABLE_COLOR_RUN | (SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255) << 16u),
    /*width-3*/ 0u | (COMPOSABLE_RAW_1P << 16u),
    0u | (COMPOSABLE_EOL_ALIGN << 16u)};

static inline bool is_scanline_after(uint32_t scanline_id1, uint32_t scanline_id2)
{
    return ((int32_t)(scanline_id1 - scanline_id2)) > 0;
}

// -- MISC stuff
static scanvideo_view_t video_mode;
// The display line the line interrupt has reached: the active lines from 0,
// then the lines of the vertical blank.
static volatile int32_t display_scanline_pos;
static int32_t v_content_start;
static int32_t v_content_end;
static volatile bool generation_allowed;

inline static void list_prepend(full_scanline_buffer_t **phead, full_scanline_buffer_t *fsb)
{
    fsb->next = *phead;
    *phead = fsb;
}

inline static full_scanline_buffer_t *list_remove_head(full_scanline_buffer_t **phead)
{
    full_scanline_buffer_t *fsb = *phead;

    if (fsb)
    {
        *phead = fsb->next;
        fsb->next = NULL;
    }

    return fsb;
}

inline static full_scanline_buffer_t *list_remove_head_ascending(full_scanline_buffer_t **phead,
                                                                 full_scanline_buffer_t **ptail)
{
    full_scanline_buffer_t *fsb = *phead;

    if (fsb)
    {
        *phead = fsb->next;

        if (!fsb->next)
            *ptail = NULL;
        fsb->next = NULL;
    }

    return fsb;
}

static inline uint32_t scanline_id_after(uint32_t scanline_id)
{
    uint32_t tmp = scanline_id & 0xffffu;

    if (tmp < video_mode.height - 1u)
    {
        return scanline_id + 1;
    }
    else
    {
        return scanline_id + 0x10000u - tmp;
    }
}

inline static void list_insert_ascending(full_scanline_buffer_t **phead, full_scanline_buffer_t **ptail,
                                         full_scanline_buffer_t *fsb)
{

    if (!*phead || !is_scanline_after(fsb->core.scanline_id, (*phead)->core.scanline_id))
    {
        if (!*phead)
        {
            *ptail = fsb;
        }

        // insert at the beginning
        list_prepend(phead, fsb);
    }
    else
    {
        if (is_scanline_after(fsb->core.scanline_id, (*ptail)->core.scanline_id))
        {
            // insert at end
            (*ptail)->next = fsb;
            *ptail = fsb;
        }
        else
        {
            // not after
            full_scanline_buffer_t *prev = *phead;

            while (prev->next && is_scanline_after(fsb->core.scanline_id, prev->next->core.scanline_id))
            {
                prev = prev->next;
            }

            fsb->next = prev->next;
            prev->next = fsb;
        }
    }
}

// The caller holds the lock. The buffer the last line's DMA was sending has
// been shown for the last time.
inline static void scanline_locked_retire(void)
{
    if (scanline_dma.retiring)
    {
        list_prepend(&shared_state.free_list, scanline_dma.retiring);
        scanline_dma.retiring = NULL;
    }
}

// The caller holds the lock. With no buffer current, the next line's is taken
// from the generated list, and any for a line gone by are freed. Returns the
// current buffer, or NULL when the next line's has not been generated.
inline static full_scanline_buffer_t *scanline_locked_latch(void)
{
    full_scanline_buffer_t *fsb = scanline_dma.current;

    if (!fsb)
    {
        while (NULL != (fsb = shared_state.generated_ascending_scanline_id_list))
        {
            if (!is_scanline_after(shared_state.next_scanline_id, fsb->core.scanline_id))
            {
                if (shared_state.next_scanline_id == fsb->core.scanline_id)
                {
                    list_remove_head_ascending(
                        &shared_state.generated_ascending_scanline_id_list,
                        &shared_state.generated_ascending_scanline_id_list_tail);
                    scanline_dma.current = fsb;
                }
                else
                {
                    fsb = NULL;
                }

                break;
            }
            else
            {
                // scanline is in the past
                list_remove_head_ascending(
                    &shared_state.generated_ascending_scanline_id_list,
                    &shared_state.generated_ascending_scanline_id_list_tail);
                list_prepend(&shared_state.free_list, fsb);
            }
        }
    }

    return fsb;
}

static inline void __not_in_flash_func(abort_all_dma_channels)(void)
{
    dma_hw->abort = SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK;
    for (uint i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
        while (dma_channel_is_busy(i))
            tight_loop_contents();
}

// Ends the last line's scanline DMA, at the start of the next line, before
// any new transfer starts. By then the three channels have finished, unless a
// state machine stalled, and those are cut short. The channels raise no
// interrupt; their raw status is read, and cleared for the next line.
static inline void __not_in_flash_func(finish_scanline_dma)(void)
{
    if (scanline_dma.in_progress)
    {
        if ((dma_hw->intr & SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK) != SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK)
            abort_all_dma_channels();
        scanline_dma.in_progress = false;
    }
    dma_hw->intr = SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK;
}

static inline void __not_in_flash_func(recover_scanline_sms)(void)
{
    for (uint sm = 0; sm < SCANVIDEO_PLANE_COUNT; sm++)
    {
        if (!pio_sm_is_tx_fifo_empty(video_pio, sm))
        {
            pio_sm_clear_fifos(video_pio, sm);
            pio_sm_exec(video_pio, sm, pio_encode_out(pio_null, 32));
        }
        if (video_pio->sm[sm].instr != PIO_WAIT_IRQ4)
        {
            pio_sm_exec(video_pio, sm, pio_encode_wait_irq(1, false, 4));
            if (pio_sm_is_exec_stalled(video_pio, sm))
            {
                if (video_pio->sm[sm].addr != PIO_OFFSET(entry_point) + 1u)
                {
                    pio_sm_exec(video_pio, sm, pio_encode_jmp(PIO_OFFSET(entry_point)));
                }
            }
            else
            {
                pio_sm_exec(video_pio, sm, pio_encode_jmp(PIO_OFFSET(entry_point) + 1));
            }
        }
    }
}

// Starts the three planes' DMA. The channel numbers are written out so each
// start is a store to a known register.
static inline void __not_in_flash_func(start_scanline_dma)(const uint32_t *data0, uint32_t used0,
                                                           const uint32_t *data1, uint32_t used1,
                                                           const uint32_t *data2, uint32_t used2)
{
    dma_channel_transfer_from_buffer_now(0, data0, used0);
    dma_channel_transfer_from_buffer_now(1, data1, used1);
    dma_channel_transfer_from_buffer_now(2, data2, used2);
    scanline_dma.in_progress = true;
}

// The words of a plane of a buffer: its pixels start at the second.
static inline uint32_t *scanline_words(const full_scanline_buffer_t *fsb, int plane)
{
    return (uint32_t *)fsb->core.pixels[plane] - 1;
}

// Active line `line` of the display. From the line interrupt to the start of
// the DMA there is the sync pulse and the back porch, less what the state
// machines hold: 1,152 cycles at 640x480, 720 at 1280x1024 and 520 at
// 1280x720. A DMA started after that loses the whole line.
static void __not_in_flash_func(prepare_for_active_scanline_irqs_enabled)(int32_t line)
{
    finish_scanline_dma();

    if (line < v_content_start || line >= v_content_end)
    {
        // Above or below the content the line is blank.
        recover_scanline_sms();
        start_scanline_dma(blank_scanline_data, count_of(blank_scanline_data),
                           blank_scanline_data, count_of(blank_scanline_data),
                           blank_scanline_data, count_of(blank_scanline_data));
        if (scanline_dma.retiring)
        {
            spin_lock_unsafe_blocking(SCANVIDEO_LOCK);
            scanline_locked_retire();
            spin_unlock_unsafe(SCANVIDEO_LOCK);
        }
        return;
    }

    spin_lock_unsafe_blocking(SCANVIDEO_LOCK);
    scanline_locked_retire();
    // The buffer latched is the next line's, or there is none and the
    // missing line is shown in its place.
    full_scanline_buffer_t *fsb = scanline_locked_latch();
    bool last = false;
    if (++scanline_dma.y_repeat_index >= video_mode.y_scale)
    {
        last = true;
        scanline_dma.y_repeat_index = 0;
        shared_state.next_scanline_id = scanline_id_after(shared_state.next_scanline_id);
        scanline_dma.current = NULL;
    }
    spin_unlock_unsafe(SCANVIDEO_LOCK);

    recover_scanline_sms();
    if (fsb)
        start_scanline_dma(scanline_words(fsb, 0), fsb->used[0],
                           scanline_words(fsb, 1), fsb->used[1],
                           scanline_words(fsb, 2), fsb->used[2]);
    else
        start_scanline_dma(missing_scanline_data, count_of(missing_scanline_data),
                           blank_scanline_data, count_of(blank_scanline_data),
                           blank_scanline_data, count_of(blank_scanline_data));
    if (last)
        scanline_dma.retiring = fsb;
}

// A line of the vertical blank, `first` for its first.
static void __not_in_flash_func(prepare_for_vblank_scanline_irqs_enabled)(bool first)
{
    finish_scanline_dma();

    spin_lock_unsafe_blocking(SCANVIDEO_LOCK);
    scanline_locked_retire();

    // The next line shown is the first of a frame.
    if (first)
    {
        scanline_dma.y_repeat_index = 0;
        if (scanvideo_scanline_number(shared_state.next_scanline_id) != 0)
            shared_state.next_scanline_id =
                (scanvideo_frame_number(shared_state.next_scanline_id) + 1u) << 16u;
    }

    // A buffer still current from a line gone by, after a line interrupt was
    // lost, is let go.
    if (scanline_dma.current && is_scanline_after(shared_state.next_scanline_id,
                                                  scanline_dma.current->core.scanline_id))
    {
        list_prepend(&shared_state.free_list, scanline_dma.current);
        scanline_dma.current = NULL;
    }
    scanline_locked_latch();

    spin_unlock_unsafe(SCANVIDEO_LOCK);
}

// The timing state machine raises one interrupt at the start of an active
// line and another at the start of a line of the vertical blank, so which
// one it is says where in the frame the display is, whatever was counted.
void __isr __not_in_flash_func(isr_pio0_0)()
{
    const int32_t v_active = video_mode.default_timing->v_active;
    if (video_pio->irq & 1u)
    {
        video_pio->irq = 1;
        // Only the vertical blank counts past the active lines, so only
        // after it is this the first. A view set part way down a frame
        // starts the count at the end of its content, and from there the
        // count stops at the last active line: the rest of that frame is
        // blank.
        int32_t line = display_scanline_pos;
        if (line > v_active)
            line = 0;
        prepare_for_active_scanline_irqs_enabled(line);
        display_scanline_pos = line < v_active ? line + 1 : v_active;
    }
    if (video_pio->irq & 2u)
    {
        video_pio->irq = 3;
        const int32_t line = display_scanline_pos;
        const bool first = line <= v_active;
        prepare_for_vblank_scanline_irqs_enabled(first);
        display_scanline_pos = (first ? v_active : line) + 1;
    }
}

// A state machine set up and left paused: a plane's, or the timing one's.
// The pixel clock is the system clock divided by a whole even number, and a
// state machine takes two of its clocks for a pixel.
static void setup_sm(uint sm)
{
    const bool is_scanline_sm = sm != SCANVIDEO_TIMING_SM;
    const uint offset = is_scanline_sm ? 0 : video_htiming_load_offset;
    pio_sm_config config = is_scanline_sm ? composable_program_get_default_config(offset)
                                          : video_htiming_program_get_default_config(offset);
    sm_config_set_clkdiv_int_frac(&config, clock_get_hz(clk_sys) / video_mode.default_timing->clock_freq / 2, 0);
    sm_config_set_out_shift(&config, true, true, 32); // autopull
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    if (is_scanline_sm)
    {
        pio_sm_set_consecutive_pindirs(video_pio, sm, SCANVIDEO_COLOR_PIN_BASE, SCANVIDEO_COLOR_PIN_COUNT, true);
        sm_config_set_out_pins(&config, SCANVIDEO_COLOR_PIN_BASE, SCANVIDEO_COLOR_PIN_COUNT);
        // A plane over the first drives the pins only for an opaque pixel.
        if (sm)
            sm_config_set_out_special(&config, 1, 1, SCANVIDEO_ALPHA_PIN);
        else
            sm_config_set_out_special(&config, 1, 0, 0);
    }
    else
    {
        // The FIFO is eight words, two lines, in front of the state machine.
        sm_config_set_out_pins(&config, SCANVIDEO_SYNC_PIN_BASE, 2);
        pio_sm_set_consecutive_pindirs(video_pio, sm, SCANVIDEO_SYNC_PIN_BASE, 2, true);
    }

    pio_sm_init(video_pio, sm, offset, &config); // now paused
}

// A buffer to draw the next line in, or NULL when there is none free or the
// frame's lines have all been handed out and the next frame is not near. The
// two tests before the lock keep a core with nothing to do off the lock, which
// the line interrupt takes on its way to starting the DMA. The first is made
// again inside it: the other core may have taken the frame's last line since,
// and the next frame's first is not drawn until the vertical blank is near.
scanvideo_scanline_buffer_t *__not_in_flash_func(scanvideo_begin_scanline_generation)(void)
{
    if (!generation_allowed)
    {
        int32_t distance = v_content_start - display_scanline_pos;
        if (distance < 0)
            distance += video_mode.default_timing->v_total;
        if (distance < SCANVIDEO_SCANLINE_BUFFER_COUNT)
            generation_allowed = true;
        else
            return NULL;
    }
    if (!__atomic_load_n(&shared_state.free_list, __ATOMIC_RELAXED))
        return NULL;

    uint32_t save = spin_lock_blocking(SCANVIDEO_LOCK);
    full_scanline_buffer_t *fsb = generation_allowed ? list_remove_head(&shared_state.free_list) : NULL;
    if (fsb)
    {
        uint32_t scanline_id = shared_state.next_scanline_id;

        if (!is_scanline_after(scanline_id, shared_state.last_scanline_id))
        {
            scanline_id = scanline_id_after(shared_state.last_scanline_id);
        }

        fsb->core.scanline_id = shared_state.last_scanline_id = scanline_id;
        if (scanvideo_scanline_number(scanline_id) >= video_mode.height - 1)
            generation_allowed = false;
    }
    spin_unlock(SCANVIDEO_LOCK, save);

    return (scanvideo_scanline_buffer_t *)fsb;
}

// The line is drawn. A plane that was filled is a run of its pixels and one
// transparent pixel to end on; one that was not is the blank line. The header
// takes the first pixel and the count takes the place the pixel left, as the
// composable program reads them.
void __not_in_flash_func(scanvideo_end_scanline_generation)(
    scanvideo_scanline_buffer_t *scanline_buffer, const bool filled[SCANVIDEO_PLANE_COUNT])
{
    full_scanline_buffer_t *fsb = (full_scanline_buffer_t *)scanline_buffer;
    const uint32_t width = video_mode.width;
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        uint32_t *data = scanline_words(fsb, i);
        if (filled[i])
        {
            data[0] = COMPOSABLE_RAW_RUN | (data[1] << 16);
            data[1] = (width - 3) | (data[1] & 0xFFFF0000);
            data[width / 2 + 1] = COMPOSABLE_RAW_1P | (0 << 16);
            data[width / 2 + 2] = COMPOSABLE_EOL_SKIP_ALIGN;
            fsb->used[i] = (uint16_t)(width / 2 + 3);
        }
        else
        {
            data[0] = COMPOSABLE_RAW_1P | (0 << 16);
            data[1] = COMPOSABLE_EOL_SKIP_ALIGN;
            fsb->used[i] = 2;
        }
    }
    uint32_t save = spin_lock_blocking(SCANVIDEO_LOCK);
    list_insert_ascending(&shared_state.generated_ascending_scanline_id_list,
                          &shared_state.generated_ascending_scanline_id_list_tail, fsb);
    spin_unlock(SCANVIDEO_LOCK, save);
}

// A mode set is in three parts: what is done once, the view, and the timing.
// A view is a canvas on a display. Views of one timing change with the sync
// carrying on, so the monitor does not lose it.

// Once: the pins, the two programs, and the claims.
static void scanvideo_once(void)
{
    spin_lock_init(2);

    uint pin_mask = 3u << SCANVIDEO_SYNC_PIN_BASE;
    bi_decl_if_func_used(bi_2pins_with_names(SCANVIDEO_SYNC_PIN_BASE, "HSync",
                                             SCANVIDEO_SYNC_PIN_BASE + 1, "VSync"));

    static_assert(SCANVIDEO_PIXEL_RSHIFT + SCANVIDEO_PIXEL_RCOUNT <= SCANVIDEO_COLOR_PIN_COUNT, "red bits do not fit in color pins");
    static_assert(SCANVIDEO_PIXEL_GSHIFT + SCANVIDEO_PIXEL_GCOUNT <= SCANVIDEO_COLOR_PIN_COUNT, "green bits do not fit in color pins");
    static_assert(SCANVIDEO_PIXEL_BSHIFT + SCANVIDEO_PIXEL_BCOUNT <= SCANVIDEO_COLOR_PIN_COUNT, "blue bits do not fit in color pins");
#define RMASK ((1u << SCANVIDEO_PIXEL_RCOUNT) - 1u)
#define GMASK ((1u << SCANVIDEO_PIXEL_GCOUNT) - 1u)
#define BMASK ((1u << SCANVIDEO_PIXEL_BCOUNT) - 1u)
    pin_mask |= RMASK << (SCANVIDEO_COLOR_PIN_BASE + SCANVIDEO_PIXEL_RSHIFT);
    pin_mask |= GMASK << (SCANVIDEO_COLOR_PIN_BASE + SCANVIDEO_PIXEL_GSHIFT);
    pin_mask |= BMASK << (SCANVIDEO_COLOR_PIN_BASE + SCANVIDEO_PIXEL_BSHIFT);
    bi_decl_if_func_used(bi_pin_mask_with_name(RMASK << (SCANVIDEO_COLOR_PIN_BASE + SCANVIDEO_PIXEL_RSHIFT), "Red"));
    bi_decl_if_func_used(bi_pin_mask_with_name(GMASK << (SCANVIDEO_COLOR_PIN_BASE + SCANVIDEO_PIXEL_GSHIFT), "Green"));
    bi_decl_if_func_used(bi_pin_mask_with_name(BMASK << (SCANVIDEO_COLOR_PIN_BASE + SCANVIDEO_PIXEL_BSHIFT), "Blue"));

    for (uint8_t i = 0; pin_mask; i++, pin_mask >>= 1u)
    {
        if (pin_mask & 1)
            gpio_set_function(i, GPIO_FUNC_PIO0);
    }

    // The composable program is first, so it is at 0. A view writes it again
    // with its own delays.
    pio_add_program(video_pio, &composable_program);
    video_htiming_load_offset = pio_add_program(video_pio, &video_htiming_program);
    pio_claim_sm_mask(video_pio, SCANVIDEO_ALL_SM_MASK);

    dma_claim_mask(SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK | SCANVIDEO_TIMING_DMA_CHANNELS_MASK);
    for (uint i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        dma_channel_config channel_config = dma_channel_get_default_config(i);
        channel_config_set_dreq(&channel_config, DREQ_PIO0_TX0 + i);
        dma_channel_configure(i,
                              &channel_config,
                              &video_pio->txf[i],
                              NULL,
                              0,
                              false);
    }

    irq_set_priority(PIO0_IRQ_0, 0);
    pio_set_irq0_source_mask_enabled(video_pio, (1u << pis_interrupt0) | (1u << pis_interrupt1), true);
}

// The view: where its lines are on the display, the composable program with
// the delays of its pixel width, and every buffer free for the frame after
// the one that was being drawn. The caller has the line interrupt off and
// the cores held. The planes' state machines are left ready and paused.
static void scanvideo_view(const scanvideo_view_t *mode, bool same_timing)
{
    for (uint i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
        dma_channel_abort(i);
    pio_set_sm_mask_enabled(video_pio, SCANVIDEO_SCANLINE_SM_MASK, false);

    const uint32_t next_frame = (scanvideo_frame_number(shared_state.next_scanline_id) + 1u) << 16u;
    video_mode = *mode;
    v_content_start = mode->y_offset;
    v_content_end = mode->y_offset +
                    (uint32_t)mode->height * mode->y_scale;
    ((uint16_t *)(missing_scanline_data))[2] = mode->width / 2 - 3;

    const unsigned delay0 = 2u * mode->x_scale - 2, delay1 = delay0 + 1;
    for (uint i = 0; i < composable_program.length; i++)
    {
        uint16_t instruction = composable_program.instructions[i];
        if (i == PIO_OFFSET(delay_a_1) || i == PIO_OFFSET(delay_b_1))
            instruction |= delay1 << 8u;
        else if (i == PIO_OFFSET(delay_c_0) || i == PIO_OFFSET(delay_d_0) ||
                 i == PIO_OFFSET(delay_e_0) || i == PIO_OFFSET(delay_h_0))
            instruction |= delay0 << 8u;
        video_pio->instr_mem[i] = instruction;
    }

    memset(&shared_state, 0, sizeof(shared_state));
    memset(&scanline_dma, 0, sizeof(scanline_dma));
    shared_state.next_scanline_id = next_frame;
    // One before, so the first line handed out is the next.
    shared_state.last_scanline_id = next_frame - 1;
    // The pixels are left as they are: a line is written before it is shown.
    for (int i = 0; i < SCANVIDEO_SCANLINE_BUFFER_COUNT; i++)
    {
        for (int plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
            scanline_buffers[i].core.pixels[plane] = (uint16_t *)(scanline_data[plane][i] + 1);
        scanline_buffers[i].next = i != SCANVIDEO_SCANLINE_BUFFER_COUNT - 1 ? &scanline_buffers[i + 1] : NULL;
    }
    shared_state.free_list = &scanline_buffers[0];
    __mem_fence_release();

    // With the timing going on, the rest of this frame is blank from the line
    // after the content, and the next frame can be drawn at once. A new
    // timing starts in the vertical blank.
    display_scanline_pos = same_timing ? v_content_end : 0;
    generation_allowed = same_timing;

    for (uint i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        setup_sm(i);
        pio_sm_exec(video_pio, i, pio_encode_jmp(PIO_OFFSET(entry_point)));
    }
}

// The words the timing state machine is fed, for each part of the frame.
static void scanvideo_timing_words(const scanvideo_timing_t *timing)
{
#define HTIMING_MIN 8

#define TIMING_CYCLE 3u
#define timing_encode(state, length, pins) (video_htiming_states_program.instructions[state] | (((uint32_t)(length) - TIMING_CYCLE) << 16u) | ((uint32_t)(pins) << 29u))
#define A_CMD SET_IRQ_0
#define A_CMD_VBLANK SET_IRQ_1
#define B1_CMD CLEAR_IRQ_SCANLINE
#define B2_CMD CLEAR_IRQ_SCANLINE
#define C_CMD SET_IRQ_SCANLINE
#define C_CMD_VBLANK CLEAR_IRQ_SCANLINE

    int h_sync_bit = timing->h_sync_polarity ? 0 : 1;
    const uint32_t a = timing_encode(A_CMD, 4, h_sync_bit);
    static_assert(HTIMING_MIN >= 4, "");
    const uint32_t a_vblank = timing_encode(A_CMD_VBLANK, 4, h_sync_bit);
    int h_back_porch = timing->h_total - timing->h_front_porch - timing->h_pulse - timing->h_active;

    assert(timing->h_pulse - 4 >= HTIMING_MIN);
    const uint32_t b1 = timing_encode(B1_CMD, timing->h_pulse - 4, h_sync_bit);

    assert(timing->h_active >= HTIMING_MIN);
    assert(h_back_porch >= HTIMING_MIN);
    assert((timing->h_total - h_back_porch - timing->h_pulse) >= HTIMING_MIN);
    const uint32_t b2 = timing_encode(B2_CMD, h_back_porch, !h_sync_bit);
    const uint32_t c = timing_encode(C_CMD, timing->h_total - h_back_porch - timing->h_pulse, 4 | !h_sync_bit);
    const uint32_t c_vblank = timing_encode(C_CMD_VBLANK, timing->h_total - h_back_porch - timing->h_pulse, !h_sync_bit);

    const uint32_t vsync_bit = 0x40000000;
    const uint32_t vsync_pulse = timing->v_sync_polarity ? 0 : vsync_bit;
    const uint32_t vsync_none = timing->v_sync_polarity ? vsync_bit : 0;
    const uint32_t part_lines[TIMING_PARTS] = {
        [TIMING_FRONT_PORCH] = timing->v_front_porch,
        [TIMING_PULSE] = timing->v_pulse,
        [TIMING_BACK_PORCH] = timing->v_total - timing->v_active - timing->v_front_porch - timing->v_pulse,
        [TIMING_ACTIVE] = timing->v_active,
    };
    for (int part = 0; part < TIMING_PARTS; part++)
    {
        assert(part_lines[part]);
        const bool active = part == TIMING_ACTIVE;
        const uint32_t vsync = part == TIMING_PULSE ? vsync_pulse : vsync_none;
        timing_lines[part][0] = (active ? a : a_vblank) | vsync;
        timing_lines[part][1] = b1 | vsync;
        timing_lines[part][2] = b2 | vsync;
        timing_lines[part][3] = (active ? c : c_vblank) | vsync;
    }

    // The timing channel, paced by the timing state machine, reads one part's
    // line round and round, then has the control channel load the next part.
    dma_channel_config cfg = dma_channel_get_default_config(SCANVIDEO_TIMING_DMA_CHANNEL);
    channel_config_set_dreq(&cfg, pio_get_dreq(video_pio, SCANVIDEO_TIMING_SM, true));
    channel_config_set_ring(&cfg, false, 4);
    channel_config_set_chain_to(&cfg, SCANVIDEO_TIMING_CB_DMA_CHANNEL);
    channel_config_set_irq_quiet(&cfg, true);
    channel_config_set_high_priority(&cfg, true);
    for (int part = 0; part < TIMING_PARTS; part++)
        timing_blocks[part] = (timing_block_t){
            .ctrl = channel_config_get_ctrl_value(&cfg),
            .write_addr = (uint32_t)&video_pio->txf[SCANVIDEO_TIMING_SM],
            .transfer_count = part_lines[part] * 4,
            .read_addr_trig = (uint32_t)timing_lines[part],
        };
    // The last block writes the start of the list to the control channel.
    cfg = dma_channel_get_default_config(SCANVIDEO_TIMING_DMA_CHANNEL);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_chain_to(&cfg, SCANVIDEO_TIMING_CB_DMA_CHANNEL);
    channel_config_set_irq_quiet(&cfg, true);
    timing_blocks[TIMING_PARTS] = (timing_block_t){
        .ctrl = channel_config_get_ctrl_value(&cfg),
        .write_addr = (uint32_t)&dma_hw->ch[SCANVIDEO_TIMING_CB_DMA_CHANNEL].read_addr,
        .transfer_count = 1,
        .read_addr_trig = (uint32_t)&timing_blocks_start,
    };
}

// Stops the line interrupt, the four state machines and the timing DMA. Sync
// is lost until scanvideo_timing_start.
static void scanvideo_timing_stop(void)
{
    irq_set_enabled(PIO0_IRQ_0, false);
    pio_set_sm_mask_enabled(video_pio, SCANVIDEO_ALL_SM_MASK, false);
    // The control channel stops first, and its writes in flight land
    // before the timing channel it writes is stopped. RP2350-E5: a
    // channel must be disabled before an abort, as must the channel
    // it chains to, or the abort can trigger it again.
    hw_clear_bits(&dma_hw->ch[SCANVIDEO_TIMING_CB_DMA_CHANNEL].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = 1u << SCANVIDEO_TIMING_CB_DMA_CHANNEL;
    while (dma_hw->abort)
        tight_loop_contents();
    hw_clear_bits(&dma_hw->ch[SCANVIDEO_TIMING_DMA_CHANNEL].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = 1u << SCANVIDEO_TIMING_DMA_CHANNEL;
    while (dma_hw->abort)
        tight_loop_contents();
}

// Starts the timing of the view from the top of a frame, and the line
// interrupt. The four state machines start on one clock, so the pixels keep
// their place against the sync.
static void scanvideo_timing_start(void)
{
    // Set up again, which empties its FIFO of the last timing's words.
    setup_sm(SCANVIDEO_TIMING_SM);
    pio_sm_exec(video_pio, SCANVIDEO_TIMING_SM, video_htiming_states_program.instructions[CLEAR_IRQ_SCANLINE]);
    pio_sm_exec(video_pio, SCANVIDEO_TIMING_SM,
                pio_encode_jmp(video_htiming_load_offset + video_htiming_offset_entry_point));
    // A line interrupt raised as the last timing stopped is not this one's.
    video_pio->irq = 3;
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_clkdiv_restart_sm_mask(video_pio, SCANVIDEO_ALL_SM_MASK);
    // The control channel loads the first part of the frame, and the
    // timing state machine starts once that has filled its FIFO.
    dma_channel_config cfg = dma_channel_get_default_config(SCANVIDEO_TIMING_CB_DMA_CHANNEL);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_ring(&cfg, true, 4);
    channel_config_set_irq_quiet(&cfg, true);
    dma_channel_configure(SCANVIDEO_TIMING_CB_DMA_CHANNEL, &cfg,
                          &dma_hw->ch[SCANVIDEO_TIMING_DMA_CHANNEL].al3_ctrl,
                          timing_blocks, 4, true);
    while (!pio_sm_is_tx_fifo_full(video_pio, SCANVIDEO_TIMING_SM))
        tight_loop_contents();
    pio_set_sm_mask_enabled(video_pio, SCANVIDEO_ALL_SM_MASK, true);
}

// The caller holds both cores off the scanline buffers.
void scanvideo_set_mode(const scanvideo_view_t *mode)
{
    const scanvideo_timing_t *const was = video_mode.default_timing; // NULL the first time
    const bool same_timing = mode->default_timing == was;

    if (!was)
        scanvideo_once();
    else if (same_timing)
        irq_set_enabled(PIO0_IRQ_0, false);
    else
        scanvideo_timing_stop();

    scanvideo_view(mode, same_timing);

    if (same_timing)
    {
        pio_clkdiv_restart_sm_mask(video_pio, SCANVIDEO_SCANLINE_SM_MASK);
        pio_set_sm_mask_enabled(video_pio, SCANVIDEO_SCANLINE_SM_MASK, true);
        irq_set_enabled(PIO0_IRQ_0, true);
    }
    else
    {
        scanvideo_timing_words(mode->default_timing);
        scanvideo_timing_start();
    }
}

#pragma GCC pop_options
