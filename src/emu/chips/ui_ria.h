/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RIA debug window — a chips-ui-style inspector beside ui_w65c02.h and
 * ui/ui_m6522.h. Unlike those (forks of upstream chip widgets), the RIA is
 * bespoke: it shares the 6502 bus, so the window shows the RIA's pins as last
 * decoded (ria_chip()->PINS, plus a synthetic RREQ over the RIA window) and a
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
#include "emu/sys/ria.h"   /* ria_t, ria_chip, RIA_PIN_RREQ */

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    const char *title;
    int x, y, w, h; /* initial window geometry; 0 w/h -> default */
    bool open;      /* initial open state */
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
#include "emu/sys/cpu.h"      /* cpu_get_phi2_khz_run (Status) */
#include "ria/api/oem.h"      /* oem_get_code_page_run (Status) */

/* The RIA shares the 6502 bus but wires only its own pins: RREQ, RW, D0-D7, and
 * the low 5 address lines (A0-A4) that select its 32-byte register window. A5-A15
 * are decoded off-chip into RREQ, so they never reach the RIA. Pins are fed live
 * from ria_chip()->PINS; RIA_PIN_RREQ is defined in emu/sys/ria.h. */
static const ui_chip_pin_t _ui_ria_pins[] = {
    {"D0", 0, M6502_D0}, {"D1", 1, M6502_D1}, {"D2", 2, M6502_D2}, {"D3", 3, M6502_D3},
    {"D4", 4, M6502_D4}, {"D5", 5, M6502_D5}, {"D6", 6, M6502_D6}, {"D7", 7, M6502_D7},
    {"RW", 9, M6502_RW}, {"IRQ", 10, M6502_IRQ}, {"RES", 11, M6502_RES}, {"RREQ", 12, RIA_PIN_RREQ},
    {"A0", 13, M6502_A0}, {"A1", 14, M6502_A1}, {"A2", 15, M6502_A2}, {"A3", 16, M6502_A3},
    {"A4", 17, M6502_A4},
};

/* The documented RIA register window ($FFE0-$FFFF, ria.rst). width 2 = a 16-bit
 * little-endian pair. The whole window (registers + vectors) lives in ram[]. */
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

/* A read-only inspector (edit registers via the Memory window if needed; the
 * xstack lives there too, as the Memory window's third layer). */
void ui_ria_draw(ui_ria_t *win)
{
    CHIPS_ASSERT(win && win->valid && win->title);
    if (!win->open)
        return;
    ImGui::SetNextWindowPos(ImVec2(win->init_x, win->init_y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(win->init_w, win->init_h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(win->title, &win->open))
    {
        /* Pins: the bus as the RIA chip last decoded it (ria_chip()->PINS) — IRQ
         * already rides in it (ria_tick drives M6502_IRQ from ria_irq_asserted).
         * Overlaid: RREQ lit when the RIA window is the addressed device, and RES
         * lit while the RIA holds the 6502 in reset (between a stop and the next
         * run — cpu_halted(), which is not the debugger's mid-run pause). */
        uint64_t p = ((const ria_t *)ria_chip())->PINS;
        uint16_t a = (uint16_t)(p & 0xFFFFu);
        if (a >= RIA_WINDOW_LO && a <= RIA_WINDOW_HI)
            p |= RIA_PIN_RREQ;
        if (cpu_halted())
            p |= M6502_RES;
        ImGui::BeginChild("##ria_pins", ImVec2(176, 0), true);
        ui_chip_draw(&win->chip, p);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##ria_state", ImVec2(0, 0), true);

        /* Internal latches the memory-mapped register file doesn't carry: the
         * xstack pointer (empty when SP == XSTACK_SIZE; live bytes are [SP,$1FF])
         * and the running code page / PHI2 (config settings with no bus register).
         * The RIA's IRQ assertion shows on the IRQ pin (driven by ria_tick). */
        if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("XSTACK SP:    $%03X", (unsigned)xstack_ptr);
            ImGui::Text("CODE PAGE:    %u", (unsigned)oem_get_code_page_run());
            ImGui::Text("PHI2:         %u kHz", (unsigned)cpu_get_phi2_khz_run());
        }

        if (ImGui::CollapsingHeader("Registers", ImGuiTreeNodeFlags_DefaultOpen))
        {
            /* The RIA register file is regs[] (no longer aliased into ram[]); the
             * vectors above the window ($FFFA-$FFFF) are real RAM. Rows match the
             * VIA panel's "NAME ($addr/dec): val" layout (ui/ui_m6522.h). */
            auto peek = [](uint16_t a) -> uint8_t {
                return (a >= RIA_WINDOW_LO && a <= RIA_WINDOW_HI) ? regs[a & 0x1F] : ram[a];
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
