/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/config.h"
#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "core/str/str.h"
#include "core/sys/proc.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "osal/os.h"
#include "core/aud/mix.h"
#include "core/dap/dbg.h"
#include "host/sokol/cli/png.h"
#include "core/sys/random.h"
#include "host/host.h"
#include "core/rom/rom.h"
#include "core/str/path.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/wdc/phi2.h"
#include "core/wdc/resb.h"
#include "core/vga/vga_emu.h"
#include "host/sokol/cli/cli.h"
#include "host/sokol/cli/script.h"
#include "host/sokol/cli/credits.h"
#include "core/sys/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef EMU_WITH_DEBUGGER
#include "core/dap/dap.h"
#include "host/sokol/dbg/dbgui.h" /* dbgui_set_config_file (--ini) */
#endif

static uint32_t g_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

/* Apply the host/window presentation options shared by both launch paths. phi2,
 * cp, seed and fill are machine settings loaded as config before sys_init, not
 * here -- which is also why they reach every launch path and these do not. */
static void apply_options(const cli_options *o)
{
    if (o->have_bg)
        gfx_set_bgcolor((uint8_t)o->bg_r, (uint8_t)o->bg_g, (uint8_t)o->bg_b);
    gfx_set_filter(o->scale_filter);
    if (o->mute)
        aud_set_enabled(false);
}

#ifdef EMU_WITH_DEBUGGER
/* DAP mode (--dap): the program is delivered by the VS Code launch request, not
 * the command line. sys_init left the machine held and no program has started
 * it, so this only serves DAP on stdio; the launch handler loads + runs the ROM
 * via proc_exec_request. The window still opens (with the debugger overlay) so the
 * program is visible while VS Code drives. */
static int run_dap(const cli_options *o)
{
    dbg_set_active(true);

    apply_options(o);

    if (o->rom_args)
        dap_set_default_args(o->n_rom_args, o->rom_args);
    dap_start(); /* DAP on stdin/stdout; entry_run pumps it each frame */
    /* The debug session lifecycle is DAP-driven (StoppedEvent/TerminatedEvent on
     * exit, the window closes on Disconnect), so the window is held (never
     * auto-closed) — the final screen stays up until the client disconnects. */
    return entry_run(g_fb, o->scale, o->have_scale, false);
}
#endif


/* The seed for this run, decided once. --seed pins it; otherwise the OS is
 * asked, and the value is reported so an unseeded run can still be repeated.
 * Asked more than once -- for the stream, for the fill, for the report -- so
 * it has to answer the same every time. */
static uint32_t run_seed;
static bool run_seed_taken;

uint32_t host_seed(void)
{
    if (!run_seed_taken)
    {
        run_seed = os_random();
        run_seed_taken = true;
    }
    return run_seed;
}

/* argv in the guest's code page, allocated to fit: os_argv_to_oem only ever
 * contracts, so the argument's own length is the bound. The caller frees. */
static char *argv_to_oem(const char *arg)
{
    size_t sz = strlen(arg) + 1;
    char *oem = malloc(sz);
    if (oem && !os_argv_to_oem(arg, oem, sz))
    {
        free(oem);
        oem = NULL;
    }
    return oem;
}

int main(int argc, char **argv)
{
    os_console_attach();
    cli_options o;
    cli_options_init(&o);
    if (cli_parse_args(argc, argv, &o))
    {
        cli_usage(stderr, argv[0]);
        script_usage(stderr);
        return 2;
    }
    if (o.help)
    {
        cli_usage(stdout, argv[0]);
        script_usage(stdout);
        return 0;
    }

    /* --version and --credits: answer and exit, before anything is initialized.
     * On the web the shell maps ?credits to the latter, and the output appears
     * in the console. */
    if (o.version)
    {
        printf("%s\n", version_string());
        return 0;
    }
    if (o.credits)
    {
        printf("%s%s\n%s", EMU_CREDITS_TITLE, version_string(), EMU_CREDITS);
        return 0;
    }

#ifdef EMU_WITH_DEBUGGER
    /* Config file the debugger persists its window layout into (an [EMU] section;
     * other sections are preserved). The launcher passes the workstation file,
     * e.g. ${workspaceFolder}/.rp6502; else the debug UI uses the OS config dir. */
    if (o.ini)
        dbgui_set_config_file(o.ini);
#else
    /* Accepting it and doing nothing is how a wrong path goes unnoticed. */
    if (o.ini)
    {
        fprintf(stderr, "rp6502-emu: built without debugger support\n");
        return 1;
    }
#endif

    /* An option whose whole effect depends on another being present is an
     * error without it. Different from one that is merely inert on a host —
     * --scale under --script — which stays quiet so a wrapper can pass one
     * set of flags to every host. */
    if (o.have_frames && !o.screenshot)
    {
        fprintf(stderr, "rp6502-emu: --frames only applies to --screenshot; "
                        "a script's frames are its own (see 'run')\n");
        return 2;
    }

    /* Load the command-line settings as config, then init the machine ONCE —
     * mirroring the firmware's cfg_init, whose *_load_* verbs run before
     * cpu_init/oem_init adopt them. Everything below needs the drivers + the
     * resolved code page (argv conversion is per-page), so it all follows
     * sys_init; the machine is started (sys_run) after the ROM loads. */
    if (o.phi2_khz > 0)
    {
        if (o.phi2_khz > UINT16_MAX || !phi2_set_khz((uint16_t)o.phi2_khz))
        {
            fprintf(stderr, "rp6502-emu: --phi2 %d out of range (%d-%d)\n",
                    o.phi2_khz, PHI2_MIN_KHZ, PHI2_MAX_KHZ);
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
    /* One seed for the run, reaching both the memory fill and the RNG the ROM
     * reads, from streams far enough apart that the fill cannot move what the
     * program's rand() returns. Set before sys_init because the fills are the
     * first thing it does. */
    if (o.have_seed)
        run_seed = (uint32_t)o.seed, run_seed_taken = true;
    sram_set_fill(o.fill_random, o.fill_value, host_seed());
    xram_set_fill(o.fill_random, o.fill_value, host_seed());
    /* Say which seed a random fill used, or a run that turns something up is a
     * run nobody can repeat. Host stderr, so nothing a script matches moves. */
    if (o.fill_random && !o.have_seed)
        fprintf(stderr, "rp6502-emu: memory filled at random; --seed %u repeats it\n",
                (unsigned)host_seed());
    sys_init();

    /* Install ROMs before the boot load / any exec can resolve them. Paths and
     * ROM args are guest-bound, so they convert from host argv encoding to OEM
     * here at the entry; --shot/--ini stay host-domain untouched. */
    for (int i = 0; i < o.n_installs; i++)
    {
        char *oem = argv_to_oem(o.installs[i]);
        bool ok = oem && rom_alias_insert(oem); /* which takes its own copy */
        free(oem);
        if (!ok)
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

    if (o.dap && o.script)
    {
        /* Both want to be the one driving, and both may want stdin. */
        fprintf(stderr, "rp6502-emu: --dap and --script cannot both drive the machine\n");
        return 2;
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
    char *rom = NULL; /* owned; NULL means none was named */
    if (o.rom)
        rom = argv_to_oem(o.rom);
    else if (o.n_installs > 0)
    {
        char *inst = argv_to_oem(o.installs[0]);
        if (inst)
        {
            const char *base = path_basename(inst);
            rom = malloc(strlen(base) + 2); /* the ':' and the null */
            if (rom)
                sprintf(rom, ":%s", base);
        }
        free(inst);
    }
    if ((o.rom || o.n_installs > 0) && !rom)
    {
        fprintf(stderr, "rp6502-emu: cannot take the ROM path\n");
        return 1;
    }

    if (!rom)
    {
        /* No ROM. --screenshot and --script are batch (nothing to shoot, nothing
         * to drive); otherwise a desktop host waits for a drag-and-dropped one.
         * Anything else prints usage. */
        if (o.screenshot || o.script || !entry_wait_for_rom())
        {
            cli_usage(stderr, argv[0]);
            script_usage(stderr);
            return 2;
        }
        apply_options(&o);
        if (o.debug)
            dbg_set_active(true); /* show the debugger overlay while waiting for a drop */
        /* Still held from sys_init, until a dropped .rp6502 boots one. */
        return entry_run(g_fb, o.scale, o.have_scale, !o.debug);
    }

    bool booted = proc_boot(rom, o.n_rom_args, o.rom_args, 0);
    free(rom);
    if (!booted)
    {
        /* rom_load said why on the machine's console, which nobody is looking
         * at: no window opens on this path. */
        fprintf(stderr, "rp6502-emu: cannot load ROM '%s'\n",
                o.rom ? o.rom : o.installs[0]); /* what was asked for, in host encoding */
        return 1;
    }

    vga_set_framebuffer(g_fb); /* the app owns the pixels; vga renders into them */

    apply_options(&o);

    /* Enable the debugger engine (the on-screen UI and the DAP adapter both
     * attach to it). Inert with no breakpoints, but --dap will also stand up the
     * stdio DAP server. */
    if (o.dap || o.debug)
        dbg_set_active(true);

    /* Armed before the machine starts so the script's first check sees the
     * program's first output. */
    if (o.script && !script_load(o.script))
        return 1;

    sys_commit(); /* proc_boot asked; this starts it */

    /* A script is the clock, always: it runs the machine here rather than under a
     * window, so a frame elapses only because the script asked for one and its
     * verdict is the process exit code. Pacing a script against the host's clock
     * would make every frame count a lower bound instead of a number. */
    if (script_loaded())
    {
        while (script_running())
        {
            script_task(); /* returns owing exactly one frame, or done */
            if (script_running())
                vga_run_frame();
        }
        if (script_exit_code() || !o.screenshot)
            return script_exit_code(); /* a passing script may still want the shot */
    }

    if (o.screenshot)
    {
        const int frames = o.frames < 1 ? 1 : o.frames;
        for (int i = 0; i < frames; i++)
            vga_run_frame(); /* the last frame lands in g_fb, registered above */
        int cw, ch;
        vga_canvas_size(&cw, &ch); /* PNG is the canvas's native resolution */
        if (!png_write(o.screenshot, cw, ch, g_fb))
            return 1;
        printf("rp6502-emu: wrote %s (%d frames; cpu %s, exit code %d)\n",
               o.screenshot, frames, resb_running() ? "running" : "halted", proc_get_exit_code());
        return 0;
    }

    /* A script has already returned by here, so this is the windowed run. */
    return entry_run(g_fb, o.scale, o.have_scale, !o.debug);
}
