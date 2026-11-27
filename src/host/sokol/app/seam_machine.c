/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/seam_machine.h"
#include "core/api/proc.h"
#include "core/com/com.h"
#include "core/hid/gamepad.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/mouse.h"
#include "core/hid/paste_host.h"
#include "core/hid/tablet.h"
#include "core/hid/vtkey.h"
#include "core/rom/rom.h"
#include "core/str/oem.h"
#include "core/sys/proc.h"
#include "core/sys/sys.h"
#include "core/term/vterm_mouse.h"
#include "core/vga/mode/mode6.h"
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
    /* A key that typed with no character from the host: the host's own
     * character for it, which comes next, is not typed again. */
    bool suppress_char;
    /* A chord acted, until its key comes up. Windows follows Alt-F4 with a
     * close, which the chord has already answered. */
    bool chord_held;
    bool pads_mapped;
    seam_status_t told;
    bool told_once;
    /* Which host controller each player is, 0 for none. Keyed on the
     * backend's id rather than its position, so unplugging one controller
     * does not renumber the remaining players' slots. */
    uint64_t player[GAMEPAD_PLAYERS];
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

/* ---------------- keys ---------------- */

static void machine_key(const seam_key_t *k)
{
    /* The host repeats a held key and its character; the terminal says
     * whether they are typed (DECARM). */
    if (k->repeat && !vtkey_repeats())
        return;
    if (k->usage)
        keyboard_set_key(k->usage, k->down);
    if (!k->down)
    {
        machine.chord_held = false;
        return;
    }
    machine.suppress_char = false;
    if (!k->acts)
        return;
    const hostkey_result_t result = hostkey_press(k->usage, k->modifier, k->repeat, k->guess);
    if (result == HOSTKEY_CHORD)
        machine.chord_held = true;
    /* A key that typed with no character from here must not type the
     * system's character too. */
    else if (result == HOSTKEY_TYPED && !k->guess)
        machine.suppress_char = true;
}

static void machine_char(const seam_char_t *c)
{
    if (c->repeat && !vtkey_repeats())
        return;
    if (machine.suppress_char)
        machine.suppress_char = false;
    else
        hostkey_char(c->codepoint);
}

/* ---------------- the pointer ---------------- */

/* Because nothing is captured while the tablet has the pointer, a mouse the
 * program mapped is fed from it as well: the one physical pointer drives
 * both blocks, as it would on hardware. */
static void machine_pointer(const seam_pointer_t *p)
{
    if (p->left || !p->inside)
        tablet_host_clear();
    else
        tablet_host_pointer(p->x, p->y, p->buttons, true);
    if (p->left || !mouse_is_mapped())
        return;
    mouse_host_buttons(p->buttons);
    if (p->moved)
        mouse_host_move(p->dx, p->dy);
}

static void machine_touch(const seam_touch_t *t)
{
    tablet_point_t pts[SEAM_TOUCHES];
    for (int i = 0; i < t->n; i++)
        pts[i] = (tablet_point_t){.x = t->at[i].x, .y = t->at[i].y};
    tablet_host_touch(pts, t->n);
}

static void machine_wheel(const seam_wheel_t *w)
{
    if (w->tablet)
        tablet_host_wheel(w->wheel, w->pan);
    if (!w->tablet || mouse_is_mapped())
        mouse_host_wheel(w->wheel, w->pan);
}

static void machine_mouse(const seam_mouse_t *m)
{
    if (m->moved)
        mouse_host_move(m->dx, m->dy);
    else
        mouse_host_buttons(m->buttons);
}

/* ---------------- controllers ---------------- */

/* Every player blanked. */
static void machine_unplug(void)
{
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
        if (machine.player[player])
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            machine.player[player] = 0;
        }
}

static void machine_gamepads(const seam_gamepads_t *g)
{
    if (!g->opened || !gamepad_is_mapped())
    {
        machine_unplug();
        return;
    }
    for (int player = 0; player < GAMEPAD_PLAYERS; player++)
    {
        if (!machine.player[player])
            continue;
        bool still_here = false;
        for (int i = 0; i < g->n; i++)
            if (g->pad[i].id == machine.player[player])
                still_here = true;
        if (!still_here)
        {
            gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
            machine.player[player] = 0;
        }
    }
    for (int i = 0; i < g->n; i++)
    {
        const gamepad_host_t *pad = &g->pad[i];
        if (!pad->id)
            continue;
        int player = -1;
        for (int p = 0; p < GAMEPAD_PLAYERS; p++)
            if (machine.player[p] == pad->id)
                player = p;
        if (player < 0)
            for (int p = 0; p < GAMEPAD_PLAYERS && player < 0; p++)
                if (!machine.player[p])
                {
                    machine.player[p] = pad->id;
                    player = p;
                }
        if (player < 0)
            continue; /* more controllers than the machine has players */
        /* A backend may become sure of the face-button labels only after a
         * poll or two, so what it claims is restated with every report. */
        gamepad_connect(player, true, pad->type, pad->sticks);
        gamepad_host_report(player, pad->dpad, pad->button0, pad->button1,
                            pad->lx, pad->ly, pad->rx, pad->ry, pad->lt, pad->rt);
    }
}

/* ---------------- the clipboard ---------------- */

/* The window's last answer to SEAM_ASK, and when it came, until taken; and
 * whether an ask is out, so a paste that polls while it waits asks once. */
static char *machine_clipboard_in;
static uint64_t machine_clipboard_in_at;
static bool machine_clipboard_asked;

void machine_clipboard_ask(void)
{
    if (!machine_clipboard_asked)
        machine_clipboard_asked = seam_to_window(SEAM_ASK, NULL, 0);
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

void machine_take(const seam_msg_t *m)
{
    const uint8_t *b = m->body;
    switch (m->kind)
    {
    case SEAM_KEY: machine_key(m->body); break;
    case SEAM_CHAR: machine_char(m->body); break;
    case SEAM_LOCKS: keyboard_set_locks(b[0]); break;
    case SEAM_POINTER: machine_pointer(m->body); break;
    case SEAM_TOUCH: machine_touch(m->body); break;
    case SEAM_WHEEL: machine_wheel(m->body); break;
    case SEAM_MOUSE: machine_mouse(m->body); break;
    case SEAM_FOCUS: vterm_focus(b[0] != 0); break;
    case SEAM_MINIMIZED: machine.minimized = b[0] != 0; break;
    case SEAM_MENU: machine.menu = b[0] != 0; break;
    case SEAM_PASTE: paste_start(m->body); break;
    case SEAM_CLIPBOARD: machine_clipboard_answer(m->body); break;
    case SEAM_GAMEPADS: machine_gamepads(m->body); break;
    case SEAM_BOOT:
        /* A boot takes the card down on the spot, so the frames run before
         * the window hears of it are no program's last. */
        if (machine_boot_rom(m->body))
        {
            machine.menu = false;
            seam_to_window(SEAM_BOOTED, NULL, 0);
        }
        break;
    case SEAM_CLOSE:
        if (!machine.chord_held)
        {
            const uint8_t closed = 1;
            seam_to_window(SEAM_QUIT, &closed, 1);
        }
        break;
    default:
        break;
    }
}

/* ---------------- what the window is told ---------------- */

void machine_status(bool follows, int64_t period_ns)
{
    const bool mouse = mouse_is_mapped();
    const bool to_tablet = tablet_is_mapped() || (tablet_wanted() && !mouse);
    tablet_host_give(to_tablet);
    const bool pads = gamepad_is_mapped();
    if (machine.pads_mapped && !pads)
        machine_unplug();
    machine.pads_mapped = pads;
    /* The VGA draws the program's own pointers, and a shape the system has
     * none of is no pointer. */
    uint8_t cursor = tablet_shape();
    if (cursor >= TABLET_CURSOR_COUNT || mode6_custom())
        cursor = TABLET_CURSOR_OFF;
    const seam_status_t s = {
        .flags = (uint8_t)((mouse ? SEAM_MOUSE_MAPPED : 0) |
                           (to_tablet ? SEAM_TO_TABLET : 0) |
                           (to_tablet && tablet_wheel_wanted() ? SEAM_WHEEL_TO_TABLET : 0) |
                           (proc_exited() ? SEAM_EXITED : 0) |
                           (pads ? SEAM_PADS_MAPPED : 0) |
                           (follows ? SEAM_FOLLOWS : 0)),
        .cursor = cursor,
        .period_ns = period_ns,
    };
    if (machine.told_once && s.flags == machine.told.flags &&
        s.cursor == machine.told.cursor && s.period_ns == machine.told.period_ns)
        return;
    /* Told again later if the window's queue is full. */
    if (seam_to_window(SEAM_STATUS, &s, sizeof s))
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
}
