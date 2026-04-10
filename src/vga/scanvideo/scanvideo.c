/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
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
#include "timing.pio.h"
#include "scanvideo.h"
#include "sys/vga.h"
#include "pico/binary_info.h"

#ifndef PICO_SCANVIDEO_ENABLE_SCANLINE_ASSERTIONS
#define PICO_SCANVIDEO_ENABLE_SCANLINE_ASSERTIONS 0
#endif

#define PICO_SCANVIDEO_SCANLINE_SM0 0u
#define PICO_SCANVIDEO_SCANLINE_SM1 1u
#define PICO_SCANVIDEO_SCANLINE_SM2 2u
#define PICO_SCANVIDEO_TIMING_SM 3u
#define PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL0 0u
#define PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL1 1u
#define PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL2 2u
#define PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK 0x7u

static const uint scanline_sm[PICO_SCANVIDEO_PLANE_COUNT] = {
    PICO_SCANVIDEO_SCANLINE_SM0, PICO_SCANVIDEO_SCANLINE_SM1, PICO_SCANVIDEO_SCANLINE_SM2};
static const uint scanline_dma_ch[PICO_SCANVIDEO_PLANE_COUNT] = {
    PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL0, PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL1, PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL2};

// == DEBUGGING =========

CU_REGISTER_DEBUG_PINS(video_timing, video_dma_buffer, video_irq, video_dma_completion, video_generation,
                       video_recovery, video_in_use, video_link)

// ---- select at most one ---
// CU_SELECT_DEBUG_PINS(video_recovery)
// CU_SELECT_DEBUG_PINS(video_generation)
// CU_SELECT_DEBUG_PINS(video_in_use)
// CU_SELECT_DEBUG_PINS(scanvideo_timing)
// CU_SELECT_DEBUG_PINS(video_irq)
// CU_SELECT_DEBUG_PINS(video_dma_buffer)
// CU_SELECT_DEBUG_PINS(video_dma_completion)
// CU_SELECT_DEBUG_PINS(video_link)

// ======================

#define GENERATING_LIST 1
#if PICO_SCANVIDEO_ENABLE_SCANLINE_ASSERTIONS
#ifndef NDEBUG
#define scanline_assert(x) assert(x)
#else
#define scanline_assert(x) hard_assert(x)
#endif
#else
#define scanline_assert(x) (void)0
#endif

#define video_pio pio0

// --- video_24mhz_composable ---

#define video_24mhz_composable_program __CONCAT(video_24mhz_composable_prefix, _program)
#define video_24mhz_composable_wrap_target __CONCAT(video_24mhz_composable_prefix, _wrap_target)
#define video_24mhz_composable_wrap __CONCAT(video_24mhz_composable_prefix, _wrap)

bool video_24mhz_composable_adapt_for_mode(const scanvideo_pio_program_t *program, const scanvideo_mode_t *mode,
                                           scanvideo_scanline_buffer_t *missing_scanline_buffer,
                                           uint16_t *modifiable_instructions);

pio_sm_config video_24mhz_composable_configure_pio(pio_hw_t *pio, uint sm, uint offset);

const scanvideo_pio_program_t video_24mhz_composable = {
    .program = &video_24mhz_composable_program,
    .entry_point = video_24mhz_composable_program_extern(entry_point),
    .adapt_for_mode = video_24mhz_composable_adapt_for_mode,
    .configure_pio = video_24mhz_composable_configure_pio};

#define PIO_WAIT_IRQ4 pio_encode_wait_irq(1, false, 4)
static uint8_t video_htiming_load_offset;
static uint8_t video_program_load_offset;

// --- video timing stuff

// 4 possible instructions; index into program below
enum
{
    SET_IRQ_0 = 0u,
    SET_IRQ_1 = 1u,
    SET_IRQ_SCANLINE = 2u,
    CLEAR_IRQ_SCANLINE = 3u,
};

static struct
{
    int32_t v_active;
    int32_t v_total;
    int32_t v_pulse_start;
    int32_t v_pulse_end;
    uint32_t vsync_bits_pulse;
    uint32_t vsync_bits_no_pulse;

    uint32_t a, a_vblank, b1, b2, c, c_vblank;
    uint32_t vsync_bits;
    uint16_t dma_state_index;
    int32_t timing_scanline;
} timing_state;

#define DMA_STATE_COUNT 4
static uint32_t dma_states[DMA_STATE_COUNT];

static uint16_t video_clock_down_times_2;

// --- scanline stuff
// private representation of scanline buffer (adds link for one list this scanline buffer is currently in)
typedef struct full_scanline_buffer
{
    scanvideo_scanline_buffer_t core;
    struct full_scanline_buffer *next;
} full_scanline_buffer_t;

full_scanline_buffer_t scanline_buffers[PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT];

static uint32_t scanline_data[PICO_SCANVIDEO_PLANE_COUNT][PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT][PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS];

// This state is sensitive as it it accessed by either core, and multiple IRQ handlers which may be re-entrant
// Nothing in here should be touched except when protected by the appropriate spin lock.
static struct
{
    struct
    {
        spin_lock_t *lock;
        full_scanline_buffer_t *in_use_ascending_scanline_id_list;
        full_scanline_buffer_t *in_use_ascending_scanline_id_list_tail;
    } in_use;

    struct
    {
        spin_lock_t *lock;
        full_scanline_buffer_t *current_scanline_buffer;
        uint32_t last_scanline_id;
        uint32_t next_scanline_id;
        uint16_t y_repeat_index;
        uint16_t y_repeat_target;
        bool in_vblank;
        full_scanline_buffer_t *generated_ascending_scanline_id_list;
        full_scanline_buffer_t *generated_ascending_scanline_id_list_tail;
#if PICO_SCANVIDEO_ENABLE_SCANLINE_ASSERTIONS && GENERATING_LIST
        full_scanline_buffer_t *generating_list;
#endif
    } scanline;

    struct
    {
        spin_lock_t *lock;
        full_scanline_buffer_t *free_list;
    } free_list;

    struct
    {
        spin_lock_t *lock;
        uint32_t dma_completion_state;
        uint8_t buffers_to_release;
        bool scanline_in_progress;
    } dma;

    int scanline_program_wait_index;
} shared_state;

// PICO_CONFIG: PICO_SCANVIDEO_MISSING_SCANLINE_COLOR, Define colour used for missing scanlines, default=PICO_SVIDEO_PIXEL_FROM_RGB8(0,0,255), group=video
#ifndef PICO_SCANVIDEO_MISSING_SCANLINE_COLOR
#define PICO_SCANVIDEO_MISSING_SCANLINE_COLOR PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255)
#endif
static uint32_t _missing_scanline_data[] =
    {
        COMPOSABLE_COLOR_RUN | (PICO_SCANVIDEO_MISSING_SCANLINE_COLOR << 16u),
        /*width-3*/ 0u | (COMPOSABLE_RAW_1P << 16u),
        0u | (COMPOSABLE_EOL_ALIGN << 16u)};

uint32_t missing_scanline_data_overlay[] = {
    // blank line
    0u | (COMPOSABLE_EOL_ALIGN << 16u)};

static full_scanline_buffer_t _missing_scanline_buffer;

static uint32_t _blank_scanline_data[] = {
    COMPOSABLE_RAW_1P | (0 << 16),
    COMPOSABLE_EOL_SKIP_ALIGN,
};
static full_scanline_buffer_t _blank_scanline_buffer;

static inline bool is_scanline_after(uint32_t scanline_id1, uint32_t scanline_id2)
{
    return ((int32_t)(scanline_id1 - scanline_id2)) > 0;
}

// -- MISC stuff
static scanvideo_mode_t video_mode;
static bool video_timing_enabled = false;
static int32_t active_scanline_number;
static int32_t vblank_scanline_number;
static int32_t v_content_start;
static int32_t v_content_end;
static volatile bool generation_allowed;
static volatile uint32_t core_generating[2];
static uint16_t complete_frame;
static uint16_t complete_count;
static uint16_t complete_reported;

static uint __no_inline_not_in_flash_func(default_scanvideo_scanline_repeat_count_fn)(uint32_t scanline_id)
{
    (void)scanline_id;
    return 1;
}

typedef uint (*scanvideo_scanline_repeat_count_fn)(uint32_t scanline_id);
static scanvideo_scanline_repeat_count_fn _scanline_repeat_count_fn;

inline static void list_prepend(full_scanline_buffer_t **phead, full_scanline_buffer_t *fsb)
{
    scanline_assert(fsb);
    scanline_assert(fsb->next == NULL);
    scanline_assert(fsb != *phead);
    fsb->next = *phead;
    *phead = fsb;
}

inline static void list_prepend_all(full_scanline_buffer_t **phead, full_scanline_buffer_t *to_prepend)
{
    full_scanline_buffer_t *fsb = to_prepend;

    if (fsb)
    {
        while (fsb->next)
        {
            fsb = fsb->next;
        }

        fsb->next = *phead;
        *phead = to_prepend;
    }
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
        scanline_assert(*ptail);
        *phead = fsb->next;

        if (!fsb->next)
        {
            scanline_assert(*ptail == fsb);
            *ptail = NULL;
        }
        else
        {
            fsb->next = NULL;
        }
    }

    return fsb;
}

inline static void list_remove(full_scanline_buffer_t **phead, full_scanline_buffer_t *fsb)
{
    scanline_assert(*phead);
    full_scanline_buffer_t *prev = *phead;

    if (prev == fsb)
    {
        *phead = fsb->next;
    }
    else
    {
        while (prev->next && prev->next != fsb)
        {
            prev = prev->next;
        }

        scanline_assert(prev->next == fsb);
        prev->next = fsb->next;
    }

    fsb->next = NULL;
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
    scanline_assert(fsb->next == NULL);
    scanline_assert(fsb != *phead);
    scanline_assert(fsb != *ptail);

    if (!*phead || !is_scanline_after(fsb->core.scanline_id, (*phead)->core.scanline_id))
    {
        if (!*phead)
        {
            scanline_assert(!*ptail);
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

            scanline_assert(prev != *ptail); // we should have already inserted at the end in this case
            fsb->next = prev->next;
            prev->next = fsb;
        }
    }
}

inline static void free_local_free_list_irqs_enabled(full_scanline_buffer_t *local_free_list)
{
    if (local_free_list)
    {
        uint32_t save = spin_lock_blocking(shared_state.free_list.lock);
        DEBUG_PINS_SET(video_timing, 4);
        list_prepend_all(&shared_state.free_list.free_list, local_free_list);
        DEBUG_PINS_CLR(video_timing, 4);
        spin_unlock(shared_state.free_list.lock, save);
        __sev();
    }
}

// Caller must own scanline_state_spin_lock
inline static full_scanline_buffer_t *scanline_locked_try_latch_fsb_if_null_irqs_disabled(
    full_scanline_buffer_t **local_free_list)
{
    scanline_assert(is_spin_locked(shared_state.scanline.lock));
    full_scanline_buffer_t *fsb = shared_state.scanline.current_scanline_buffer;

    if (!fsb)
    {
        while (NULL != (fsb = shared_state.scanline.generated_ascending_scanline_id_list))
        {
            if (!is_scanline_after(shared_state.scanline.next_scanline_id, fsb->core.scanline_id))
            {
                if (shared_state.scanline.next_scanline_id == fsb->core.scanline_id)
                {
                    full_scanline_buffer_t __unused *dbg = list_remove_head_ascending(
                        &shared_state.scanline.generated_ascending_scanline_id_list,
                        &shared_state.scanline.generated_ascending_scanline_id_list_tail);
                    scanline_assert(dbg == fsb);
                    spin_lock_unsafe_blocking(shared_state.in_use.lock);
                    DEBUG_PINS_SET(video_timing, 2);
                    DEBUG_PINS_XOR(video_in_use, 1);
                    list_insert_ascending(&shared_state.in_use.in_use_ascending_scanline_id_list,
                                          &shared_state.in_use.in_use_ascending_scanline_id_list_tail, fsb);
                    DEBUG_PINS_CLR(video_timing, 2);
                    spin_unlock_unsafe(shared_state.in_use.lock);
                    shared_state.scanline.current_scanline_buffer = fsb;
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
                full_scanline_buffer_t __unused *dbg = list_remove_head_ascending(
                    &shared_state.scanline.generated_ascending_scanline_id_list,
                    &shared_state.scanline.generated_ascending_scanline_id_list_tail);
                scanline_assert(dbg == fsb);
                list_prepend(local_free_list, fsb);
            }
        }
    }

    return fsb;
}

static inline void release_scanline_irqs_enabled(int buffers_to_free_count,
                                                 full_scanline_buffer_t **local_free_list)
{
    if (buffers_to_free_count)
    {
        uint32_t save = spin_lock_blocking(shared_state.in_use.lock);
        while (buffers_to_free_count--)
        {
            DEBUG_PINS_SET(video_dma_buffer, 2);
            DEBUG_PINS_XOR(video_in_use, 2);
            full_scanline_buffer_t *fsb = list_remove_head_ascending(
                &shared_state.in_use.in_use_ascending_scanline_id_list,
                &shared_state.in_use.in_use_ascending_scanline_id_list_tail);
            list_prepend(local_free_list, fsb);
            DEBUG_PINS_CLR(video_dma_buffer, 2);
        }
        spin_unlock(shared_state.in_use.lock, save);
    }
}

static inline void __not_in_flash_func(abort_all_dma_channels)(void)
{
    dma_hw->abort = PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK;
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
        while (dma_channel_is_busy(scanline_dma_ch[i]))
            tight_loop_contents();
    dma_hw->ints0 = PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK;
}

static inline bool update_dma_transfer_state_irqs_enabled(bool cancel_if_not_complete,
                                                          int *scanline_buffers_to_release)
{
    uint32_t save = spin_lock_blocking(shared_state.dma.lock);
    if (!shared_state.dma.scanline_in_progress)
    {
        assert(!shared_state.dma.dma_completion_state);
        assert(!shared_state.dma.buffers_to_release);
        spin_unlock(shared_state.dma.lock, save);
        return true;
    }
    uint32_t old_completed = shared_state.dma.dma_completion_state;
    uint32_t new_completed;
    while (0 != (new_completed = dma_hw->ints0 & PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK))
    {
        scanline_assert(!(old_completed & new_completed));
        dma_hw->ints0 = new_completed;
        DEBUG_PINS_SET(video_dma_completion, new_completed);
        DEBUG_PINS_CLR(video_dma_completion, new_completed);
        new_completed |= old_completed;
        if (new_completed == PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK)
        {
            *scanline_buffers_to_release = shared_state.dma.buffers_to_release;
            shared_state.dma.buffers_to_release = 0;
            if (*scanline_buffers_to_release)
            {
                scanline_assert(*scanline_buffers_to_release == 1);
                DEBUG_PINS_SET(video_dma_completion, 1);
                DEBUG_PINS_CLR(video_dma_completion, 1);
            }
            shared_state.dma.dma_completion_state = shared_state.dma.scanline_in_progress = 0;
            spin_unlock(shared_state.dma.lock, save);
            return true;
        }
        else
        {
            DEBUG_PINS_SET(video_dma_completion, 2);
            DEBUG_PINS_CLR(video_dma_completion, 2);
            shared_state.dma.dma_completion_state = old_completed = new_completed;
        }
    }
    if (cancel_if_not_complete)
    {
        if (shared_state.dma.buffers_to_release)
        {
            shared_state.dma.dma_completion_state = shared_state.dma.scanline_in_progress = 0;
            *scanline_buffers_to_release = shared_state.dma.buffers_to_release;
            shared_state.dma.buffers_to_release = 0;
            DEBUG_PINS_XOR(video_in_use, 4);
        }
        abort_all_dma_channels();
    }
    spin_unlock(shared_state.dma.lock, save);
    return cancel_if_not_complete;
}

static inline void scanline_dma_complete_irqs_enabled()
{
    DEBUG_PINS_SET(video_dma_completion, 4);
    int buffers_to_free_count = 0;
    bool is_completion_trigger = update_dma_transfer_state_irqs_enabled(false, &buffers_to_free_count);
    full_scanline_buffer_t *local_free_list = NULL;
    if (is_completion_trigger)
    {
        uint32_t save = spin_lock_blocking(shared_state.scanline.lock);
        DEBUG_PINS_SET(video_timing, 1);
        scanline_locked_try_latch_fsb_if_null_irqs_disabled(&local_free_list);
        DEBUG_PINS_CLR(video_timing, 1);
        spin_unlock(shared_state.scanline.lock, save);
    }

    release_scanline_irqs_enabled(buffers_to_free_count, &local_free_list);
    free_local_free_list_irqs_enabled(local_free_list);
    DEBUG_PINS_CLR(video_dma_completion, 4);
}

static void set_next_scanline_id(uint32_t scanline_id)
{
    shared_state.scanline.next_scanline_id = scanline_id;
    shared_state.scanline.y_repeat_target = _scanline_repeat_count_fn(scanline_id) * video_mode.yscale;
}

static inline void __not_in_flash_func(recover_scanline_sms)(void)
{
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
    {
        uint sm = scanline_sm[i];
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
                if (video_pio->sm[sm].addr != shared_state.scanline_program_wait_index + 1u)
                {
                    pio_sm_exec(video_pio, sm, pio_encode_jmp(shared_state.scanline_program_wait_index));
                }
            }
            else
            {
                pio_sm_exec(video_pio, sm, pio_encode_jmp(shared_state.scanline_program_wait_index + 1));
            }
        }
    }
}

static inline void __not_in_flash_func(recover_pio_sms_and_dma_blank)(int *buffers_to_free_count)
{
    update_dma_transfer_state_irqs_enabled(true, buffers_to_free_count);

    recover_scanline_sms();

    dma_channel_transfer_from_buffer_now(PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL0, _blank_scanline_buffer.core.data0,
                                         (uint32_t)_blank_scanline_buffer.core.data0_used);
    dma_channel_transfer_from_buffer_now(PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL1, _blank_scanline_buffer.core.data1,
                                         (uint32_t)_blank_scanline_buffer.core.data1_used);
    dma_channel_transfer_from_buffer_now(PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL2, _blank_scanline_buffer.core.data2,
                                         (uint32_t)_blank_scanline_buffer.core.data2_used);
}

void __not_in_flash_func(prepare_for_active_scanline_irqs_enabled)()
{
    DEBUG_PINS_SET(video_timing, 1);

    // Offset scanlines: DMA blank data, skip scanline state advancement
    if (active_scanline_number < v_content_start || active_scanline_number >= v_content_end)
    {
        int buffers_to_free_count = 0;
        recover_pio_sms_and_dma_blank(&buffers_to_free_count);

        uint32_t save = spin_lock_blocking(shared_state.scanline.lock);
        shared_state.scanline.in_vblank = false;
        spin_lock_unsafe_blocking(shared_state.dma.lock);
        shared_state.dma.scanline_in_progress = 1;
        spin_unlock_unsafe(shared_state.dma.lock);
        spin_unlock(shared_state.scanline.lock, save);

        full_scanline_buffer_t *local_free_list = NULL;
        release_scanline_irqs_enabled(buffers_to_free_count, &local_free_list);
        free_local_free_list_irqs_enabled(local_free_list);
        DEBUG_PINS_CLR(video_timing, 1);
        return;
    }

    // Content scanlines: normal path
    full_scanline_buffer_t *local_free_list = NULL;
    int buffers_to_free_count = 0;
    uint32_t save = spin_lock_blocking(shared_state.scanline.lock);
    // VERY IMPORTANT: THIS CODE CAN ONLY TAKE ABOUT 4.5 us BEFORE LAUNCHING DMA
    full_scanline_buffer_t *fsb = scanline_locked_try_latch_fsb_if_null_irqs_disabled(&local_free_list);

    spin_unlock(shared_state.scanline.lock, save);
    DEBUG_PINS_CLR(video_timing, 1);
    if (!fsb || fsb->core.scanline_id != shared_state.scanline.next_scanline_id)
        fsb = &_missing_scanline_buffer;

    update_dma_transfer_state_irqs_enabled(true, &buffers_to_free_count);
    recover_scanline_sms();
    dma_channel_transfer_from_buffer_now(PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL0, fsb->core.data0,
                                         (uint32_t)fsb->core.data0_used);
    dma_channel_transfer_from_buffer_now(PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL1, fsb->core.data1, (uint32_t)fsb->core.data1_used);
    dma_channel_transfer_from_buffer_now(PICO_SCANVIDEO_SCANLINE_DMA_CHANNEL2, fsb->core.data2, (uint32_t)fsb->core.data2_used);

    save = spin_lock_blocking(shared_state.scanline.lock);
    DEBUG_PINS_SET(video_timing, 1);
    shared_state.scanline.in_vblank = false;
    bool was_correct_scanline = (fsb != &_missing_scanline_buffer);
    bool free_scanline = false;
    shared_state.scanline.y_repeat_index += video_mode.yscale_denominator;
    if (shared_state.scanline.y_repeat_index >= shared_state.scanline.y_repeat_target)
    {
        if (was_correct_scanline)
        {
            free_scanline = true;
        }

        shared_state.scanline.y_repeat_index -= shared_state.scanline.y_repeat_target;
        set_next_scanline_id(scanline_id_after(shared_state.scanline.next_scanline_id));
        shared_state.scanline.current_scanline_buffer = NULL;
    }
    else if (!was_correct_scanline)
    {
        shared_state.scanline.current_scanline_buffer = NULL;
    }
    // safe to nest dma lock we never nest the other way
    spin_lock_unsafe_blocking(shared_state.dma.lock);
    shared_state.dma.scanline_in_progress = 1;
    if (free_scanline)
    {
        scanline_assert(!shared_state.dma.buffers_to_release);
        shared_state.dma.buffers_to_release++;
    }
    spin_unlock_unsafe(shared_state.dma.lock);
    DEBUG_PINS_CLR(video_timing, 1);
    spin_unlock(shared_state.scanline.lock, save);

    release_scanline_irqs_enabled(buffers_to_free_count, &local_free_list);
    free_local_free_list_irqs_enabled(local_free_list);
}

static void __not_in_flash_func(prepare_for_vblank_scanline_irqs_enabled)()
{
    bool signal = false;

    int buffers_to_free_count = 0;
    update_dma_transfer_state_irqs_enabled(true, &buffers_to_free_count);

    uint32_t save = spin_lock_blocking(shared_state.scanline.lock);
    DEBUG_PINS_SET(video_timing, 1);
    full_scanline_buffer_t *local_free_list = NULL;

    if (!shared_state.scanline.in_vblank)
    {
        shared_state.scanline.in_vblank = true;
        shared_state.scanline.y_repeat_index = 0;

        if (scanvideo_scanline_number(shared_state.scanline.next_scanline_id) != 0)
        {
            shared_state.scanline.next_scanline_id =
                (scanvideo_frame_number(shared_state.scanline.next_scanline_id) + 1u) << 16u;
            shared_state.scanline.y_repeat_target = _scanline_repeat_count_fn(shared_state.scanline.next_scanline_id);
        }

        signal = true;
    }

    if (!shared_state.scanline.current_scanline_buffer || is_scanline_after(shared_state.scanline.next_scanline_id,
                                                                            shared_state.scanline.current_scanline_buffer->core.scanline_id))
    {
        if (shared_state.scanline.current_scanline_buffer)
        {
            buffers_to_free_count++;
            shared_state.scanline.current_scanline_buffer = NULL;
        }
        scanline_locked_try_latch_fsb_if_null_irqs_disabled(&local_free_list);
    }

    DEBUG_PINS_CLR(video_timing, 1);
    spin_unlock(shared_state.scanline.lock, save);

    release_scanline_irqs_enabled(buffers_to_free_count, &local_free_list);
    free_local_free_list_irqs_enabled(local_free_list);

    if (signal)
    {
        vga_scanline_complete(video_mode.height);
        __sev();
    }
}

#define setup_dma_states_vblank()              \
    if (true)                                  \
    {                                          \
        dma_states[0] = timing_state.a_vblank; \
        dma_states[1] = timing_state.b1;       \
        dma_states[2] = timing_state.b2;       \
        dma_states[3] = timing_state.c_vblank; \
    }                                          \
    else                                       \
        __builtin_unreachable()
#define setup_dma_states_no_vblank()     \
    if (true)                            \
    {                                    \
        dma_states[0] = timing_state.a;  \
        dma_states[1] = timing_state.b1; \
        dma_states[2] = timing_state.b2; \
        dma_states[3] = timing_state.c;  \
    }                                    \
    else                                 \
        __builtin_unreachable()

static inline void top_up_timing_pio_fifo()
{
    while (!(video_pio->fstat & (1u << (PICO_SCANVIDEO_TIMING_SM + PIO_FSTAT_TXFULL_LSB))))
    {
        DEBUG_PINS_XOR(video_irq, 1);
        DEBUG_PINS_XOR(video_irq, 1);
        pio_sm_put(video_pio, PICO_SCANVIDEO_TIMING_SM, dma_states[timing_state.dma_state_index] | timing_state.vsync_bits);

        if (++timing_state.dma_state_index >= DMA_STATE_COUNT)
        {
            timing_state.dma_state_index = 0;
            timing_state.timing_scanline++;

            if (timing_state.timing_scanline >= timing_state.v_active)
            {
                if (timing_state.timing_scanline >= timing_state.v_total)
                {
                    timing_state.timing_scanline = 0;
                    setup_dma_states_no_vblank();
                }
                else if (timing_state.timing_scanline <= timing_state.v_pulse_end)
                {
                    if (timing_state.timing_scanline == timing_state.v_active)
                    {
                        setup_dma_states_vblank();
                    }
                    else if (timing_state.timing_scanline == timing_state.v_pulse_start)
                    {
                        timing_state.vsync_bits = timing_state.vsync_bits_pulse;
                    }
                    else if (timing_state.timing_scanline == timing_state.v_pulse_end)
                    {
                        timing_state.vsync_bits = timing_state.vsync_bits_no_pulse;
                    }
                }
            }
        }
    }
}

void __isr __not_in_flash_func(isr_pio0_0)()
{
    if (video_pio->irq & 1u)
    {
        video_pio->irq = 1;
        DEBUG_PINS_SET(video_irq, 1);
        prepare_for_active_scanline_irqs_enabled();
        DEBUG_PINS_CLR(video_irq, 1);
        active_scanline_number++;
        vblank_scanline_number = 0;
    }
    if (video_pio->irq & 2u)
    {
        video_pio->irq = 3;
        DEBUG_PINS_SET(video_irq, 2);
        prepare_for_vblank_scanline_irqs_enabled();
        DEBUG_PINS_CLR(video_irq, 2);
        vblank_scanline_number++;
        active_scanline_number = 0;
    }
}

// irq for PIO FIFO
void __isr __not_in_flash_func(isr_pio0_1)()
{
    DEBUG_PINS_SET(video_irq, 4);
    top_up_timing_pio_fifo();
    DEBUG_PINS_CLR(video_irq, 4);
}

// DMA complete
void __isr __not_in_flash_func(isr_dma_0)()
{
    DEBUG_PINS_SET(video_irq, 4);
    scanline_dma_complete_irqs_enabled();
    DEBUG_PINS_CLR(video_irq, 4);
}

static inline bool is_scanline_sm(int sm)
{
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
        if ((uint)sm == scanline_sm[i])
            return true;
    return false;
}

void setup_sm(int sm, uint offset)
{
    pio_sm_config config = is_scanline_sm(sm) ? video_mode.pio_program->configure_pio(video_pio, sm, offset) : video_htiming_program_get_default_config(offset);

    sm_config_set_clkdiv_int_frac(&config, video_clock_down_times_2 / 2, (video_clock_down_times_2 & 1u) << 7u);

    if (!is_scanline_sm(sm))
    {
        sm_config_set_out_shift(&config, true, true, 32);
        const uint BASE = PICO_SCANVIDEO_SYNC_PIN_BASE;
        uint pin_count = 2;
        sm_config_set_out_pins(&config, BASE, pin_count);
        pio_sm_set_consecutive_pindirs(video_pio, sm, BASE, pin_count, true);
    }

    pio_sm_init(video_pio, sm, offset, &config); // now paused
}

extern scanvideo_scanline_buffer_t *__not_in_flash_func(scanvideo_begin_scanline_generation)(void)
{
    DEBUG_PINS_SET(video_link, 1);
    DEBUG_PINS_SET(video_generation, 1);

    if (!generation_allowed)
    {
        int32_t pos = vblank_scanline_number > 0
                          ? timing_state.v_active + vblank_scanline_number
                          : active_scanline_number;
        int32_t distance = v_content_start - pos;
        if (distance < 0)
            distance += timing_state.v_total;
        if (distance <= PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT)
            generation_allowed = true;
        else
        {
            DEBUG_PINS_CLR(video_link, 1);
            DEBUG_PINS_CLR(video_generation, 1);
            return NULL;
        }
    }

    uint32_t save = spin_lock_blocking(shared_state.free_list.lock);
    full_scanline_buffer_t *fsb = list_remove_head(&shared_state.free_list.free_list);
    spin_unlock(shared_state.free_list.lock, save);

    if (fsb)
    {
        save = spin_lock_blocking(shared_state.scanline.lock);
        DEBUG_PINS_SET(video_timing, 1);
#if PICO_SCANVIDEO_ENABLE_SCANLINE_ASSERTIONS && GENERATING_LIST
        list_prepend(&shared_state.scanline.generating_list, fsb);
#endif
        uint32_t scanline_id = shared_state.scanline.next_scanline_id;

        if (!is_scanline_after(scanline_id, shared_state.scanline.last_scanline_id))
        {
            scanline_id = scanline_id_after(shared_state.scanline.last_scanline_id);
        }

        fsb->core.scanline_id = shared_state.scanline.last_scanline_id = scanline_id;
        core_generating[get_core_num()] = scanline_id + 1;
        if (scanvideo_scanline_number(scanline_id) >= video_mode.height - 1)
            generation_allowed = false;
        DEBUG_PINS_CLR(video_timing, 1);
        spin_unlock(shared_state.scanline.lock, save);
    }

    DEBUG_PINS_CLR(video_link, 1);
    DEBUG_PINS_CLR(video_generation, 1);
    return (scanvideo_scanline_buffer_t *)fsb;
}

extern void __not_in_flash_func(scanvideo_end_scanline_generation)(
    scanvideo_scanline_buffer_t *scanline_buffer)
{
    DEBUG_PINS_SET(video_generation, 2);
    full_scanline_buffer_t *fsb = (full_scanline_buffer_t *)scanline_buffer;
    uint32_t save = spin_lock_blocking(shared_state.scanline.lock);
#if PICO_SCANVIDEO_ENABLE_SCANLINE_ASSERTIONS && GENERATING_LIST
    list_remove(&shared_state.scanline.generating_list, fsb);
#endif
    list_insert_ascending(&shared_state.scanline.generated_ascending_scanline_id_list,
                          &shared_state.scanline.generated_ascending_scanline_id_list_tail, fsb);
    core_generating[get_core_num()] = 0;
    {
        uint16_t frame = scanvideo_frame_number(scanline_buffer->scanline_id);
        if (frame != complete_frame)
        {
            complete_frame = frame;
            complete_count = 0;
            complete_reported = UINT16_MAX;
        }
        complete_count++;
        // Find lowest in-flight scanline for this frame
        uint16_t gap = UINT16_MAX;
        for (int i = 0; i < 2; i++)
        {
            if (core_generating[i] &&
                scanvideo_frame_number(core_generating[i] - 1) == frame)
            {
                uint16_t s = scanvideo_scanline_number(core_generating[i] - 1);
                if (s < gap)
                    gap = s;
            }
        }
        // Contiguous completion: up to just before the gap,
        // or up to total submitted if no gap
        uint16_t safe = (gap < UINT16_MAX)
                            ? (gap > 0 ? gap - 1 : UINT16_MAX)
                            : complete_count - 1;
        if (safe < UINT16_MAX && safe != complete_reported)
        {
            complete_reported = safe;
            vga_scanline_complete(safe);
        }
    }
    spin_unlock(shared_state.scanline.lock, save);
    DEBUG_PINS_CLR(video_generation, 2);
}

static void scanvideo_set_scanline_repeat_fn(scanvideo_scanline_repeat_count_fn fn)
{
    _scanline_repeat_count_fn = fn ? fn : default_scanvideo_scanline_repeat_count_fn;
}

static pio_program_t copy_program(const pio_program_t *program, uint16_t *instructions,
                                  uint32_t max_instructions)
{
    assert(max_instructions >= program->length);
    pio_program_t copy = *program;
    __builtin_memcpy(instructions, program->instructions, MIN(program->length, max_instructions) * sizeof(uint16_t));
    copy.instructions = instructions;
    return copy;
}

static void init_shared_state(void)
{
    memset(&shared_state, 0, sizeof(shared_state));
    shared_state.scanline.lock = spin_lock_init(PICO_SPINLOCK_ID_VIDEO_SCANLINE_LOCK);
    shared_state.dma.lock = spin_lock_init(PICO_SPINLOCK_ID_VIDEO_DMA_LOCK);
    shared_state.free_list.lock = spin_lock_init(PICO_SPINLOCK_ID_VIDEO_FREE_LIST_LOCK);
    shared_state.in_use.lock = spin_lock_init(PICO_SPINLOCK_ID_VIDEO_IN_USE_LOCK);
    // Must be next_scanline_id - 1 so is_scanline_after() succeeds on first generation
    shared_state.scanline.last_scanline_id = shared_state.scanline.next_scanline_id - 1;
    shared_state.scanline.y_repeat_target = video_mode.yscale;
}

static void init_scanline_buffers(void)
{
    memset(scanline_data, 0, sizeof(scanline_data));
    for (int i = 0; i < PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT; i++)
    {
        scanline_buffers[i].core.data0 = scanline_data[0][i];
        scanline_buffers[i].core.data0_max = PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS;
        scanline_buffers[i].core.data1 = scanline_data[1][i];
        scanline_buffers[i].core.data1_max = PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS;
        scanline_buffers[i].core.data2 = scanline_data[2][i];
        scanline_buffers[i].core.data2_max = PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS;
        scanline_buffers[i].next = i != PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT - 1 ? &scanline_buffers[i + 1] : NULL;
    }
    shared_state.free_list.free_list = &scanline_buffers[0];
    __mem_fence_release();
}

static void init_blank_scanline_buffer(void)
{
    _blank_scanline_buffer.core.data0 = _blank_scanline_data;
    _blank_scanline_buffer.core.data0_used = _blank_scanline_buffer.core.data0_max = count_of(_blank_scanline_data);
    _blank_scanline_buffer.core.data1 = missing_scanline_data_overlay;
    _blank_scanline_buffer.core.data1_used = _blank_scanline_buffer.core.data1_max = count_of(missing_scanline_data_overlay);
    _blank_scanline_buffer.core.data2 = missing_scanline_data_overlay;
    _blank_scanline_buffer.core.data2_used = _blank_scanline_buffer.core.data2_max = count_of(missing_scanline_data_overlay);
    _blank_scanline_buffer.core.status = SCANLINE_OK;
}

static int find_program_wait_index(const uint16_t *instructions, int length)
{
    for (int i = 0; i < length; i++)
        if (instructions[i] == PIO_WAIT_IRQ4)
            return i;
    assert(false);
    return -1;
}

static bool scanvideo_setup(const scanvideo_mode_t *mode)
{
    const scanvideo_timing_t *timing = mode->default_timing;

    video_mode = *mode;
    video_mode.default_timing = timing;
    static_assert(BPP == 16, "");
    if (!video_mode.yscale_denominator)
        video_mode.yscale_denominator = 1;

    init_shared_state();

    active_scanline_number = 0;
    vblank_scanline_number = 0;

    v_content_start = mode->v_offset;
    v_content_end = mode->v_offset +
                    ((uint32_t)mode->height * mode->yscale + video_mode.yscale_denominator - 1) /
                        video_mode.yscale_denominator;

    init_blank_scanline_buffer();
    ((uint16_t *)(_missing_scanline_data))[2] = mode->width / 2 - 3;
    init_scanline_buffers();

    uint pin_mask = 3u << PICO_SCANVIDEO_SYNC_PIN_BASE;
    bi_decl_if_func_used(bi_2pins_with_names(PICO_SCANVIDEO_SYNC_PIN_BASE, "HSync",
                                             PICO_SCANVIDEO_SYNC_PIN_BASE + 1, "VSync"));

    static_assert(PICO_SCANVIDEO_PIXEL_RSHIFT + PICO_SCANVIDEO_PIXEL_RCOUNT <= PICO_SCANVIDEO_COLOR_PIN_COUNT, "red bits do not fit in color pins");
    static_assert(PICO_SCANVIDEO_PIXEL_GSHIFT + PICO_SCANVIDEO_PIXEL_GCOUNT <= PICO_SCANVIDEO_COLOR_PIN_COUNT, "green bits do not fit in color pins");
    static_assert(PICO_SCANVIDEO_PIXEL_BSHIFT + PICO_SCANVIDEO_PIXEL_BCOUNT <= PICO_SCANVIDEO_COLOR_PIN_COUNT, "blue bits do not fit in color pins");
#define RMASK ((1u << PICO_SCANVIDEO_PIXEL_RCOUNT) - 1u)
#define GMASK ((1u << PICO_SCANVIDEO_PIXEL_GCOUNT) - 1u)
#define BMASK ((1u << PICO_SCANVIDEO_PIXEL_BCOUNT) - 1u)
    pin_mask |= RMASK << (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_PIXEL_RSHIFT);
    pin_mask |= GMASK << (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_PIXEL_GSHIFT);
    pin_mask |= BMASK << (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_PIXEL_BSHIFT);
    bi_decl_if_func_used(bi_pin_mask_with_name(RMASK << (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_PIXEL_RSHIFT), RMASK == 1 ? "Red" : ("Red 0-" __XSTRING(PICO_SCANVIDEO_PIXEL_GCOUNT))));
    bi_decl_if_func_used(bi_pin_mask_with_name(GMASK << (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_PIXEL_GSHIFT), GMASK == 1 ? "Green" : ("Green 0-" __XSTRING(PICO_SCANVIDEO_PIXEL_GCOUNT))));
    bi_decl_if_func_used(bi_pin_mask_with_name(BMASK << (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_PIXEL_BSHIFT), BMASK == 1 ? "Blue" : ("Blue 0-" __XSTRING(PICO_SCANVIDEO_PIXEL_BCOUNT))));

    for (uint8_t i = 0; pin_mask; i++, pin_mask >>= 1u)
    {
        if (pin_mask & 1)
            gpio_set_function(i, GPIO_FUNC_PIO0);
    }

    uint sys_clk = clock_get_hz(clk_sys);
    video_clock_down_times_2 = sys_clk / timing->clock_freq;
    if (video_clock_down_times_2 * timing->clock_freq != sys_clk)
    {
        panic("System clock (%d) must be an integer multiple of the requested pixel clock (%d).", sys_clk, timing->clock_freq);
    }

    assert(mode->width * mode->xscale <= timing->h_active);
    assert(v_content_end <= (int32_t)timing->v_active);

    uint16_t instructions[32];
    pio_program_t modified_program = copy_program(mode->pio_program->program, instructions,
                                                  count_of(instructions));

    if (!mode->pio_program->adapt_for_mode(mode->pio_program, mode, &_missing_scanline_buffer.core, instructions))
    {
        assert(false);
    }
    assert(_missing_scanline_buffer.core.data0 && _missing_scanline_buffer.core.data0_used);
    video_program_load_offset = pio_add_program(video_pio, &modified_program);

    shared_state.scanline_program_wait_index =
        find_program_wait_index(instructions, mode->pio_program->program->length);

    assert(_missing_scanline_buffer.core.data1 && _missing_scanline_buffer.core.data1_used);
    assert(_missing_scanline_buffer.core.data2 && _missing_scanline_buffer.core.data2_used);
    _missing_scanline_buffer.core.status = SCANLINE_OK;

    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
        setup_sm(scanline_sm[i], video_program_load_offset);

    uint32_t side_set_xor = 0;
    modified_program = copy_program(&video_htiming_program, instructions, count_of(instructions));

    if (timing->clock_polarity)
    {
        side_set_xor = 0x1000;

        for (uint i = 0; i < video_htiming_program.length; i++)
        {
            instructions[i] ^= side_set_xor;
        }
    }

    video_htiming_load_offset = pio_add_program(video_pio, &modified_program);

    setup_sm(PICO_SCANVIDEO_TIMING_SM, video_htiming_load_offset);

#if PICO_DEFAULT_IRQ_PRIORITY < 0x40
#warning pico_scanvideo_dpi may not always function correctly without PIO_IRQ_0 at a higher priority than other interrupts.
    irq_set_priority(PIO0_IRQ_1, 0x40);
    irq_set_priority(DMA_IRQ_0, 0x80);
#else
    irq_set_priority(PIO0_IRQ_0, 0);
    irq_set_priority(PIO0_IRQ_1, 0x40);
    irq_set_priority(DMA_IRQ_0, 0x40);
#endif

    dma_claim_mask(PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK);
    dma_set_irq0_channel_mask_enabled(PICO_SCANVIDEO_SCANLINE_DMA_CHANNELS_MASK, true);

    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
    {
        dma_channel_config channel_config = dma_channel_get_default_config(scanline_dma_ch[i]);
        channel_config_set_dreq(&channel_config, DREQ_PIO0_TX0 + scanline_sm[i]);
        dma_channel_configure(scanline_dma_ch[i],
                              &channel_config,
                              &video_pio->txf[scanline_sm[i]],
                              NULL,
                              0,
                              false);
    }

    // clear scanline irq
    pio_sm_exec(video_pio, PICO_SCANVIDEO_TIMING_SM, video_htiming_states_program.instructions[CLEAR_IRQ_SCANLINE]);

    timing_state.v_total = timing->v_total;
    timing_state.v_active = timing->v_active;
    timing_state.v_pulse_start = timing->v_active + timing->v_front_porch;
    timing_state.v_pulse_end = timing_state.v_pulse_start + timing->v_pulse;
    const uint32_t vsync_bit = 0x40000000;
    timing_state.vsync_bits_pulse = timing->v_sync_polarity ? 0 : vsync_bit;
    timing_state.vsync_bits_no_pulse = timing->v_sync_polarity ? vsync_bit : 0;

#define HTIMING_MIN 8

#define TIMING_CYCLE 3u
#define timing_encode(state, length, pins) ((video_htiming_states_program.instructions[state] ^ side_set_xor) | (((uint32_t)(length) - TIMING_CYCLE) << 16u) | ((uint32_t)(pins) << 29u))
#define A_CMD SET_IRQ_0
#define A_CMD_VBLANK SET_IRQ_1
#define B1_CMD CLEAR_IRQ_SCANLINE
#define B2_CMD CLEAR_IRQ_SCANLINE
#define C_CMD SET_IRQ_SCANLINE
#define C_CMD_VBLANK CLEAR_IRQ_SCANLINE

    int h_sync_bit = timing->h_sync_polarity ? 0 : 1;
    timing_state.a = timing_encode(A_CMD, 4, h_sync_bit);
    static_assert(HTIMING_MIN >= 4, "");
    timing_state.a_vblank = timing_encode(A_CMD_VBLANK, 4, h_sync_bit);
    int h_back_porch = timing->h_total - timing->h_front_porch - timing->h_pulse - timing->h_active;

    assert(timing->h_pulse - 4 >= HTIMING_MIN);
    timing_state.b1 = timing_encode(B1_CMD, timing->h_pulse - 4, h_sync_bit);

    assert(timing->h_active >= HTIMING_MIN);
    assert(h_back_porch >= HTIMING_MIN);
    assert((timing->h_total - h_back_porch - timing->h_pulse) >= HTIMING_MIN);
    timing_state.b2 = timing_encode(B2_CMD, h_back_porch, !h_sync_bit);
    timing_state.c = timing_encode(C_CMD, timing->h_total - h_back_porch - timing->h_pulse, 4 | !h_sync_bit);
    timing_state.c_vblank = timing_encode(C_CMD_VBLANK, timing->h_total - h_back_porch - timing->h_pulse, !h_sync_bit);

    setup_dma_states_vblank();
    timing_state.vsync_bits = timing_state.vsync_bits_no_pulse;
    scanvideo_set_scanline_repeat_fn(NULL);
    return true;
}

bool video_24mhz_composable_adapt_for_mode(const scanvideo_pio_program_t *program, const scanvideo_mode_t *mode,
                                           scanvideo_scanline_buffer_t *missing_scanline_buffer,
                                           uint16_t *modifiable_instructions)
{
    (void)program;
    (void)mode;
    int delay0 = 2 * mode->xscale - 2;
    int delay1 = delay0 + 1;
    assert(delay0 <= 31);
    assert(delay1 <= 31);

    modifiable_instructions[video_24mhz_composable_program_extern(delay_a_1)] |= (unsigned)delay1 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_b_1)] |= (unsigned)delay1 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_c_0)] |= (unsigned)delay0 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_d_0)] |= (unsigned)delay0 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_e_0)] |= (unsigned)delay0 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_f_1)] |= (unsigned)delay1 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_g_0)] |= (unsigned)delay0 << 8u;
    modifiable_instructions[video_24mhz_composable_program_extern(delay_h_0)] |= (unsigned)delay0 << 8u;

    missing_scanline_buffer->data0 = _missing_scanline_data;
    missing_scanline_buffer->data0_used = missing_scanline_buffer->data0_max = sizeof(_missing_scanline_data) / 4;
    missing_scanline_buffer->data1 = missing_scanline_data_overlay;
    missing_scanline_buffer->data1_used = missing_scanline_buffer->data1_max = sizeof(missing_scanline_data_overlay) / 4;
    missing_scanline_buffer->data2 = missing_scanline_data_overlay;
    missing_scanline_buffer->data2_used = missing_scanline_buffer->data2_max = sizeof(missing_scanline_data_overlay) / 4;
    return true;
}

static void scanvideo_default_configure_pio(pio_hw_t *pio, uint sm, uint offset, pio_sm_config *config, bool overlay)
{
    (void)offset;
    (void)config;
    (void)overlay;
    pio_sm_set_consecutive_pindirs(pio, sm, PICO_SCANVIDEO_COLOR_PIN_BASE, PICO_SCANVIDEO_COLOR_PIN_COUNT, true);
    sm_config_set_out_pins(config, PICO_SCANVIDEO_COLOR_PIN_BASE, PICO_SCANVIDEO_COLOR_PIN_COUNT);
    sm_config_set_out_shift(config, true, true, 32); // autopull
    sm_config_set_fifo_join(config, PIO_FIFO_JOIN_TX);
    if (overlay)
    {
        sm_config_set_out_special(config, 1, 1, PICO_SCANVIDEO_ALPHA_PIN);
    }
    else
    {
        sm_config_set_out_special(config, 1, 0, 0);
    }
}

pio_sm_config video_24mhz_composable_configure_pio(pio_hw_t *pio, uint sm, uint offset)
{
    pio_sm_config config = video_24mhz_composable_default_program_get_default_config(offset);
    scanvideo_default_configure_pio(pio, sm, offset, &config, sm != PICO_SCANVIDEO_SCANLINE_SM0);
    return config;
}

static void scanvideo_timing_enable(bool enable)
{
    if (enable != video_timing_enabled)
    {
        pio_set_irq0_source_mask_enabled(video_pio, (1u << pis_interrupt0) | (1u << pis_interrupt1), true);
        pio_set_irq1_source_enabled(video_pio, pis_sm0_tx_fifo_not_full + PICO_SCANVIDEO_TIMING_SM, true);
        irq_set_mask_enabled((1u << PIO0_IRQ_0) | (1u << PIO0_IRQ_1) | (1u << DMA_IRQ_0),
                             enable);
        uint32_t sm_mask = 1u << PICO_SCANVIDEO_TIMING_SM;
        for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
            sm_mask |= 1u << scanline_sm[i];
        pio_claim_sm_mask(video_pio, sm_mask);
        pio_set_sm_mask_enabled(video_pio, sm_mask, false);
        pio_clkdiv_restart_sm_mask(video_pio, sm_mask);

        if (enable)
        {
            uint jmp = video_program_load_offset + pio_encode_jmp(video_mode.pio_program->entry_point);
            for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
                pio_sm_exec(video_pio, scanline_sm[i], jmp);
            pio_sm_exec(video_pio, PICO_SCANVIDEO_TIMING_SM,
                        pio_encode_jmp(video_htiming_load_offset + video_htiming_offset_entry_point));
            pio_set_sm_mask_enabled(video_pio, sm_mask, true);
        }
        video_timing_enabled = enable;
    }
}

static void scanvideo_teardown(void)
{
    // Abort and unclaim DMA channels
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
    {
        dma_channel_abort(scanline_dma_ch[i]);
        if (dma_channel_is_claimed(scanline_dma_ch[i]))
            dma_channel_unclaim(scanline_dma_ch[i]);
    }

    // Clear PIO instruction memory
    pio_clear_instruction_memory(video_pio);

    // scanvideo_timing_enable calls pio_claim_sm_mask internally,
    // so we must unclaim first, then again after to fully release.
    for (int sm = 0; sm < 4; sm++)
        if (pio_sm_is_claimed(video_pio, sm))
            pio_sm_unclaim(video_pio, sm);
    scanvideo_timing_enable(false);
    for (int sm = 0; sm < 4; sm++)
        if (pio_sm_is_claimed(video_pio, sm))
            pio_sm_unclaim(video_pio, sm);
}

void scanvideo_set_mode(const scanvideo_mode_t *mode)
{
    bool first_call = !video_timing_enabled;
    bool timing_changed = first_call || mode->default_timing != video_mode.default_timing;

    if (timing_changed)
    {
        // Full teardown + setup + enable for timing changes and first call.
        if (!first_call)
            scanvideo_teardown();
        scanvideo_setup(mode);
        scanvideo_timing_enable(true);
        return;
    }

    // Same timing: keep timing SM (SM3) running so the monitor stays in sync.
    // Disable scanline and DMA IRQs to freeze scanline processing.
    // PIO0_IRQ_1 (timing FIFO top-up) stays enabled.
    irq_set_mask_enabled((1u << PIO0_IRQ_0) | (1u << DMA_IRQ_0), false);

    // Abort scanline DMA and stop scanline SMs
    uint32_t sm_mask = 0;
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
    {
        dma_channel_abort(scanline_dma_ch[i]);
        sm_mask |= 1u << scanline_sm[i];
    }
    pio_set_sm_mask_enabled(video_pio, sm_mask, false);

    // Update mode state
    const scanvideo_timing_t *timing = mode->default_timing;
    video_mode = *mode;
    video_mode.default_timing = timing;
    if (!video_mode.yscale_denominator)
        video_mode.yscale_denominator = 1;

    v_content_start = mode->v_offset;
    v_content_end = mode->v_offset +
                    ((uint32_t)mode->height * mode->yscale + video_mode.yscale_denominator - 1) /
                        video_mode.yscale_denominator;
    assert(v_content_end <= (int32_t)timing->v_active);

    ((uint16_t *)(_missing_scanline_data))[2] = mode->width / 2 - 3;

    // Re-adapt PIO program for new xscale and reload in-place
    uint16_t instructions[32];
    copy_program(mode->pio_program->program, instructions, count_of(instructions));
    mode->pio_program->adapt_for_mode(mode->pio_program, mode, &_missing_scanline_buffer.core, instructions);
    _missing_scanline_buffer.core.status = SCANLINE_OK;
    for (uint i = 0; i < mode->pio_program->program->length; i++)
        video_pio->instr_mem[video_program_load_offset + i] = instructions[i];

    // Reset shared state, advancing to the next frame
    uint32_t next_frame = (scanvideo_frame_number(shared_state.scanline.next_scanline_id) + 1u) << 16u;
    init_shared_state();
    shared_state.scanline.next_scanline_id = next_frame;
    shared_state.scanline.last_scanline_id = next_frame - 1;
    shared_state.scanline_program_wait_index =
        find_program_wait_index(instructions, mode->pio_program->program->length);

    init_scanline_buffers();
    init_blank_scanline_buffer();

    // Reset file-scope state not covered by shared_state memset
    core_generating[0] = 0;
    core_generating[1] = 0;
    complete_frame = 0;
    complete_count = 0;
    complete_reported = UINT16_MAX;
    scanvideo_set_scanline_repeat_fn(NULL);

    // Force remaining scanlines to blank until vblank resets to 0
    active_scanline_number = v_content_end;
    vblank_scanline_number = 0;
    generation_allowed = true;

    // Re-init and restart scanline SMs
    uint jmp = video_program_load_offset + pio_encode_jmp(video_mode.pio_program->entry_point);
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
        setup_sm(scanline_sm[i], video_program_load_offset);
    pio_clkdiv_restart_sm_mask(video_pio, sm_mask);
    for (int i = 0; i < PICO_SCANVIDEO_PLANE_COUNT; i++)
        pio_sm_exec(video_pio, scanline_sm[i], jmp);
    pio_set_sm_mask_enabled(video_pio, sm_mask, true);

    // Re-enable IRQs
    irq_set_mask_enabled((1u << PIO0_IRQ_0) | (1u << DMA_IRQ_0), true);
}

#pragma GCC pop_options
