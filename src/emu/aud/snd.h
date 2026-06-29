/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Audio generation (snd.c): the RIA audio devices (PSG/OPL + the always-mixed
 * bell) generate one stereo sample per IRQ on hardware; the emulator pumps the
 * active driver's handler instead, into a native-rate ring the window app drains.
 */

#ifndef _EMU_SND_H_
#define _EMU_SND_H_

#ifdef __cplusplus
extern "C"
{
#endif

void snd_task(void);  /* generate this frame's samples from the active device */
void snd_reset(void); /* silence + clear the ring (machine reset / exec) */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SND_H_ */
