/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_AUDIO_OUT_H_
#define _EMU_APP_AUDIO_OUT_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* Drain this frame's native-rate samples, resample to the host device rate, and
 * push them to sokol-audio. A no-op when built without audio. */
void audio_out_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_APP_AUDIO_OUT_H_ */
