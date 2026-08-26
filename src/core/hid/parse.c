/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A HID report descriptor, read once into the four structs the drivers
 * process reports with.
 *
 * Only a machine that meets real USB or Bluetooth devices needs this.
 * The others state their devices outright -- see the Sony controllers in
 * gamepad.c, whose own descriptors lie, and the Pocket's dock -- so they do
 * not link it.
 */

#include "core/hid/parse.h"
#include <string.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_PARSE)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* Per-field info the descriptor walk yields. */
typedef struct
{
    uint32_t app_usage;
    uint16_t usage_page;
    uint16_t usage;
    /* An Array field's slots each hold one usage out of this range, so the
     * range is the field's identity where usage is a Variable field's. */
    uint16_t usage_min;
    uint16_t usage_max;
    uint16_t report_id; // 0xFFFF if no report ID
    uint16_t bit_pos;   // Bit offset within report (excludes report ID byte)
    uint8_t size;       // Field size in bits
    int32_t logical_min;
    int32_t logical_max;
    uint8_t input_flags; // Raw Input item flags byte (bit 1 = variable, bit 2 = relative)
} hid_field_t;

#define HID_FIELD_IS_ARRAY(f) (!((f)->input_flags & 0x02))

// Return false to stop parsing.
typedef bool (*hid_field_cb_t)(const hid_field_t *field, void *context);

static void hid_descriptor_parse(const uint8_t *desc, uint16_t desc_len, hid_field_cb_t cb, void *context)
{
    // Global state
    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint16_t report_id = 0xFFFF;
    uint16_t bit_pos = 0;

    /* The Application Collection in scope, and how deep inside it we are, so
     * a device that nests Physical collections still reports what it is. */
    uint32_t app_usage = HID_APP_NONE;
    uint16_t app_depth = 0;
    uint16_t depth = 0;

    // Local state (cleared after each Main item)
    uint16_t usages[32];
    const uint8_t max_usages = sizeof(usages) / sizeof(usages[0]);
    uint8_t usage_count = 0;
    uint16_t usage_min = 0;
    uint16_t usage_max = 0;
    bool have_usage_range = false;

    uint16_t pos = 0;
    while (pos < desc_len)
    {
        uint8_t b = desc[pos];
        uint8_t sz = b & 0x03;
        if (sz == 3)
            sz = 4;
        uint8_t type = (b >> 2) & 0x03;
        uint8_t tag = (b >> 4) & 0x0F;

        if (pos + 1 + sz > desc_len)
            break;

        uint32_t val = 0;
        for (uint8_t i = 0; i < sz; i++)
            val |= (uint32_t)desc[pos + 1 + i] << (8 * i);
        int32_t sval = (int32_t)val;
        if (sz > 0 && sz < 4 && (val & (1u << (sz * 8 - 1))))
            sval = (int32_t)(val | (~0u << (sz * 8)));

        switch (type)
        {
        case 1: // Global
            switch (tag)
            {
            case 0:
                usage_page = val;
                break;
            case 1:
                logical_min = sval;
                break;
            case 2:
                logical_max = sval;
                break;
            case 7:
                report_size = val;
                break;
            case 8:
                report_id = val;
                bit_pos = 0;
                break;
            case 9:
                report_count = val;
                break;
            }
            break;

        case 2: // Local
            switch (tag)
            {
            case 0: // Usage
                if (usage_count < max_usages)
                    usages[usage_count++] = val;
                break;
            case 1: // Usage Minimum
                usage_min = val;
                have_usage_range = true;
                break;
            case 2: // Usage Maximum
                usage_max = val;
                break;
            }
            break;

        case 0: // Main
            if (tag == 10)
            {
                if (val == 1 && app_usage == HID_APP_NONE) // Application
                {
                    app_usage = ((uint32_t)usage_page << 16) |
                                (usage_count > 0 ? usages[0] : usage_min);
                    app_depth = depth;
                }
                ++depth;
            }
            else if (tag == 12) // End Collection
            {
                if (depth > 0)
                    --depth;
                if (depth == app_depth && app_usage != HID_APP_NONE)
                    app_usage = HID_APP_NONE;
            }
            else if (tag == 8) // Input
            {
                bool is_array = !(val & 0x02);
                uint16_t lo = have_usage_range  ? usage_min
                              : usage_count > 0 ? usages[0]
                                                : 0;
                uint16_t hi = have_usage_range    ? usage_max
                              : usage_count > 0   ? usages[usage_count - 1]
                                                  : 0;

                for (uint32_t i = 0; i < report_count; i++)
                {
                    /* An array's slots all select from the whole range, so
                     * none of them has a usage of its own. A variable field
                     * takes the i'th usage; a range is walked rather than
                     * expanded, because a keyboard bitmap declares 256 of
                     * them and no list here is that long. */
                    uint16_t usage;
                    if (is_array)
                        usage = lo;
                    else if (usage_count > 0)
                        usage = usages[i < usage_count ? i : (uint32_t)usage_count - 1];
                    else if (have_usage_range)
                    {
                        uint32_t u = (uint32_t)usage_min + i;
                        usage = u > (uint32_t)usage_max ? usage_max : (uint16_t)u;
                    }
                    else
                        usage = 0;

                    hid_field_t field = {
                        .app_usage = app_usage,
                        .usage_page = usage_page,
                        .usage = usage,
                        .usage_min = is_array ? lo : usage,
                        .usage_max = is_array ? hi : usage,
                        .report_id = report_id,
                        .bit_pos = bit_pos,
                        .size = report_size,
                        .logical_min = logical_min,
                        .logical_max = logical_max,
                        .input_flags = val,
                    };

                    if (!(val & 1)) // Not constant
                        if (!cb(&field, context))
                            return;

                    bit_pos += report_size;
                }
            }
            // Clear local state after any Main item
            usage_count = 0;
            have_usage_range = false;
            usage_min = 0;
            usage_max = 0;
            break;
        }

        pos += 1 + sz;
    }
}

/* Which report each driver reads. A device may put its keyboard on one
 * report and its mouse on another, so they are chosen apart: for each
 * driver, the first report carrying a field that driver can use. A
 * digitizer outranks a bare X and Y, so a pen or touch panel that also
 * offers a mouse-compatibility collection is decoded as the absolute
 * device it is and not as its relative alias. */
#define HID_NO_REPORT 0xFFFF

typedef struct
{
    uint16_t keyboard, mouse, tablet, digitizer, gamepad;
} hid_choice_t;

static void hid_choose(uint16_t *chosen, uint16_t report_id)
{
    if (*chosen == HID_NO_REPORT)
        *chosen = report_id;
}

static bool hid_choose_field(const hid_field_t *f, void *context)
{
    hid_choice_t *c = (hid_choice_t *)context;
    bool axis = f->usage_page == 0x01 && f->usage >= 0x30 && f->usage <= 0x39;
    bool button = f->usage_page == 0x09 && f->usage >= 1 && f->usage <= GAMEPAD_MAX_BUTTONS;

    if (f->usage_page == 0x07)
        hid_choose(&c->keyboard, f->report_id);
    if (axis || button || (f->usage_page == 0x02 && (f->usage == 0xC4 || f->usage == 0xC5)))
        hid_choose(&c->gamepad, f->report_id);
    if ((f->usage_page == 0x01 && f->usage == 0x30 && (f->input_flags & 0x04)) || button)
        hid_choose(&c->mouse, f->report_id);
    if ((f->usage_page == 0x01 && (f->usage == 0x30 || f->usage == 0x31)) || button ||
        (f->usage_page == 0x0D && (f->usage == 0x42 || f->usage == 0x32)))
        hid_choose(&c->tablet, f->report_id);
    if (f->app_usage == HID_APP_DIGITIZER || f->app_usage == HID_APP_PEN ||
        f->app_usage == HID_APP_TOUCH || (f->usage_page == 0x0D && f->usage == 0x42))
        hid_choose(&c->digitizer, f->report_id);
    return true;
}

/* The four fills. Each takes the first declaration of a field it wants,
 * because a descriptor that says X twice in one report means the first
 * one -- the rest are an alternate collection's view of it. */

typedef struct
{
    hid_parsed_t *out;
    const hid_choice_t *choice;
    uint32_t keyboard_app, mouse_app, gamepad_app;
} hid_fill_t;

static void hid_locate(uint16_t *offset, uint8_t *size, const hid_field_t *f)
{
    if (*size)
        return; // first declaration wins
    *offset = f->bit_pos;
    *size = f->size;
}

static void hid_locate_range(uint16_t *offset, uint8_t *size,
                             int32_t *min, int32_t *max, const hid_field_t *f)
{
    if (*size)
        return;
    *offset = f->bit_pos;
    *size = f->size;
    *min = f->logical_min;
    *max = f->logical_max;
}

static void hid_fill_keyboard(keyboard_connection_t *keyboard, const hid_field_t *f)
{
    if (f->usage_page != 0x07)
        return;
    if (HID_FIELD_IS_ARRAY(f) && f->size == 8)
    {
        // Consecutive slots of one array; a gap starts nothing new.
        if (!keyboard->codes_count)
        {
            keyboard->codes_offset = f->bit_pos;
            keyboard->codes_count = 1;
        }
        else if (f->bit_pos == keyboard->codes_offset + (keyboard->codes_count * 8))
            keyboard->codes_count++;
        return;
    }
    if (HID_FIELD_IS_ARRAY(f) || f->size != 1 || f->usage > 0xFF)
        return;
    /* A bit per usage. Both shapes a keyboard declares -- the modifier
     * byte and an NKRO bitmap -- are runs of consecutive usages one bit
     * apart, so a field either continues the open run or opens a new one. */
    for (int i = 0; i < KEYBOARD_KEY_RUNS; i++)
    {
        keyboard_key_run_t *run = &keyboard->runs[i];
        if (!run->count)
        {
            run->bit_pos = f->bit_pos;
            run->usage_min = f->usage;
            run->count = 1;
            return;
        }
        if (f->bit_pos == run->bit_pos + run->count &&
            f->usage == run->usage_min + run->count)
        {
            run->count++;
            return;
        }
    }
}

static void hid_fill_mou(mouse_connection_t *mouse, const hid_field_t *f)
{
    if (f->usage_page == 0x09)
    {
        if (f->usage >= 1 && f->usage <= 8 && !mouse->button_offsets[f->usage - 1])
            mouse->button_offsets[f->usage - 1] = f->bit_pos;
        return;
    }
    if (f->usage_page == 0x0C && f->usage == 0x238)
        hid_locate(&mouse->pan_offset, &mouse->pan_size, f);
    if (f->usage_page != 0x01)
        return;
    switch (f->usage)
    {
    case 0x30:
        if (!mouse->x_size)
            mouse->x_relative = (f->input_flags & 0x04) != 0;
        hid_locate(&mouse->x_offset, &mouse->x_size, f);
        break;
    case 0x31: hid_locate(&mouse->y_offset, &mouse->y_size, f); break;
    case 0x38: hid_locate(&mouse->wheel_offset, &mouse->wheel_size, f); break;
    }
}

static void hid_fill_tablet(tablet_connection_t *tablet, const hid_field_t *f)
{
    if (f->usage_page == 0x09)
    {
        if (f->usage >= 1 && f->usage <= 5 && tablet->button_offsets[f->usage - 1] == HID_ABSENT)
            tablet->button_offsets[f->usage - 1] = f->bit_pos;
        return;
    }
    if (f->usage_page == 0x0D)
    {
        if (f->usage == 0x42 && tablet->tip_offset == HID_ABSENT)
            tablet->tip_offset = f->bit_pos;
        else if (f->usage == 0x32 && tablet->inrange_offset == HID_ABSENT)
            tablet->inrange_offset = f->bit_pos;
        return;
    }
    if (f->usage_page == 0x0C && f->usage == 0x238)
        hid_locate(&tablet->pan_offset, &tablet->pan_size, f);
    if (f->usage_page != 0x01)
        return;
    switch (f->usage)
    {
    case 0x30:
        if (!tablet->x_size)
            tablet->x_relative = (f->input_flags & 0x04) != 0;
        hid_locate_range(&tablet->x_offset, &tablet->x_size, &tablet->x_min, &tablet->x_max, f);
        break;
    case 0x31: hid_locate_range(&tablet->y_offset, &tablet->y_size, &tablet->y_min, &tablet->y_max, f); break;
    case 0x38: hid_locate(&tablet->wheel_offset, &tablet->wheel_size, f); break;
    }
}

static void hid_fill_gamepad(gamepad_connection_t *gamepad, const hid_field_t *f)
{
    if (f->usage_page == 0x09)
    {
        if (f->usage >= 1 && f->usage <= GAMEPAD_MAX_BUTTONS &&
            gamepad->button_offsets[f->usage - 1] == HID_ABSENT)
            gamepad->button_offsets[f->usage - 1] = f->bit_pos;
        return;
    }
    if (f->usage_page == 0x02) // Simulation: the pedals a wheel reports triggers on
    {
        if (f->usage == 0xC5)
            hid_locate_range(&gamepad->rx_offset, &gamepad->rx_size, &gamepad->rx_min, &gamepad->rx_max, f);
        else if (f->usage == 0xC4)
            hid_locate_range(&gamepad->ry_offset, &gamepad->ry_size, &gamepad->ry_min, &gamepad->ry_max, f);
        return;
    }
    if (f->usage_page != 0x01)
        return;
    switch (f->usage)
    {
    case 0x30: // left stick X
        if (!gamepad->x_size)
            gamepad->x_absolute = !(f->input_flags & 0x04);
        hid_locate_range(&gamepad->x_offset, &gamepad->x_size, &gamepad->x_min, &gamepad->x_max, f);
        break;
    case 0x31: hid_locate_range(&gamepad->y_offset, &gamepad->y_size, &gamepad->y_min, &gamepad->y_max, f); break;
    case 0x32: hid_locate_range(&gamepad->z_offset, &gamepad->z_size, &gamepad->z_min, &gamepad->z_max, f); break;
    case 0x33: hid_locate_range(&gamepad->rx_offset, &gamepad->rx_size, &gamepad->rx_min, &gamepad->rx_max, f); break;
    case 0x34: hid_locate_range(&gamepad->ry_offset, &gamepad->ry_size, &gamepad->ry_min, &gamepad->ry_max, f); break;
    case 0x35: hid_locate_range(&gamepad->rz_offset, &gamepad->rz_size, &gamepad->rz_min, &gamepad->rz_max, f); break;
    case 0x39: hid_locate_range(&gamepad->hat_offset, &gamepad->hat_size, &gamepad->hat_min, &gamepad->hat_max, f); break;
    }
}

static bool hid_fill_field(const hid_field_t *f, void *context)
{
    hid_fill_t *fill = (hid_fill_t *)context;
    const hid_choice_t *c = fill->choice;

    if (f->report_id == c->keyboard)
    {
        if (f->usage_page == 0x07)
            fill->keyboard_app = f->app_usage;
        hid_fill_keyboard(&fill->out->keyboard, f);
    }
    if (f->report_id == c->mouse)
    {
        if (fill->mouse_app == HID_APP_NONE)
            fill->mouse_app = f->app_usage;
        hid_fill_mou(&fill->out->mouse, f);
    }
    if (f->report_id == c->tablet)
        hid_fill_tablet(&fill->out->tablet, f);
    if (f->report_id == c->gamepad)
    {
        if (fill->gamepad_app == HID_APP_NONE)
            fill->gamepad_app = f->app_usage;
        hid_fill_gamepad(&fill->out->gamepad, f);
    }
    return true;
}

void hid_parse(const uint8_t *desc, uint16_t desc_len, hid_parsed_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 5; i++)
        out->tablet.button_offsets[i] = HID_ABSENT;
    out->tablet.tip_offset = HID_ABSENT;
    out->tablet.inrange_offset = HID_ABSENT;
    for (int i = 0; i < GAMEPAD_MAX_BUTTONS; i++)
        out->gamepad.button_offsets[i] = HID_ABSENT;

    hid_choice_t choice = {HID_NO_REPORT, HID_NO_REPORT, HID_NO_REPORT,
                           HID_NO_REPORT, HID_NO_REPORT};
    hid_descriptor_parse(desc, desc_len, hid_choose_field, &choice);
    if (choice.digitizer != HID_NO_REPORT)
        choice.tablet = choice.digitizer;

    hid_fill_t fill = {out, &choice, HID_APP_NONE, HID_APP_NONE, HID_APP_NONE};
    hid_descriptor_parse(desc, desc_len, hid_fill_field, &fill);

    /* A report id of 0xFFFF means the device declared none, and then the
     * report has no leading id byte to skip. */
    out->keyboard.report_id = choice.keyboard == HID_NO_REPORT ? 0 : (uint8_t)choice.keyboard;
    out->mouse.report_id = choice.mouse == HID_NO_REPORT ? 0 : (uint8_t)choice.mouse;
    out->tablet.report_id = choice.tablet == HID_NO_REPORT ? 0 : (uint8_t)choice.tablet;
    out->gamepad.report_id = choice.gamepad == HID_NO_REPORT ? 0 : (uint8_t)choice.gamepad;

    /* What each driver will take. A descriptor that says what it is gets
     * believed; one that does not is judged by what turned up. */
    out->keyboard.valid = out->keyboard.codes_count || out->keyboard.runs[0].count;

    // If it squeaks like a mouse: an X the device moves us by, not to.
    out->mouse.valid = out->mouse.x_size > 0 &&
                     (fill.mouse_app == HID_APP_MOUSE || out->mouse.x_relative);

    /* A relative mouse or an absolute digitizer/pen; not an absolute
     * Generic-Desktop device with no digitizer usage, which is a gamepad's
     * sticks. */
    out->tablet.valid = out->tablet.x_size > 0 && out->tablet.y_size > 0 &&
                     (out->tablet.x_relative || out->tablet.tip_offset != HID_ABSENT ||
                      out->tablet.inrange_offset != HID_ABSENT);

    /* If it creaks like a gamepad. A mouse has buttons and an X too, but
     * its X is relative, and a digital gamepad has no axes at all, so its
     * discrete dpad buttons are what say it isn't a keyboard. */
    bool axes = out->gamepad.x_size || out->gamepad.y_size || out->gamepad.z_size ||
                out->gamepad.rz_size || out->gamepad.rx_size || out->gamepad.ry_size ||
                out->gamepad.hat_size;
    if (fill.gamepad_app == HID_APP_GAMEPAD || fill.gamepad_app == HID_APP_JOYSTICK)
        out->gamepad.valid = axes || out->gamepad.button_offsets[0] != HID_ABSENT;
    else
        out->gamepad.valid = out->gamepad.button_offsets[0] != HID_ABSENT &&
                         (axes ? out->gamepad.x_absolute
                               : out->gamepad.button_offsets[16] != HID_ABSENT);
}
