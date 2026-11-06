/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_AUD_PCM_SHIM_H_
#define _TESTS_AUD_PCM_SHIM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* One pcm_t of core/aud/pcm.c at AUD_NATIVE_RATE. */
    uint64_t shim_pcm_step(uint32_t rate);
    void shim_pcm_reset(uint32_t rate, uint8_t bits, uint8_t channels);
    uint32_t shim_pcm_put(const uint8_t *src, uint32_t n);
    void shim_pcm_clear(void);
    uint16_t shim_pcm_level(void);
    uint32_t shim_pcm_underruns(void);
    void shim_pcm_set_underruns(uint32_t n);
    void shim_pcm_sample(int32_t *l, int32_t *r);
    /* The last sample's pcm_blend before its rounding. */
    void shim_pcm_v(int64_t *vl, int64_t *vr);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_AUD_PCM_SHIM_H_ */
