/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Android window host: the sokol entry (sokol_main returns a sapp_desc — sokol's
 * NativeActivity glue owns the real entry point), the native gamepad/menu input
 * hook, the on-screen ROM-select menu (sdtx overlay + storage scanning + JNI
 * all-files-access permission), and the host_window_* hooks. The render/frame/
 * present pipeline is in app/window_core.c.
 */

#include "emu/app/window.h"
#include "emu/app/window_core.h"
#include "sokol_app.h"
#include "sokol_gfx.h" /* sokol_debugtext.h needs sg_* types declared first */
#include "sokol_log.h"
#include "util/sokol_debugtext.h"
#include "emu/hid/pad.h"
#include "emu/main.h"
#include "emu/host/rom.h"
#include "emu/sys/cpu.h"
#include "emu/sys/vga.h"
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
static char g_rom_files[MAX_ROMS][ROM_NAME_MAX];
static int g_rom_count = 0;
static int g_rom_selected_index = 0;
static bool g_android_menu_active = false;
static float g_last_menu_y = 0.0f;
static char g_rom_dir[256] = "";

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

static void android_request_storage_permission(void)
{
    ANativeActivity* activity = (ANativeActivity*)sapp_android_get_native_activity();
    if (!activity) return;

    JavaVM* jvm = activity->vm;
    JNIEnv* env = NULL;
    (*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6);
    if (!env)
    {
        (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    }
    if (!env) return;

    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");

    jstring action_str = (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
    jstring uri_str = (*env)->NewStringUTF(env, "package:com.picocomputer.rp6502");

    jmethodID uri_parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jobject uri_obj = (*env)->CallStaticObjectMethod(env, uri_class, uri_parse, uri_str);

    jmethodID intent_init = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;)V");
    jobject intent_obj = (*env)->NewObject(env, intent_class, intent_init, action_str);

    jmethodID set_data_method = (*env)->GetMethodID(env, intent_class, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;");
    (*env)->CallObjectMethod(env, intent_obj, set_data_method, uri_obj);

    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID start_activity_method = (*env)->GetMethodID(env, activity_class, "startActivity", "(Landroid/content/Intent;)V");
    (*env)->CallVoidMethod(env, activity->clazz, start_activity_method, intent_obj);
}

static void detect_rom_directory(void)
{
    // 1. Try physical SD Card first: scan /storage/
    DIR* dir = opendir("/storage");
    if (dir)
    {
        struct dirent* de;
        while ((de = readdir(dir)) != NULL)
        {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0 ||
                strcmp(de->d_name, "self") == 0 ||
                strcmp(de->d_name, "emulated") == 0)
            {
                continue;
            }

            // Try /storage/ID/Download/rp6502
            char path[512];
            snprintf(path, sizeof(path), "/storage/%s/Download/rp6502", de->d_name);
            DIR* d = opendir(path);
            if (d)
            {
                closedir(d);
                strncpy(g_rom_dir, path, sizeof(g_rom_dir) - 1);
                g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
                closedir(dir);
                return;
            }

            // Try /storage/ID/rp6502
            snprintf(path, sizeof(path), "/storage/%s/rp6502", de->d_name);
            d = opendir(path);
            if (d)
            {
                closedir(d);
                strncpy(g_rom_dir, path, sizeof(g_rom_dir) - 1);
                g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
                closedir(dir);
                return;
            }
        }
        closedir(dir);
    }

    // 2. Try internal storage Download/rp6502 next
    DIR* d = opendir("/sdcard/Download/rp6502");
    if (d)
    {
        closedir(d);
        strcpy(g_rom_dir, "/sdcard/Download/rp6502");
        return;
    }

    // 3. Try to create internal storage Download/rp6502
    if (mkdir("/sdcard/Download/rp6502", 0777) == 0 || errno == EEXIST)
    {
        d = opendir("/sdcard/Download/rp6502");
        if (d)
        {
            closedir(d);
            strcpy(g_rom_dir, "/sdcard/Download/rp6502");
            return;
        }
    }

    // 4. Fallback to app internal data path
    const void* native_act = sapp_android_get_native_activity();
    if (native_act)
    {
        ANativeActivity* activity = (ANativeActivity*)native_act;
        if (activity->internalDataPath)
        {
            strncpy(g_rom_dir, activity->internalDataPath, sizeof(g_rom_dir) - 1);
            g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
            return;
        }
        else if (activity->externalDataPath)
        {
            strncpy(g_rom_dir, activity->externalDataPath, sizeof(g_rom_dir) - 1);
            g_rom_dir[sizeof(g_rom_dir) - 1] = '\0';
            return;
        }
    }

    // Absolute fallback
    strcpy(g_rom_dir, ".");
}

static void android_scan_roms(void)
{
    detect_rom_directory();
    chdir(g_rom_dir);

    g_rom_count = 0;
    DIR* d = opendir(".");
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL)
    {
        size_t len = strlen(de->d_name);
        if (de->d_name[0] != '.' && len > 7 && strcasecmp(de->d_name + len - 7, ".rp6502") == 0)
        {
            strncpy(g_rom_files[g_rom_count], de->d_name, ROM_NAME_MAX - 1);
            g_rom_files[g_rom_count][ROM_NAME_MAX - 1] = '\0';
            g_rom_count++;
            if (g_rom_count >= MAX_ROMS) break;
        }
    }
    closedir(d);
}

bool rp6502_android_input_hook(const void* native_event)
{
    const AInputEvent* event = (const AInputEvent*)native_event;
    int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY)
    {
        int32_t key_code = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        bool down = (action == AKEY_EVENT_ACTION_DOWN);

        // Handle menu navigation if menu is active
        if (g_android_menu_active)
        {
            if (down)
            {
                switch (key_code)
                {
                    case AKEYCODE_DPAD_UP:
                        g_rom_selected_index--;
                        if (g_rom_selected_index < 0) g_rom_selected_index = g_rom_count - 1;
                        return 1;
                    case AKEYCODE_DPAD_DOWN:
                        g_rom_selected_index++;
                        if (g_rom_selected_index >= g_rom_count) g_rom_selected_index = 0;
                        return 1;
                    case AKEYCODE_BUTTON_A:
                        if (g_rom_count > 0 &&
                            window_core_boot_rom(g_rom_files[g_rom_selected_index]))
                        {
                            // Reset key and button states to prevent stuck inputs after closing the menu
                            g_android_button0 = 0;
                            g_android_button1 = 0;
                            g_android_dpad = 0;
                            g_android_lx = 0;
                            g_android_ly = 0;
                            g_android_rx = 0;
                            g_android_ry = 0;
                            g_android_lt = 0;
                            g_android_rt = 0;
                            pad_connect(0, true);
                            g_android_menu_active = false;
                        }
                        return 1;
                    case AKEYCODE_BUTTON_SELECT:
                    case AKEYCODE_BUTTON_START:
                    case AKEYCODE_BUTTON_MODE:
                        // Request permission page and refresh the list
                        android_request_storage_permission();
                        android_scan_roms();
                        return 1;
                }
            }
            // Block all other keys from propagating when menu is active
            return 1;
        }

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
                    g_android_menu_active = true;
                    android_scan_roms();
                }
                break;
            case AKEYCODE_BUTTON_START:
                if (down) g_android_button1 |= 0x08; else g_android_button1 &= ~0x08;
                // Toggle ROM select menu when SELECT + START are both pressed
                if (down && (g_android_button1 & 0x04))
                {
                    g_android_menu_active = true;
                    android_scan_roms();
                }
                break;
            case AKEYCODE_BUTTON_MODE: // Home button
                if (down) g_android_button1 |= 0x10; else g_android_button1 &= ~0x10;
                if (down)
                {
                    g_android_menu_active = true;
                    android_scan_roms();
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
        pad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                        g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt, false);
        return 1; // Handled
    }
    else if (type == AINPUT_EVENT_TYPE_MOTION)
    {
        // Handle menu navigation if menu is active
        if (g_android_menu_active)
        {
            float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
            float stick_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);

            float input_y = 0.0f;
            if (hat_y < -0.5f || stick_y < -0.5f) input_y = -1.0f;
            else if (hat_y > 0.5f || stick_y > 0.5f) input_y = 1.0f;

            if (input_y == -1.0f && g_last_menu_y != -1.0f)
            {
                g_rom_selected_index--;
                if (g_rom_selected_index < 0) g_rom_selected_index = g_rom_count - 1;
            }
            else if (input_y == 1.0f && g_last_menu_y != 1.0f)
            {
                g_rom_selected_index++;
                if (g_rom_selected_index >= g_rom_count) g_rom_selected_index = 0;
            }
            g_last_menu_y = input_y;
            return 1; // Consume event
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

        pad_host_report(0, g_android_dpad, g_android_button0, g_android_button1,
                        g_android_lx, g_android_ly, g_android_rx, g_android_ry,
                        g_android_lt, g_android_rt, false);
        return 1; // Handled
    }
    return 0; // Not handled
}

void host_window_resize(int w, int h) { (void)w, (void)h; }
void host_window_set_aspect_hint(int cw, int ch) { (void)cw, (void)ch; }
void host_window_files_dropped(void) {} /* sokol has no Android drag-n-drop */

void host_window_init(void)
{
    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_c64(),
        .logger.func = slog_func,
    });
}

bool host_window_menu_active(void) { return g_android_menu_active; }

void host_window_menu_draw(void)
{
    if (g_android_menu_active)
    {
        sdtx_canvas(320.0f, 240.0f);
        sdtx_origin(2.0f, 2.0f);
        sdtx_color3b(255, 255, 0); // Yellow
        sdtx_puts("PICOCOMPUTER 6502 - ROM SELECT\n");
        sdtx_puts("==============================\n\n");

        if (g_rom_count == 0)
        {
            sdtx_color3b(255, 100, 100); // Red
            sdtx_puts("No ROM files (.rp6502) found.\n\n");
            sdtx_color3b(200, 200, 200);
            sdtx_puts("Please copy ROMs to folder:\n");
            sdtx_printf("%s/\n\n", g_rom_dir);
            sdtx_color3b(255, 255, 0); // Yellow
            sdtx_puts("Press SELECT/START/HOME to request\n");
            sdtx_puts("SD Card folder access permission");
        }
        else
        {
            sdtx_color3b(200, 200, 200);
            for (int i = 0; i < g_rom_count; i++)
            {
                if (i == g_rom_selected_index)
                {
                    sdtx_color3b(100, 255, 100); // Green selection cursor
                    sdtx_printf("> %s\n", g_rom_files[i]);
                    sdtx_color3b(200, 200, 200);
                }
                else
                {
                    sdtx_printf("  %s\n", g_rom_files[i]);
                }
            }
            sdtx_puts("\n\nUse DPAD Up/Down to navigate\n");
            sdtx_puts("Press A to Boot Selected ROM");
        }
    }
    sdtx_draw();
}

// Global framebuffer for Android
static uint32_t android_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

sapp_desc sokol_main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    detect_rom_directory();
    chdir(g_rom_dir);

    // Initialize the drivers once; the machine is started per-program (main_run).
    main_init();

    // Try to load a default rom (boot.rp6502) if it exists, otherwise activate the menu
    if (rom_load("boot.rp6502"))
    {
        g_android_menu_active = false;
        main_run(); // start the boot ROM
    }
    else
    {
        g_android_menu_active = true;
        cpu_set_halted(true); // no program yet — hold until the menu boots one
        android_scan_roms();
    }

    // Connect gamepad player 0
    pad_connect(0, true);

    // Seed the core's window/render state (also sets the vga framebuffer to
    // android_fb). Android opens at a fixed 640x480, so the computed size is unused.
    int win_w, win_h;
    window_core_prepare(android_fb, 1.0, false, true, false, &win_w, &win_h);

    return (sapp_desc){
        .init_cb = window_core_init,
        .frame_cb = window_core_frame,
        .event_cb = window_core_event,
        .cleanup_cb = window_core_cleanup,
        .android = {
            .native_event_cb = rp6502_android_input_hook,
        },
        .width = 640,
        .height = 480,
        .window_title = "Picocomputer 6502",
        .logger.func = slog_func,
    };
}
