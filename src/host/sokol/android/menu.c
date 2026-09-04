/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ROM browser this platform opens with. There is no command line on a
 * phone and no file to drag onto the window, so the list of what is on the
 * card is how a program gets started here: find the folder, scan it, draw it
 * as a text overlay, and boot what the pad selects.
 */

#include "host/sokol/android/menu.h"
#include "host/sokol/app/app.h"
#include "core/hid/gamepad.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h" /* sokol_debugtext.h needs sg_* types declared first */
#include "sokol/sokol_log.h"
#include "sokol/util/sokol_debugtext.h"
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <jni.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
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

void menu_request_permission(void)
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

void menu_scan(void)
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


bool menu_active(void) { return g_android_menu_active; }

void menu_draw(void)
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


/* The menu owns the pad while it is up: navigate, boot, ask for permission,
 * and swallow everything else so a keypress cannot reach the machine behind
 * it. False when the menu is not up and the key is the caller's to decode. */
bool menu_key(int key_code, bool down)
{
    if (!g_android_menu_active)
        return false;
    if (!down)
        return true; /* the menu acts on the press; block the release */
    switch (key_code)
    {
    case AKEYCODE_DPAD_UP:
        if (--g_rom_selected_index < 0)
            g_rom_selected_index = g_rom_count - 1;
        break;
    case AKEYCODE_DPAD_DOWN:
        if (++g_rom_selected_index >= g_rom_count)
            g_rom_selected_index = 0;
        break;
    case AKEYCODE_BUTTON_A:
        if (g_rom_count > 0 && app_boot_rom(g_rom_files[g_rom_selected_index]))
        {
            gamepad_connect(0, true, GAMEPAD_TYPE_UNKNOWN, true);
            g_android_menu_active = false;
        }
        break;
    case AKEYCODE_BUTTON_SELECT:
    case AKEYCODE_BUTTON_START:
    case AKEYCODE_BUTTON_MODE:
        menu_request_permission();
        menu_scan();
        break;
    }
    return true;
}

void menu_setup(void)
{
    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_c64(),
        .logger.func = slog_func,
    });
}

void menu_open(void)
{
    menu_scan();
    g_android_menu_active = true;
}

/* Navigation from a stick or hat, which arrive as absolute axes: act on the
 * crossing, not on every event that reports the same push. */
void menu_stick(float hat_y, float stick_y)
{
    float y = 0.0f;
    if (hat_y < -0.5f || stick_y < -0.5f)
        y = -1.0f;
    else if (hat_y > 0.5f || stick_y > 0.5f)
        y = 1.0f;
    if (y == -1.0f && g_last_menu_y != -1.0f)
    {
        if (--g_rom_selected_index < 0)
            g_rom_selected_index = g_rom_count - 1;
    }
    else if (y == 1.0f && g_last_menu_y != 1.0f)
    {
        if (++g_rom_selected_index >= g_rom_count)
            g_rom_selected_index = 0;
    }
    g_last_menu_y = y;
}

/* Where the ROMs are is also where a program's own files are, so the guest
 * starts there. */
void menu_chdir(void)
{
    detect_rom_directory();
    chdir(g_rom_dir);
}
