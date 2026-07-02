/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * On-screen machine debugger overlay, shown only in debug mode (--debug/--dap).
 * Hosts floooh/chips ui_dbg (disassembly), a forked ui_m6502 (CPU; our ui_w65c02.h,
 * with the 6510 I/O-port panel removed) and ui_m6522 (VIA) as VIEWS, plus small native
 * windows (run/step/breakpoints, RIA) that drive the one engine in dbg.c. Layout
 * persistence lives in dbgui_layout.cc. The DAP adapter drives the same dbg.c, so VS Code and
 * this overlay stay consistent. We never let ui_dbg drive the CPU: ui_dbg_tick
 * runs every cycle (heatmap/history/PC plus its breakpoint engine, the only
 * evaluator of the non-EXEC types), but a trap only files a break request with
 * dbg.c, which gates the CPU; we reflect its stop-state into ui_dbg for display.
 * We never set ui_dbg's step_mode, so it never self-steps.
 */

#include "imgui.h"
#include "imgui_internal.h" /* chips ui_*.h reach a few internal ImGui APIs */

extern "C"
{
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/sys/cpu.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/via.h"
}
#include "emu/dbg/dbgui.h"        /* the C-callable entry points this TU defines */
#include "emu/dbg/dbgui_layout.h" /* ImGui-owned layout persistence (file side) */
#include "emu/app/window.h"       /* window-scale presets */
#include "emu/app/credits.h"      /* EMU_CREDITS */

#include "emu/chips/w65c02.h" /* m6502_t (type + macros; CHIPS_IMPL is in w65c02.c) */
#include "m6522.h"            /* m6522_t (type; CHIPS_IMPL is in via.c) */

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "util/sokol_imgui.h" /* simgui_* + simgui_imtextureid */

/* chips UI headers — CHIPS_UI_IMPL is set by CMake on this TU only. Order per
 * ui_dbg.h's "include before the implementation" note. */
#include "ui/ui_util.h"
#include "ui/ui_settings.h"
#include "ui/ui_chip.h"
#include "ui/ui_memedit.h"
#include "ui/ui_memmap.h"
#include "ui/ui_audio.h"
#define CHIPS_UTIL_IMPL           /* emit m6502dasm_op (the disassembler ui_dbg calls) */
#include "emu/chips/w65c02dasm.h" /* 65C02 fork of chips/util/m6502dasm.h (CMOS opcodes) */
#include "emu/chips/ui_dasm.h"    /* our fork of ui/ui_dasm.h: 65C02 jump arrows; after w65c02dasm.h (impl calls m6502dasm_op) */
#include "emu/chips/ui_w65c02.h"  /* our fork of ui/ui_m6502.h: no 6510 I/O-port panel */
#include "emu/chips/ui_rp6502.h"  /* our RIA debug window (bespoke, not a chips fork) */
#include "emu/chips/ui_ini.h"     /* dummy elements: [RP6502][Launch] + [Window][Manager] */
#include "ui/ui_m6522.h"
#include "emu/chips/ui_dbg.h" /* our fork of ui/ui_dbg.h: history column draws chars, not bytes */

#include <cstdio> /* snprintf, sscanf */

static ui_dbg_t g_dbg;
static ui_w65c02_t g_cpuwin;
static ui_m6522_t g_viawin;
static ui_memedit_t g_memedit;
static ui_memmap_t g_memmap;
static ui_rp6502_t g_ria;
static ui_dasm_t g_dasm;
static ui_audio_t g_audio;
static bool g_inited;
static bool g_control_open = false; /* the native "Debug Control" window */
static bool g_credits_open = false; /* transient about-box; not persisted */
static float g_menu_h;              /* main-menu-bar height in ImGui points (see dbgui_menu_height) */

/* ---- ui_dbg texture callbacks: back its heatmap with a sokol-gfx image, used
 * as an ImTextureID via simgui_imtextureid. Called from ui_dbg_draw on the main
 * thread, so sokol-gfx access is safe. ---- */
struct dbg_tex
{
    sg_image img;
    sg_view view;
    bool used;
};
static dbg_tex g_tex[4];

static ui_texture_t tex_create(int w, int h, const char *label)
{
    (void)label;
    for (auto &t : g_tex)
    {
        if (t.used)
            continue;
        sg_image_desc id{};
        id.usage.stream_update = true;
        id.width = w;
        id.height = h;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        t.img = sg_make_image(&id);
        sg_view_desc vd{};
        vd.texture.image = t.img;
        t.view = sg_make_view(&vd);
        t.used = true;
        return simgui_imtextureid(t.view);
    }
    return 0;
}

static dbg_tex *tex_find(ui_texture_t h)
{
    for (auto &t : g_tex)
        if (t.used && simgui_imtextureid(t.view) == h)
            return &t;
    return nullptr;
}

static void tex_update(ui_texture_t h, void *data, int size)
{
    dbg_tex *t = tex_find(h);
    if (!t)
        return;
    sg_image_data d{};
    d.mip_levels[0].ptr = data;
    d.mip_levels[0].size = (size_t)size;
    sg_update_image(t->img, &d);
}

static void tex_destroy(ui_texture_t h)
{
    dbg_tex *t = tex_find(h);
    if (!t)
        return;
    sg_destroy_view(t->view);
    sg_destroy_image(t->img);
    t->used = false;
}

/* Side-effect-free peek/poke of the 6502 address space for the debugger views.
 * The RIA register file lives in ram[] ($FFE0-$FFFF), so a plain ram[] access
 * reads the live registers and the API trampoline opcodes the 6502 executes
 * there — and, unlike ria_reg_read(), without the side effects (xstack pop, IRQ
 * ack, RW auto-increment) that a debugger view must never trigger. */
static uint8_t mem_peek(uint16_t addr)
{
    return ram[addr];
}

static void mem_poke(uint16_t addr, uint8_t data)
{
    ram[addr] = data;
}

/* ui_dbg memory read callback (the disassembler + heatmap). */
static uint8_t mem_read(int layer, uint16_t addr, void *user)
{
    (void)layer;
    (void)user;
    return mem_peek(addr);
}

/* ui_memedit callbacks. Layer 0 is the 6502 space ($0000-$FFFF; the RIA register
 * file lives in it at $FFE0); layer 1 is XRAM, which the system maps at $10000-$1FFFF (its
 * own 64KB bank — the 6502 reaches it only through the RIA's RW0/RW1 windows);
 * layer 2 is the 512-byte RIA xstack ($000-$1FF, a top-down LIFO). dbgui_draw
 * scopes the window's max_addr to 512 while that layer is selected, so the editor
 * only addresses $000-$1FF there; the bound below is a defensive guard. */
static uint8_t memedit_read(int layer, uint16_t addr, void *user)
{
    (void)user;
    switch (layer)
    {
    case 1:
        return xram[addr];
    case 2:
        return addr < XSTACK_SIZE ? xstack[addr] : 0;
    default:
        return mem_peek(addr);
    }
}

static void memedit_write(int layer, uint16_t addr, uint8_t data, void *user)
{
    (void)user;
    switch (layer)
    {
    case 1:
        xram[addr] = data;
        break;
    case 2:
        if (addr < XSTACK_SIZE)
            xstack[addr] = data;
        break;
    default:
        mem_poke(addr, data);
        break;
    }
}

/* The native control window: run state + step/continue/pause + breakpoints, all
 * driving dbg.c (the same engine VS Code drives over DAP). */
static void draw_control(void)
{
    if (!g_control_open)
        return;
    if (ImGui::Begin("Debug Control", &g_control_open))
    {
        const bool stopped = dbg_is_stopped();
        if (emu_cpu_halted)
            ImGui::Text("exited (code %d)", emu_exit_code); /* no CPU to step/pause */
        else if (stopped)
            ImGui::Text("STOPPED at $%04X", dbg_stop_pc());
        else
            ImGui::TextUnformatted("running");

        if (stopped)
        {
            if (ImGui::Button("Continue"))
                dbg_continue();
            ImGui::SameLine();
            if (ImGui::Button("Step Into"))
                dbg_step(DBG_STEP_INSTR);
            ImGui::SameLine();
            if (ImGui::Button("Step Over"))
                dbg_step(DBG_STEP_LINE_OVER);
        }
        else if (!emu_cpu_halted && ImGui::Button("Pause"))
            dbg_request_pause();

        ImGui::Separator();
        static char bpbuf[8] = "";
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("hex", bpbuf, sizeof bpbuf, ImGuiInputTextFlags_CharsHexadecimal);
        unsigned a = 0;
        ImGui::SameLine();
        if (ImGui::Button("Add BP") && std::sscanf(bpbuf, "%x", &a) == 1)
            dbg_add_breakpoint((uint16_t)a);
        ImGui::SameLine();
        if (ImGui::Button("Del BP") && std::sscanf(bpbuf, "%x", &a) == 1)
            dbg_remove_breakpoint((uint16_t)a);

        ImGui::TextUnformatted("Breakpoints:");
        bool first_bp = true;
        for (int addr = 0; addr <= 0xFFFF; addr++)
            if (dbg_has_breakpoint((uint16_t)addr))
            {
                if (!first_bp)
                    ImGui::SameLine();
                first_bp = false;
                ImGui::Text("$%04X", addr);
            }
    }
    ImGui::End();
}

/* Third-party credits, the same text --credits prints. */
static void draw_credits(void)
{
    if (!g_credits_open)
        return;
    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Credits", &g_credits_open))
        ImGui::TextUnformatted(EMU_CREDITS);
    ImGui::End();
}

/* Pin diagrams for the chip windows. ui_chip requires a named desc with pins. */
static const ui_chip_pin_t pins_6502[] = {
    {"D0", 0, M6502_D0},
    {"D1", 1, M6502_D1},
    {"D2", 2, M6502_D2},
    {"D3", 3, M6502_D3},
    {"D4", 4, M6502_D4},
    {"D5", 5, M6502_D5},
    {"D6", 6, M6502_D6},
    {"D7", 7, M6502_D7},
    {"RW", 9, M6502_RW},
    {"SYNC", 10, M6502_SYNC},
    {"RDY", 11, M6502_RDY},
    {"IRQ", 12, M6502_IRQ},
    {"NMI", 13, M6502_NMI},
    {"RES", 14, M6502_RES},
    {"A0", 16, M6502_A0},
    {"A1", 17, M6502_A1},
    {"A2", 18, M6502_A2},
    {"A3", 19, M6502_A3},
    {"A4", 20, M6502_A4},
    {"A5", 21, M6502_A5},
    {"A6", 22, M6502_A6},
    {"A7", 23, M6502_A7},
    {"A8", 24, M6502_A8},
    {"A9", 25, M6502_A9},
    {"A10", 26, M6502_A10},
    {"A11", 27, M6502_A11},
    {"A12", 28, M6502_A12},
    {"A13", 29, M6502_A13},
    {"A14", 30, M6502_A14},
    {"A15", 31, M6502_A15},
};

static const ui_chip_pin_t pins_6522[] = {
    {"D0", 0, M6522_D0},
    {"D1", 1, M6522_D1},
    {"D2", 2, M6522_D2},
    {"D3", 3, M6522_D3},
    {"D4", 4, M6522_D4},
    {"D5", 5, M6522_D5},
    {"D6", 6, M6522_D6},
    {"D7", 7, M6522_D7},
    {"RS0", 9, M6522_RS0},
    {"RS1", 10, M6522_RS1},
    {"RS2", 11, M6522_RS2},
    {"RS3", 12, M6522_RS3},
    {"RW", 14, M6522_RW},
    {"CS1", 15, M6522_CS1},
    {"CS2", 16, M6522_CS2},
    {"IRQ", 17, M6522_IRQ},
    {"PA0", 20, M6522_PA0},
    {"PA1", 21, M6522_PA1},
    {"PA2", 22, M6522_PA2},
    {"PA3", 23, M6522_PA3},
    {"PA4", 24, M6522_PA4},
    {"PA5", 25, M6522_PA5},
    {"PA6", 26, M6522_PA6},
    {"PA7", 27, M6522_PA7},
    {"PB0", 28, M6522_PB0},
    {"PB1", 29, M6522_PB1},
    {"PB2", 30, M6522_PB2},
    {"PB3", 31, M6522_PB3},
    {"PB4", 32, M6522_PB4},
    {"PB5", 33, M6522_PB5},
    {"PB6", 34, M6522_PB6},
    {"PB7", 35, M6522_PB7},
};

/* ---- Layout persistence: the config file is owned by ImGui's settings system.
 * Two custom ImGuiSettingsHandlers (registered in dbgui_init) ride in that ini:
 *   [Chips][<window title>]  IsOpen=1  — per-window open flags (chips ui_settings_t)
 *   [RP6502][Launch] + [Window][Manager] entry — ui_ini.h's elements
 * Window geometry rides in ImGui's built-in [Window] handler; the menu bar (below)
 * just toggles each window's open flag. ---- */
static ui_settings_t g_settings;

/* Collect every window's open flag into g_settings. The native "Debug Control"
 * window has no chips struct, so it is added by title here. */
static void dbgui_collect_settings(void)
{
    ui_settings_init(&g_settings);
    ui_dbg_save_settings(&g_dbg, &g_settings);
    ui_w65c02_save_settings(&g_cpuwin, &g_settings);
    ui_m6522_save_settings(&g_viawin, &g_settings);
    ui_memedit_save_settings(&g_memedit, &g_settings);
    ui_memmap_save_settings(&g_memmap, &g_settings);
    ui_rp6502_save_settings(&g_ria, &g_settings);
    ui_dasm_save_settings(&g_dasm, &g_settings);
    ui_audio_save_settings(&g_audio, &g_settings);
    ui_settings_add(&g_settings, "Debug Control", g_control_open);
}

/* A bit signature of every window's open flag, for cheap per-frame change
 * detection: toggling a window (menu item or its title-bar X) flips our own bool
 * and does NOT dirty ImGui's settings, so WantSaveIniSettings won't fire — we
 * watch the flags directly and persist the change ourselves. (<= 32 windows.) */
static unsigned dbgui_open_sig(void)
{
    dbgui_collect_settings();
    unsigned sig = 0;
    for (int i = 0; i < g_settings.num_slots; i++)
        sig = (sig << 1) | (g_settings.slots[i].open ? 1u : 0u);
    return sig;
}

/* [Chips] handler: per-window open flags (mirrors chips-test examples/common/ui.cc). */
static int g_chips_cur_slot = -1;
static void chips_ini_clear(ImGuiContext *, ImGuiSettingsHandler *) { ui_settings_init(&g_settings); }
static void *chips_ini_readopen(ImGuiContext *, ImGuiSettingsHandler *, const char *name)
{
    g_chips_cur_slot = ui_settings_add(&g_settings, name, false) ? g_settings.num_slots - 1 : -1;
    return (void *)&g_settings; /* any non-null: this section has lines to read */
}
static void chips_ini_readline(ImGuiContext *, ImGuiSettingsHandler *, void *, const char *line)
{
    int is_open = 0;
    if (g_chips_cur_slot >= 0 && std::sscanf(line, "IsOpen=%i", &is_open) == 1)
        g_settings.slots[g_chips_cur_slot].open = (is_open != 0);
}
static void chips_ini_applyall(ImGuiContext *, ImGuiSettingsHandler *)
{
    ui_dbg_load_settings(&g_dbg, &g_settings);
    ui_w65c02_load_settings(&g_cpuwin, &g_settings);
    ui_m6522_load_settings(&g_viawin, &g_settings);
    ui_memedit_load_settings(&g_memedit, &g_settings);
    ui_memmap_load_settings(&g_memmap, &g_settings);
    ui_rp6502_load_settings(&g_ria, &g_settings);
    ui_dasm_load_settings(&g_dasm, &g_settings);
    ui_audio_load_settings(&g_audio, &g_settings);
    g_control_open = ui_settings_isopen(&g_settings, "Debug Control");
}
static void chips_ini_writeall(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
    dbgui_collect_settings();
    for (int i = 0; i < g_settings.num_slots; i++)
    {
        buf->appendf("[%s][%s]\n", handler->TypeName, g_settings.slots[i].window_title.buf);
        if (g_settings.slots[i].open)
            buf->append("IsOpen=1\n");
        buf->append("\n");
    }
}

/* The [RP6502][Launch] block and the dummy [Window][Manager] entry ride in
 * ui_ini.h's elements. */
static ui_ini_t g_ini;

/* The last debug session's window size, wanted BEFORE the window opens (it goes
 * in sapp_desc; post-open resizes are unreliable under WSLg) — so before ImGui
 * exists. Rather than hand-parse the ini, load it through a throwaway ImGui
 * context: the built-in [Window] handler parses the Manager entry there, no
 * custom handler needed. False if absent or implausible. */
bool dbgui_window_size(int *w, int *h)
{
    if (!ImGui::GetCurrentContext())
    {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        dbgui_layout_load();
        bool have = ui_ini_window_size(w, h);
        ImGui::DestroyContext();
        return have;
    }
    return ui_ini_window_size(w, h);
}

/* Register both settings handlers; called once in dbgui_init, before the layout
 * load, so they fire during LoadIniSettingsFromMemory / SaveIniSettingsToMemory. */
static void dbgui_register_settings_handlers(void)
{
    ImGuiSettingsHandler chips_h;
    chips_h.TypeName = "Chips";
    chips_h.TypeHash = ImHashStr("Chips");
    chips_h.ClearAllFn = chips_ini_clear;
    chips_h.ReadOpenFn = chips_ini_readopen;
    chips_h.ReadLineFn = chips_ini_readline;
    chips_h.ApplyAllFn = chips_ini_applyall;
    chips_h.WriteAllFn = chips_ini_writeall;
    ImGui::AddSettingsHandler(&chips_h);

    ui_ini_register(&g_ini);
}

/* Emulated VGA frame rate for the menu readout: target 60 Hz, dropping when the
 * host can't run the machine in real time. Measured from emu_vga_frame_count over
 * wall-clock windows — NOT io.Framerate, which is the host's uncapped present rate
 * (often hundreds of Hz) and says nothing about whether the emulation keeps pace.
 * Counts hold flat while stopped at a breakpoint, so it reads ~0 when paused. */
static float dbgui_vga_fps(void)
{
    static double win_time;
    static unsigned long win_base;
    static bool primed;
    static float fps;
    if (!primed)
    {
        primed = true;
        win_base = emu_vga_frame_count;
    }
    win_time += ImGui::GetIO().DeltaTime;
    if (win_time >= 0.5) /* refresh the reading twice a second */
    {
        unsigned long now = emu_vga_frame_count;
        fps = (float)((double)(now - win_base) / win_time);
        win_base = now;
        win_time = 0.0;
    }
    return fps;
}

/* The menu bar: each item toggles a window's open flag, so a window closed with
 * its title-bar X can always be brought back. */
static void dbgui_draw_menu(void)
{
    if (ImGui::BeginMainMenuBar())
    {
        /* Record the bar's drawn height so the window layer can reserve a strip
         * for it and lay the emulated canvas out below it (dbgui_menu_height). */
        g_menu_h = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("Hardware"))
        {
            ImGui::MenuItem("MOS 65C02 (CPU)", nullptr, &g_cpuwin.open);
            ImGui::MenuItem("MOS 65C22 (VIA)", nullptr, &g_viawin.open);
            ImGui::MenuItem("RP6502 (RIA)", nullptr, &g_ria.open);
            ImGui::MenuItem("Audio", nullptr, &g_audio.open);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug"))
        {
            ImGui::MenuItem("Debug Control", nullptr, &g_control_open);
            ImGui::MenuItem("Breakpoints", nullptr, &g_dbg.ui.breakpoints.open);
            ImGui::MenuItem("Stopwatch", nullptr, &g_dbg.ui.stopwatch.open);
            ImGui::MenuItem("Disassembler", nullptr, &g_dbg.ui.open);
            ImGui::MenuItem("Disassembly Browser", nullptr, &g_dasm.open);
            ImGui::MenuItem("Execution History", nullptr, &g_dbg.ui.history.open);
            ImGui::MenuItem("Memory", nullptr, &g_memedit.open);
            ImGui::MenuItem("Memory Heatmap", nullptr, &g_dbg.ui.heatmap.open);
            ImGui::MenuItem("Memory Segments", nullptr, &g_memmap.open);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            /* Window-size presets (the --scale values): a reset to a known size
             * after a manual resize, so docked panels deliberately don't factor
             * in. The check marks the preset the window currently matches. */
            static const double scales[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0};
            double cur = emu_get_window_scale();
            for (double s : scales)
            {
                char label[5];
                std::snprintf(label, sizeof label, "%.1fx", s);
                if (ImGui::MenuItem(label, nullptr, cur > s - 0.01 && cur < s + 0.01))
                    emu_set_window_scale(s);
            }
            ImGui::Separator();
            ImGui::MenuItem("Credits", nullptr, &g_credits_open);
            ImGui::EndMenu();
        }
        ui_util_options_menu(); /* chips "Options": UI/BG alpha + theme */
        /* Host frame time + emulated VGA rate, right-aligned (e.g. "2.1 ms  59.9 FPS")
         * and held a few pixels off the window edge. The ms is ImGui's rolling host
         * frame average; the FPS is the emulation keeping pace at ~60. */
        float host_rate = ImGui::GetIO().Framerate;
        char stats[24];
        std::snprintf(stats, sizeof stats, "%.1f ms  %.1f FPS",
                      host_rate > 0.0f ? 1000.0f / host_rate : 0.0f, dbgui_vga_fps());
        float pad = ImGui::GetStyle().ItemSpacing.x + ImGui::GetFontSize() * 0.5f;
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(stats).x - pad);
        ImGui::TextUnformatted(stats);
        ImGui::EndMainMenuBar();
    }
}

/* (Re)build the memory-map description from the loaded program's linker segments
 * (pushed via dbg_set_segments by the DAP launch): one band per segment showing
 * its load address + size. Re-run whenever dbg_segments_generation() changes.
 * Segments only — ui_memmap holds at most UI_MEMMAP_MAX_REGIONS(16) bands per
 * layer, which suits the handful of linker segments but not the dozens of
 * functions/globals the symbol table carries. */
static unsigned g_memmap_seg_gen = (unsigned)-1; /* force the first build */
static void dbgui_build_memmap(void)
{
    ui_memmap_reset(&g_memmap);

    const dbg_segment_t *segs = nullptr;
    int nseg = dbg_get_segments(&segs);
    if (nseg > UI_MEMMAP_MAX_REGIONS)
        nseg = UI_MEMMAP_MAX_REGIONS; /* the layer holds at most this many bands */
    if (nseg > 0)
    {
        ui_memmap_layer(&g_memmap, "Segments");
        for (int i = 0; i < nseg; i++)
        {
            int len = (int)segs[i].size;
            if (len > 0x10000)
                len = 0x10000;
            ui_memmap_region(&g_memmap, segs[i].name, segs[i].addr, len, true);
        }
    }

    g_memmap_seg_gen = dbg_segments_generation();
}

void dbgui_init(void)
{
    if (g_inited)
        return;
    /* Leave the pixel formats at 0: sokol-gfx then defaults them to the same
     * swapchain formats the canvas pipelines use, so the ImGui pipeline matches
     * in the final (swapchain) pass. (Setting them explicitly mismatched.) */
    simgui_desc_t sd{};
    simgui_setup(&sd);
    /* Docking only; no multi-viewports (sokol_app is single-window). */
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    /* We drive load/save to our own config path (dbgui_layout.cc), so disable
     * ImGui's automatic ini file I/O. The custom Chips/RP6502 settings handlers
     * still ride inside the ImGui-format file via Load/SaveIniSettingsToMemory. */
    ImGui::GetIO().IniFilename = nullptr;
    dbgui_register_settings_handlers();

    m6502_t *cpu = (m6502_t *)cpu_chip();
    m6522_t *via = (m6522_t *)via_chip();

    uint32_t freq = (uint32_t)cpu_get_phi2_khz_run() * 1000u;
    if (freq == 0)
        freq = 8000000u;

    ui_dbg_desc_t dd{};
    dd.title = "Disassembler";
    dd.m6502 = cpu;
    dd.freq_hz = freq;
    dd.frame_ticks = freq / EMU_VGA_HZ;
    dd.scanline_ticks = dd.frame_ticks / EMU_VGA_SCANLINES;
    if (dd.scanline_ticks == 0)
        dd.scanline_ticks = 1;
    dd.read_cb = mem_read;
    dd.read_layer = 0;
    dd.texture_cbs.create_cb = tex_create;
    dd.texture_cbs.update_cb = tex_update;
    dd.texture_cbs.destroy_cb = tex_destroy;
    dd.x = 10;
    dd.y = 30;
    dd.open = false;
    /* Hotkeys must be named ImGuiKey_* values (legacy 0..511 indices assert in
     * modern ImGui). These drive ui_dbg's own toolbar shortcuts. */
    dd.keys.cont = {ImGuiKey_F5, "F5"};
    dd.keys.stop = {ImGuiKey_F6, "F6"};
    dd.keys.step_over = {ImGuiKey_F10, "F10"};
    dd.keys.step_into = {ImGuiKey_F11, "F11"};
    dd.keys.step_tick = {ImGuiKey_F8, "F8"};
    dd.keys.toggle_breakpoint = {ImGuiKey_F9, "F9"};
    ui_dbg_init(&g_dbg, &dd);
    /* We own stepping/breakpoints via dbg.c; tell ui_dbg an external debugger is
     * in control so it doesn't steal window focus on every stop. */
    ui_dbg_external_debugger_connected(&g_dbg);

    ui_w65c02_desc_t cd{};
    cd.title = "MOS 65C02 (CPU)";
    cd.cpu = cpu;
    cd.x = 10;
    cd.y = 360;
    cd.open = false;
    UI_CHIP_INIT_DESC(&cd.chip_desc, "6502", 32, pins_6502);
    ui_w65c02_init(&g_cpuwin, &cd);

    ui_m6522_desc_t vd{};
    vd.title = "MOS 65C22 (VIA)";
    vd.via = via;
    vd.x = 420;
    vd.y = 360;
    vd.open = false;
    UI_CHIP_INIT_DESC(&vd.chip_desc, "6522", 40, pins_6522);
    ui_m6522_init(&g_viawin, &vd);

    ui_rp6502_desc_t rd{};
    rd.title = "RP6502 (RIA)";
    rd.x = 860;
    rd.y = 30;
    ui_rp6502_init(&g_ria, &rd);

    /* Memory editor: three layers (the 6502 space, the XRAM bank, and the RIA
     * xstack). The layer names carry the system address ranges, since the editor
     * itself addresses each layer 0..max_addr-1 (its read/write callbacks are
     * 16-bit). */
    ui_memedit_desc_t med{};
    med.title = "Memory";
    med.layers[0] = "CPU";
    med.layers[1] = "XRAM";
    med.layers[2] = "XSTACK";
    med.read_cb = memedit_read;
    med.write_cb = memedit_write;
    med.max_addr = 0x10000;
    med.x = 430;
    med.y = 30;
    med.h = 200;
    med.open = false;
    ui_memedit_init(&g_memedit, &med);

    /* Memory Segments: the loaded program's linker segments, one band each, fed
     * from dbg_get_segments. dbgui_build_memmap fills it; dbgui_draw rebuilds it
     * when the DAP launch pushes a new segment set. */
    ui_memmap_desc_t mmd{};
    mmd.title = "Memory Segments";
    mmd.x = 430;
    mmd.y = 250;
    mmd.open = false;
    ui_memmap_init(&g_memmap, &mmd);
    dbgui_build_memmap();

    /* Free-browsing disassembler, decoupled from the CPU PC (the Disassembler
     * window follows execution; this one navigates anywhere, following jumps). */
    ui_dasm_desc_t dsd{};
    dsd.title = "Disassembly Browser";
    dsd.layers[0] = "RAM";
    dsd.cpu_type = UI_DASM_CPUTYPE_M6502;
    dsd.start_addr = 0x0200; /* the RP6502 program org */
    dsd.read_cb = mem_read;
    dsd.x = 620;
    dsd.y = 30;
    dsd.open = false;
    ui_dasm_init(&g_dasm, &dsd);

    /* Waveform of the produced audio (snd.c's mono viz tap). */
    ui_audio_desc_t ad{};
    ad.title = "Audio";
    ad.sample_buffer = emu_audio_viz_buffer(&ad.num_samples);
    ad.x = 620;
    ad.y = 480;
    ad.open = false;
    ui_audio_init(&g_audio, &ad);

    /* Restore geometry + open flags from the config file (ImGui settings + our
     * Chips/RP6502 handlers). After all windows are initialized, so ApplyAll can
     * push the loaded open flags into them. */
    dbgui_layout_load();
    g_inited = true;

    /* Drive ui_dbg's view from cpu.c's tick loop (heatmap/history/PC). */
    emu_dbg_cycle_cb = dbgui_tick;
}

void dbgui_discard(void)
{
    if (!g_inited)
        return;
    emu_dbg_cycle_cb = nullptr; /* stop feeding ui_dbg before it is destroyed */
    dbgui_layout_save();        /* final flush of geometry + open flags */
    ui_audio_discard(&g_audio);
    ui_dasm_discard(&g_dasm);
    ui_memmap_discard(&g_memmap);
    ui_memedit_discard(&g_memedit);
    ui_rp6502_discard(&g_ria);
    ui_m6522_discard(&g_viawin);
    ui_w65c02_discard(&g_cpuwin);
    ui_dbg_discard(&g_dbg);
    simgui_shutdown();
    g_inited = false;
}

void dbgui_new_frame(int width, int height, double delta_time, float dpi_scale)
{
    simgui_frame_desc_t fd{};
    fd.width = width;
    fd.height = height;
    fd.delta_time = delta_time;
    fd.dpi_scale = dpi_scale;
    simgui_new_frame(&fd);
}

/* Does ui_dbg's breakpoint list hold an execution breakpoint at addr? */
static bool ui_has_exec_bp(uint16_t addr)
{
    for (int i = 0; i < g_dbg.dbg.num_breakpoints; i++)
        if (g_dbg.dbg.breakpoints[i].type == UI_DBG_BREAKTYPE_EXEC &&
            g_dbg.dbg.breakpoints[i].addr == addr)
            return true;
    return false;
}

/* Dockspace central-node rect in framebuffer pixels, refreshed each dbgui_draw and
 * read by the window layer to size the emulated canvas (see dbgui_canvas_rect). */
static int g_canvas_x, g_canvas_y, g_canvas_w, g_canvas_h;
static bool g_canvas_valid;

bool dbgui_canvas_rect(int *x, int *y, int *w, int *h)
{
    if (!g_canvas_valid)
        return false;
    *x = g_canvas_x;
    *y = g_canvas_y;
    *w = g_canvas_w;
    *h = g_canvas_h;
    return true;
}

void dbgui_draw(void)
{
    /* Persist layout changes. ImGui sets WantSaveIniSettings (after its timer) for
     * geometry moves/resizes; window open-flags don't dirty ImGui's settings, so we
     * watch them with a cheap signature and flush on any change too. The signature
     * reflects the previous frame's final state (toggles happen mid-draw), so a
     * change saves on the next frame — independent of the exit path. */
    static unsigned dbgui_last_open_sig;
    static bool dbgui_open_sig_primed;
    unsigned open_sig = dbgui_open_sig();
    if (!dbgui_open_sig_primed)
    {
        dbgui_last_open_sig = open_sig;
        dbgui_open_sig_primed = true;
    }
    if (ImGui::GetIO().WantSaveIniSettings || open_sig != dbgui_last_open_sig)
    {
        dbgui_layout_save();
        ImGui::GetIO().WantSaveIniSettings = false;
        dbgui_last_open_sig = open_sig;
    }

    /* Reflect dbg.c's run state into ui_dbg so its toolbar + disassembly show the
     * stop. (ui_dbg may also flip its own 'stopped' when ui_dbg_tick traps on a
     * mirrored breakpoint; this keeps the two in agreement either way.) */
    const bool stopped = dbg_is_stopped();
    if (stopped != ui_dbg_stopped(&g_dbg))
    {
        if (stopped)
            ui_dbg_break(&g_dbg);
        else
            ui_dbg_continue(&g_dbg, false);
    }

    /* ui_dbg_tick (driven from cpu.c every cycle) maintains the PC highlight while
     * the CPU runs. When stopped, pin it to dbg.c's authoritative stop PC (also
     * covers dbg_note_stop, which is not produced by a tick) and scroll to it on
     * entry to the stop; while running, ui_dbg's own draw keeps the PC in view. */
    static bool prev_stopped;
    if (stopped)
    {
        g_dbg.dbg.cur_op_pc = dbg_stop_pc();
        if (!prev_stopped)
            g_dbg.ui.request_scroll = true;
    }
    prev_stopped = stopped;

    /* Refresh the memory-map segments if the DAP launch loaded a new program. */
    if (dbg_segments_generation() != g_memmap_seg_gen)
        dbgui_build_memmap();

    dbgui_draw_menu();
    /* Host dockspace over the main viewport; PassthruCentralNode leaves the empty
     * center transparent so the emulated canvas shows through it. The window layer
     * sizes the canvas to that central node, so docked panels take space beside the
     * screen rather than over it. */
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    if (const ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(dockspace_id))
    {
        const float s = ImGui::GetIO().DisplayFramebufferScale.x; /* points -> fb px */
        g_canvas_x = (int)(central->Pos.x * s);
        g_canvas_y = (int)(central->Pos.y * s);
        g_canvas_w = (int)(central->Size.x * s);
        g_canvas_h = (int)(central->Size.y * s);
        g_canvas_valid = true;
    }
    else
        g_canvas_valid = false;
    draw_control();
    draw_credits();
    ui_rp6502_draw(&g_ria);

    /* dbg.c is the authoritative run/stop engine + EXEC breakpoint store (shared
     * with the DAP adapter); ui_dbg's gutter/toolbar/hotkeys are a front-end
     * bridged to it. The non-EXEC types (Byte/Word/IRQ/NMI) live only in ui_dbg's
     * list: its tick engine evaluates them and dbgui_tick bridges a trap into a
     * dbg.c stop. Mirror dbg.c's breakpoints into ui_dbg's list so the disassembly
     * gutter shows them (and remember the set), let ui_dbg draw — during which the
     * user may toggle a gutter dot, press F9, or click a toolbar button — then
     * push the resulting EXEC deltas back to dbg.c after the draw. ui_dbg's list
     * is fixed at UI_DBG_MAX_BREAKPOINTS; mirror only the breakpoints in the
     * displayed disassembly window (line_array) so every visible dot shows even
     * when dbg.c holds more than that across the 64K space. */
    uint16_t before[UI_DBG_MAX_BREAKPOINTS];
    int n_before = 0;
    int keep = 0; /* drop last frame's EXEC mirror, keep the user's non-EXEC entries */
    for (int i = 0; i < g_dbg.dbg.num_breakpoints; i++)
        if (g_dbg.dbg.breakpoints[i].type != UI_DBG_BREAKTYPE_EXEC)
            g_dbg.dbg.breakpoints[keep++] = g_dbg.dbg.breakpoints[i];
    g_dbg.dbg.num_breakpoints = keep;
    for (int li = 0; li < UI_DBG_NUM_LINES && g_dbg.dbg.num_breakpoints < UI_DBG_MAX_BREAKPOINTS; li++)
    {
        uint16_t a = g_dbg.ui.line_array[li].addr;
        if (dbg_has_breakpoint(a) && !ui_has_exec_bp(a)) /* line_array repeats addr in the backtrace half */
        {
            before[n_before++] = a;
            ui_dbg_add_breakpoint(&g_dbg, a);
        }
    }

    /* ui_dbg's own toolbar (Continue/Over/Into/Tick/Break) and hotkeys
     * (F5/F10/F11/F8/F6) record their action in ui_dbg's state; translate it into
     * dbg.c after the draw. dbg_step is a no-op unless stopped, so a stray step is
     * harmless. */
    bool ui_was_stopped = ui_dbg_stopped(&g_dbg);
    ui_dbg_draw(&g_dbg);
    if (g_dbg.dbg.step_mode == UI_DBG_STEPMODE_OVER)
        dbg_step(DBG_STEP_LINE_OVER);
    else if (g_dbg.dbg.step_mode != UI_DBG_STEPMODE_NONE) /* INTO / TICK -> one instruction */
        dbg_step(DBG_STEP_INSTR);
    else if (ui_was_stopped && !ui_dbg_stopped(&g_dbg))
        dbg_continue(); /* Continue button / F5 */
    else if (!ui_was_stopped && ui_dbg_stopped(&g_dbg))
        dbg_request_pause();                    /* Break button / F6 */
    g_dbg.dbg.step_mode = UI_DBG_STEPMODE_NONE; /* consumed; ui_dbg's own stepper stays idle */

    /* Breakpoint edits the user just made in ui_dbg -> dbg.c: a removed gutter dot
     * was in 'before' but is gone now; an added one is an exec bp dbg.c still lacks.
     * dbg.c is unchanged since the mirror above (only ui_dbg's list moved), so the
     * deltas are exactly the user's edits (Debug Control / DAP edits already landed
     * in dbg.c and were mirrored in). */
    for (int i = 0; i < n_before; i++)
        if (!ui_has_exec_bp(before[i]))
            dbg_remove_breakpoint(before[i]);
    for (int i = 0; i < g_dbg.dbg.num_breakpoints; i++)
    {
        const ui_dbg_breakpoint_t *bp = &g_dbg.dbg.breakpoints[i];
        if (bp->type == UI_DBG_BREAKTYPE_EXEC && !dbg_has_breakpoint(bp->addr))
            dbg_add_breakpoint(bp->addr);
    }

    ui_w65c02_draw(&g_cpuwin);
    ui_m6522_draw(&g_viawin);
    /* The hex editor shares one max_addr across all layers; scope the XSTACK
     * layer (2) to its 512 bytes so it doesn't show 64KB of 6502 space. CurLayer
     * was chosen last frame and also keys the read/write callbacks, so the
     * window size and the layer's data switch together (no mismatched frame). */
    g_memedit.max_addr = (g_memedit.ed->CurLayer == 2) ? XSTACK_SIZE : 0x10000;
    ui_memedit_draw(&g_memedit);
    ui_memmap_draw(&g_memmap);
    ui_dasm_draw(&g_dasm);
    ui_audio_draw(&g_audio, emu_audio_viz_pos());
}

void dbgui_render(void) { simgui_render(); }

/* Per-cycle view update (registered as emu_dbg_cycle_cb; called from cpu.c). This
 * is the chips-native way to keep the disassembly view honest: ui_dbg_tick records
 * the execution heatmap (which the disassembler back-scans to find instruction
 * boundaries), the history, and cur_op_pc. It also runs ui_dbg's OWN breakpoint
 * engine — the only evaluator of the non-EXEC types (Byte/Word at each opcode
 * fetch, IRQ/NMI on a rising pin edge). A trap files a break request that dbg.c
 * honors at the next M6502_SYNC, so dbg.c stays the one stop authority; an
 * op-level trap lands on the very instruction that tripped it because this runs
 * before cpu.c's dbg_at_instruction on the same cycle. We never set ui_dbg's
 * step_mode, so it never self-steps. */
void dbgui_tick(uint64_t pins)
{
    if (!g_inited)
        return;
    ui_dbg_tick(&g_dbg, pins);
    if (g_dbg.dbg.last_trap_id >= UI_DBG_BP_BASE_TRAPID)
        dbg_request_break();
}

/* Height (in ImGui points) of the main menu bar this overlay draws across the
 * top of the window, captured during dbgui_draw. The window layer reserves this
 * strip (scaled to framebuffer pixels by the DPI factor) so the menu no longer
 * covers the emulated canvas. 0 until the first dbgui_draw runs. */
float dbgui_menu_height(void) { return g_inited ? g_menu_h : 0.0f; }

/* Pre-first-frame estimate of the menu-bar height (ImGui points), so the window
 * can open tall enough for the bar before any frame has measured it. simgui's
 * default font bakes at 13 px; the bar is FontSize + FramePadding.y*2 (3*2) = 19,
 * measured to the pixel on the actual UI. dbgui_menu_height takes over once the
 * bar is drawn. */
float dbgui_menu_bar_estimate(void) { return 19.0f; }

bool dbgui_handle_event(const void *evp)
{
    const sapp_event *e = (const sapp_event *)evp;
    simgui_handle_event(e);
    ImGuiIO &io = ImGui::GetIO();
    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
    case SAPP_EVENTTYPE_KEY_UP:
    case SAPP_EVENTTYPE_CHAR:
        return io.WantCaptureKeyboard;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP:
    case SAPP_EVENTTYPE_MOUSE_MOVE:
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        return io.WantCaptureMouse;
    default:
        return false;
    }
}
