/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows gamepads, which take two backends because Windows has two kinds.
 *
 * XInput reports a fixed Xbox layout and is the only way to read an
 * Xbox-class gamepad properly: that same device also presents a HID collection,
 * but its collection shares one axis between the two triggers for DirectInput
 * compatibility, so L2 and R2 would be indistinguishable there. Everything
 * else — DualSense, DualShock, Switch Pro, arcade sticks, no-name USB gamepads —
 * is invisible to XInput and is read here as raw HID instead.
 *
 * The HID half needs no mapping database, because hid.dll parses the report
 * descriptor for us and hands back Button 1..n and the Generic Desktop axes.
 * That is the same thing core/hid/hid.c hands core/hid/gamepad.c, so the two file
 * the same usages in the same places and a no-name gamepad behaves the same here
 * as it does plugged into the real machine.
 */

#include "core/hid/gamepad.h"
#include "host/sokol/app/entry.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* hid.dll's functions return NTSTATUS, which is a driver-kit type that
 * windows.h does not declare and that hidpi.h does not declare for itself. */
#ifndef _NTDEF_
#define _NTDEF_
typedef LONG NTSTATUS;
#endif

#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Frames between looking for controllers we have not seen. An XInputGetState
 * on an empty slot costs about a millisecond on the older runtimes, so four of
 * them every frame is a visible cost on a 16ms budget. */
#define GAMEPAD_WIN_RESCAN 60

/* ---------------------------------------------------------------- XInput -- */

#define GAMEPAD_XIN_MAX 4

typedef struct
{
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT thumb_lx;
    SHORT thumb_ly;
    SHORT thumb_rx;
    SHORT thumb_ry;
} gamepad_xin_gamepad_t;

typedef struct
{
    DWORD packet;
    gamepad_xin_gamepad_t gamepad;
} gamepad_xin_state_t;

typedef DWORD(WINAPI *gamepad_xin_get_state_t)(DWORD, gamepad_xin_state_t *);

#define GAMEPAD_XIN_DPAD_UP 0x0001
#define GAMEPAD_XIN_DPAD_DOWN 0x0002
#define GAMEPAD_XIN_DPAD_LEFT 0x0004
#define GAMEPAD_XIN_DPAD_RIGHT 0x0008
#define GAMEPAD_XIN_START 0x0010
#define GAMEPAD_XIN_BACK 0x0020
#define GAMEPAD_XIN_LEFT_THUMB 0x0040
#define GAMEPAD_XIN_RIGHT_THUMB 0x0080
#define GAMEPAD_XIN_LEFT_SHOULDER 0x0100
#define GAMEPAD_XIN_RIGHT_SHOULDER 0x0200
#define GAMEPAD_XIN_GUIDE 0x0400 /* only the undocumented entry point reports it */
#define GAMEPAD_XIN_A 0x1000
#define GAMEPAD_XIN_B 0x2000
#define GAMEPAD_XIN_X 0x4000
#define GAMEPAD_XIN_Y 0x8000

static HMODULE gamepad_xin_dll;
static gamepad_xin_get_state_t gamepad_xin_get_state;
static bool gamepad_xin_connected[GAMEPAD_XIN_MAX];
static int gamepad_xin_probe;

static void gamepad_xin_open(void)
{
    /* Newest first. The ordinal 100 entry is XInputGetStateEx, which is the
     * only one that reports the Guide button; it is undocumented and absent
     * from every header, so it is asked for by number and the documented
     * function stands in when it is missing. */
    static const wchar_t *const names[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !gamepad_xin_dll; i++)
        gamepad_xin_dll = LoadLibraryW(names[i]);
    if (!gamepad_xin_dll)
        return;
    gamepad_xin_get_state =
        (gamepad_xin_get_state_t)(void *)GetProcAddress(gamepad_xin_dll, (LPCSTR)100);
    if (!gamepad_xin_get_state)
        gamepad_xin_get_state =
            (gamepad_xin_get_state_t)(void *)GetProcAddress(gamepad_xin_dll, "XInputGetState");
}

static void gamepad_xin_close(void)
{
    if (gamepad_xin_dll)
        FreeLibrary(gamepad_xin_dll);
    gamepad_xin_dll = NULL;
    gamepad_xin_get_state = NULL;
    memset(gamepad_xin_connected, 0, sizeof(gamepad_xin_connected));
    gamepad_xin_probe = 0;
}

/* Inverted after scaling rather than before: negating the raw -32768 would
 * overflow, and negating it as -1-value leaves a centred stick reading -1. */
static int8_t gamepad_xin_stick(SHORT value, bool invert)
{
    int scaled = value >> 8; /* -32768..32767 is exactly -128..127 */
    if (invert)
        scaled = -scaled;
    if (scaled > 127)
        scaled = 127;
    if (scaled < -128)
        scaled = -128;
    return (int8_t)scaled;
}

static int gamepad_xin_poll(gamepad_host_t *gamepads, int max)
{
    if (!gamepad_xin_get_state)
        return 0;
    bool probe_empty = --gamepad_xin_probe <= 0;
    if (probe_empty)
        gamepad_xin_probe = GAMEPAD_WIN_RESCAN;

    int count = 0;
    for (DWORD slot = 0; slot < GAMEPAD_XIN_MAX && count < max; slot++)
    {
        if (!gamepad_xin_connected[slot] && !probe_empty)
            continue;
        gamepad_xin_state_t state;
        memset(&state, 0, sizeof(state));
        if (gamepad_xin_get_state(slot, &state) != ERROR_SUCCESS)
        {
            gamepad_xin_connected[slot] = false;
            continue;
        }
        gamepad_xin_connected[slot] = true;

        gamepad_host_t *gamepad = &gamepads[count++];
        memset(gamepad, 0, sizeof(*gamepad));
        /* An XInput slot number is stable while the gamepad is in it. Kept clear
         * of the HID half's ids, which are hashes of a device path. */
        gamepad->id = 1 + slot;
        gamepad->type = GAMEPAD_TYPE_WESTERN; /* XUSB is an Xbox layout by construction */
        gamepad->sticks = true;

        WORD b = state.gamepad.buttons;
        static const struct
        {
            WORD bit;
            gamepad_button_t button;
        } map[] = {
            {GAMEPAD_XIN_DPAD_UP, GAMEPAD_BTN_DPAD_UP},
            {GAMEPAD_XIN_DPAD_DOWN, GAMEPAD_BTN_DPAD_DOWN},
            {GAMEPAD_XIN_DPAD_LEFT, GAMEPAD_BTN_DPAD_LEFT},
            {GAMEPAD_XIN_DPAD_RIGHT, GAMEPAD_BTN_DPAD_RIGHT},
            {GAMEPAD_XIN_A, GAMEPAD_BTN_A},
            {GAMEPAD_XIN_B, GAMEPAD_BTN_B},
            {GAMEPAD_XIN_X, GAMEPAD_BTN_X},
            {GAMEPAD_XIN_Y, GAMEPAD_BTN_Y},
            {GAMEPAD_XIN_LEFT_SHOULDER, GAMEPAD_BTN_L1},
            {GAMEPAD_XIN_RIGHT_SHOULDER, GAMEPAD_BTN_R1},
            {GAMEPAD_XIN_BACK, GAMEPAD_BTN_SELECT},
            {GAMEPAD_XIN_START, GAMEPAD_BTN_START},
            {GAMEPAD_XIN_GUIDE, GAMEPAD_BTN_HOME},
            {GAMEPAD_XIN_LEFT_THUMB, GAMEPAD_BTN_L3},
            {GAMEPAD_XIN_RIGHT_THUMB, GAMEPAD_BTN_R3},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            gamepad_button_apply(map[i].button, (b & map[i].bit) != 0,
                             &gamepad->dpad, &gamepad->button0, &gamepad->button1);

        gamepad->lx = gamepad_xin_stick(state.gamepad.thumb_lx, false);
        gamepad->rx = gamepad_xin_stick(state.gamepad.thumb_rx, false);
        /* XInput's Y is up-positive and the report's is down-positive. */
        gamepad->ly = gamepad_xin_stick(state.gamepad.thumb_ly, true);
        gamepad->ry = gamepad_xin_stick(state.gamepad.thumb_ry, true);
        gamepad->lt = state.gamepad.left_trigger;
        gamepad->rt = state.gamepad.right_trigger;
    }
    return count;
}

/* ------------------------------------------------------------- raw HID -- */

#define GAMEPAD_HID_MAX 4
#define GAMEPAD_HID_REPORT_MAX 256

/* The Generic Desktop usages core/hid/gamepad.c reads, in the same roles. */
enum
{
    GAMEPAD_HID_X,  /* left stick X  */
    GAMEPAD_HID_Y,  /* left stick Y  */
    GAMEPAD_HID_Z,  /* right stick X */
    GAMEPAD_HID_RZ, /* right stick Y */
    GAMEPAD_HID_RX, /* left trigger  */
    GAMEPAD_HID_RY, /* right trigger */
    GAMEPAD_HID_HAT,
    GAMEPAD_HID_VALUE_COUNT,
};

static const USAGE gamepad_hid_usage[GAMEPAD_HID_VALUE_COUNT] = {
    0x30, 0x31, 0x32, 0x35, 0x33, 0x34, 0x39};

typedef struct
{
    HANDLE file;
    OVERLAPPED overlapped;
    bool reading;
    uint64_t id;
    PHIDP_PREPARSED_DATA preparsed;
    USHORT report_len;
    USHORT button_caps_len;
    bool has[GAMEPAD_HID_VALUE_COUNT];
    LONG min[GAMEPAD_HID_VALUE_COUNT];
    LONG max[GAMEPAD_HID_VALUE_COUNT];
    USHORT bits[GAMEPAD_HID_VALUE_COUNT];
    BYTE report[GAMEPAD_HID_REPORT_MAX];
    gamepad_host_t state;
} gamepad_hid_t;

static gamepad_hid_t gamepad_hids[GAMEPAD_HID_MAX];
static int gamepad_hid_rescan;

static uint64_t gamepad_hid_hash(const wchar_t *text)
{
    uint64_t hash = 1469598103934665603ull; /* FNV-1a, so a device path is an id */
    for (; *text; text++)
    {
        hash ^= (uint64_t)*text;
        hash *= 1099511628211ull;
    }
    return hash | 0x8000000000000000ull; /* never collides with an XInput slot */
}

static void gamepad_hid_close_one(gamepad_hid_t *hid)
{
    if (hid->reading)
        CancelIo(hid->file);
    if (hid->preparsed)
        HidD_FreePreparsedData(hid->preparsed);
    if (hid->overlapped.hEvent)
        CloseHandle(hid->overlapped.hEvent);
    if (hid->file && hid->file != INVALID_HANDLE_VALUE)
        CloseHandle(hid->file);
    memset(hid, 0, sizeof(*hid));
}

/* hid.c's scaling, over the ranges hid.dll read out of the descriptor. */
static uint8_t gamepad_hid_scale(LONG value, LONG min, LONG max)
{
    if (max <= min)
        return 128;
    if (value < min)
        value = min;
    if (value > max)
        value = max;
    return (uint8_t)(((int64_t)(value - min) * 255) / (max - min));
}

static bool gamepad_hid_open_one(gamepad_hid_t *hid, const wchar_t *path, uint64_t id)
{
    memset(hid, 0, sizeof(*hid));
    hid->file = CreateFileW(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hid->file == INVALID_HANDLE_VALUE)
    {
        hid->file = NULL;
        return false; /* another process may hold it exclusively; not ours to fix */
    }

    HIDP_CAPS caps;
    if (!HidD_GetPreparsedData(hid->file, &hid->preparsed) ||
        HidP_GetCaps(hid->preparsed, &caps) != HIDP_STATUS_SUCCESS ||
        caps.InputReportByteLength == 0 ||
        caps.InputReportByteLength > GAMEPAD_HID_REPORT_MAX)
    {
        gamepad_hid_close_one(hid);
        return false;
    }
    hid->report_len = caps.InputReportByteLength;
    hid->button_caps_len = caps.NumberInputButtonCaps;

    HIDP_VALUE_CAPS values[64];
    USHORT value_count = sizeof values / sizeof values[0];
    if (HidP_GetValueCaps(HidP_Input, values, &value_count, hid->preparsed) ==
        HIDP_STATUS_SUCCESS)
        for (USHORT i = 0; i < value_count; i++)
        {
            if (values[i].UsagePage != HID_USAGE_PAGE_GENERIC || values[i].IsRange)
                continue;
            for (int slot = 0; slot < GAMEPAD_HID_VALUE_COUNT; slot++)
                if (values[i].NotRange.Usage == gamepad_hid_usage[slot])
                {
                    hid->has[slot] = true;
                    hid->min[slot] = values[i].LogicalMin;
                    hid->max[slot] = values[i].LogicalMax;
                    hid->bits[slot] = values[i].BitSize;
                }
        }

    hid->state.sticks = hid->has[GAMEPAD_HID_X] && hid->has[GAMEPAD_HID_Y] &&
                        hid->has[GAMEPAD_HID_Z] && hid->has[GAMEPAD_HID_RZ];

    /* Only the vendors whose labels are not in doubt. */
    HIDD_ATTRIBUTES attributes;
    attributes.Size = sizeof(attributes);
    if (HidD_GetAttributes(hid->file, &attributes))
        switch (attributes.VendorID)
        {
        case 0x054C: hid->state.type = GAMEPAD_TYPE_PLAYSTATION; break;
        case 0x045E: hid->state.type = GAMEPAD_TYPE_WESTERN; break;
        case 0x057E: hid->state.type = GAMEPAD_TYPE_EASTERN; break;
        }

    hid->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hid->overlapped.hEvent)
    {
        gamepad_hid_close_one(hid);
        return false;
    }
    hid->id = id;
    return true;
}

static void gamepad_hid_parse(gamepad_hid_t *hid)
{
    gamepad_host_t *state = &hid->state;
    state->dpad = state->button0 = state->button1 = 0;

    /* Button n at index n-1, then the same two bytes plus a d-pad the
     * firmware packs them into. */
    USAGE usages[64];
    ULONG usage_count = sizeof usages / sizeof usages[0];
    if (hid->button_caps_len &&
        HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages, &usage_count,
                       hid->preparsed, (PCHAR)hid->report,
                       hid->report_len) == HIDP_STATUS_SUCCESS)
        for (ULONG i = 0; i < usage_count; i++)
        {
            static const gamepad_button_t map[] = {
                GAMEPAD_BTN_A, GAMEPAD_BTN_B, GAMEPAD_BTN_C, GAMEPAD_BTN_X,
                GAMEPAD_BTN_Y, GAMEPAD_BTN_Z, GAMEPAD_BTN_L1, GAMEPAD_BTN_R1,
                GAMEPAD_BTN_L2, GAMEPAD_BTN_R2, GAMEPAD_BTN_SELECT, GAMEPAD_BTN_START,
                GAMEPAD_BTN_HOME, GAMEPAD_BTN_L3, GAMEPAD_BTN_R3};
            unsigned index = usages[i] - 1u;
            if (index < sizeof map / sizeof map[0])
                gamepad_button_apply(map[index], true, &state->dpad,
                                     &state->button0, &state->button1);
            /* Usages 17-20 are the discrete d-pad an Xbox-style descriptor
             * uses instead of a hat, the same place gamepad.c reads them. */
            else if (index >= 16 && index <= 19)
                gamepad_button_apply((gamepad_button_t)(GAMEPAD_BTN_DPAD_UP + (index - 16)),
                                 true, &state->dpad, &state->button0,
                                 &state->button1);
        }

    for (int slot = 0; slot < GAMEPAD_HID_VALUE_COUNT; slot++)
    {
        if (!hid->has[slot])
            continue;
        ULONG raw = 0;
        if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                               gamepad_hid_usage[slot], &raw, hid->preparsed,
                               (PCHAR)hid->report,
                               hid->report_len) != HIDP_STATUS_SUCCESS)
            continue;
        /* HidP hands back the field's bits, not its value: an axis declared
         * with a negative logical minimum arrives zero-extended and has to be
         * signed here, the same way core/hid/hid.c does it. */
        LONG value = (LONG)raw;
        if (hid->min[slot] < 0 && hid->bits[slot] > 0 && hid->bits[slot] < 32 &&
            (raw & (1ul << (hid->bits[slot] - 1))))
            value = (LONG)(raw | (0xFFFFFFFFul << hid->bits[slot]));

        if (slot == GAMEPAD_HID_HAT)
        {
            /* gamepad.c's hat table: N, NE, E, SE, S, SW, W, NW. */
            static const uint8_t hat_to_dpad[8] = {1, 9, 8, 10, 2, 6, 4, 5};
            LONG index = value - hid->min[slot];
            if (hid->max[slot] - hid->min[slot] == 7 && index >= 0 && index < 8)
                state->dpad |= hat_to_dpad[index];
            continue;
        }
        uint8_t scaled = gamepad_hid_scale(value, hid->min[slot], hid->max[slot]);
        switch (slot)
        {
        case GAMEPAD_HID_X: state->lx = (int8_t)(scaled - 128); break;
        case GAMEPAD_HID_Y: state->ly = (int8_t)(scaled - 128); break;
        case GAMEPAD_HID_Z: state->rx = (int8_t)(scaled - 128); break;
        case GAMEPAD_HID_RZ: state->ry = (int8_t)(scaled - 128); break;
        case GAMEPAD_HID_RX: state->lt = scaled; break;
        case GAMEPAD_HID_RY: state->rt = scaled; break;
        }
    }
}

static bool gamepad_hid_holds(uint64_t id)
{
    for (int i = 0; i < GAMEPAD_HID_MAX; i++)
        if (gamepad_hids[i].file && gamepad_hids[i].id == id)
            return true;
    return false;
}

/* Raw input's device list, which needs no window — only WM_INPUT delivery
 * does, and the reports are read from the device directly instead. */
static void gamepad_hid_scan(void)
{
    UINT count = 0;
    if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || !count)
        return;
    RAWINPUTDEVICELIST *list =
        (RAWINPUTDEVICELIST *)calloc(count, sizeof(RAWINPUTDEVICELIST));
    if (!list)
        return;
    if (GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
    {
        free(list);
        return;
    }

    for (UINT i = 0; i < count; i++)
    {
        if (list[i].dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO info;
        UINT size = info.cbSize = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) ==
                (UINT)-1 ||
            info.hid.usUsagePage != HID_USAGE_PAGE_GENERIC ||
            (info.hid.usUsage != HID_USAGE_GENERIC_GAMEPAD &&
             info.hid.usUsage != HID_USAGE_GENERIC_JOYSTICK))
            continue;

        wchar_t path[512];
        size = sizeof path / sizeof path[0];
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path, &size) ==
            (UINT)-1)
            continue;
        /* An XInput device is in this list too, wearing its DirectInput
         * shim. Microsoft's own way of spotting one is this substring, and
         * the XInput half above reads it properly. */
        if (wcsstr(path, L"IG_"))
            continue;

        uint64_t id = gamepad_hid_hash(path);
        if (gamepad_hid_holds(id))
            continue;
        for (int slot = 0; slot < GAMEPAD_HID_MAX; slot++)
            if (!gamepad_hids[slot].file &&
                gamepad_hid_open_one(&gamepad_hids[slot], path, id))
                break;
    }
    free(list);
}

static int gamepad_hid_poll(gamepad_host_t *gamepads, int max)
{
    if (gamepad_hid_rescan-- <= 0)
    {
        gamepad_hid_rescan = GAMEPAD_WIN_RESCAN;
        gamepad_hid_scan();
    }

    for (int i = 0; i < GAMEPAD_HID_MAX; i++)
    {
        gamepad_hid_t *hid = &gamepad_hids[i];
        if (!hid->file)
            continue;
        /* Drain what has arrived so the state is this frame's, but a device
         * reporting faster than we ask is not allowed to hold the frame. */
        for (int drain = 0; drain < 32; drain++)
        {
            if (!hid->reading)
            {
                ResetEvent(hid->overlapped.hEvent);
                if (!ReadFile(hid->file, hid->report, hid->report_len, NULL,
                              &hid->overlapped))
                {
                    if (GetLastError() != ERROR_IO_PENDING)
                    {
                        gamepad_hid_close_one(hid); /* unplugged */
                        break;
                    }
                }
                hid->reading = true;
            }
            DWORD got = 0;
            if (!GetOverlappedResult(hid->file, &hid->overlapped, &got, FALSE))
            {
                if (GetLastError() != ERROR_IO_INCOMPLETE)
                {
                    gamepad_hid_close_one(hid);
                }
                break; /* nothing new this frame; the read stays outstanding */
            }
            hid->reading = false;
            if (got)
                gamepad_hid_parse(hid);
        }
    }

    int count = 0;
    for (int i = 0; i < GAMEPAD_HID_MAX && count < max; i++)
        if (gamepad_hids[i].file)
        {
            gamepads[count] = gamepad_hids[i].state;
            gamepads[count].id = gamepad_hids[i].id;
            count++;
        }
    return count;
}

/* ----------------------------------------------------------------- seam -- */

bool host_gamepad_open(void)
{
    gamepad_xin_open();
    gamepad_xin_probe = 0;
    gamepad_hid_scan();
    gamepad_hid_rescan = GAMEPAD_WIN_RESCAN;
    return true; /* nothing plugged in yet is ordinary */
}

void host_gamepad_close(void)
{
    gamepad_xin_close();
    for (int i = 0; i < GAMEPAD_HID_MAX; i++)
        gamepad_hid_close_one(&gamepad_hids[i]);
}

int host_gamepad_poll(gamepad_host_t *gamepads, int max)
{
    int count = gamepad_xin_poll(gamepads, max);
    return count + gamepad_hid_poll(gamepads + count, max - count);
}
