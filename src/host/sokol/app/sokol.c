/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/util/sokol_framebuffer.h"
#include "sokol/util/sokol_letterbox.h"
/* sokol_audio defaults to PulseAudio on Linux. The emulator stays on ALSA, which
 * its build and its runtime already need. */
#define SAUDIO_USE_ALSA (1)
#include "sokol/sokol_audio.h"
#include "sokol/util/sokol_debugtext.h"
#include "sokol/util/sokol_gl.h"
