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
#include "host/sokol/app/app.h"
#include "osal/os.h"
#include "osal/console.h"
#include "core/aud/mix.h"
#include "core/com/com.h"
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
#include "host/sokol/cli/state.h"
#include "host/sokol/cli/console.h"
#include "host/sokol/cli/streams.h"
#include "host/sokol/cli/credits.h"
#include "core/sys/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef EMU_WITH_DEBUGGER
#include "core/dap/dap.h"
#include "host/sokol/dbg/dbgui.h"
#endif

static uint32_t g_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

static void apply_options(const cli_options *o)
{
    if (o->have_bg)
        gfx_set_bgcolor((uint8_t)o->bg_r, (uint8_t)o->bg_g, (uint8_t)o->bg_b);
    gfx_set_filter(o->scale_filter);
    if (o->mute)
        aud_set_enabled(false);
    app_set_unpaced(o->unpaced);
}

#ifdef EMU_WITH_DEBUGGER
/* DAP mode (--dap): the program arrives in the client's launch request rather
 * than on the command line, so the machine is still held here and dap.cpp
 * boots the ROM with proc_exec_request. The window opens anyway, with the
 * debugger overlay, so the program is visible while the client drives it. */
static int run_dap(const cli_options *o)
{
    dbg_set_active(true);

    apply_options(o);

    if (o->rom_args)
        dap_set_default_args(o->n_rom_args, o->rom_args);
    dap_start(); /* entry_run pumps it each frame */
    /* The window never closes itself on halt, because the client ends the
     * session and the final screen has to stay up until it disconnects. */
    return entry_run(g_fb, o->scale, o->have_scale, false);
}
#endif


/* The seed for this run, drawn once. --seed pins it, and otherwise the OS is
 * asked. The memory fills, the RNG the program reads and the script's `seed`
 * verb all ask separately, so it has to answer the same every time. */
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
    app_set_break(os_console_break_asked, os_console_break_exit);
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
    if (o.ini)
        dbgui_set_config_file(o.ini);
#else
    if (o.ini)
    {
        fprintf(stderr, "rp6502-emu: built without debugger support\n");
        return 1;
    }
#endif

    state_slot_init(o.rom);

    if (o.have_frames && !o.screenshot && !o.crc)
    {
        fprintf(stderr, "rp6502-emu: --frames only applies to --screenshot or --crc; "
                        "a script's frames are its own (see 'run')\n");
        return 2;
    }

    /* The command-line settings are applied as config before sys_init.
     * Everything after sys_init needs the drivers and the resolved code page,
     * because converting argv is per code page. */
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
    /* One seed for the run reaches both the memory fills and the RNG the
     * program reads, from separate streams so that the fills cannot move what
     * the program's rand() returns. It is set before sys_init, which is where
     * sram_init and xram_init fill. */
    if (o.have_seed)
        run_seed = (uint32_t)o.seed, run_seed_taken = true;
    sram_set_fill(o.fill_random, o.fill_value, host_seed());
    xram_set_fill(o.fill_random, o.fill_value, host_seed());
    sys_init();

    streams_mirror_stderr();

    /* EMU_ECHO mirrors the machine's console to the host's stderr, so a run
     * that failed can be read without rendering a frame. It is set up here
     * rather than past the launch paths that return early, so that every mode
     * gets it. com.c holds one console tap, which a script takes for itself,
     * so the script is asked to echo rather than displaced. */
    if (getenv("EMU_ECHO"))
    {
        if (o.script)
            script_set_echo(streams_stderr);
        else
            com_set_tx_tap(streams_stderr);
    }

    /* Install ROMs before the boot load, or an exec, can resolve them. A path
     * or an argument the guest will see converts from the host's argv encoding
     * to OEM here; a path only the host opens stays as it came. */
    for (int i = 0; i < o.n_installs; i++)
    {
        char *oem = argv_to_oem(o.installs[i]);
        bool ok = oem && rom_alias_insert(oem);
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
        fprintf(stderr, "rp6502-emu: --dap and --script cannot both drive the machine\n");
        return 2;
    }
    if (o.headless && (o.script || o.screenshot || o.crc || o.dap || o.debug))
    {
        fprintf(stderr, "rp6502-emu: --headless cannot be combined with --script, "
                        "--screenshot, --crc, --dap or --debug\n");
        return 2;
    }
    if (o.console && (o.script || o.crc || o.dap))
    {
        fprintf(stderr, "rp6502-emu: --stdin cannot be combined with --script, "
                        "--crc or --dap\n");
        return 2;
    }

#ifdef EMU_WITH_DEBUGGER
    if (o.dap)
        return run_dap(&o);
#else
    if (o.dap)
    {
        fprintf(stderr, "rp6502-emu: built without debugger/DAP support\n");
        return 1;
    }
#endif

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
        /* With no ROM, --screenshot and --crc have no canvas to capture and
         * --script and --headless have no program to run, so only a host that
         * can still be given one -- by drag and drop -- goes on to open a
         * window. */
        if (o.screenshot || o.crc || o.script || o.headless || !entry_wait_for_rom())
        {
            cli_usage(stderr, argv[0]);
            script_usage(stderr);
            return 2;
        }
        apply_options(&o);
        if (o.debug)
            dbg_set_active(true);
        streams_mirror_stdout();
        return entry_run(g_fb, o.scale, o.have_scale, !o.debug);
    }

    bool booted = proc_boot(rom, o.n_rom_args, o.rom_args, 0);
    free(rom);
    if (!booted)
    {
        /* rom_load said why on the machine's console, which no window on this
         * path ever shows, so the reason is repeated here. */
        fprintf(stderr, "rp6502-emu: cannot load ROM '%s'\n",
                o.rom ? o.rom : o.installs[0]);
        return 1;
    }

    if (!o.headless)
        vga_set_framebuffer(g_fb);

    apply_options(&o);

    /* An active debugger puts bus.c on its per-cycle loop, which costs at
     * 8 MHz, so it is turned on only where one was asked for. */
    if (o.dap || o.debug)
        dbg_set_active(true);

    /* Armed before the machine starts so the script's first check sees the
     * program's first output. */
    if (o.script && !script_load(o.script))
        return 1;

    /* A host terminal becomes the machine's console and carries its screen,
     * so it is already showing everything the mirror below would repeat. */
    bool console_is_terminal = o.console && console_open();

    /* The program's stdout on the host's too, except where host stdout is
     * already the emulator's own channel: a script's replies, a CRC. */
    if (!o.script && !o.crc && !console_is_terminal)
        streams_mirror_stdout();

    sys_commit();

    if (o.headless)
    {
        /* Paced the way a window paces, without one: a deadline a frame ahead
         * and a sleep up to it. A stall of more than three frames, such as a
         * blocking console read, moves the deadline to now rather than being
         * made up in a burst of frames. --phi2 0 drops the sleep. */
        uint64_t deadline = os_mono_ns() + VGA_FRAME_NS;
        while (!proc_exited() && !os_console_break_asked())
        {
            vga_run_frame();
            console_idle();
            if (o.unpaced)
                continue;
            const uint64_t now = os_mono_ns();
            if (deadline > now)
                os_sleep_ns(deadline - now);
            else if (now - deadline > 3 * VGA_FRAME_NS)
                deadline = now;
            deadline += VGA_FRAME_NS;
        }
        if (os_console_break_asked())
            sys_break_request();
        sys_stop();
        sys_commit();
        fflush(stdout);
        if (os_console_break_asked())
            os_console_break_exit(); /* does not return */
        return proc_get_exit_code();
    }

    /* A script runs the machine here rather than under a window, so a frame
     * elapses only because the script asked for one. Pacing it against the
     * host's clock would make every frame count a lower bound instead of a
     * number. A script that passed still takes the screenshot or the CRC it
     * asked for; anything else ends the run here. */
    if (script_loaded())
    {
        while (script_running())
        {
            script_task();
            if (script_running())
                vga_run_frame();
        }
        if (script_exit_code() || !(o.screenshot || o.crc))
        {
            sys_stop();
            sys_commit();
            return script_exit_code();
        }
    }

    if (o.screenshot || o.crc)
    {
        const int frames = o.frames < 1 ? 1 : o.frames;
        for (int i = 0; i < frames; i++)
            vga_run_frame();
        if (o.screenshot)
        {
            int cw, ch;
            vga_canvas_size(&cw, &ch);
            if (!png_write(o.screenshot, cw, ch, g_fb))
                return 1;
            fprintf(stderr, "rp6502-emu: wrote %s (%d frames; cpu %s, exit code %d)\n",
                    o.screenshot, frames, resb_running() ? "running" : "halted", proc_get_exit_code());
        }
        if (o.crc)
        {
            uint32_t crc;
            vga_frame_crc(&crc);
            printf("%08X\n", crc);
        }
        sys_stop();
        sys_commit();
        return 0;
    }

    return entry_run(g_fb, o.scale, o.have_scale, !o.debug);
}
