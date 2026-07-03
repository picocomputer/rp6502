/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
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
