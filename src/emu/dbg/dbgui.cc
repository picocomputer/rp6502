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
 * this overlay stay consistent. We do NOT run ui_dbg's own stepping engine
 * (ui_dbg_tick) — dbg.c gates the CPU and we reflect its stop-state into ui_dbg
 * for display.
 */

#include "imgui.h"
#include "imgui_internal.h" /* chips ui_*.h reach a few internal ImGui APIs */

extern "C"
{
#include "emu/dbg/dbg.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "emu/sys/via.h"
}
#include "emu/dbg/dbgui.h"        /* the C-callable entry points this TU defines */
#include "emu/dbg/dbgui_layout.h" /* [EMU]-section layout persistence (file/INI side) */

#include "emu/sys/w65c02.h" /* m6502_t (type + macros; CHIPS_IMPL is in w65c02.c) */
#include "m6522.h"  /* m6522_t (type; CHIPS_IMPL is in via.c) */
#include "emu/sys/ria.h"    /* ria_t (the RIA chip instance, via ria_chip()) */

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
#define CHIPS_UTIL_IMPL          /* emit m6502dasm_op (the disassembler ui_dbg calls) */
#include "emu/dbg/w65c02dasm.h"  /* 65C02 fork of chips/util/m6502dasm.h (CMOS opcodes) */
#include "emu/dbg/ui_w65c02.h"   /* our fork of ui/ui_m6502.h: no 6510 I/O-port panel */
#include "ui/ui_m6522.h"
#include "ui/ui_dbg.h"

#include <cstdio>  /* snprintf, sscanf */
#include <cstring> /* strchr (control/ria windows) */

static ui_dbg_t g_dbg;
static ui_w65c02_t g_cpuwin;
static ui_m6522_t g_viawin;
static ui_memedit_t g_memedit;
static ui_memmap_t g_memmap;
static ui_chip_t g_riachip; /* pin diagram for the native "RIA" window */
static bool g_inited;
static bool g_control_open = false; /* the native "Debug Control" window */
static bool g_riawin_open = false;  /* the native "RIA" window */
static float g_menu_h;             /* main-menu-bar height in ImGui points (see dbgui_menu_height) */

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
 * The RIA register window ($FFE0-$FFF9) is backed by regs[], NOT ram[]: it holds
 * the live registers AND the API trampoline opcodes the 6502 actually executes
 * (the self-modifying BRA-spin at $FFF1, plus the LDX/LDA/RTS return patch). So
 * the disassembler must read regs[] there or it shows stale RAM (zeros = BRK)
 * over real code. We read the shadow directly rather than ria_reg_read(), whose
 * reads have side effects (pop the xstack, ack IRQ flags, advance the RW
 * pointers) — a debugger view must never perturb the machine. (The VIA window
 * $FFD0-$FFDF lives inside the m6522 chip with no no-side-effect peek; it reads
 * back as RAM, which is fine — it carries no code.) */
static uint8_t mem_peek(uint16_t addr)
{
    if (addr >= RIA_WINDOW_LO && addr <= RIA_WINDOW_HI)
        return REGS(addr);
    return ram[addr];
}

static void mem_poke(uint16_t addr, uint8_t data)
{
    if (addr >= RIA_WINDOW_LO && addr <= RIA_WINDOW_HI)
        REGS(addr) = data; /* raw poke of the shadow; do NOT trigger a syscall */
    else
        ram[addr] = data;
}

/* ui_dbg memory read callback (the disassembler + heatmap). */
static uint8_t mem_read(int layer, uint16_t addr, void *user)
{
    (void)layer;
    (void)user;
    return mem_peek(addr);
}

/* ui_memedit callbacks. Layer 0 is the 6502 space ($0000-$FFFF, RIA-aware via
 * mem_peek/poke); layer 1 is XRAM, which the system maps at $10000-$1FFFF (its
 * own 64KB bank — the 6502 reaches it only through the RIA's RW0/RW1 windows);
 * layer 2 is the 512-byte RIA xstack ($000-$1FF, a top-down LIFO). dbgui_draw
 * scopes the window's max_addr to 512 while that layer is selected, so the editor
 * only addresses $000-$1FF there; the bound below is a defensive guard. */
static uint8_t memedit_read(int layer, uint16_t addr, void *user)
{
    (void)user;
    switch (layer)
    {
    case 1: return xram[addr];
    case 2: return addr < XSTACK_SIZE ? xstack[addr] : 0;
    default: return mem_peek(addr);
    }
}

static void memedit_write(int layer, uint16_t addr, uint8_t data, void *user)
{
    (void)user;
    switch (layer)
    {
    case 1: xram[addr] = data; break;
    case 2: if (addr < XSTACK_SIZE) xstack[addr] = data; break;
    default: mem_poke(addr, data); break;
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
        for (int addr = 0; addr <= 0xFFFF; addr++)
            if (dbg_has_breakpoint((uint16_t)addr))
            {
                ImGui::SameLine();
                ImGui::Text("$%04X", addr);
            }
    }
    ImGui::End();
}

/* Pin diagrams for the chip windows. ui_chip requires a named desc with pins. */
static const ui_chip_pin_t pins_6502[] = {
    {"D0", 0, M6502_D0}, {"D1", 1, M6502_D1}, {"D2", 2, M6502_D2}, {"D3", 3, M6502_D3},
    {"D4", 4, M6502_D4}, {"D5", 5, M6502_D5}, {"D6", 6, M6502_D6}, {"D7", 7, M6502_D7},
    {"RW", 9, M6502_RW}, {"SYNC", 10, M6502_SYNC}, {"RDY", 11, M6502_RDY},
    {"IRQ", 12, M6502_IRQ}, {"NMI", 13, M6502_NMI}, {"RES", 14, M6502_RES},
    {"A0", 16, M6502_A0}, {"A1", 17, M6502_A1}, {"A2", 18, M6502_A2}, {"A3", 19, M6502_A3},
    {"A4", 20, M6502_A4}, {"A5", 21, M6502_A5}, {"A6", 22, M6502_A6}, {"A7", 23, M6502_A7},
    {"A8", 24, M6502_A8}, {"A9", 25, M6502_A9}, {"A10", 26, M6502_A10}, {"A11", 27, M6502_A11},
    {"A12", 28, M6502_A12}, {"A13", 29, M6502_A13}, {"A14", 30, M6502_A14}, {"A15", 31, M6502_A15},
};

static const ui_chip_pin_t pins_6522[] = {
    {"D0", 0, M6522_D0}, {"D1", 1, M6522_D1}, {"D2", 2, M6522_D2}, {"D3", 3, M6522_D3},
    {"D4", 4, M6522_D4}, {"D5", 5, M6522_D5}, {"D6", 6, M6522_D6}, {"D7", 7, M6522_D7},
    {"RS0", 9, M6522_RS0}, {"RS1", 10, M6522_RS1}, {"RS2", 11, M6522_RS2}, {"RS3", 12, M6522_RS3},
    {"RW", 14, M6522_RW}, {"CS1", 15, M6522_CS1}, {"CS2", 16, M6522_CS2}, {"IRQ", 17, M6522_IRQ},
    {"PA0", 20, M6522_PA0}, {"PA1", 21, M6522_PA1}, {"PA2", 22, M6522_PA2}, {"PA3", 23, M6522_PA3},
    {"PA4", 24, M6522_PA4}, {"PA5", 25, M6522_PA5}, {"PA6", 26, M6522_PA6}, {"PA7", 27, M6522_PA7},
    {"PB0", 28, M6522_PB0}, {"PB1", 29, M6522_PB1}, {"PB2", 30, M6522_PB2}, {"PB3", 31, M6522_PB3},
    {"PB4", 32, M6522_PB4}, {"PB5", 33, M6522_PB5}, {"PB6", 34, M6522_PB6}, {"PB7", 35, M6522_PB7},
};

/* The RIA is not a chips-modeled chip; it shares the 6502 bus, so its "pins" are
 * that bus as the RIA decodes it (fed live from ria_chip()->PINS), plus a synthetic CS
 * lit while the RIA window is addressed. RIA_PIN_CS rides a free bit above
 * M6502_PIN_MASK (bit 40), so it never collides with a real CPU pin. */
#define RIA_PIN_CS (1ULL << 40)
static const ui_chip_pin_t pins_ria[] = {
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
 * little-endian pair. The vectors $FFFA-$FFFF live in RAM and the rest in regs[];
 * mem_peek routes each to the right backing store. */
static const struct { uint16_t addr; const char *name; uint8_t width; } RIA_REGS[] = {
    {0xFFE0, "READY", 1}, {0xFFE1, "TX", 1}, {0xFFE2, "RX", 1}, {0xFFE3, "VSYNC", 1},
    {0xFFE4, "RW0", 1}, {0xFFE5, "STEP0", 1}, {0xFFE6, "ADDR0", 2},
    {0xFFE8, "RW1", 1}, {0xFFE9, "STEP1", 1}, {0xFFEA, "ADDR1", 2},
    {0xFFEC, "XSTACK", 1}, {0xFFED, "ERRNO", 2}, {0xFFEF, "OP", 1},
    {0xFFF0, "IRQ", 1}, {0xFFF1, "SPIN", 1}, {0xFFF2, "BUSY", 1}, {0xFFF3, "LDA", 1},
    {0xFFF4, "A", 1}, {0xFFF5, "LDX", 1}, {0xFFF6, "X", 1}, {0xFFF7, "RTS", 1},
    {0xFFF8, "SREG", 2}, {0xFFFA, "NMIB", 2}, {0xFFFC, "RESB", 2}, {0xFFFE, "IRQB", 2},
};

/* The native RIA window: the bus-pin diagram + the register window. A read-only
 * inspector (edit registers via the Memory window if needed; the xstack lives
 * there too, as the Memory window's third layer). */
static void draw_ria(void)
{
    if (!g_riawin_open)
        return;
    ImGui::SetNextWindowPos(ImVec2(860, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RIA", &g_riawin_open))
    {
        /* Pins: the bus as the RIA chip last decoded it (ria_chip()->PINS), CS lit
         * when the RIA window is the addressed device. */
        uint64_t p = ((const ria_t *)ria_chip())->PINS;
        uint16_t a = (uint16_t)(p & 0xFFFFu);
        if (a >= RIA_WINDOW_LO && a <= RIA_WINDOW_HI)
            p |= RIA_PIN_CS;
        ImGui::BeginChild("##ria_pins", ImVec2(176, 0), true);
        ui_chip_draw(&g_riachip, p);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##ria_state", ImVec2(0, 0), true);

        /* The xstack pointer — an internal latch the memory-mapped register file
         * doesn't carry (empty when SP == XSTACK_SIZE; live bytes are [SP,$1FF]). */
        ImGui::Text("XSTACK SP $%03X", (unsigned)xstack_ptr);
        ImGui::Spacing();

        /* Registers. */
        ImGui::TextUnformatted("Registers");
        ImGui::Separator();
        ImGui::BeginChild("##ria_regs", ImVec2(0, 0), false);
        if (ImGui::BeginTable("##regs", 3,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit))
        {
            for (auto &r : RIA_REGS)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("$%04X", r.addr);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name);
                ImGui::TableNextColumn();
                if (r.width == 2)
                    ImGui::Text("$%04X", (unsigned)(mem_peek(r.addr) |
                                                    (mem_peek((uint16_t)(r.addr + 1)) << 8)));
                else
                {
                    uint8_t v = mem_peek(r.addr);
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

/* ---- Window manager: the menu bar to toggle the overlays, plus the window
 * registry that drives layout persistence. The config-file read/write lives in
 * dbgui_layout.cc; here we own only the registry (key, title, open flag), since
 * some open flags live inside the chips UI structs. ---- */

/* The persistable windows: [EMU] key, ImGui window title (must match the title
 * each window draws under), open flag. */
#define DBGUI_WIN_COUNT 7
static const char *const DBGUI_WIN_KEY[DBGUI_WIN_COUNT] = {
    "control", "disasm", "cpu", "via", "mem", "memmap", "ria"};
static const char *const DBGUI_WIN_TITLE[DBGUI_WIN_COUNT] = {
    "Debug Control", "Disassembler", "MOS 6502", "MOS 6522 (VIA)", "Memory", "Memory Map", "RIA"};
static bool *dbgui_win_open(int i)
{
    switch (i)
    {
    case 0: return &g_control_open;
    case 1: return &g_dbg.ui.open;
    case 2: return &g_cpuwin.open;
    case 3: return &g_viawin.open;
    case 4: return &g_memedit.open;
    case 5: return &g_memmap.open;
    case 6: return &g_riawin_open;
    }
    return nullptr;
}
/* Build the {key, title, open-flag} table the layout persistence acts on. The
 * open flags live in this TU (some inside chips UI structs), so we hand the
 * layout module pointers to them; dbgui_layout.cc does the file/INI work. */
static int dbgui_win_table(dbgui_win_t *out)
{
    for (int i = 0; i < DBGUI_WIN_COUNT; i++)
    {
        out[i].key = DBGUI_WIN_KEY[i];
        out[i].title = DBGUI_WIN_TITLE[i];
        out[i].open = dbgui_win_open(i);
    }
    return DBGUI_WIN_COUNT;
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
        if (ImGui::BeginMenu("Overlays"))
        {
            ImGui::MenuItem("Debug Control", nullptr, &g_control_open);
            ImGui::MenuItem("Disassembler", nullptr, &g_dbg.ui.open);
            ImGui::MenuItem("MOS 6502 (CPU)", nullptr, &g_cpuwin.open);
            ImGui::MenuItem("MOS 6522 (VIA)", nullptr, &g_viawin.open);
            ImGui::MenuItem("RIA", nullptr, &g_riawin_open);
            ImGui::Separator();
            ImGui::MenuItem("Memory", nullptr, &g_memedit.open);
            ImGui::MenuItem("Memory Map", nullptr, &g_memmap.open);
            ImGui::EndMenu();
        }
        /* Emulated VGA rate, right-aligned (e.g. "59.9 FPS"; ~60 when keeping up). */
        char fps[24];
        std::snprintf(fps, sizeof fps, "%.1f FPS", dbgui_vga_fps());
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fps).x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextUnformatted(fps);
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
    /* We persist layout ourselves into the [EMU] section of the config file (see
     * dbgui_load/save_window_state), so disable ImGui's own ini writer -- it would
     * clobber any foreign sections (e.g. [RP6502]) in that file. */
    ImGui::GetIO().IniFilename = nullptr;

    m6502_t *cpu = (m6502_t *)sys_cpu();
    m6522_t *via = (m6522_t *)via_chip();

    uint32_t freq = (uint32_t)emu_get_phi2_khz() * 1000u;
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
    cd.title = "MOS 6502";
    cd.cpu = cpu;
    cd.x = 10;
    cd.y = 360;
    cd.open = false;
    UI_CHIP_INIT_DESC(&cd.chip_desc, "6502", 32, pins_6502);
    ui_w65c02_init(&g_cpuwin, &cd);

    ui_m6522_desc_t vd{};
    vd.title = "MOS 6522 (VIA)";
    vd.via = via;
    vd.x = 420;
    vd.y = 360;
    vd.open = false;
    UI_CHIP_INIT_DESC(&vd.chip_desc, "6522", 40, pins_6522);
    ui_m6522_init(&g_viawin, &vd);

    /* The RIA pin diagram (a standalone ui_chip; the native draw_ria window owns
     * the registers + xstack). Fed the live bus pins from ria_chip()->PINS. */
    ui_chip_desc_t rcd{};
    UI_CHIP_INIT_DESC(&rcd, "RIA", 32, pins_ria);
    ui_chip_init(&g_riachip, &rcd);

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

    /* Memory map: the 6502 address-space layout + a "Segments" layer fed from the
     * loaded linker output. dbgui_build_memmap fills it; dbgui_draw rebuilds it
     * when the DAP launch pushes a new segment set. */
    ui_memmap_desc_t mmd{};
    mmd.title = "Memory Map";
    mmd.x = 430;
    mmd.y = 250;
    mmd.open = false;
    ui_memmap_init(&g_memmap, &mmd);
    dbgui_build_memmap();

    /* Restore which windows were open last session (positions come from the ini). */
    dbgui_win_t wins[DBGUI_WIN_COUNT];
    dbgui_layout_load(wins, dbgui_win_table(wins));
    g_inited = true;

    /* Drive ui_dbg's view from sys.c's tick loop (heatmap/history/PC). */
    emu_dbg_cycle_cb = dbgui_tick;
}

void dbgui_discard(void)
{
    if (!g_inited)
        return;
    emu_dbg_cycle_cb = nullptr; /* stop feeding ui_dbg before it is destroyed */
    dbgui_win_t wins[DBGUI_WIN_COUNT];
    dbgui_layout_save(wins, dbgui_win_table(wins)); /* remember open windows */
    ui_memmap_discard(&g_memmap);
    ui_memedit_discard(&g_memedit);
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

void dbgui_draw(void)
{
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

    /* ui_dbg_tick (driven from sys.c every cycle) maintains the PC highlight while
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
    draw_control();
    draw_ria();

    /* dbg.c is the one authoritative engine + breakpoint store (shared with the
     * DAP adapter); ui_dbg's gutter/toolbar/hotkeys are a front-end bridged to it.
     * Mirror dbg.c's breakpoints into ui_dbg's list so the disassembly gutter shows
     * them (and remember the set), let ui_dbg draw — during which the user may
     * toggle a gutter dot, press F9, or click a toolbar button — then push the
     * resulting deltas back to dbg.c after the draw. ui_dbg's list is fixed at
     * UI_DBG_MAX_BREAKPOINTS; mirror only the breakpoints in the displayed
     * disassembly window (line_array) so every visible dot shows even when dbg.c
     * holds more than that across the 64K space. */
    uint16_t before[UI_DBG_MAX_BREAKPOINTS];
    int n_before = 0;
    g_dbg.dbg.num_breakpoints = 0; /* rebuild ui_dbg's list from dbg.c */
    for (int li = 0; li < UI_DBG_NUM_LINES && n_before < UI_DBG_MAX_BREAKPOINTS; li++)
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
        dbg_request_pause(); /* Break button / F6 */
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
}

void dbgui_render(void) { simgui_render(); }

/* Per-cycle view update (registered as emu_dbg_cycle_cb; called from sys.c). This
 * is the chips-native way to keep the disassembly view honest: ui_dbg_tick records
 * the execution heatmap (which the disassembler back-scans to find instruction
 * boundaries), the history, and cur_op_pc. It also runs ui_dbg's OWN breakpoint
 * engine, but that only flips ui_dbg's display flag — dbg.c gates the real CPU and
 * its breakpoints are mirrored in (dbgui_draw), so the two agree. We never set
 * ui_dbg's step_mode, so it never self-steps. */
void dbgui_tick(uint64_t pins)
{
    if (g_inited)
        ui_dbg_tick(&g_dbg, pins);
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
