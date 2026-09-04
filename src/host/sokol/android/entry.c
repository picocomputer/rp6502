/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This platform's entry: sokol_main returns a sapp_desc, because sokol's
 * NativeActivity glue owns the real entry point and there is no main() here —
 * so this file also does what cli/main.c does for everyone else. Then the
 * hooks, and the AInputEvent decoder, which is this platform's gamepad driver
 * and offers each event to the ROM browser in menu.c first.
 */

#include "host/host.h"
#include "osal/os.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "host/sokol/android/menu.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h" /* sokol_debugtext.h needs sg_* types declared first */
#include "sokol/sokol_log.h"
#include "sokol/util/sokol_debugtext.h"
#include "core/hid/gamepad.h"
#include "core/sys/sys.h"
#include "core/rom/rom.h"
#include "core/sys/proc.h"
#include "core/vga/vga_emu.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <jni.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ROMS 64
#define ROM_NAME_MAX 128

/* Android gamepad button/axis state tracking. */
static uint8_t g_android_button0 = 0;
static uint8_t g_android_button1 = 0;
static uint8_t g_android_dpad = 0;
static int g_android_lx = 0;
static int g_android_ly = 0;
static int g_android_rx = 0;
static int g_android_ry = 0;
static int g_android_lt = 0;
static int g_android_rt = 0;

bool rp6502_android_input_hook(const void* native_event)
{
    const AInputEvent* event = (const AInputEvent*)native_event;
    int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY)
    {
        int32_t key_code = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        bool down = (action == AKEY_EVENT_ACTION_DOWN);

        if (menu_key(key_code, down))
            return 1;

        switch (key_code)
        {
            // Retroid Pocket 3+ physical gamepad mapping
            case AKEYCODE_BUTTON_A:
                if (down) g_android_button0 |= 0x01; else g_android_button0 &= ~0x01;
                break;
            case AKEYCODE_BUTTON_B:
                if (down) g_android_button0 |= 0x02; else g_android_button0 &= ~0x02;
                break;
            case AKEYCODE_BUTTON_X:
                if (down) g_android_button0 |= 0x08; else g_android_button0 &= ~0x08;
                break;
            case AKEYCODE_BUTTON_Y:
                if (down) g_android_button0 |= 0x10; else g_android_button0 &= ~0x10;
                break;
            case AKEYCODE_BUTTON_L1:
                if (down) g_android_button0 |= 0x40; else g_android_button0 &= ~0x40;
                break;
            case AKEYCODE_BUTTON_R1:
                if (down) g_android_button0 |= 0x80; else g_android_button0 &= ~0x80;
                break;

            case AKEYCODE_BUTTON_L2:
                if (down) g_android_button1 |= 0x01; else g_android_button1 &= ~0x01;
                break;
            case AKEYCODE_BUTTON_R2:
                if (down) g_android_button1 |= 0x02; else g_android_button1 &= ~0x02;
                break;
            case AKEYCODE_BUTTON_SELECT:
                if (down) g_android_button1 |= 0x04; else g_android_button1 &= ~0x04;
                // Toggle ROM select menu when SELECT + START are both pressed
                if (down && (g_android_button1 & 0x08))
                {
                    menu_open();
                }
                break;
            case AKEYCODE_BUTTON_START:
                if (down) g_android_button1 |= 0x08; else g_android_button1 &= ~0x08;
                // Toggle ROM select menu when SELECT + START are both pressed
                if (down && (g_android_button1 & 0x04))
                {
                    menu_open();
                }
                break;
            case AKEYCODE_BUTTON_MODE: // Home button
                if (down) g_android_button1 |= 0x10; else g_android_button1 &= ~0x10;
                if (down)
                {
                    menu_open();
                }
                break;
            case AKEYCODE_BUTTON_THUMBL:
                if (down) g_android_button1 |= 0x20; else g_android_button1 &= ~0x20;
                break;
            case AKEYCODE_BUTTON_THUMBR:
                if (down) g_android_button1 |= 0x40; else g_android_button1 &= ~0x40;
                break;

            case AKEYCODE_DPAD_UP:
                if (down) g_android_dpad |= 0x01; else g_android_dpad &= ~0x01;
                break;
            case AKEYCODE_DPAD_DOWN:
                if (down) g_android_dpad |= 0x02; else g_android_dpad &= ~0x02;
                break;
            case AKEYCODE_DPAD_LEFT:
                if (down) g_android_dpad |= 0x04; else g_android_dpad &= ~0x04;
                break;
            case AKEYCODE_DPAD_RIGHT:
                if (down) g_android_dpad |= 0x08; else g_android_dpad &= ~0x08;
                break;

            default:
                return 0; // Not handled
        }
        gamepad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                            g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt);
        return 1; // Handled
    }
    else if (type == AINPUT_EVENT_TYPE_MOTION)
    {
        if (menu_active())
        {
            menu_stick(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0),
                       AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
            return 1;
        }

        // Read Hat/D-pad axes
        float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);

        g_android_dpad = 0;
        if (hat_x < -0.5f) g_android_dpad |= 0x04; // LEFT
        if (hat_x > 0.5f)  g_android_dpad |= 0x08; // RIGHT
        if (hat_y < -0.5f) g_android_dpad |= 0x01; // UP
        if (hat_y > 0.5f)  g_android_dpad |= 0x02; // DOWN

        // Read Analog Stick axes
        float lx_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
        float ly_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
        float rx_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
        float ry_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);

        g_android_lx = (int)(lx_val * 127.0f);
        g_android_ly = (int)(ly_val * 127.0f);
        g_android_rx = (int)(rx_val * 127.0f);
        g_android_ry = (int)(ry_val * 127.0f);

        // Read Trigger axes
        float lt_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        float rt_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        g_android_lt = (int)(lt_val * 255.0f);
        g_android_rt = (int)(rt_val * 255.0f);

        gamepad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                            g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt);
        return 1; // Handled
    }
    return 0; // Not handled
}

/* The seed for this run, decided once: no --seed here, so it is the OS's,
 * and it is asked for both the stream and the memory fill. */
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

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_files_dropped(void) {} /* sokol has no Android drag-n-drop */
void host_window_open_url(const char *url) { (void)url; } /* no desktop drop-a-ROM prompt */

void host_window_init(void) { menu_setup(); }

bool host_window_menu_active(void) { return menu_active(); }
void host_window_menu_draw(void) { menu_draw(); }

// Global framebuffer for Android
static uint32_t android_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

sapp_desc sokol_main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    menu_chdir(); /* the folder the list is read from is also the guest's cwd */

    // Initialize the drivers once; the machine is started per-program (sys_run).
    sys_init();

    // Try to load a default rom (boot.rp6502) if it exists, otherwise activate the menu
    if (proc_boot("boot.rp6502", 0, NULL, 0))
    {
        sys_commit();
    }
    else
    {
        menu_open(); // still held from sys_init, until the menu boots one

    }

    /* Connect gamepad player 0. Sticks unconditionally: the motion handler
     * reads AXIS_X/Y/Z/RZ from whatever is attached, and Android never says
     * whose labels these are. */
    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, true);

    // Seed the core's window/render state (also sets the vga framebuffer to
    // android_fb). Android opens at a fixed 640x480, so the computed size is unused.
    int win_w, win_h;
    app_prepare(android_fb, 1.0, false, false, &win_w, &win_h);

    return (sapp_desc){
        .init_cb = app_init,
        .frame_cb = app_frame,
        .event_cb = app_input,
        .cleanup_cb = app_cleanup,
        .android = {
            .native_event_cb = rp6502_android_input_hook,
        },
        .width = 640,
        .height = 480,
        .window_title = "Picocomputer 6502",
        .logger.func = slog_func,
    };
}
