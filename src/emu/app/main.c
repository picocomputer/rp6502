/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "api/oem.h"
#include "emu/api/pro.h"
#include "emu/host/window.h"
#include "emu/plat.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/app/png.h"
#include "emu/app/rand.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/api/tmpfs.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/cpu.h"
#include "emu/main.h"
#include "emu/sys/vga.h"
#include "emu/app/cli.h"
#include "emu/app/credits.h"
#include "sys/com.h"
#include <stdio.h>
#include <string.h>
#ifdef EMU_WITH_DEBUGGER
#include "emu/dbg/dap.h"
#include "emu/dbg/dbgui.h" /* dbgui_set_config_file (--ini) */
#endif

static uint32_t g_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

/* Queue scripted keystrokes onto the keyboard input stream, converting the
 * host-encoding option text to OEM. Newlines become carriage returns so the
 * line editor treats them as Enter. Bytes wait in the com ring until the
 * program's first stdin read drains them. */
static void queue_input(const char *text)
{
    char oem[1024]; /* the 512-byte kbd ring caps what can queue anyway */
    if (!os_argv_to_oem(text, oem, sizeof oem))
    {
        fprintf(stderr, "rp6502-emu: --input too long\n");
        return;
    }
    for (const char *p = oem; *p; p++)
        com_kbd_push_byte((uint8_t)(*p == '\n' ? '\r' : *p));
}

/* Fold the launch ROM's "emulator" asset (if any) into o at lower priority than
 * the command line: parse the asset first, then re-apply argv over it. */
static void merge_rom_args(cli_options *o, int argc, char **argv)
{
    char asset[2048];
    long n = rom_read_asset("emulator", asset, sizeof asset - 1);
    if (n < 0)
        return; /* no such asset: command line stands alone */
    asset[n] = 0;

    static char store[2048];
    static char *rom_argv[65];
    rom_argv[0] = (char *)"emulator"; /* getopt skips argv[0]; give it a name */
    int rom_argc = cli_tokenize_args(asset, rom_argv + 1, 64, store, sizeof store) + 1;

    cli_options merged;
    cli_options_init(&merged);
    if (cli_parse_args(rom_argc, rom_argv, &merged))
        fprintf(stderr, "rp6502-emu: ignoring the rest of the ROM 'emulator' options after a bad token\n");
    cli_parse_args(argc, argv, &merged); /* the command line wins within allowed fields */

    /* Allowlist: a ROM's "emulator" asset may preset only presentation/timing
     * options — never anything that touches the host filesystem, injects input,
     * or controls the session (rom/shot/input/frames/tmpdrive/inidir/installs/
     * debug/dap/rom_args stay command-line only). Take the allowed fields from
     * the merged parse (the command line already overrode the asset there) and
     * leave every other field of o at its command-line value. */
    if (merged.have_bg)
    {
        o->have_bg = true;
        o->bg_r = merged.bg_r;
        o->bg_g = merged.bg_g;
        o->bg_b = merged.bg_b;
    }
    if (merged.have_scale)
    {
        o->have_scale = true;
        o->scale = merged.scale;
    }
    o->vsync = merged.vsync;
    o->scale_filter = merged.scale_filter;
    if (merged.phi2_khz > 0)
        o->phi2_khz = merged.phi2_khz;
    if (merged.code_page > 0)
        o->code_page = merged.code_page;
    o->mute = merged.mute;
    if (merged.have_seed)
    {
        o->have_seed = true;
        o->seed = merged.seed;
    }
}

/* Apply the presentation/timing options shared by both launch paths. False (with
 * a message) on an out-of-range --phi2 or an unsupported --code-page. */
static bool apply_options(const cli_options *o)
{
    if (o->have_bg)
        window_set_bgcolor((uint8_t)o->bg_r, (uint8_t)o->bg_g, (uint8_t)o->bg_b);
    window_set_scale_filter(o->scale_filter);
    if (o->have_seed)
        rand_set_seed((uint64_t)o->seed);
    if (o->mute)
        aud_set_enabled(false);
    if (o->phi2_khz > 0)
    {
        if (o->phi2_khz < CPU_PHI2_MIN_KHZ || o->phi2_khz > CPU_PHI2_MAX_KHZ)
        {
            fprintf(stderr, "rp6502-emu: --phi2 %d out of range (%d-%d)\n",
                    o->phi2_khz, CPU_PHI2_MIN_KHZ, CPU_PHI2_MAX_KHZ);
            return false;
        }
        cpu_set_phi2_khz_run((uint16_t)o->phi2_khz);
    }
    if (o->code_page > 0)
    {
        if (o->code_page > UINT16_MAX || !oem_set_code_page((uint16_t)o->code_page))
        {
            fprintf(stderr, "rp6502-emu: unsupported code page %d\n", o->code_page);
            return false;
        }
    }
    return true;
}

#ifdef EMU_WITH_DEBUGGER
/* DAP mode (--dap): the program is delivered by the VS Code launch request, not
 * the command line. Boot the machine held (CPU stopped, no program) and serve
 * DAP on stdio; the launch handler loads + runs the ROM. The window still opens
 * (with the debugger overlay) so the program is visible while VS Code drives. */
static int run_dap(const cli_options *o)
{
    main_init();
    cpu_set_halted(true); /* hold until the DAP launch loads a program */
    dbg_set_active(true);

    if (!apply_options(o))
        return 1;

    if (o->rom_args)
        dap_set_default_args(o->n_rom_args, o->rom_args);
    dap_start(); /* DAP on stdin/stdout; window_run pumps it each frame */
    /* The debug session lifecycle is DAP-driven (StoppedEvent/TerminatedEvent on
     * exit, the window closes on Disconnect), so the window is held (never
     * auto-closed) — the final screen stays up until the client disconnects. */
    return window_run(g_fb, o->scale, o->have_scale, o->vsync, false);
}
#endif

int main(int argc, char **argv)
{
    cli_options o;
    cli_options_init(&o);
    if (cli_parse_args(argc, argv, &o))
    {
        cli_usage(argv[0]);
        return 2;
    }

    /* --credits: print third-party notices and exit (no ROM needed). On the web
     * the shell maps ?credits to this, and the output appears in the console. */
    if (o.credits)
    {
        fputs(EMU_CREDITS, stdout);
        return 0;
    }

#ifdef EMU_WITH_DEBUGGER
    /* Config file the debugger persists its window layout into (an [EMU] section;
     * other sections are preserved). The launcher passes the workstation file,
     * e.g. ${workspaceFolder}/.rp6502; else the debug UI uses the OS config dir. */
    if (o.inidir)
        dbgui_set_config_file(o.inidir);
#endif

    /* MSC0: is the native host filesystem — whatever the process cwd is.
     * --tmpdrive instead runs the ROM against a fresh throwaway RAM FatFs
     * (isolation). This locates the drive, so it comes from the command line,
     * not the ROM's asset args. */
    if (o.tmpdrive && !tmpfs_mount())
    {
        fprintf(stderr, "rp6502-emu: cannot create --tmpdrive\n");
        return 1;
    }

    /* Seed the OEM code page before anything converts guest-bound argv or
     * opens a ROM (conversion is per-page; unseeded, every non-ASCII char
     * flattens to 0x7F). --cp applies best-effort now so paths convert in the
     * page the guest will run; apply_options validates it after main_init
     * re-seeds these same defaults (all idempotent here). */
    oem_locale_changed(437);
    oem_set_code_page(0);
    if (o.code_page > 0 && o.code_page <= UINT16_MAX)
        oem_set_code_page((uint16_t)o.code_page);

    /* Install ROMs before the boot load / any exec can resolve them. Paths and
     * ROM args are guest-bound, so they convert from host argv encoding to OEM
     * here at the entry; --shot/--ini stay host-domain untouched. */
    for (int i = 0; i < o.n_installs; i++)
    {
        char oem[4096];
        if (!os_argv_to_oem(o.installs[i], oem, sizeof oem) || !install_rom(oem))
        {
            fprintf(stderr, "rp6502-emu: cannot install --rom '%s'\n", o.installs[i]);
            return 1;
        }
    }

    static char args_store[2048];
    static char *args_oem[64];
    if (o.rom_args)
    {
        size_t used = 0;
        if (o.n_rom_args > (int)(sizeof args_oem / sizeof *args_oem))
        {
            fprintf(stderr, "rp6502-emu: ROM argv overflow\n");
            return 1;
        }
        for (int i = 0; i < o.n_rom_args; i++)
        {
            if (!os_argv_to_oem(o.rom_args[i], args_store + used, sizeof args_store - used))
            {
                fprintf(stderr, "rp6502-emu: ROM argv overflow\n");
                return 1;
            }
            args_oem[i] = args_store + used;
            used += strlen(args_oem[i]) + 1;
        }
        o.rom_args = args_oem;
    }

#ifdef EMU_WITH_DEBUGGER
    if (o.dap) /* the program comes from the DAP launch request, not argv */
        return run_dap(&o);
#else
    if (o.dap)
    {
        fprintf(stderr, "rp6502-emu: built without debugger/DAP support\n");
        return 1;
    }
#endif

    /* No positional ROM but installs given: boot the first installed ROM (:name). */
    char bootbuf[256];
    char rom_oem[4096];
    const char *rom = NULL;
    if (o.rom)
    {
        if (!os_argv_to_oem(o.rom, rom_oem, sizeof rom_oem))
        {
            fprintf(stderr, "rp6502-emu: ROM path too long\n");
            return 1;
        }
        rom = rom_oem;
    }
    else if (o.n_installs > 0)
    {
        char inst[4096];
        if (!os_argv_to_oem(o.installs[0], inst, sizeof inst))
        {
            fprintf(stderr, "rp6502-emu: ROM path too long\n");
            return 1;
        }
        snprintf(bootbuf, sizeof(bootbuf), ":%s", cli_base_name(inst));
        rom = bootbuf;
    }

    if (!rom)
    {
        cli_usage(argv[0]);
        return 2;
    }

    if (!rom_load(rom))
        return 1;

    /* The ROM is loaded; fold its "emulator" asset args under the command line. */
    merge_rom_args(&o, argc, argv);

    main_init();
    vga_set_framebuffer(g_fb); /* the app owns the pixels; vga renders into them */

    if (!pro_set_argv(rom, o.n_rom_args, o.rom_args))
    {
        fprintf(stderr, "rp6502-emu: ROM argv overflow\n");
        return 1;
    }

    if (!apply_options(&o))
        return 1;

    /* Enable the debugger engine (the on-screen UI and the DAP adapter both
     * attach to it). Inert with no breakpoints, but --dap will also stand up the
     * stdio DAP server. */
    if (o.dap || o.debug)
        dbg_set_active(true);

    if (o.input)
        queue_input(o.input);

    if (o.shot)
    {
        int frames = o.frames < 1 ? 1 : o.frames;
        /* Only the final frame is captured, so settle the earlier ones without
         * the per-scanline pixel work (most of the per-frame cost); render the
         * last one and snapshot it. */
        for (int i = 0; i < frames - 1; i++)
            main_run_frame_norender();
        main_run_frame(); /* renders into g_fb (registered above) */
        int cw, ch;
        vga_canvas_size(&cw, &ch); /* PNG is the canvas's native resolution */
        if (!png_write(o.shot, cw, ch, g_fb))
            return 1;
        printf("rp6502-emu: wrote %s (%d frames; cpu %s, exit code %d)\n",
               o.shot, frames, cpu_halted() ? "halted" : "running", main_exit_code());
        return 0;
    }

    return window_run(g_fb, o.scale, o.have_scale, o.vsync, !o.debug);
}
