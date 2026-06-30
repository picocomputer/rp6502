/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host stand-in for <hardware/pwm.h>. On the RP2350 the audio drivers
 * (psg/opl/bel) emit each sample by writing its level to a PWM channel; the
 * emulator captures those writes instead. Only the handful of PWM calls that
 * survive on the host (the ones inside the sample handlers) are provided —
 * the firmware aud.c's slice/IRQ setup has no host stand-in because the
 * emulator links its own aud.c instead. The capture itself lives in snd.c.
 */

#ifndef _EMU_SHIM_HARDWARE_PWM_H_
#define _EMU_SHIM_HARDWARE_PWM_H_

#include <stdint.h>

/* The slice/channel numbers are only used by the host as opaque keys to tell
 * the left and right outputs apart, so the GPIO pin doubles as the slice. */
static inline unsigned pwm_gpio_to_slice_num(unsigned gpio) { return gpio; }
static inline unsigned pwm_gpio_to_channel(unsigned gpio) { return gpio & 1u; }

void pwm_clear_irq(unsigned slice);
void pwm_set_chan_level(unsigned slice, unsigned chan, uint16_t level);

#endif /* _EMU_SHIM_HARDWARE_PWM_H_ */
