/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The single translation unit that emits the sokol implementations.
 * Backend is selected by the build (SOKOL_GLCORE on desktop Linux).
 */

#include "sokol_log.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "util/sokol_gl.h"
#ifdef EMU_WITH_AUDIO
#include "sokol_audio.h"
#endif
