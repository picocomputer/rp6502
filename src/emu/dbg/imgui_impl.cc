/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Dear ImGui rendering + input backend, emitted via sokol_imgui in C++ mode
 * (so it binds the C++ imgui.h directly — no cimgui). This translation unit only
 * carries the SOKOL_IMGUI_IMPL implementation (set by CMake); the Dear ImGui core
 * (imgui*.cpp) and the debugger windows (dbgui.cc) are their own units. The
 * sokol-gfx implementation itself lives in sokol_impl.c.
 */

#include "imgui.h"

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "util/sokol_imgui.h" /* SOKOL_IMGUI_IMPL provided by CMake */
