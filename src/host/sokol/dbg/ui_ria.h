/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RIA's debug window. The chip windows beside it are forks of upstream
 * chips-ui widgets and the RIA has no upstream, so this one is written here: it
 * shows the pins as ria_tick last decoded them, and a read-only view of the
 * register file, the XSTACK pointer, the code page and PHI2.
 *
 * This header carries its implementation under CHIPS_UI_IMPL, which one
 * translation unit defines (dbgui.cc), following the chips-ui convention. That
 * unit must have included ImGui already, because this file does not.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "chips/ui/ui_chip.h"
#include "chips/ui/ui_settings.h"
#include "core/ria/ria.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    const char *title;
    int x, y, w, h; /* initial geometry; a 0 width or height takes the default */
    bool open;
} ui_ria_desc_t;

typedef struct
{
    const char *title;
    float init_x, init_y, init_w, init_h;
    bool open;
    bool valid;
    ui_chip_t chip;
} ui_ria_t;

void ui_ria_init(ui_ria_t *win, const ui_ria_desc_t *desc);
void ui_ria_discard(ui_ria_t *win);
void ui_ria_draw(ui_ria_t *win);
void ui_ria_save_settings(ui_ria_t *win, ui_settings_t *settings);
void ui_ria_load_settings(ui_ria_t *win, const ui_settings_t *settings);

#ifdef __cplusplus
}
#endif

#ifdef CHIPS_UI_IMPL
#include <cstdio>
#include <string.h>
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif
#include "core/ria/regs.h"
#include "core/wdc/sram.h"
#include "core/wdc/phi2.h"
#include "core/wdc/resb.h"
#include "core/str/oem.h"

/* Five address lines select a register within the RIA's 32-byte window. A5
 * through A15 are decoded off-chip into CS, so they never reach the RIA. */
static const ui_chip_pin_t _ui_ria_pins[] = {
    {"D0", 0, RIA_PIN_D0 << 0}, {"D1", 1, RIA_PIN_D0 << 1},
    {"D2", 2, RIA_PIN_D0 << 2}, {"D3", 3, RIA_PIN_D0 << 3},
    {"D4", 4, RIA_PIN_D0 << 4}, {"D5", 5, RIA_PIN_D0 << 5},
    {"D6", 6, RIA_PIN_D0 << 6}, {"D7", 7, RIA_PIN_D0 << 7},
    {"RW", 9, RIA_PIN_RW}, {"IRQ", 10, RIA_PIN_IRQ},
    {"RES", 11, RIA_PIN_RES}, {"CS", 12, RIA_PIN_CS},
    {"A0", 13, RIA_PIN_A0 << 0}, {"A1", 14, RIA_PIN_A0 << 1},
    {"A2", 15, RIA_PIN_A0 << 2}, {"A3", 16, RIA_PIN_A0 << 3},
    {"A4", 17, RIA_PIN_A0 << 4},
};

/* The documented RIA register window, $FFE0 to $FFFF (ria.rst). A width of 2
 * is a little-endian 16-bit pair. */
static const struct { uint16_t addr; const char *name; uint8_t width; } _ui_ria_regs[] = {
    {0xFFE0, "READY", 1}, {0xFFE1, "TX", 1}, {0xFFE2, "RX", 1}, {0xFFE3, "VSYNC", 1},
    {0xFFE4, "RW0", 1}, {0xFFE5, "STEP0", 1}, {0xFFE6, "ADDR0", 2},
    {0xFFE8, "RW1", 1}, {0xFFE9, "STEP1", 1}, {0xFFEA, "ADDR1", 2},
    {0xFFEC, "XSTACK", 1}, {0xFFED, "ERRNO", 2}, {0xFFEF, "OP", 1},
    {0xFFF0, "IRQ", 1}, {0xFFF1, "SPIN", 1}, {0xFFF2, "BUSY", 1}, {0xFFF3, "LDA", 1},
    {0xFFF4, "A", 1}, {0xFFF5, "LDX", 1}, {0xFFF6, "X", 1}, {0xFFF7, "RTS", 1},
    {0xFFF8, "SREG", 2}, {0xFFFA, "NMIB", 2}, {0xFFFC, "RESB", 2}, {0xFFFE, "IRQB", 2},
};

void ui_ria_init(ui_ria_t *win, const ui_ria_desc_t *desc)
{
    CHIPS_ASSERT(win && desc && desc->title);
    memset(win, 0, sizeof(*win));
    win->title = desc->title;
    win->init_x = (float)desc->x;
    win->init_y = (float)desc->y;
    win->init_w = (float)((desc->w == 0) ? 440 : desc->w);
    win->init_h = (float)((desc->h == 0) ? 480 : desc->h);
    win->open = desc->open;
    win->valid = true;
    ui_chip_desc_t cd;
    UI_CHIP_INIT_DESC(&cd, "RIA", 26, _ui_ria_pins);
    ui_chip_init(&win->chip, &cd);
}

void ui_ria_discard(ui_ria_t *win)
{
    CHIPS_ASSERT(win && win->valid);
    win->valid = false;
}

void ui_ria_draw(ui_ria_t *win)
{
    CHIPS_ASSERT(win && win->valid && win->title);
    if (!win->open)
        return;
    ImGui::SetNextWindowPos(ImVec2(win->init_x, win->init_y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(win->init_w, win->init_h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(win->title, &win->open))
    {
        /* ria_tick writes every pin here except RES, which the RIA has no
         * input for, so RES is overlaid while the machine holds the 6502 in
         * reset. That is resb_running(), the hold between a stop and the next
         * run, and not the debugger's mid-run pause. */
        uint64_t p = ((const ria_t *)ria_chip())->PINS;
        if (!resb_running())
            p |= RIA_PIN_RES;
        ImGui::BeginChild("##ria_pins", ImVec2(176, 0), ImGuiChildFlags_Borders);
        ui_chip_draw(&win->chip, p);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##ria_state", ImVec2(0, 0), ImGuiChildFlags_Borders);

        /* Three values the memory-mapped register file does not carry. The
         * xstack grows down from XSTACK_SIZE, so it is empty when the pointer
         * is $200 and the live bytes are the ones from the pointer to $1FF.
         * The code page and PHI2 are settings with no bus register. */
        if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("XSTACK SP:    $%03X", (unsigned)xstack_ptr);
            ImGui::Text("CODE PAGE:    %u", (unsigned)oem_get_code_page_run());
            ImGui::Text("PHI2:         %u kHz", (unsigned)phi2_get_khz_run());
        }

        if (ImGui::CollapsingHeader("Registers", ImGuiTreeNodeFlags_DefaultOpen))
        {
            /* The register window runs to $FFFF, so the vectors are read out
             * of regs[] as well, which is where the CPU fetches them from. */
            auto peek = [](uint16_t a) -> uint8_t {
                return (a >= RIA_MMAP_LO) ? regs[a & 0x1F] : sram[a];
            };
            for (auto &r : _ui_ria_regs)
            {
                if (r.width == 2)
                {
                    ImGui::Text("%-6s ($%04X/%5d): $%04X", r.name, r.addr, r.addr,
                                (unsigned)(peek(r.addr) | (peek((uint16_t)(r.addr + 1)) << 8)));
                    continue;
                }
                uint8_t v = peek(r.addr);
                ImGui::Text("%-6s ($%04X/%5d): $%02X", r.name, r.addr, r.addr, v);
                char buf[24] = "";
                if (r.addr == 0xFFE0)
                    std::snprintf(buf, sizeof buf, "%s%s", (v & 0x80) ? "TX " : "", (v & 0x40) ? "RX" : "");
                else if (r.addr == 0xFFF0)
                    std::snprintf(buf, sizeof buf, "%s%s", (v & 0x80) ? "VSYNC " : "", (v & 0x40) ? "SIGINT" : "");
                else if (r.addr == 0xFFF2 && (v & 0x80))
                    std::snprintf(buf, sizeof buf, "BUSY");
                if (buf[0])
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", buf);
                }
            }
        }

        ImGui::EndChild();
    }
    ImGui::End();
}

void ui_ria_save_settings(ui_ria_t *win, ui_settings_t *settings)
{
    CHIPS_ASSERT(win && settings);
    ui_settings_add(settings, win->title, win->open);
}

void ui_ria_load_settings(ui_ria_t *win, const ui_settings_t *settings)
{
    CHIPS_ASSERT(win && settings);
    win->open = ui_settings_isopen(settings, win->title);
}
#endif /* CHIPS_UI_IMPL */
