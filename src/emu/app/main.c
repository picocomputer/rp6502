/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "api/oem.h"
#include "str/str.h"
#include "emu/api/pro.h"
#include "emu/host/window.h"
#include "emu/plat.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/app/png.h"
#include "emu/app/rand.h"
#include "emu/host/rom.h"
#include "emu/host/tmp.h"
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

/* Apply the host/window presentation options shared by both launch paths. phi2/cp
 * are machine settings loaded as config before main_init, not here. */
static void apply_options(const cli_options *o)
{
    if (o->have_bg)
        window_set_bgcolor((uint8_t)o->bg_r, (uint8_t)o->bg_g, (uint8_t)o->bg_b);
    window_set_scale_filter(o->scale_filter);
    if (o->have_seed)
        rand_set_seed((uint64_t)o->seed);
    if (o->mute)
        aud_set_enabled(false);
}

#ifdef EMU_WITH_DEBUGGER
/* DAP mode (--dap): the program is delivered by the VS Code launch request, not
 * the command line. Boot the machine held (CPU stopped, no program) and serve
 * DAP on stdio; the launch handler loads + runs the ROM. The window still opens
 * (with the debugger overlay) so the program is visible while VS Code drives. */
static int run_dap(const cli_options *o)
{
    cpu_set_halted(true); /* the machine is initialized; hold it (no program yet)
                           * until the DAP launch loads + runs one via pro_exec */
    dbg_set_active(true);

    apply_options(o);

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
     * (isolation). */
    if (o.tmpdrive && !tmp_mount())
    {
        fprintf(stderr, "rp6502-emu: cannot create --tmpdrive\n");
        return 1;
    }

    /* Load the command-line settings as config, then init the machine ONCE —
     * mirroring the firmware's cfg_init, whose *_load_* verbs run before
     * cpu_init/oem_init adopt them. Everything below needs the drivers + the
     * resolved code page (argv conversion is per-page), so it all follows
     * main_init; the machine is started (main_run) after the ROM loads. */
    if (o.phi2_khz > 0)
    {
        if (o.phi2_khz > UINT16_MAX || !cpu_set_phi2_khz((uint16_t)o.phi2_khz))
        {
            fprintf(stderr, "rp6502-emu: --phi2 %d out of range (%d-%d)\n",
                    o.phi2_khz, CPU_PHI2_MIN_KHZ, CPU_PHI2_MAX_KHZ);
            return 1;
        }
    }
    if (o.code_page > 0)
    {
        if (o.code_page > UINT16_MAX || !oem_set_code_page((uint16_t)o.code_page))
        {
            fprintf(stderr, "rp6502-emu: unsupported code page %d\n", o.code_page);
            return 1;
        }
    }
    main_init();

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

    vga_set_framebuffer(g_fb); /* the app owns the pixels; vga renders into them */

    if (!pro_set_argv(rom, o.n_rom_args, o.rom_args))
    {
        fprintf(stderr, "rp6502-emu: ROM argv overflow\n");
        return 1;
    }

    apply_options(&o);

    /* Enable the debugger engine (the on-screen UI and the DAP adapter both
     * attach to it). Inert with no breakpoints, but --dap will also stand up the
     * stdio DAP server. */
    if (o.dap || o.debug)
        dbg_set_active(true);

    if (o.input)
        queue_input(o.input);

    main_run(); /* start the machine — main_init only initialized the drivers */

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
