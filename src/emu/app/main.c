/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Entry point. A ROM is mandatory (the emulator has nothing to run
 * otherwise). Default mode opens a window; --screenshot renders headlessly
 * to a PNG, which is how the build verifies output without a display.
 *
 * The launch ROM may carry an "emulator" asset whose contents are parsed as
 * options (the usual --flag --opt=value quoted form). Those apply first and the
 * real command line overrides them, so a ROM can ship its preferred defaults.
 * The CLI parser itself lives in cli.c.
 */

#include "emu/api/oem.h"
#include "emu/api/pro.h"
#include "emu/app/window.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/host/png.h"
#include "emu/host/rand.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/host/fs.h"
#include "emu/usb/msc.h"
#include "emu/sys/mem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/cpu.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "emu/app/cli.h"
#include "emu/app/credits.h"
#include "sys/com.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef EMU_WITH_DEBUGGER
#include "emu/dbg/dap.h"
#include "emu/dbg/dbgui.h" /* dbgui_set_config_file (--ini) */
#endif

static uint32_t g_fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];

/* Queue scripted keystrokes onto the keyboard input stream. Newlines become
 * carriage returns so the line editor treats them as Enter. Bytes wait in the
 * com ring until the program's first stdin read drains them. */
static void queue_input(const char *text)
{
    for (const char *p = text; *p; p++)
        com_kbd_push_byte((uint8_t)(*p == '\n' ? '\r' : *p));
}

/* Fold the launch ROM's "emulator" asset (if any) into o at lower priority than
 * the command line: parse the asset first, then re-apply argv over it. */
static void merge_rom_args(options *o, int argc, char **argv)
{
    char asset[2048];
    long n = rom_read_asset("emulator", asset, sizeof asset - 1);
    if (n < 0)
        return; /* no such asset: command line stands alone */
    asset[n] = 0;

    static char store[2048];
    static char *rom_argv[65];
    rom_argv[0] = (char *)"emulator"; /* getopt skips argv[0]; give it a name */
    int rom_argc = tokenize_args(asset, rom_argv + 1, 64, store, sizeof store) + 1;

    options merged;
    options_init(&merged);
    if (parse_args(rom_argc, rom_argv, &merged))
        fprintf(stderr, "rp6502-emu: ignoring the rest of the ROM 'emulator' args after a bad token\n");
    parse_args(argc, argv, &merged); /* the command line wins */

    /* The drive/rom selection was already acted on from the command-line pass;
     * keep it and take everything else from the merged result. */
    merged.rom = o->rom;
    merged.tmpdrive = o->tmpdrive;
    merged.inidir = o->inidir; /* a workstation/CLI concern, not a ROM preset */
    for (int i = 0; i < o->n_installs; i++)
        merged.installs[i] = o->installs[i];
    merged.n_installs = o->n_installs;
    *o = merged;
}

#ifdef EMU_WITH_DEBUGGER
/* DAP mode (--dap): the program is delivered by the VS Code launch request, not
 * the command line. Boot the machine held (CPU stopped, no program) and serve
 * DAP on stdio; the launch handler loads + runs the ROM. The window still opens
 * (with the debugger overlay) so the program is visible while VS Code drives. */
static int run_dap(const options *o)
{
    emu_init();
    emu_cpu_halted = true; /* hold until the DAP launch loads a program */
    dbg_set_active(true);

    if (o->have_bg)
        emu_set_bgcolor((uint8_t)o->bg_r, (uint8_t)o->bg_g, (uint8_t)o->bg_b);
    emu_set_scale_filter(o->scale_filter);
    if (o->have_seed)
        emu_set_random_seed((uint64_t)o->seed);
    if (o->mute)
        emu_set_audio_enabled(false);
    if (o->phi2_khz > 0)
        cpu_set_phi2_khz_run((uint16_t)o->phi2_khz);
    if (o->code_page > 0)
    {
        if (o->code_page > UINT16_MAX || !oem_set_code_page((uint16_t)o->code_page))
        {
            fprintf(stderr, "rp6502-emu: unsupported code page %d\n", o->code_page);
            return 1;
        }
    }

    dap_start(); /* DAP on stdin/stdout; emu_run_window pumps it each frame */
    /* The debug session lifecycle is DAP-driven (StoppedEvent/TerminatedEvent on
     * exit, the window closes on Disconnect), so the window is held (never
     * auto-closed) — the final screen stays up until the client disconnects. */
    return emu_run_window(o->scale, o->vsync, false);
}
#endif

int main(int argc, char **argv)
{
    options o;
    options_init(&o);
    if (parse_args(argc, argv, &o))
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
    if (o.tmpdrive && !emu_ramdrive_mount())
    {
        fprintf(stderr, "rp6502-emu: cannot create --tmpdrive\n");
        return 1;
    }

    /* Install ROMs before the boot load / any exec can resolve them. */
    for (int i = 0; i < o.n_installs; i++)
        if (!fs_install_rom(o.installs[i]))
        {
            fprintf(stderr, "rp6502-emu: cannot install --rom '%s'\n", o.installs[i]);
            return 1;
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
    const char *rom = o.rom;
    if (!rom && o.n_installs > 0)
    {
        snprintf(bootbuf, sizeof(bootbuf), ":%s", base_name(o.installs[0]));
        rom = bootbuf;
    }

    if (!rom)
    {
        cli_usage(argv[0]);
        return 2;
    }

    if (!emu_rom_load(rom))
        return 1;

    /* The ROM is loaded; fold its "emulator" asset args under the command line. */
    merge_rom_args(&o, argc, argv);

    emu_init();
    vga_set_framebuffer(g_fb); /* the app owns the pixels; vga renders into them */

    /* argv[0] is the program's own path (in MSC0: form) so it can re-exec
     * itself; later argv paths from EXEC are resolved the same way. A drive path
     * or an installed ":name" is already in 6502 form; a host path maps back. */
    if (fs_has_drive_prefix(rom) || rom[0] == ':')
        pro_set_argv0(rom);
    else
    {
        char abs[FS_HOST_MAX_PATH], msc[FS_HOST_MAX_PATH];
        if (realpath(rom, abs))
        {
            fs_host_to_msc(abs, msc, sizeof(msc));
            pro_set_argv0(msc);
        }
        else
            pro_set_argv0(rom);
    }

    if (o.have_bg)
        emu_set_bgcolor((uint8_t)o.bg_r, (uint8_t)o.bg_g, (uint8_t)o.bg_b);
    emu_set_scale_filter(o.scale_filter);

    if (o.have_seed) /* force a reproducible RNG stream (else host entropy) */
        emu_set_random_seed((uint64_t)o.seed);

    if (o.mute) /* no synth work, and the window opens no OS audio device */
        emu_set_audio_enabled(false);

    if (o.phi2_khz > 0) /* override the default PHI2 (emu_init reset it) */
        cpu_set_phi2_khz_run((uint16_t)o.phi2_khz);

    if (o.code_page > 0) /* override the default 437 (emu_init reset it) */
    {
        if (o.code_page > UINT16_MAX || !oem_set_code_page((uint16_t)o.code_page))
        {
            fprintf(stderr, "rp6502-emu: unsupported code page %d\n", o.code_page);
            return 1;
        }
    }

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
            emu_run_frame_norender();
        emu_run_frame(); /* renders into g_fb (registered above) */
        int cw, ch;
        emu_canvas_size(&cw, &ch); /* PNG is the canvas's native resolution */
        if (!emu_write_png(o.shot, cw, ch, g_fb))
            return 1;
        printf("rp6502-emu: wrote %s (%d frames; cpu %s, exit code %d)\n",
               o.shot, frames, emu_cpu_halted ? "halted" : "running", emu_exit_code);
        return 0;
    }

    return emu_run_window(o.scale, o.vsync, !o.debug);
}
