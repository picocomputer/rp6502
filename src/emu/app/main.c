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

#include "emu/api/api.h"
#include "emu/api/oem.h"
#include "emu/api/pro.h"
#include "emu/app/window.h"
#include "emu/aud/aud.h"
#include "emu/dbg/dbg.h"
#include "emu/host/png.h"
#include "emu/host/rand.h"
#include "emu/mon/install.h"
#include "emu/mon/rom.h"
#include "emu/msc/mscpath.h"
#include "emu/sys/mem.h"
#include "emu/sys/ria.h"
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

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <rom.rp6502> [options]\n"
            "  --screenshot <file.png>   render headlessly to PNG and exit\n"
            "  --frames <n>              frames to run before screenshot (default 120)\n"
            "  --scale <n>               window scale, fractional ok (default 1.5)\n"
            "  --scale-filter <f>        nearest|linear|sharp (default sharp)\n"
            "  --input <text>            queue keystrokes for stdin ('\\n' = Enter)\n"
            "  --fs <dir>                MSC0: mount directory (default: the launch dir)\n"
            "  --tmpdrive                MSC0: = a fresh throwaway temp dir (isolate the ROM)\n"
            "  --rom <file>              install a .rp6502 on the null drive, reached\n"
            "                            as :basename; repeatable, the first one boots\n"
            "  --bgcolor RRGGBB          letterbox/pillarbox fill color (default 000000)\n"
            "  --phi2 <khz>              6502 clock in kHz (100-8000, default 8000)\n"
            "  --cp <n>                  OEM code page (437/720/737/775/850/852/855/\n"
            "                            857/860-866/869, default 437)\n"
            "  --seed <n>                fixed RNG seed for reproducible runs\n"
            "                            (default: host entropy)\n"
            "  --no-audio                disable audio (no synth, no OS audio device)\n"
            "  --debug                   on-screen machine debugger (CPU/VIA/disasm); holds\n"
            "                            the window open on stop for inspection\n"
            "  --dap                     act as a DAP debug adapter on stdio (implies --debug)\n"
            "  --credits                 print third-party credits/licenses and exit\n"
            "  --ini <file>              config file for the debugger UI layout (an\n"
            "                            [EMU] section; e.g. the workspace .rp6502)\n"
            "A ROM's 'emulator' asset can preset these; the command line overrides it.\n",
            argv0);
}

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
    long n = fs_read_rom_asset("emulator", asset, sizeof asset - 1);
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
    merged.fsdir = o->fsdir;
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
    if (o->no_audio)
        emu_set_audio_enabled(false);
    if (o->phi2_khz > 0)
        emu_set_phi2_khz((uint16_t)o->phi2_khz);
    if (o->code_page > 0 && o->code_page <= UINT16_MAX)
        oem_set_code_page((uint16_t)o->code_page);

    dap_start(); /* DAP on stdin/stdout; emu_run_window pumps it each frame */
    /* The debug session lifecycle is DAP-driven (StoppedEvent/TerminatedEvent on
     * exit, the window closes on Disconnect), so the window is held (never
     * auto-closed) — the final screen stays up until the client disconnects. */
    return emu_run_window(o->scale, false);
}
#endif

int main(int argc, char **argv)
{
    options o;
    options_init(&o);
    if (parse_args(argc, argv, &o))
    {
        usage(argv[0]);
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

    /* Where MSC0: is mounted: a throwaway temp dir (--tmpdrive, isolates the
     * ROM) or a chosen host dir (--fs); with neither, MSC0: defaults to the
     * launch dir on first use (mscpath.c). The mount persists for the whole
     * session, exec included. These locate the drive, so they come from the
     * command line, not the ROM's asset args. */
    if (o.tmpdrive)
    {
        if (!fs_use_tmpdrive())
        {
            fprintf(stderr, "rp6502-emu: cannot create --tmpdrive\n");
            return 1;
        }
    }
    else if (o.fsdir && !fs_set_cwd(o.fsdir))
    {
        fprintf(stderr, "rp6502-emu: cannot use --fs directory '%s'\n", o.fsdir);
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
        usage(argv[0]);
        return 2;
    }

    if (!emu_rom_load(rom))
        return 1;

    /* The ROM is loaded; fold its "emulator" asset args under the command line. */
    merge_rom_args(&o, argc, argv);

    emu_init();

    /* argv[0] is the program's own path (in MSC0: form) so it can re-exec
     * itself; ria_execl/exec resolve later argv paths the same way. A drive path
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

    if (o.no_audio) /* no synth work, and the window opens no OS audio device */
        emu_set_audio_enabled(false);

    if (o.phi2_khz > 0) /* override the default PHI2 (emu_init reset it) */
        emu_set_phi2_khz((uint16_t)o.phi2_khz);

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
        for (int i = 0; i < frames; i++)
            emu_run_frame();
        emu_render(g_fb);
        int cw, ch;
        emu_canvas_size(&cw, &ch); /* PNG is the canvas's native resolution */
        if (!emu_write_png(o.shot, cw, ch, g_fb))
            return 1;
        printf("rp6502-emu: wrote %s (%d frames; cpu %s, exit code %d)\n",
               o.shot, frames, emu_cpu_halted ? "halted" : "running", emu_exit_code);
        return 0;
    }

    return emu_run_window(o.scale, !o.debug);
}
