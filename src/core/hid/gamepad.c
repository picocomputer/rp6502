/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/gamepad.h"
#include "core/sys/xram.h"
#include "machine.h"
#include <string.h>

#if defined(DEBUG_HID) || defined(DEBUG_HID_GAMEPAD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// If you're here to remap HID buttons on a new HID gamepad, create
// a new gamepad_remap_ function and add it to gamepad_distill().

// This is the report we generate for XRAM.
// Direction bits: 0-up, 1-down, 2-left, 3-right
// Feature bits 0x30 are the GAMEPAD_TYPE_ of the face button labels
// Feature bit 0x40 is on when both analog sticks are present
// Feature bit 0x80 is on when valid gamepad connected
typedef struct
{
    uint8_t dpad;    // dpad (0x0F) and feature (0xF0) bits
    uint8_t sticks;  // left (0x0F) and right (0xF0) sticks
    uint8_t button0; // buttons
    uint8_t button1; // buttons
    int8_t lx;       // left analog-stick
    int8_t ly;       // left analog-stick
    int8_t rx;       // right analog-stick
    int8_t ry;       // right analog-stick
    uint8_t lt;      // analog left trigger
    uint8_t rt;      // analog right trigger
} gamepad_xram_t;


// Deadzone is generous enough for moderately worn sticks.
// This is only for the analog to digital conversions so
// it doesn't need to be first-person shooter tight.
#define GAMEPAD_DEADZONE 32

// Room for button0 and button1 plus a dpad if needed.





// Where in XRAM to place reports, 0xFFFF when disabled.
static uint16_t gamepad_xram;

/* The block as it stands, so a change to one field does not need the rest
 * decoded again -- and so a host that decodes its own controller has
 * somewhere to put what it decoded. */
static gamepad_xram_t gamepad_reports[GAMEPAD_MAX_PLAYERS];

// Parsed descriptor structure for fast report parsing.
static gamepad_connection_t gamepad_connections[GAMEPAD_MAX_PLAYERS];

static inline void gamepad_swap_buttons(gamepad_connection_t *conn, int b0, int b1)
{
    uint16_t temp = conn->button_offsets[b0];
    conn->button_offsets[b0] = conn->button_offsets[b1];
    conn->button_offsets[b1] = temp;
}

// These are USB gamepads for the Classic, a remake of the PS1/PSOne.
static void gamepad_remap_playstation_classic(
    gamepad_connection_t *conn, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x054C || product_id != 0x05C2)
        return;
    DBG("Playstation Classic remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
    conn->features = GAMEPAD_FEAT_TYPE(GAMEPAD_TYPE_PLAYSTATION);
    gamepad_swap_buttons(conn, 0, 2); // buttons
    gamepad_swap_buttons(conn, 2, 3); // buttons
    gamepad_swap_buttons(conn, 4, 8); // l1/l2
    gamepad_swap_buttons(conn, 5, 9); // r1/r2
    gamepad_swap_buttons(conn, 4, 6); // l1/bt
    gamepad_swap_buttons(conn, 5, 7); // r1/st
}

// The 8BitDo M30 is a Sega-style gamepad with wonky button mappings.
// It has different L1/R1/L2/R2 mappings for XInput and DInput, which we leave alone.
// Sadly, remapping C/Z into the correct place would mean a confusing third mapping.
// The barrier to a better map is that we can't detect an M30 using a USB Bluetooth adapter.
// The wired DInput mode is unlike any other 8BitDo device so we fix it up here.
static void gamepad_remap_8bitdo_m30(
    gamepad_connection_t *conn, uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id != 0x2DC8 || product_id != 0x5006)
        return;
    DBG("8BitDo M30 remap: vid=0x%04X, pid=0x%04X\n", vendor_id, product_id);
    // Our analog trigger emulation conflicts
    // with the M30's reversed analog triggers.
    conn->rx_size = 0;
    conn->ry_size = 0;
    // home is on 2 because reasons
    gamepad_swap_buttons(conn, 2, GAMEPAD_HOME_BUTTON);
}

// Sony DualShock 4 detection
static bool gamepad_is_sony_ds4(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x05C4: // DualShock 4 (1st gen)
        case 0x09CC: // DualShock 4 (2nd gen)
        case 0x0BA0: // DualShock 4 USB receiver
        case 0x0DAE: // DualShock 4 (special edition variant)
        case 0x0CDA: // DualShock 4 (Asia region, special edition)
        case 0x0D9A: // DualShock 4 (Japan region, special edition)
        case 0x0E04: // DualShock 4 (rare, but reported)
            return true;
        }
    }
    if (vendor_id == 0x0C12) // Zeroplus/Cirka
    {
        switch (product_id)
        {
        case 0x1E1A: // Cirka Wired
        case 0x0E10: // Zeroplus PS4 compatible
        case 0x0E20: // Zeroplus PS4 compatible
            return true;
        }
    }
    if (vendor_id == 0x20D6) // PowerA
    {
        switch (product_id)
        {
        case 0xA711: // PowerA PS4 Wired
            return true;
        }
    }
    if (vendor_id == 0x24C6) // PowerA (formerly BDA, LLC)
    {
        switch (product_id)
        {
        case 0x5501: // PowerA PS4 Wired
            return true;
        }
    }
    if (vendor_id == 0x0F0D) // Hori
    {
        switch (product_id)
        {
        case 0x0055: // Hori PS4 Mini Wired Gamepad
        case 0x005E: // Hori PS4 Mini Wired Gamepad
        case 0x00C5: // Hori PS4 Fighting Commander
        case 0x00D9: // Hori PS4 Fighting Stick Mini
        case 0x00EE: // Hori PS4 Fighting Commander
        case 0x00F6: // Hori PS4 Mini Gamepad
        case 0x00F7: // Hori PS4 Mini Gamepad
            return true;
        }
    }
    return false;
}

// Sony DualSense 5 detection
static bool gamepad_is_sony_ds5(uint16_t vendor_id, uint16_t product_id)
{
    if (vendor_id == 0x054C) // Sony Interactive Entertainment
    {
        switch (product_id)
        {
        case 0x0CE6: // DualSense
        case 0x0DF2: // DualSense Edge
        case 0x0E5C: // DualSense (special edition Spider-Man 2)
        case 0x0E8A: // DualSense (special edition FF16)
        case 0x0E9A: // DualSense (special edition LeBron James)
        case 0x0E6F: // DualSense (special edition Gray Camouflage)
        case 0x0E9C: // DualSense (special edition Volcanic Red)
        case 0x0EA6: // DualSense (special edition Sterling Silver)
        case 0x0EBA: // DualSense (special edition Cobalt Blue)
        case 0x0ED0: // DualSense (special edition Midnight Black V2)
            return true;
        }
    }
    if (vendor_id == 0x0F0D) // Hori (third-party DualSense compatible)
    {
        switch (product_id)
        {
        case 0x0184: // Hori DualSense compatible (Onyx Plus, etc)
        case 0x019C: // Hori Fighting Commander OCTA for PS5
        case 0x01A0: // Hori Fighting Stick α for PS5
            return true;
        }
    }
    return false;
}

// Sony DualShock 4 is HID but presents no descriptor
static const gamepad_connection_t gamepad_desc_sony_ds4 = {
    .valid = true,
    .x_absolute = true,
    .features = GAMEPAD_FEAT_TYPE(GAMEPAD_TYPE_PLAYSTATION),
    .report_id = 1,
    .x_offset = 0 * 8, // left stick X
    .x_size = 8,
    .x_min = 0,
    .x_max = 255,
    .y_offset = 1 * 8, // left stick Y
    .y_size = 8,
    .y_min = 0,
    .y_max = 255,
    .z_offset = 2 * 8, // right stick X
    .z_size = 8,
    .z_min = 0,
    .z_max = 255,
    .rz_offset = 3 * 8, // right stick Y
    .rz_size = 8,
    .rz_min = 0,
    .rz_max = 255,
    .rx_offset = 7 * 8, // L2 trigger
    .rx_size = 8,
    .rx_min = 0,
    .rx_max = 255,
    .ry_offset = 8 * 8, // R2 trigger
    .ry_size = 8,
    .ry_min = 0,
    .ry_max = 255,
    .hat_offset = 4 * 8, // D-pad
    .hat_size = 4,
    .hat_min = 0,
    .hat_max = 7,
    .button_offsets = {
        // X, Circle, Unused, Square, Triangle, Unused, L1, R1
        37, 38, 0xFFFF, 36, 39, 0xFFFF, 40, 41,
        // L2, R2, Share, Options, PS, L3, R3, Touchpad
        42, 43, 44, 45, 48, 46, 47, 49,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

// Sony DualSense 5 is HID but presents no descriptor
static const gamepad_connection_t gamepad_desc_sony_ds5 = {
    .valid = true,
    .x_absolute = true,
    .features = GAMEPAD_FEAT_TYPE(GAMEPAD_TYPE_PLAYSTATION),
    .report_id = 1,
    .x_offset = 0 * 8, // left stick X
    .x_size = 8,
    .x_min = 0,
    .x_max = 255,
    .y_offset = 1 * 8, // left stick Y
    .y_size = 8,
    .y_min = 0,
    .y_max = 255,
    .z_offset = 2 * 8, // right stick X
    .z_size = 8,
    .z_min = 0,
    .z_max = 255,
    .rz_offset = 3 * 8, // right stick Y
    .rz_size = 8,
    .rz_min = 0,
    .rz_max = 255,
    .rx_offset = 4 * 8, // L2 trigger
    .rx_size = 8,
    .rx_min = 0,
    .rx_max = 255,
    .ry_offset = 5 * 8, // R2 trigger
    .ry_size = 8,
    .ry_min = 0,
    .ry_max = 255,
    .hat_offset = 7 * 8, // D-pad
    .hat_size = 4,
    .hat_min = 0,
    .hat_max = 7,
    .button_offsets = {
        // X, Circle, Unused, Square, Triangle, Unused, L1, R1
        61, 62, 0xFFFF, 60, 63, 0xFFFF, 64, 65,
        // L2, R2, Create, Options, PS, L3, R3, Touchpad
        66, 67, 68, 69, 72, 70, 71, 73,
        // Hat buttons computed from HID hat
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}};

static void gamepad_distill(
    gamepad_connection_t *conn, const gamepad_connection_t *desc,
    uint16_t vendor_id, uint16_t product_id, uint8_t button_type)
{
    conn->valid = false;

    // Sony gamepads use a pre-computed descriptor.
    // Some may report a descriptor, which we discard.
    if (gamepad_is_sony_ds4(vendor_id, product_id))
    {
        *conn = gamepad_desc_sony_ds4;
        conn->led_type = GAMEPAD_LED_DS4;
        DBG("Detected Sony DS4 gamepad, using pre-computed descriptor.\n");
    }
    else if (gamepad_is_sony_ds5(vendor_id, product_id))
    {
        *conn = gamepad_desc_sony_ds5;
        conn->led_type = GAMEPAD_LED_DS5;
        DBG("Detected Sony DS5 gamepad, using pre-computed descriptor.\n");
    }
    else
    {
        *conn = *desc;

        // Add your gamepad override here.
        gamepad_remap_8bitdo_m30(conn, vendor_id, product_id);
        gamepad_remap_playstation_classic(conn, vendor_id, product_id);
    }

    if (!conn->valid)
    {
        DBG("HID descriptor not a gamepad.\n");
        return;
    }

    // A device we recognized by id has already labelled itself; otherwise the
    // transport's claim stands, which is GAMEPAD_TYPE_UNKNOWN for generic HID.
    if (!(conn->features & GAMEPAD_FEAT_TYPE_MASK))
        conn->features |= GAMEPAD_FEAT_TYPE(button_type);
    // Both sticks or neither: one stick is not "sticks".
    if (conn->x_size && conn->y_size && conn->z_size && conn->rz_size)
        conn->features |= GAMEPAD_FEAT_STICKS;
    conn->features |= GAMEPAD_FEAT_CONNECTED;
}

static uint8_t gamepad_encode_stick(int8_t x, int8_t y)
{
    // Deadzone check
    if (x >= -GAMEPAD_DEADZONE && x <= GAMEPAD_DEADZONE &&
        y >= -GAMEPAD_DEADZONE && y <= GAMEPAD_DEADZONE)
        return 0; // No direction

    // Get absolute values
    int16_t abs_x = (x < 0) ? -x : x;
    int16_t abs_y = (y < 0) ? -y : y;

    // Use a 2:1 ratio to distinguish cardinal from diagonal
    if (abs_y >= (abs_x * 2))
        return (y < 0) ? 1 : 2; // North : South
    if (abs_x >= (abs_y * 2))
        return (x < 0) ? 4 : 8; // West : East

    // Mixed movement - diagonal
    uint8_t result = 0;
    // Vertical component
    if (y < 0)
        result |= 1; // North
    else
        result |= 2; // South
    // Horizontal component
    if (x < 0)
        result |= 4; // West
    else
        result |= 8; // East

    return result;
}

static void gamepad_parse_report(int player, uint8_t const *data, uint16_t report_len, gamepad_xram_t *report)
{
    // Default empty gamepad report
    memset(report, 0, sizeof(gamepad_xram_t));

    // Add feature bits to dpad
    gamepad_connection_t *conn = &gamepad_connections[player];
    if (conn->valid)
        report->dpad |= conn->features;

    // A blank report was requested
    if (report_len == 0)
        return;

    // Extract analog sticks
    if (conn->x_size > 0)
    {
        uint32_t raw_x = hid_extract_bits(data, report_len, conn->x_offset, conn->x_size);
        report->lx = hid_scale_analog_signed(raw_x, conn->x_size, conn->x_min, conn->x_max);
    }
    if (conn->y_size > 0)
    {
        uint32_t raw_y = hid_extract_bits(data, report_len, conn->y_offset, conn->y_size);
        report->ly = hid_scale_analog_signed(raw_y, conn->y_size, conn->y_min, conn->y_max);
    }
    if (conn->z_size > 0)
    {
        uint32_t raw_z = hid_extract_bits(data, report_len, conn->z_offset, conn->z_size);
        report->rx = hid_scale_analog_signed(raw_z, conn->z_size, conn->z_min, conn->z_max);
    }
    if (conn->rz_size > 0)
    {
        uint32_t raw_rz = hid_extract_bits(data, report_len, conn->rz_offset, conn->rz_size);
        report->ry = hid_scale_analog_signed(raw_rz, conn->rz_size, conn->rz_min, conn->rz_max);
    }

    // Extract triggers
    if (conn->rx_size > 0)
    {
        uint32_t raw_rx = hid_extract_bits(data, report_len, conn->rx_offset, conn->rx_size);
        report->lt = hid_scale_analog(raw_rx, conn->rx_size, conn->rx_min, conn->rx_max);
    }
    if (conn->ry_size > 0)
    {
        uint32_t raw_ry = hid_extract_bits(data, report_len, conn->ry_offset, conn->ry_size);
        report->rt = hid_scale_analog(raw_ry, conn->ry_size, conn->ry_min, conn->ry_max);
    }

    // Extract buttons using individual bit offsets
    uint32_t buttons = 0;
    for (int i = 0; i < GAMEPAD_MAX_BUTTONS; i++)
    {
        if (conn->button_offsets[i] == 0xFFFF)
            continue;
        if (hid_extract_bits(data, report_len, conn->button_offsets[i], 1))
            buttons |= (1UL << i);
    }
    report->button0 = buttons & 0xFF;
    report->button1 = (buttons & 0xFF00) >> 8;

    // Extract D-pad/hat
    if (conn->hat_size == 4 && conn->hat_max - conn->hat_min == 7)
    {
        // Convert HID hat format to individual direction bits
        static const uint8_t hat_to_gamepad[] = {1, 9, 8, 10, 2, 6, 4, 5};
        uint32_t raw_hat = hid_extract_bits(data, report_len, conn->hat_offset, conn->hat_size);
        unsigned index = raw_hat - conn->hat_min;
        if (index < 8)
            report->dpad |= hat_to_gamepad[index];
    }
    else
    {
        // Look for xbone-style discrete dpad buttons in 16-19
        report->dpad |= (buttons & 0xF0000) >> 16;
    }

    // Generate dpad values for sticks
    uint8_t stick_l = gamepad_encode_stick(report->lx, report->ly);
    uint8_t stick_r = gamepad_encode_stick(report->rx, report->ry);
    report->sticks = stick_l | (stick_r << 4);

    // If L2/R2 buttons pressed without any analog movement
    if ((buttons & (1 << 8)) && (report->lt == 0))
        report->lt = 255;
    if ((buttons & (1 << 9)) && (report->rt == 0))
        report->rt = 255;

    // Inject Xbox One home button
    if (conn->home_pressed)
        report->button1 |= (1 << (GAMEPAD_HOME_BUTTON - 8));

    // If L2/R2 analog movement, ensure button press
    if (report->lt > GAMEPAD_DEADZONE)
        report->button1 |= (1 << 0); // L2
    if (report->rt > GAMEPAD_DEADZONE)
        report->button1 |= (1 << 1); // R2
}

void HOST_IN_FLASH("gamepad_init") gamepad_init(void)
{
    gamepad_stop();
}

void gamepad_stop(void)
{
    gamepad_xram = 0xFFFF;
}

static void gamepad_publish(int player)
{
    if (gamepad_xram == 0xFFFF)
        return;
    memcpy((uint8_t *)&xram[gamepad_xram + player * (sizeof(gamepad_xram_t))],
           &gamepad_reports[player], sizeof(gamepad_xram_t));
}

// Provides first and final updates in xram
static void gamepad_reset_xram(int player)
{
    gamepad_parse_report(player, 0, 0, &gamepad_reports[player]); // get blank
    gamepad_publish(player);
}

bool gamepad_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - (sizeof(gamepad_xram_t)) * GAMEPAD_MAX_PLAYERS)
        return false;
    gamepad_xram = word;
    for (int i = 0; i < GAMEPAD_MAX_PLAYERS; i++)
        gamepad_reset_xram(i);
    return true;
}

bool HOST_IN_FLASH("gamepad_mount") gamepad_mount(int slot, const gamepad_connection_t *desc,
                                       uint16_t vendor_id, uint16_t product_id,
                                       uint8_t button_type)
{
    /* A Sony controller is recognized by its ids alone, because the
     * descriptor it offers is wrong; anything else has to have been
     * read as a gamepad already. */
    if (!desc->valid && !gamepad_is_sony_ds4(vendor_id, product_id) &&
        !gamepad_is_sony_ds5(vendor_id, product_id))
        return false;

    gamepad_connection_t *conn = NULL;
    int player;
    for (int i = 0; i < GAMEPAD_MAX_PLAYERS; i++)
    {
        if (!gamepad_connections[i].valid)
        {
            conn = &gamepad_connections[i];
            player = i;
            break;
        }
    }
    if (!conn)
    {
        DBG("gamepad_mount: No available descriptor slots, max players reached\n");
        return false;
    }
    DBG("gamepad_mount: mounting player %d\n", player);

    gamepad_distill(conn, desc, vendor_id, product_id, button_type);
    if (conn->valid)
    {
        conn->slot = slot;
        gamepad_reset_xram(player);
        return true;
    }
    return false;
}

// Useful for gamepads that indicate player number.
int gamepad_get_player_num(int slot)
{
    for (int i = 0; i < GAMEPAD_MAX_PLAYERS; i++)
        if (gamepad_connections[i].slot == slot && gamepad_connections[i].valid)
            return i;
    return -1;
}

bool gamepad_umount(int slot)
{
    int player = gamepad_get_player_num(slot);
    if (player < 0)
        return false;
    gamepad_connection_t *conn = &gamepad_connections[player];
    conn->valid = false;
    gamepad_reset_xram(player);
    return true;
}

void gamepad_report(int slot, uint8_t const *data, uint16_t len)
{
    int player = gamepad_get_player_num(slot);
    if (player < 0)
        return;
    gamepad_connection_t *conn = &gamepad_connections[player];

    const uint8_t *report_data = data;
    uint16_t report_data_len = len;
    if (conn->report_id != 0)
    {
        if (len == 0 || data[0] != conn->report_id)
            return;
        // Skip report ID byte
        report_data = &data[1];
        report_data_len = len - 1;
    }

    gamepad_parse_report(player, report_data, report_data_len, &gamepad_reports[player]);
    gamepad_publish(player);
}

// This is for XBox One/Series gamepads which send
// the home button down a different path.
void gamepad_home_button(int slot, bool pressed)
{
    int player = gamepad_get_player_num(slot);
    if (player < 0)
        return;
    gamepad_connection_t *conn = &gamepad_connections[player];

    // Inject out of band home button into reports
    conn->home_pressed = pressed;

    if (pressed)
        gamepad_reports[player].button1 |= (1 << (GAMEPAD_HOME_BUTTON - 8));
    else
        gamepad_reports[player].button1 &= ~(1 << (GAMEPAD_HOME_BUTTON - 8));
    gamepad_publish(player);
}

// Build LED output report for player indicator on Sony controllers.
// Writes into buf which must be GAMEPAD_LED_REPORT_MAX bytes.
// Sets report_id and report_len. Returns true if a LED report was written.
_Static_assert(GAMEPAD_LED_REPORT_MAX >= 47, "GAMEPAD_LED_REPORT_MAX too small for DS5");
_Static_assert(GAMEPAD_LED_REPORT_MAX >= 31, "GAMEPAD_LED_REPORT_MAX too small for DS4");
bool gamepad_build_led_report(int slot, uint8_t buf[GAMEPAD_LED_REPORT_MAX],
                              uint8_t *report_id, uint16_t *report_len)
{
    int player = gamepad_get_player_num(slot);
    if (player < 0)
        return false;

    gamepad_connection_t *conn = &gamepad_connections[player];

    // Player indicator colors: Blue, Red, Green, Pink
    static const uint8_t player_colors[][3] = {
        {0x00, 0x00, 0x40},
        {0x40, 0x00, 0x00},
        {0x00, 0x40, 0x00},
        {0x20, 0x00, 0x20},
    };

    switch (conn->led_type)
    {
    case GAMEPAD_LED_DS5:
    {
        // DualSense: player indicator LEDs + lightbar color
        // Player LED patterns: P1=center, P2=inner pair, P3=three, P4=four
        static const uint8_t ds5_player_leds[] = {0x04, 0x0A, 0x15, 0x1B};
        memset(buf, 0, 47);
        buf[1] = 0x14;                      // valid_flag1: player LEDs (0x10) + lightbar (0x04)
        buf[38] = 0x02;                     // valid_flag2: lightbar setup
        buf[43] = ds5_player_leds[player];  // player LED pattern
        buf[44] = player_colors[player][0]; // R
        buf[45] = player_colors[player][1]; // G
        buf[46] = player_colors[player][2]; // B
        *report_id = 2;
        *report_len = 47;
        return true;
    }
    case GAMEPAD_LED_DS4:
    {
        // DualShock 4: lightbar color for player indication
        memset(buf, 0, 31);
        buf[0] = 0xFF;                     // enable all features
        buf[5] = player_colors[player][0]; // R
        buf[6] = player_colors[player][1]; // G
        buf[7] = player_colors[player][2]; // B
        *report_id = 5;
        *report_len = 31;
        return true;
    }
    default:
        return false;
    }
}

bool gamepad_is_mapped(void)
{
    return gamepad_xram != 0xFFFF;
}

/* Where a flat button id sits in a report: which field, and which bit. */
static bool gamepad_button_loc(gamepad_button_t button, int *field, uint8_t *mask)
{
    enum { GAMEPAD_F_DPAD, GAMEPAD_F_BUTTON0, GAMEPAD_F_BUTTON1 };
    switch (button)
    {
    case GAMEPAD_BTN_DPAD_UP:    *field = GAMEPAD_F_DPAD;    *mask = 0x01; return true;
    case GAMEPAD_BTN_DPAD_DOWN:  *field = GAMEPAD_F_DPAD;    *mask = 0x02; return true;
    case GAMEPAD_BTN_DPAD_LEFT:  *field = GAMEPAD_F_DPAD;    *mask = 0x04; return true;
    case GAMEPAD_BTN_DPAD_RIGHT: *field = GAMEPAD_F_DPAD;    *mask = 0x08; return true;
    case GAMEPAD_BTN_A:          *field = GAMEPAD_F_BUTTON0; *mask = 0x01; return true;
    case GAMEPAD_BTN_B:          *field = GAMEPAD_F_BUTTON0; *mask = 0x02; return true;
    case GAMEPAD_BTN_C:          *field = GAMEPAD_F_BUTTON0; *mask = 0x04; return true;
    case GAMEPAD_BTN_X:          *field = GAMEPAD_F_BUTTON0; *mask = 0x08; return true;
    case GAMEPAD_BTN_Y:          *field = GAMEPAD_F_BUTTON0; *mask = 0x10; return true;
    case GAMEPAD_BTN_Z:          *field = GAMEPAD_F_BUTTON0; *mask = 0x20; return true;
    case GAMEPAD_BTN_L1:         *field = GAMEPAD_F_BUTTON0; *mask = 0x40; return true;
    case GAMEPAD_BTN_R1:         *field = GAMEPAD_F_BUTTON0; *mask = 0x80; return true;
    case GAMEPAD_BTN_L2:         *field = GAMEPAD_F_BUTTON1; *mask = 0x01; return true;
    case GAMEPAD_BTN_R2:         *field = GAMEPAD_F_BUTTON1; *mask = 0x02; return true;
    case GAMEPAD_BTN_SELECT:     *field = GAMEPAD_F_BUTTON1; *mask = 0x04; return true;
    case GAMEPAD_BTN_START:      *field = GAMEPAD_F_BUTTON1; *mask = 0x08; return true;
    case GAMEPAD_BTN_HOME:       *field = GAMEPAD_F_BUTTON1; *mask = 0x10; return true;
    case GAMEPAD_BTN_L3:         *field = GAMEPAD_F_BUTTON1; *mask = 0x20; return true;
    case GAMEPAD_BTN_R3:         *field = GAMEPAD_F_BUTTON1; *mask = 0x40; return true;
    }
    return false;
}

void gamepad_button_apply(gamepad_button_t button, bool down,
                          uint8_t *dpad, uint8_t *button0, uint8_t *button1)
{
    int field;
    uint8_t mask;
    if (!gamepad_button_loc(button, &field, &mask))
        return;
    uint8_t *target = field == 0 ? dpad : field == 1 ? button0 : button1;
    if (down)
        *target |= mask;
    else
        *target &= (uint8_t)~mask;
}

void gamepad_connect(int player, bool connected, uint8_t type, bool sticks)
{
    if (player < 0 || player >= GAMEPAD_PLAYERS)
        return;
    gamepad_xram_t *report = &gamepad_reports[player];
    if (connected)
        report->dpad = (uint8_t)((report->dpad & 0x0F) | GAMEPAD_FEAT_CONNECTED |
                                 GAMEPAD_FEAT_TYPE(type) | (sticks ? GAMEPAD_FEAT_STICKS : 0));
    else
        memset(report, 0, sizeof(*report)); // a blank record is unplugged
    gamepad_publish(player);
}

void gamepad_hid_set(int player, gamepad_button_t button, bool down)
{
    if (player < 0 || player >= GAMEPAD_PLAYERS)
        return;
    gamepad_xram_t *report = &gamepad_reports[player];
    uint8_t dpad = report->dpad & 0x0F;
    gamepad_button_apply(button, down, &dpad, &report->button0, &report->button1);
    report->dpad = (uint8_t)((report->dpad & 0xF0) | (dpad & 0x0F));
    gamepad_publish(player);
}

void gamepad_host_report(int player, uint8_t dpad, uint8_t button0, uint8_t button1,
                         int lx, int ly, int rx, int ry, int lt, int rt)
{
    if (player < 0 || player >= GAMEPAD_PLAYERS)
        return;
    gamepad_xram_t *report = &gamepad_reports[player];

    /* The analog triggers and the L2/R2 buttons imply each other, the same
     * way a parsed report makes them: a digital press with no analog reads
     * full scale, and past-deadzone analog asserts the button. */
    if ((button1 & 0x01) && lt == 0)
        lt = 255;
    if ((button1 & 0x02) && rt == 0)
        rt = 255;
    if (lt > GAMEPAD_DEADZONE)
        button1 |= 0x01;
    if (rt > GAMEPAD_DEADZONE)
        button1 |= 0x02;

    report->dpad = (uint8_t)((report->dpad & 0xF0) | (dpad & 0x0F));
    report->button0 = button0;
    report->button1 = button1;
    report->lx = (int8_t)lx;
    report->ly = (int8_t)ly;
    report->rx = (int8_t)rx;
    report->ry = (int8_t)ry;
    report->lt = (uint8_t)lt;
    report->rt = (uint8_t)rt;
    report->sticks = (uint8_t)(gamepad_encode_stick(report->lx, report->ly) |
                               (gamepad_encode_stick(report->rx, report->ry) << 4));
    gamepad_publish(player);
}
