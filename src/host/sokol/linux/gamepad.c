/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Linux gamepads, through evdev. The kernel's drivers name every button and
 * axis, over USB and Bluetooth alike, so there is no mapping database here,
 * though the drivers differ on where X and Y are. The usual layout is the
 * kernel's gamepad API (Documentation/input/gamepad.rst), which is not the
 * HID layout core/hid/gamepad.c parses: the triggers are ABS_Z and ABS_RZ, and
 * the right stick is ABS_RX and ABS_RY.
 */

#include "core/hid/gamepad.h"
#include "host/sokol/app/entry.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define GAMEPAD_LINUX_RESCAN 60

#define GAMEPAD_BIT_LONGS(n) (((n) + (8 * sizeof(long)) - 1) / (8 * sizeof(long)))
#define GAMEPAD_BIT_TEST(bits, n) ((bits)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long))) & 1)

enum
{
    GAMEPAD_AXIS_LX,
    GAMEPAD_AXIS_LY,
    GAMEPAD_AXIS_RX,
    GAMEPAD_AXIS_RY,
    GAMEPAD_AXIS_LT,
    GAMEPAD_AXIS_RT,
    GAMEPAD_AXIS_COUNT,
};

static const uint16_t gamepad_axis_code[GAMEPAD_AXIS_COUNT] = {
    ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};

/* A pad whose driver passes its HID usages through, such as an Xbox pad on
 * Bluetooth under hid-microsoft, puts its right stick on Z and Rz when its
 * triggers are Brake and Accelerator, as core/hid/parse.c reads it. */
static const uint16_t gamepad_axis_code_pedal[GAMEPAD_AXIS_COUNT] = {
    ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_BRAKE, ABS_GAS};

typedef struct
{
    int fd;
    uint64_t id;
    const uint16_t *axis_code;
    bool present[GAMEPAD_AXIS_COUNT];
    int32_t min[GAMEPAD_AXIS_COUNT];
    int32_t max[GAMEPAD_AXIS_COUNT];
    gamepad_host_t state;
} gamepad_device_t;

static gamepad_device_t gamepad_devices[GAMEPAD_PLAYERS];
static int gamepad_rescan;

static uint8_t gamepad_scale(int32_t value, int32_t min, int32_t max)
{
    if (max <= min)
        return 128;
    if (value < min)
        value = min;
    if (value > max)
        value = max;
    return (uint8_t)(((int64_t)(value - min) * 255) / (max - min));
}

static void gamepad_apply_axis(gamepad_device_t *dev, int axis, int32_t value)
{
    uint8_t scaled = gamepad_scale(value, dev->min[axis], dev->max[axis]);
    switch (axis)
    {
    case GAMEPAD_AXIS_LX: dev->state.lx = (int8_t)(scaled - 128); break;
    case GAMEPAD_AXIS_LY: dev->state.ly = (int8_t)(scaled - 128); break;
    case GAMEPAD_AXIS_RX: dev->state.rx = (int8_t)(scaled - 128); break;
    case GAMEPAD_AXIS_RY: dev->state.ry = (int8_t)(scaled - 128); break;
    case GAMEPAD_AXIS_LT: dev->state.lt = scaled; break;
    case GAMEPAD_AXIS_RT: dev->state.rt = scaled; break;
    }
}

static void gamepad_apply_button(gamepad_device_t *dev, uint16_t code, bool down)
{
    gamepad_button_t button;
    switch (code)
    {
    /* BTN_X is BTN_NORTH and BTN_Y is BTN_WEST. xpad and hid-generic send X
     * and Y by label, so an Xbox X is reported as BTN_NORTH. Sony's drivers send
     * Square and Triangle by position, and hid-nintendo sends a Switch
     * controller's A and B by position. */
    case BTN_SOUTH: button = dev->state.type == GAMEPAD_TYPE_EASTERN ? GAMEPAD_BTN_B : GAMEPAD_BTN_A; break;
    case BTN_EAST: button = dev->state.type == GAMEPAD_TYPE_EASTERN ? GAMEPAD_BTN_A : GAMEPAD_BTN_B; break;
    case BTN_C: button = GAMEPAD_BTN_C; break;
    case BTN_X: button = dev->state.type == GAMEPAD_TYPE_PLAYSTATION ? GAMEPAD_BTN_Y : GAMEPAD_BTN_X; break;
    case BTN_Y: button = dev->state.type == GAMEPAD_TYPE_PLAYSTATION ? GAMEPAD_BTN_X : GAMEPAD_BTN_Y; break;
    case BTN_Z: button = GAMEPAD_BTN_Z; break;
    case BTN_TL: button = GAMEPAD_BTN_L1; break;
    case BTN_TR: button = GAMEPAD_BTN_R1; break;
    case BTN_TL2: button = GAMEPAD_BTN_L2; break;
    case BTN_TR2: button = GAMEPAD_BTN_R2; break;
    case BTN_SELECT: button = GAMEPAD_BTN_SELECT; break;
    case BTN_START: button = GAMEPAD_BTN_START; break;
    case BTN_MODE: button = GAMEPAD_BTN_HOME; break;
    case BTN_THUMBL: button = GAMEPAD_BTN_L3; break;
    case BTN_THUMBR: button = GAMEPAD_BTN_R3; break;
    case BTN_DPAD_UP: button = GAMEPAD_BTN_DPAD_UP; break;
    case BTN_DPAD_DOWN: button = GAMEPAD_BTN_DPAD_DOWN; break;
    case BTN_DPAD_LEFT: button = GAMEPAD_BTN_DPAD_LEFT; break;
    case BTN_DPAD_RIGHT: button = GAMEPAD_BTN_DPAD_RIGHT; break;
    default:
        /* hid-generic numbers a HID Joystick's buttons from BTN_JOYSTICK, and
         * those codes give no positions, so the buttons fill the report in
         * that order, as on the Pico. */
        if (code < BTN_JOYSTICK || code > BTN_JOYSTICK + (GAMEPAD_BTN_R3 - GAMEPAD_BTN_A))
            return;
        button = (gamepad_button_t)(GAMEPAD_BTN_A + (code - BTN_JOYSTICK));
        break;
    }
    gamepad_button_apply(button, down, &dev->state.dpad,
                         &dev->state.button0, &dev->state.button1);
}

static void gamepad_apply_hat(gamepad_device_t *dev, uint16_t code, int32_t value)
{
    if (code == ABS_HAT0X)
    {
        gamepad_button_apply(GAMEPAD_BTN_DPAD_LEFT, value < 0, &dev->state.dpad,
                             &dev->state.button0, &dev->state.button1);
        gamepad_button_apply(GAMEPAD_BTN_DPAD_RIGHT, value > 0, &dev->state.dpad,
                             &dev->state.button0, &dev->state.button1);
    }
    else
    {
        gamepad_button_apply(GAMEPAD_BTN_DPAD_UP, value < 0, &dev->state.dpad,
                             &dev->state.button0, &dev->state.button1);
        gamepad_button_apply(GAMEPAD_BTN_DPAD_DOWN, value > 0, &dev->state.dpad,
                             &dev->state.button0, &dev->state.button1);
    }
}

static void gamepad_close_device(gamepad_device_t *dev)
{
    if (dev->fd >= 0)
        close(dev->fd);
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
}

/* A /dev/input node has to be opened before its capabilities can be read, so
 * a device that reports neither BTN_SOUTH nor BTN_TRIGGER is closed again here
 * rather than holding one of the four gamepad_devices slots. A mouse with more
 * than 16 buttons has codes up to BTN_TRIGGER, and its sixteenth button,
 * BTN_JOYSTICK - 1, marks it as a mouse. */
static bool gamepad_open_device(gamepad_device_t *dev, const char *path, uint64_t id)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;

    unsigned long keys[GAMEPAD_BIT_LONGS(KEY_CNT)];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
        !(GAMEPAD_BIT_TEST(keys, BTN_SOUTH) ||
          (GAMEPAD_BIT_TEST(keys, BTN_TRIGGER) && !GAMEPAD_BIT_TEST(keys, BTN_JOYSTICK - 1))))
    {
        close(fd);
        return false;
    }

    memset(dev, 0, sizeof(*dev));
    dev->fd = fd;
    dev->id = id;

    unsigned long axes[GAMEPAD_BIT_LONGS(ABS_CNT)];
    memset(axes, 0, sizeof(axes));
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(axes)), axes);
    dev->axis_code = GAMEPAD_BIT_TEST(axes, ABS_BRAKE) && GAMEPAD_BIT_TEST(axes, ABS_GAS)
                         ? gamepad_axis_code_pedal
                         : gamepad_axis_code;
    for (int axis = 0; axis < GAMEPAD_AXIS_COUNT; axis++)
    {
        struct input_absinfo info;
        if (!GAMEPAD_BIT_TEST(axes, dev->axis_code[axis]) ||
            ioctl(fd, EVIOCGABS(dev->axis_code[axis]), &info) < 0)
            continue;
        dev->present[axis] = true;
        dev->min[axis] = info.minimum;
        dev->max[axis] = info.maximum;
        gamepad_apply_axis(dev, axis, info.value);
    }
    for (uint16_t code = ABS_HAT0X; code <= ABS_HAT0Y; code++)
    {
        struct input_absinfo info;
        if (GAMEPAD_BIT_TEST(axes, code) && ioctl(fd, EVIOCGABS(code), &info) >= 0)
            gamepad_apply_hat(dev, code, info.value);
    }

    dev->state.sticks = dev->present[GAMEPAD_AXIS_LX] && dev->present[GAMEPAD_AXIS_LY] &&
                        dev->present[GAMEPAD_AXIS_RX] && dev->present[GAMEPAD_AXIS_RY];

    struct input_id ids;
    if (ioctl(fd, EVIOCGID, &ids) >= 0)
        switch (ids.vendor)
        {
        case 0x054C: dev->state.type = GAMEPAD_TYPE_PLAYSTATION; break;
        case 0x045E: dev->state.type = GAMEPAD_TYPE_WESTERN; break;
        case 0x057E:
            /* Wii Remotes, the Classic Controller and the NES, N64 and Genesis
             * pads lack the type 2 layout. Bit 15 of the version marks
             * hid-nintendo's mapping; joycond's combined Joy-Cons (0x2008)
             * pass that mapping on with version 0. */
            if (((ids.version & 0x8000) &&
                 (ids.product == 0x2009 || ids.product == 0x2017)) ||
                ids.product == 0x2008)
                dev->state.type = GAMEPAD_TYPE_EASTERN;
            break;
        }

    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
        for (uint16_t code = BTN_JOYSTICK; code < KEY_CNT; code++)
            if (GAMEPAD_BIT_TEST(keys, code))
                gamepad_apply_button(dev, code, true);

    return true;
}

static bool gamepad_holds(uint64_t id)
{
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        if (gamepad_devices[i].fd >= 0 && gamepad_devices[i].id == id)
            return true;
    return false;
}

static void gamepad_scan(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strncmp(entry->d_name, "event", 5))
            continue;
        char path[sizeof("/dev/input/") + sizeof(entry->d_name)];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        struct stat info;
        if (stat(path, &info) < 0 || gamepad_holds((uint64_t)info.st_rdev))
            continue;
        for (int i = 0; i < GAMEPAD_PLAYERS; i++)
            if (gamepad_devices[i].fd < 0 &&
                gamepad_open_device(&gamepad_devices[i], path, (uint64_t)info.st_rdev))
                break;
    }
    closedir(dir);
}

bool host_gamepad_open(void)
{
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        gamepad_devices[i].fd = -1;
    gamepad_scan();
    gamepad_rescan = GAMEPAD_LINUX_RESCAN;
    return true;
}

void host_gamepad_close(void)
{
    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
        gamepad_close_device(&gamepad_devices[i]);
}

int host_gamepad_poll(gamepad_host_t *gamepads, int max)
{
    if (gamepad_rescan-- <= 0)
    {
        gamepad_rescan = GAMEPAD_LINUX_RESCAN;
        gamepad_scan();
    }

    for (int i = 0; i < GAMEPAD_PLAYERS; i++)
    {
        gamepad_device_t *dev = &gamepad_devices[i];
        if (dev->fd < 0)
            continue;
        struct input_event events[32];
        ssize_t got;
        while ((got = read(dev->fd, events, sizeof(events))) > 0)
        {
            for (size_t e = 0; e < (size_t)got / sizeof(events[0]); e++)
            {
                const struct input_event *event = &events[e];
                if (event->type == EV_KEY)
                    gamepad_apply_button(dev, event->code, event->value != 0);
                else if (event->type != EV_ABS)
                    continue;
                else if (event->code == ABS_HAT0X || event->code == ABS_HAT0Y)
                    gamepad_apply_hat(dev, event->code, event->value);
                else
                    for (int axis = 0; axis < GAMEPAD_AXIS_COUNT; axis++)
                        if (dev->present[axis] && dev->axis_code[axis] == event->code)
                            gamepad_apply_axis(dev, axis, event->value);
            }
        }
        /* A device that has been unplugged fails the read with ENODEV rather
         * than with the EAGAIN of a device that has nothing to say. */
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
            gamepad_close_device(dev);
    }

    int count = 0;
    for (int i = 0; i < GAMEPAD_PLAYERS && count < max; i++)
        if (gamepad_devices[i].fd >= 0)
        {
            gamepads[count] = gamepad_devices[i].state;
            gamepads[count].id = gamepad_devices[i].id;
            count++;
        }
    return count;
}
