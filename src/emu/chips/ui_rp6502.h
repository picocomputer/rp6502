/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RIA debug window for the RP6502 — a chips-ui-style inspector beside ui_w65c02.h
 * and ui/ui_m6522.h. Unlike those (forks of upstream chip widgets), the RIA is
 * bespoke: it shares the 6502 bus, so the window shows the bus as the RIA last
 * decoded it (ria_chip()->PINS, plus a synthetic CS over the RIA window) and a
 * read-only view of the register file ($FFE0-$FFFF, in ram[]) and the XSTACK SP.
 *
 * Header-only with the implementation under CHIPS_UI_IMPL, emitted by the single
 * TU that defines it (dbgui.cc), matching the chips-ui convention. ImGui is
 * assumed already included by that TU.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui/ui_chip.h"       /* ui_chip_t / ui_chip_desc_t */
#include "ui/ui_settings.h"   /* ui_settings_t */
#include "emu/chips/rp6502.h" /* ria_t, ria_chip */

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    const char *title;
    int x, y, w, h; /* initial window geometry; 0 w/h -> default */
    bool open;      /* initial open state */
} ui_rp6502_desc_t;

typedef struct
{
    const char *title;
    float init_x, init_y, init_w, init_h;
    bool open;
    bool valid;
    ui_chip_t chip;
} ui_rp6502_t;

void ui_rp6502_init(ui_rp6502_t *win, const ui_rp6502_desc_t *desc);
void ui_rp6502_discard(ui_rp6502_t *win);
void ui_rp6502_draw(ui_rp6502_t *win);
void ui_rp6502_save_settings(ui_rp6502_t *win, ui_settings_t *settings);
void ui_rp6502_load_settings(ui_rp6502_t *win, const ui_settings_t *settings);

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_UI_IMPL
#include <cstdio>  /* std::snprintf */
#include <string.h>
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif
#include "emu/chips/w65c02.h" /* M6502_* pin macros (for the pin diagram) */
#include "emu/sys/mem.h"      /* ram, xstack_ptr, RIA_WINDOW_LO/HI */

/* The RIA is not a chips-modeled chip; it shares the 6502 bus, so its "pins" are
 * that bus as the RIA decodes it (fed live from ria_chip()->PINS), plus a synthetic
 * CS lit while the RIA window is addressed. RIA_PIN_CS rides a free bit above
 * M6502_PIN_MASK (bit 40), so it never collides with a real CPU pin. */
#define RIA_PIN_CS (1ULL << 40)
static const ui_chip_pin_t _ui_rp6502_pins[] = {
    {"D0", 0, M6502_D0}, {"D1", 1, M6502_D1}, {"D2", 2, M6502_D2}, {"D3", 3, M6502_D3},
    {"D4", 4, M6502_D4}, {"D5", 5, M6502_D5}, {"D6", 6, M6502_D6}, {"D7", 7, M6502_D7},
    {"RW", 9, M6502_RW}, {"SYNC", 10, M6502_SYNC}, {"IRQ", 11, M6502_IRQ},
    {"RES", 12, M6502_RES}, {"CS", 13, RIA_PIN_CS},
    {"A0", 16, M6502_A0}, {"A1", 17, M6502_A1}, {"A2", 18, M6502_A2}, {"A3", 19, M6502_A3},
    {"A4", 20, M6502_A4}, {"A5", 21, M6502_A5}, {"A6", 22, M6502_A6}, {"A7", 23, M6502_A7},
    {"A8", 24, M6502_A8}, {"A9", 25, M6502_A9}, {"A10", 26, M6502_A10}, {"A11", 27, M6502_A11},
    {"A12", 28, M6502_A12}, {"A13", 29, M6502_A13}, {"A14", 30, M6502_A14}, {"A15", 31, M6502_A15},
};

/* The documented RIA register window ($FFE0-$FFFF, ria.rst). width 2 = a 16-bit
 * little-endian pair. The whole window (registers + vectors) lives in ram[]. */
static const struct { uint16_t addr; const char *name; uint8_t width; } _ui_rp6502_regs[] = {
    {0xFFE0, "READY", 1}, {0xFFE1, "TX", 1}, {0xFFE2, "RX", 1}, {0xFFE3, "VSYNC", 1},
    {0xFFE4, "RW0", 1}, {0xFFE5, "STEP0", 1}, {0xFFE6, "ADDR0", 2},
    {0xFFE8, "RW1", 1}, {0xFFE9, "STEP1", 1}, {0xFFEA, "ADDR1", 2},
    {0xFFEC, "XSTACK", 1}, {0xFFED, "ERRNO", 2}, {0xFFEF, "OP", 1},
    {0xFFF0, "IRQ", 1}, {0xFFF1, "SPIN", 1}, {0xFFF2, "BUSY", 1}, {0xFFF3, "LDA", 1},
    {0xFFF4, "A", 1}, {0xFFF5, "LDX", 1}, {0xFFF6, "X", 1}, {0xFFF7, "RTS", 1},
    {0xFFF8, "SREG", 2}, {0xFFFA, "NMIB", 2}, {0xFFFC, "RESB", 2}, {0xFFFE, "IRQB", 2},
};

void ui_rp6502_init(ui_rp6502_t *win, const ui_rp6502_desc_t *desc)
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
    UI_CHIP_INIT_DESC(&cd, "RIA", 32, _ui_rp6502_pins);
    ui_chip_init(&win->chip, &cd);
}

void ui_rp6502_discard(ui_rp6502_t *win)
{
    CHIPS_ASSERT(win && win->valid);
    win->valid = false;
}

/* A read-only inspector (edit registers via the Memory window if needed; the
 * xstack lives there too, as the Memory window's third layer). */
void ui_rp6502_draw(ui_rp6502_t *win)
{
    CHIPS_ASSERT(win && win->valid && win->title);
    if (!win->open)
        return;
    ImGui::SetNextWindowPos(ImVec2(win->init_x, win->init_y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(win->init_w, win->init_h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(win->title, &win->open))
    {
        /* Pins: the bus as the RIA chip last decoded it (ria_chip()->PINS), CS lit
         * when the RIA window is the addressed device. */
        uint64_t p = ((const ria_t *)ria_chip())->PINS;
        uint16_t a = (uint16_t)(p & 0xFFFFu);
        if (a >= RIA_WINDOW_LO && a <= RIA_WINDOW_HI)
            p |= RIA_PIN_CS;
        ImGui::BeginChild("##ria_pins", ImVec2(176, 0), true);
        ui_chip_draw(&win->chip, p);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##ria_state", ImVec2(0, 0), true);

        /* The xstack pointer — an internal latch the memory-mapped register file
         * doesn't carry (empty when SP == XSTACK_SIZE; live bytes are [SP,$1FF]). */
        ImGui::Text("XSTACK SP $%03X", (unsigned)xstack_ptr);
        ImGui::Spacing();

        ImGui::TextUnformatted("Registers");
        ImGui::Separator();
        ImGui::BeginChild("##ria_regs", ImVec2(0, 0), false);
        if (ImGui::BeginTable("##regs", 3,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit))
        {
            for (auto &r : _ui_rp6502_regs)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("$%04X", r.addr);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name);
                ImGui::TableNextColumn();
                if (r.width == 2)
                    ImGui::Text("$%04X", (unsigned)(ram[r.addr] |
                                                    (ram[(uint16_t)(r.addr + 1)] << 8)));
                else
                {
                    uint8_t v = ram[r.addr];
                    ImGui::Text("$%02X", v);
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
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::EndChild();
    }
    ImGui::End();
}

void ui_rp6502_save_settings(ui_rp6502_t *win, ui_settings_t *settings)
{
    CHIPS_ASSERT(win && settings);
    ui_settings_add(settings, win->title, win->open);
}

void ui_rp6502_load_settings(ui_rp6502_t *win, const ui_settings_t *settings)
{
    CHIPS_ASSERT(win && settings);
    win->open = ui_settings_isopen(settings, win->title);
}
#endif /* CHIPS_UI_IMPL */
