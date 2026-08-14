/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windows gamepads, which take two backends because Windows has two kinds.
 *
 * XInput reports a fixed Xbox layout and is the only way to read an
 * Xbox-class pad properly: that same device also presents a HID collection,
 * but its collection shares one axis between the two triggers for DirectInput
 * compatibility, so L2 and R2 would be indistinguishable there. Everything
 * else — DualSense, DualShock, Switch Pro, arcade sticks, no-name USB pads —
 * is invisible to XInput and is read here as raw HID instead.
 *
 * The HID half needs no mapping database, because hid.dll parses the report
 * descriptor for us and hands back Button 1..n and the Generic Desktop axes.
 * That is the same thing ria/hid/hid.c hands ria/hid/pad.c, so the two file
 * the same usages in the same places and a no-name pad behaves the same here
 * as it does plugged into the real machine.
 */

#include "emu/app/pad_input.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Frames between looking for controllers we have not seen. An XInputGetState
 * on an empty slot costs about a millisecond on the older runtimes, so four of
 * them every frame is a visible cost on a 16ms budget. */
#define PAD_WIN_RESCAN 60

/* ---------------------------------------------------------------- XInput -- */

#define PAD_XIN_MAX 4

typedef struct
{
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT thumb_lx;
    SHORT thumb_ly;
    SHORT thumb_rx;
    SHORT thumb_ry;
} pad_xin_gamepad_t;

typedef struct
{
    DWORD packet;
    pad_xin_gamepad_t gamepad;
} pad_xin_state_t;

typedef DWORD(WINAPI *pad_xin_get_state_t)(DWORD, pad_xin_state_t *);

#define PAD_XIN_DPAD_UP 0x0001
#define PAD_XIN_DPAD_DOWN 0x0002
#define PAD_XIN_DPAD_LEFT 0x0004
#define PAD_XIN_DPAD_RIGHT 0x0008
#define PAD_XIN_START 0x0010
#define PAD_XIN_BACK 0x0020
#define PAD_XIN_LEFT_THUMB 0x0040
#define PAD_XIN_RIGHT_THUMB 0x0080
#define PAD_XIN_LEFT_SHOULDER 0x0100
#define PAD_XIN_RIGHT_SHOULDER 0x0200
#define PAD_XIN_GUIDE 0x0400 /* only the undocumented entry point reports it */
#define PAD_XIN_A 0x1000
#define PAD_XIN_B 0x2000
#define PAD_XIN_X 0x4000
#define PAD_XIN_Y 0x8000

static HMODULE pad_xin_dll;
static pad_xin_get_state_t pad_xin_get_state;
static bool pad_xin_connected[PAD_XIN_MAX];
static int pad_xin_probe;

static void pad_xin_open(void)
{
    /* Newest first. The ordinal 100 entry is XInputGetStateEx, which is the
     * only one that reports the Guide button; it is undocumented and absent
     * from every header, so it is asked for by number and the documented
     * function stands in when it is missing. */
    static const wchar_t *const names[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !pad_xin_dll; i++)
        pad_xin_dll = LoadLibraryW(names[i]);
    if (!pad_xin_dll)
        return;
    pad_xin_get_state =
        (pad_xin_get_state_t)(void *)GetProcAddress(pad_xin_dll, (LPCSTR)100);
    if (!pad_xin_get_state)
        pad_xin_get_state =
            (pad_xin_get_state_t)(void *)GetProcAddress(pad_xin_dll, "XInputGetState");
}

static void pad_xin_close(void)
{
    if (pad_xin_dll)
        FreeLibrary(pad_xin_dll);
    pad_xin_dll = NULL;
    pad_xin_get_state = NULL;
    memset(pad_xin_connected, 0, sizeof(pad_xin_connected));
    pad_xin_probe = 0;
}

static int8_t pad_xin_stick(SHORT value)
{
    return (int8_t)(value >> 8); /* -32768..32767 is exactly -128..127 */
}

static int pad_xin_poll(pad_host_t *pads, int max)
{
    if (!pad_xin_get_state)
        return 0;
    bool probe_empty = --pad_xin_probe <= 0;
    if (probe_empty)
        pad_xin_probe = PAD_WIN_RESCAN;

    int count = 0;
    for (DWORD slot = 0; slot < PAD_XIN_MAX && count < max; slot++)
    {
        if (!pad_xin_connected[slot] && !probe_empty)
            continue;
        pad_xin_state_t state;
        memset(&state, 0, sizeof(state));
        if (pad_xin_get_state(slot, &state) != ERROR_SUCCESS)
        {
            pad_xin_connected[slot] = false;
            continue;
        }
        pad_xin_connected[slot] = true;

        pad_host_t *pad = &pads[count++];
        memset(pad, 0, sizeof(*pad));
        /* An XInput slot number is stable while the pad is in it. Kept clear
         * of the HID half's ids, which are hashes of a device path. */
        pad->id = 1 + slot;
        pad->type = PAD_TYPE_WESTERN; /* XUSB is an Xbox layout by construction */
        pad->sticks = true;

        WORD b = state.gamepad.buttons;
        static const struct
        {
            WORD bit;
            pad_button_t button;
        } map[] = {
            {PAD_XIN_DPAD_UP, PAD_BTN_DPAD_UP},
            {PAD_XIN_DPAD_DOWN, PAD_BTN_DPAD_DOWN},
            {PAD_XIN_DPAD_LEFT, PAD_BTN_DPAD_LEFT},
            {PAD_XIN_DPAD_RIGHT, PAD_BTN_DPAD_RIGHT},
            {PAD_XIN_A, PAD_BTN_A},
            {PAD_XIN_B, PAD_BTN_B},
            {PAD_XIN_X, PAD_BTN_X},
            {PAD_XIN_Y, PAD_BTN_Y},
            {PAD_XIN_LEFT_SHOULDER, PAD_BTN_L1},
            {PAD_XIN_RIGHT_SHOULDER, PAD_BTN_R1},
            {PAD_XIN_BACK, PAD_BTN_SELECT},
            {PAD_XIN_START, PAD_BTN_START},
            {PAD_XIN_GUIDE, PAD_BTN_HOME},
            {PAD_XIN_LEFT_THUMB, PAD_BTN_L3},
            {PAD_XIN_RIGHT_THUMB, PAD_BTN_R3},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            pad_button_apply(map[i].button, (b & map[i].bit) != 0,
                             &pad->dpad, &pad->button0, &pad->button1);

        pad->lx = pad_xin_stick(state.gamepad.thumb_lx);
        pad->rx = pad_xin_stick(state.gamepad.thumb_rx);
        /* XInput's Y is up-positive and the report's is down-positive. */
        pad->ly = pad_xin_stick((SHORT)(-1 - state.gamepad.thumb_ly));
        pad->ry = pad_xin_stick((SHORT)(-1 - state.gamepad.thumb_ry));
        pad->lt = state.gamepad.left_trigger;
        pad->rt = state.gamepad.right_trigger;
    }
    return count;
}

/* ------------------------------------------------------------- raw HID -- */

#define PAD_HID_MAX 4
#define PAD_HID_REPORT_MAX 256

/* The Generic Desktop usages ria/hid/pad.c reads, in the same roles. */
enum
{
    PAD_HID_X,  /* left stick X  */
    PAD_HID_Y,  /* left stick Y  */
    PAD_HID_Z,  /* right stick X */
    PAD_HID_RZ, /* right stick Y */
    PAD_HID_RX, /* left trigger  */
    PAD_HID_RY, /* right trigger */
    PAD_HID_HAT,
    PAD_HID_VALUE_COUNT,
};

static const USAGE pad_hid_usage[PAD_HID_VALUE_COUNT] = {
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
    bool has[PAD_HID_VALUE_COUNT];
    LONG min[PAD_HID_VALUE_COUNT];
    LONG max[PAD_HID_VALUE_COUNT];
    BYTE report[PAD_HID_REPORT_MAX];
    pad_host_t state;
} pad_hid_t;

static pad_hid_t pad_hids[PAD_HID_MAX];
static int pad_hid_rescan;

static uint64_t pad_hid_hash(const wchar_t *text)
{
    uint64_t hash = 1469598103934665603ull; /* FNV-1a, so a device path is an id */
    for (; *text; text++)
    {
        hash ^= (uint64_t)*text;
        hash *= 1099511628211ull;
    }
    return hash | 0x8000000000000000ull; /* never collides with an XInput slot */
}

static void pad_hid_close_one(pad_hid_t *hid)
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
static uint8_t pad_hid_scale(LONG value, LONG min, LONG max)
{
    if (max <= min)
        return 128;
    if (value < min)
        value = min;
    if (value > max)
        value = max;
    return (uint8_t)(((int64_t)(value - min) * 255) / (max - min));
}

static bool pad_hid_open_one(pad_hid_t *hid, const wchar_t *path, uint64_t id)
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
        caps.InputReportByteLength > PAD_HID_REPORT_MAX)
    {
        pad_hid_close_one(hid);
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
            for (int slot = 0; slot < PAD_HID_VALUE_COUNT; slot++)
                if (values[i].NotRange.Usage == pad_hid_usage[slot])
                {
                    hid->has[slot] = true;
                    hid->min[slot] = values[i].LogicalMin;
                    hid->max[slot] = values[i].LogicalMax;
                }
        }

    hid->state.sticks = hid->has[PAD_HID_X] && hid->has[PAD_HID_Y] &&
                        hid->has[PAD_HID_Z] && hid->has[PAD_HID_RZ];

    /* Only the vendors whose labels are not in doubt. */
    HIDD_ATTRIBUTES attributes;
    attributes.Size = sizeof(attributes);
    if (HidD_GetAttributes(hid->file, &attributes))
        switch (attributes.VendorID)
        {
        case 0x054C: hid->state.type = PAD_TYPE_PLAYSTATION; break;
        case 0x045E: hid->state.type = PAD_TYPE_WESTERN; break;
        case 0x057E: hid->state.type = PAD_TYPE_EASTERN; break;
        }

    hid->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hid->overlapped.hEvent)
    {
        pad_hid_close_one(hid);
        return false;
    }
    hid->id = id;
    return true;
}

static void pad_hid_parse(pad_hid_t *hid)
{
    pad_host_t *state = &hid->state;
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
            static const pad_button_t map[] = {
                PAD_BTN_A, PAD_BTN_B, PAD_BTN_C, PAD_BTN_X,
                PAD_BTN_Y, PAD_BTN_Z, PAD_BTN_L1, PAD_BTN_R1,
                PAD_BTN_L2, PAD_BTN_R2, PAD_BTN_SELECT, PAD_BTN_START,
                PAD_BTN_HOME, PAD_BTN_L3, PAD_BTN_R3};
            unsigned index = usages[i] - 1u;
            if (index < sizeof map / sizeof map[0])
                pad_button_apply(map[index], true, &state->dpad,
                                 &state->button0, &state->button1);
            /* Usages 17-20 are the discrete d-pad an Xbox-style descriptor
             * uses instead of a hat, the same place pad.c reads them. */
            else if (index >= 16 && index <= 19)
                pad_button_apply((pad_button_t)(PAD_BTN_DPAD_UP + (index - 16)),
                                 true, &state->dpad, &state->button0,
                                 &state->button1);
        }

    for (int slot = 0; slot < PAD_HID_VALUE_COUNT; slot++)
    {
        if (!hid->has[slot])
            continue;
        ULONG raw = 0;
        if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0,
                               pad_hid_usage[slot], &raw, hid->preparsed,
                               (PCHAR)hid->report,
                               hid->report_len) != HIDP_STATUS_SUCCESS)
            continue;
        if (slot == PAD_HID_HAT)
        {
            /* pad.c's hat table: N, NE, E, SE, S, SW, W, NW. */
            static const uint8_t hat_to_dpad[8] = {1, 9, 8, 10, 2, 6, 4, 5};
            LONG index = (LONG)raw - hid->min[slot];
            if (hid->max[slot] - hid->min[slot] == 7 && index >= 0 && index < 8)
                state->dpad |= hat_to_dpad[index];
            continue;
        }
        uint8_t scaled = pad_hid_scale((LONG)raw, hid->min[slot], hid->max[slot]);
        switch (slot)
        {
        case PAD_HID_X: state->lx = (int8_t)(scaled - 128); break;
        case PAD_HID_Y: state->ly = (int8_t)(scaled - 128); break;
        case PAD_HID_Z: state->rx = (int8_t)(scaled - 128); break;
        case PAD_HID_RZ: state->ry = (int8_t)(scaled - 128); break;
        case PAD_HID_RX: state->lt = scaled; break;
        case PAD_HID_RY: state->rt = scaled; break;
        }
    }
}

static bool pad_hid_holds(uint64_t id)
{
    for (int i = 0; i < PAD_HID_MAX; i++)
        if (pad_hids[i].file && pad_hids[i].id == id)
            return true;
    return false;
}

/* Raw input's device list, which needs no window — only WM_INPUT delivery
 * does, and the reports are read from the device directly instead. */
static void pad_hid_scan(void)
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

        uint64_t id = pad_hid_hash(path);
        if (pad_hid_holds(id))
            continue;
        for (int slot = 0; slot < PAD_HID_MAX; slot++)
            if (!pad_hids[slot].file &&
                pad_hid_open_one(&pad_hids[slot], path, id))
                break;
    }
    free(list);
}

static int pad_hid_poll(pad_host_t *pads, int max)
{
    if (pad_hid_rescan-- <= 0)
    {
        pad_hid_rescan = PAD_WIN_RESCAN;
        pad_hid_scan();
    }

    for (int i = 0; i < PAD_HID_MAX; i++)
    {
        pad_hid_t *hid = &pad_hids[i];
        if (!hid->file)
            continue;
        for (;;)
        {
            if (!hid->reading)
            {
                ResetEvent(hid->overlapped.hEvent);
                if (!ReadFile(hid->file, hid->report, hid->report_len, NULL,
                              &hid->overlapped))
                {
                    if (GetLastError() != ERROR_IO_PENDING)
                    {
                        pad_hid_close_one(hid); /* unplugged */
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
                    pad_hid_close_one(hid);
                }
                break; /* nothing new this frame; the read stays outstanding */
            }
            hid->reading = false;
            if (got)
                pad_hid_parse(hid);
        }
    }

    int count = 0;
    for (int i = 0; i < PAD_HID_MAX && count < max; i++)
        if (pad_hids[i].file)
        {
            pads[count] = pad_hids[i].state;
            pads[count].id = pad_hids[i].id;
            count++;
        }
    return count;
}

/* ----------------------------------------------------------------- seam -- */

bool host_pad_open(void)
{
    pad_xin_open();
    pad_xin_probe = 0;
    pad_hid_rescan = 0;
    pad_hid_scan();
    return true; /* nothing plugged in yet is ordinary */
}

void host_pad_close(void)
{
    pad_xin_close();
    for (int i = 0; i < PAD_HID_MAX; i++)
        pad_hid_close_one(&pad_hids[i]);
}

int host_pad_poll(pad_host_t *pads, int max)
{
    int count = pad_xin_poll(pads, max);
    return count + pad_hid_poll(pads + count, max - count);
}
