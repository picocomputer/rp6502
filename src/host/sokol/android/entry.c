/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/host.h"
#include "osal/os.h"
#include "host/sokol/app/gfx.h"
#include "host/sokol/app/app.h"
#include "host/sokol/app/prompt.h"
#include "host/sokol/android/menu.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/util/sokol_debugtext.h"
#include "core/hid/gamepad.h"
#include "core/sys/sys.h"
#include "core/rom/rom.h"
#include "core/sys/proc.h"
#include "core/vga/vga_emu.h"
#include "core/sys/debug_log.h"
#include <android/input.h>
#include <android/log.h>
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

void host_log(int level, const char *category, const char *fmt, ...)
{
    static const int prio[] = {ANDROID_LOG_SILENT, ANDROID_LOG_ERROR, ANDROID_LOG_WARN,
                               ANDROID_LOG_INFO, ANDROID_LOG_DEBUG};
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(prio[level], category, fmt, ap);
    va_end(ap);
}

#define MAX_ROMS 64
#define ROM_NAME_MAX 128

/* The masks below are the gamepad report's own bit layout, spelled out in
 * gamepad_button_loc (core/hid/gamepad.c). */
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
                if (down && (g_android_button1 & 0x08))
                {
                    menu_open();
                }
                break;
            case AKEYCODE_BUTTON_START:
                if (down) g_android_button1 |= 0x08; else g_android_button1 &= ~0x08;
                if (down && (g_android_button1 & 0x04))
                {
                    menu_open();
                }
                break;
            case AKEYCODE_BUTTON_MODE:
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
                return 0;
        }
        gamepad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                            g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt);
        return 1;
    }
    else if (type == AINPUT_EVENT_TYPE_MOTION)
    {
        if (menu_active())
        {
            menu_stick(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0),
                       AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
            return 1;
        }

        float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);

        g_android_dpad = 0;
        if (hat_x < -0.5f) g_android_dpad |= 0x04;
        if (hat_x > 0.5f)  g_android_dpad |= 0x08;
        if (hat_y < -0.5f) g_android_dpad |= 0x01;
        if (hat_y > 0.5f)  g_android_dpad |= 0x02;

        float lx_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
        float ly_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
        float rx_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
        float ry_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);

        g_android_lx = (int)(lx_val * 127.0f);
        g_android_ly = (int)(ly_val * 127.0f);
        g_android_rx = (int)(rx_val * 127.0f);
        g_android_ry = (int)(ry_val * 127.0f);

        float lt_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        float rt_val = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        g_android_lt = (int)(lt_val * 255.0f);
        g_android_rt = (int)(rt_val * 255.0f);

        gamepad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                            g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt);
        return 1;
    }
    return 0;
}

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
void host_window_files_dropped(void) {}
void host_window_open_url(const char *url) { (void)url; }

void host_window_init(void) { menu_setup(); }

bool host_window_menu_active(void) { return menu_active(); }
void host_window_menu_draw(void) { menu_draw(); }

static uint32_t android_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

sapp_desc sokol_main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    menu_chdir();
    sys_init();

    if (proc_boot("boot.rp6502", 0, NULL, 0))
    {
        sys_commit();
    }
    else
    {
        menu_open();
    }

    /* Sticks are claimed unconditionally because the motion handler reads
     * AXIS_X, AXIS_Y, AXIS_Z and AXIS_RZ from whatever is attached, and the
     * type is unknown because Android does not say whose labels these are. */
    gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, true);

    /* app_prepare points vga at android_fb and computes a window size. The
     * size is discarded because the window here always opens at 640x480. */
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
        .logger.func = app_log,
    };
}
