/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/evq_machine.h"
#include "core/com/com.h"
#include "core/sys/event.h"
#include "core/rom/rom.h"
#include "core/str/oem.h"
#include "core/sys/proc.h"
#include "core/sys/sys.h"
#include "osal/dir.h"
#include "osal/os.h"
#ifdef EMU_WITH_DEBUGGER
#include "core/dap/dap.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct
{
    bool window;
    bool menu;
    bool minimized;
    evq_status_t told;
    bool told_once;
} machine;

void machine_set_window(bool on)
{
    machine.window = on;
}

bool machine_menu_up(void)
{
    return machine.menu;
}

bool machine_minimized(void)
{
    return machine.minimized;
}

bool machine_has_window(void)
{
    return machine.window;
}

/* ---------------- the clipboard ---------------- */

/* The window's last answer to EVQ_ASK, and when it came, until taken; and
 * whether an ask is out, so a paste that polls while it waits asks once. */
static char *machine_clipboard_in;
static uint64_t machine_clipboard_in_at;
static bool machine_clipboard_asked;

void machine_clipboard_ask(void)
{
    if (!machine_clipboard_asked)
        machine_clipboard_asked = evq_to_window(EVQ_ASK, NULL, 0);
}

static void machine_clipboard_answer(const char *utf8)
{
    machine_clipboard_asked = false;
    free(machine_clipboard_in);
    const size_t n = strlen(utf8) + 1;
    machine_clipboard_in = malloc(n);
    if (machine_clipboard_in)
        memcpy(machine_clipboard_in, utf8, n);
    machine_clipboard_in_at = os_mono_ns();
}

char *machine_clipboard_take(uint64_t *at)
{
    char *in = machine_clipboard_in;
    machine_clipboard_in = NULL;
    *at = machine_clipboard_in_at;
    return in;
}

/* ---------------- a dropped ROM ---------------- */

char *machine_rom_path(const char *host)
{
    if (oem_maps_utf8(host))
    {
        /* oem_from_utf8 writes one byte per UTF-8 sequence, so the UTF-8
         * length holds the result. A name FAT refuses has no drive path
         * either, and os_dir_realpath returns NULL for it. So does a path
         * whose absolute form is longer than API_PATH_MAX. */
        size_t sz = strlen(host) + 1;
        char *oem = malloc(sz);
        if (oem)
            oem_from_utf8(host, oem, sz);
        char *abs = oem ? os_dir_realpath(oem) : NULL;
        bool named = abs && strlen(abs) <= API_PATH_MAX;
        free(abs);
        if (named)
            return oem;
        free(oem);
    }
    const char *name = rom_alias_insert(host);
    char *rom = name ? malloc(strlen(name) + 2) : NULL;
    if (rom)
        sprintf(rom, ":%s", name);
    return rom;
}

/* The ":name" installed on the null drive for the last dropped file that
 * booted, or NULL. */
static char *machine_dropped;

bool machine_boot_rom(const char *path)
{
#ifdef EMU_WITH_DEBUGGER
    /* A DAP client controls the run state. A plain --debug session has no DAP
     * client, so a ROM dropped on it boots. */
    if (dap_is_active())
        return false;
#endif
    /* The file is checked before proc_boot stops the machine, so an accidental
     * drop leaves the running program running; the loader would refuse the file
     * too, but only after stopping that program. Its first line is read from
     * the host's file, with nothing the running program may be using: the
     * file system's one transfer, which a program streaming its assets keeps
     * busy, and the buffers its calls read into. */
    char head[ROM_HEAD];
    FILE *f = os_fopen(path, "rb");
    const size_t n = f ? fread(head, 1, sizeof head, f) : 0;
    const bool read = f && !ferror(f);
    if (f)
        fclose(f);
    if (!read || !rom_head_ok(head, n))
    {
        com_printf(read ? "not a .rp6502 file (bad magic)\n" : "cannot read dropped file\n");
        return false;
    }
    char *rom = machine_rom_path(path);
    if (!rom)
    {
        com_printf("cannot read dropped file\n");
        return false;
    }
    bool ok = proc_boot(rom, 0, NULL, 0);
    /* The install from the previous drop is removed once proc_boot has
     * stopped the machine, whether or not this boot succeeded, unless this
     * drop installed a ROM of the same name, which replaced it. */
    if (machine_dropped && strcasecmp(machine_dropped, rom) != 0)
        rom_alias_remove(machine_dropped);
    if (!ok && rom[0] == ':')
        rom_alias_remove(rom);
    free(machine_dropped);
    machine_dropped = ok && rom[0] == ':' ? rom : NULL;
    if (rom != machine_dropped)
        free(rom);
    if (!ok)
        return false;
    /* proc_boot only requests the machine, and a caller outside a driver pass
     * carries the request out itself. */
    sys_commit();
    return true;
}

/* ---------------- each message ---------------- */

void machine_take(const evq_msg_t *m)
{
    const uint8_t *b = m->body;
    switch (m->kind)
    {
    case EVQ_KEY: event_key(m->body); break;
    case EVQ_CHAR: event_char(m->body); break;
    case EVQ_LOCKS: event_locks(b[0]); break;
    case EVQ_POINTER: event_pointer(m->body); break;
    case EVQ_TOUCH: event_touch(m->body); break;
    case EVQ_WHEEL: event_wheel(m->body); break;
    case EVQ_MOUSE: event_mouse(m->body); break;
    case EVQ_FOCUS: event_focus(b[0] != 0); break;
    case EVQ_PASTE: event_paste(m->body); break;
    case EVQ_GAMEPADS: event_gamepads(m->body); break;
    case EVQ_MINIMIZED: machine.minimized = b[0] != 0; break;
    case EVQ_MENU: machine.menu = b[0] != 0; break;
    case EVQ_CLIPBOARD: machine_clipboard_answer(m->body); break;
    case EVQ_BOOT:
        /* A boot takes the card down on the spot, so the frames run before
         * the window hears of it are no program's last. */
        if (machine_boot_rom(m->body))
        {
            machine.menu = false;
            evq_to_window(EVQ_BOOTED, NULL, 0);
        }
        break;
    case EVQ_CLOSE:
        /* A chord the key behind the close acted on (Alt-F4 on Windows) has
         * answered it already. */
        if (!event_chord_held())
        {
            const uint8_t closed = 1;
            evq_to_window(EVQ_QUIT, &closed, 1);
        }
        break;
    default:
        break;
    }
}

/* ---------------- what the window is told ---------------- */

void machine_status(bool follows, int64_t period_ns)
{
    const evq_status_t s = {.event = event_status(), .follows = follows, .period_ns = period_ns};
    if (machine.told_once && s.event.flags == machine.told.event.flags &&
        s.event.cursor == machine.told.event.cursor && s.follows == machine.told.follows &&
        s.period_ns == machine.told.period_ns)
        return;
    /* Told again later if the window's queue is full. */
    if (evq_to_window(EVQ_STATUS, &s, sizeof s))
    {
        machine.told = s;
        machine.told_once = true;
    }
}

void machine_reset(void)
{
    free(machine_clipboard_in);
    machine_clipboard_in = NULL;
    machine_clipboard_asked = false;
    memset(&machine, 0, sizeof machine);
    event_reset();
}
