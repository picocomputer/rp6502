/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The RIA's MSC driver (src/host/pico/ria/usb/msc.c) and FatFs, run against a
 * fake USB host and models of the two USB floppy drives on the rig. Both are
 * UFI over CBI with a command completion interrupt. The models answer the way
 * the drives did on the rig, so each case holds a sequence that once failed. */

#include "tusb.h"
#include "class/msc/msc.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "ria/usb/msc.h"
#include "ria/usb/usb.h"
#include "pico/aon_timer.h"
#include "pico/time.h"
#include "utest.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DADDR 1
#define EP_IN 0x81
#define EP_OUT 0x02
#define EP_STATUS 0x83
#define BLOCKS 2880
#define STEP_US 50000 // a head step, measured on the TEAC

/* The clock. Every pump of the main loop is one millisecond. */

static uint64_t now_us;

absolute_time_t get_absolute_time(void) { return now_us; }
uint32_t tusb_time_millis_api(void) { return (uint32_t)(now_us / 1000); }
bool aon_timer_get_time(struct timespec *ts)
{
    (void)ts;
    return false;
}

/* The drives. A floppy drive learns that a disk went in or out from its disk
 * change line, which a head step clears when a disk is in. */

typedef enum
{
    MODEL_TEAC, // 0644:0000: answers from before the step
    MODEL_YE,   // 057b:0000: steps, then answers
} model_t;

typedef struct
{
    model_t model;
    bool disk_in;
    bool disk_change; // set by a removal
    bool had_disk;    // the drive last saw a disk
    bool removal_ua;  // the TEAC's first answer after a removal is 06/28, not 02/3A
    bool power_ua;    // 06/28 for the first command after power up
    uint8_t sk, asc;  // sense of the last command
    uint32_t bad_lba; // when not 0, a READ of it moves the data, then fails
    bool hang_reads;  // a READ never moves its data
    bool halted_in;   // bulk IN halted after the last data
    unsigned steps;   // ka-chunks
    unsigned reads_empty;
    unsigned request_senses;
    unsigned sense_stalled; // REQUEST SENSE found bulk IN halted
    unsigned clears[4];     // CLEAR_FEATURE(ENDPOINT_HALT), by endpoint number
    uint8_t image[BLOCKS * 512];
} drive_t;

static drive_t drive;

static void drive_insert(void)
{
    drive.disk_in = true;
}

static void drive_eject(void)
{
    drive.disk_in = false;
    drive.disk_change = true;
}

// A command that finds the disk change line set steps the head, which clears
// the line when a disk is in. The TEAC answers for the disk it saw before the
// step, so its first answer after a disk goes in is no medium. The Y-E answers
// for the disk after the step.
static bool drive_media_check(void)
{
    if (drive.power_ua)
    {
        drive.power_ua = false;
        drive.sk = 0x06, drive.asc = 0x28;
        return false;
    }
    if (!drive.disk_change)
        return true;
    drive.steps++;
    now_us += STEP_US;
    bool const had = drive.had_disk;
    drive.had_disk = drive.disk_in;
    drive.disk_change = !drive.disk_in;
    bool const media_changed = drive.model == MODEL_TEAC
                                   ? had && drive.removal_ua
                                   : drive.disk_in;
    if (media_changed)
        drive.sk = 0x06, drive.asc = 0x28;
    else
        drive.sk = 0x02, drive.asc = 0x3A;
    return false;
}

/* One command in flight: the data it moves and its interrupt status. */

static struct
{
    uint8_t cdb[12];
    uint8_t data[64];
    uint32_t data_len;
    bool failed;  // the interrupt data reports failure
    bool no_data; // the data phase STALLs
} cmd;

static uint32_t cmd_lba(void)
{
    return ((uint32_t)cmd.cdb[2] << 24) | ((uint32_t)cmd.cdb[3] << 16) |
           ((uint32_t)cmd.cdb[4] << 8) | cmd.cdb[5];
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void drive_command(const uint8_t cdb[12])
{
    memcpy(cmd.cdb, cdb, 12);
    memset(cmd.data, 0, sizeof(cmd.data));
    cmd.data_len = 0;
    cmd.failed = false;
    cmd.no_data = false;
    uint8_t const op = cdb[0];
    if (op == 0x1D) // Command Block Reset
        return;
    if (op == 0x03) // REQUEST SENSE keeps the sense and clears a UNIT ATTENTION
    {
        drive.request_senses++;
        cmd.data[0] = 0x70;
        cmd.data[2] = drive.sk;
        cmd.data[7] = 10;
        cmd.data[12] = drive.asc;
        cmd.data_len = 18;
        if (drive.sk == 0x06)
            drive.sk = 0, drive.asc = 0;
        return;
    }
    if (op == 0x12) // INQUIRY leaves the sense alone
    {
        cmd.data[1] = 0x80; // removable
        memcpy(&cmd.data[8], "TEST    FLOPPY          0.00", 28);
        cmd.data_len = 36;
        return;
    }
    drive.sk = 0, drive.asc = 0;
    if (op == 0x5A) // MODE SENSE(10) header
    {
        cmd.data[1] = 6;
        cmd.data_len = 8;
        return;
    }
    if (!drive_media_check())
    {
        cmd.failed = cmd.no_data = true;
        return;
    }
    switch (op)
    {
    case 0x00: // TEST UNIT READY
        break;
    case 0x23: // READ FORMAT CAPACITIES
        cmd.data[3] = 8;
        be32(&cmd.data[4], BLOCKS);
        cmd.data[8] = 0x02; // formatted media
        cmd.data[10] = 0x02;
        cmd.data_len = 12;
        break;
    case 0x28: // READ(10)
    case 0x2A: // WRITE(10)
        cmd.data_len = (uint32_t)((cdb[7] << 8) | cdb[8]) * 512;
        if (op == 0x28 && drive.bad_lba && drive.bad_lba >= cmd_lba() &&
            drive.bad_lba < cmd_lba() + cmd.data_len / 512)
        {
            drive.sk = 0x03, drive.asc = 0x11; // UNRECOVERED READ ERROR
            cmd.failed = true;
        }
        break;
    default:
        drive.sk = 0x05, drive.asc = 0x20;
        cmd.failed = cmd.no_data = true;
        break;
    }
}

/* The fake USB host. Completions are queued and delivered by sys_task. */

typedef struct
{
    bool control;
    tuh_xfer_t xfer;
    uint8_t ep;
    xfer_result_t result;
    uint32_t len;
} event_t;

static event_t events[8];
static unsigned event_count;

static void queue(event_t ev)
{
    events[event_count++] = ev;
}

static bool ep_queued(uint8_t ep)
{
    for (unsigned i = 0; i < event_count; i++)
        if (!events[i].control && events[i].ep == ep)
            return true;
    return false;
}

bool tuh_control_xfer(tuh_xfer_t *xfer)
{
    // ADSC carries the command block. Every other request passes.
    tusb_control_request_t const *req = xfer->setup;
    if (req->bRequest == TUSB_REQ_CLEAR_FEATURE)
    {
        drive.clears[req->wIndex & 0x0F]++;
        if (req->wIndex == EP_IN)
            drive.halted_in = false;
    }
    else if (req->bRequest == 0 && req->wLength == 12)
        drive_command(xfer->buffer);
    queue((event_t){.control = true, .xfer = *xfer, .result = XFER_RESULT_SUCCESS});
    return true;
}

bool tuh_control_xfer_drop(uint8_t daddr, tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
    (void)daddr;
    (void)user_data;
    for (unsigned i = 0; i < event_count; i++)
        if (events[i].control && events[i].xfer.complete_cb == complete_cb)
        {
            memmove(&events[i], &events[i + 1], (--event_count - i) * sizeof(event_t));
            return true;
        }
    return false;
}

bool usbh_edpt_xfer_with_callback(uint8_t daddr, uint8_t ep, uint8_t *buf, uint16_t len,
                                  tuh_xfer_cb_t complete_cb, uintptr_t user_data)
{
    (void)daddr;
    (void)complete_cb;
    (void)user_data;
    event_t ev = {.ep = ep, .result = XFER_RESULT_SUCCESS};
    if (ep == EP_STATUS)
    {
        buf[0] = cmd.failed ? drive.asc : 0;
        buf[1] = 0;
        ev.len = 2;
    }
    else if (ep == EP_IN && drive.halted_in)
    {
        // usbh clears a halt it finds and completes the transfer as STALLED.
        drive.halted_in = false;
        ev.result = XFER_RESULT_STALLED;
        if (cmd.cdb[0] == 0x03)
            drive.sense_stalled++;
    }
    else if (cmd.no_data)
    {
        ev.result = XFER_RESULT_STALLED;
        if (cmd.cdb[0] == 0x28 && !drive.disk_in)
            drive.reads_empty++;
    }
    else if (ep == EP_IN && cmd.cdb[0] == 0x28)
    {
        if (drive.hang_reads)
            return true;
        memcpy(buf, &drive.image[cmd_lba() * 512], len);
        ev.len = len;
        drive.halted_in = cmd.failed;
    }
    else if (ep == EP_IN)
    {
        ev.len = len < cmd.data_len ? len : cmd.data_len;
        memcpy(buf, cmd.data, ev.len);
    }
    else
    {
        memcpy(&drive.image[cmd_lba() * 512], buf, len);
        ev.len = len;
    }
    queue(ev);
    return true;
}

bool usbh_edpt_busy(uint8_t daddr, uint8_t ep)
{
    (void)daddr;
    return ep_queued(ep);
}

bool usbh_edpt_claim(uint8_t daddr, uint8_t ep)
{
    (void)daddr;
    (void)ep;
    return true;
}

bool usbh_edpt_release(uint8_t daddr, uint8_t ep)
{
    (void)daddr;
    (void)ep;
    return true;
}

bool tuh_edpt_open(uint8_t daddr, tusb_desc_endpoint_t const *desc_ep)
{
    (void)daddr;
    (void)desc_ep;
    return true;
}

bool tuh_edpt_close(uint8_t daddr, uint8_t ep)
{
    (void)daddr;
    (void)ep;
    return true;
}

bool tuh_edpt_abort_xfer(uint8_t daddr, uint8_t ep)
{
    (void)daddr;
    (void)ep;
    return true;
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t daddr, uint8_t ep)
{
    (void)rhport;
    (void)daddr;
    (void)ep;
    return true;
}

uint8_t usbh_get_rhport(uint8_t daddr)
{
    (void)daddr;
    return 0;
}

void usbh_driver_set_config_complete(uint8_t daddr, uint8_t itf_num)
{
    (void)daddr;
    (void)itf_num;
}

void sys_task(void)
{
    now_us += 1000;
    if (!event_count)
        return;
    event_t ev = events[0];
    memmove(&events[0], &events[1], --event_count * sizeof(event_t));
    if (ev.control)
    {
        ev.xfer.result = ev.result;
        ev.xfer.complete_cb(&ev.xfer);
    }
    else
        msc_class_driver_xfer_cb(DADDR, ev.ep, ev.result, ev.len);
}

/* What msc.c needs from the rest of the RIA. */

const void *usb_string_fetch_serial(uint8_t dev_addr)
{
    (void)dev_addr;
    return NULL;
}

void usb_desc_string_to_oem(const void *desc, size_t desc_size, char *dst, size_t dst_size)
{
    (void)desc;
    (void)desc_size;
    (void)dst;
    (void)dst_size;
}

const char STR_STATUS_MSC[] = "%s: %s %.8s %.16s rev %.4s\n";

const char *S(int id)
{
    (void)id;
    return "";
}

int oem_snprintf(char *dst, size_t dst_size, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(dst, dst_size, fmt, va);
    va_end(va);
    return n;
}

void str_size(uint64_t bytes, char *out, size_t out_size)
{
    snprintf(out, out_size, "%llu", (unsigned long long)bytes);
}

/* What ff.c needs: ASCII names only, and no DISK preview. */

WCHAR ff_oem2uni(WCHAR oem, WORD cp)
{
    (void)cp;
    return oem < 0x80 ? oem : 0;
}

WCHAR ff_uni2oem(DWORD uni, WORD cp)
{
    (void)cp;
    return uni < 0x80 ? (WCHAR)uni : 0;
}

DWORD ff_wtoupper(DWORD uni)
{
    return uni < 0x80 ? (DWORD)toupper((int)uni) : uni;
}

int drive_mkfs_capture(BYTE fsty, DWORD au_sectors)
{
    (void)fsty;
    (void)au_sectors;
    return 0;
}

unsigned long long drive_min_gpt(void)
{
    return 0;
}

/* A drive with a disk in, plugged in and registered as MSC0. */

static void plug_in(model_t model, bool removal_ua)
{
    static const uint8_t desc[] = {
        9, TUSB_DESC_INTERFACE, 0, 0, 3, TUSB_CLASS_MSC, MSC_SUBCLASS_UFI, MSC_PROTOCOL_CBI, 0,
        7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, 64, 0, 0,
        7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_BULK, 64, 0, 0,
        7, TUSB_DESC_ENDPOINT, EP_STATUS, TUSB_XFER_INTERRUPT, 2, 0, 127};
    msc_class_driver_close(DADDR); // the last case's drive, even if it failed
    memset(&drive, 0, sizeof(drive));
    drive.model = model;
    drive.removal_ua = removal_ua;
    drive.disk_in = true;
    drive.had_disk = true;
    drive.power_ua = true;
    event_count = 0;
    now_us = 1000000;
    msc_class_driver_init();
    msc_class_driver_open(0, DADDR, (tusb_desc_interface_t const *)desc, sizeof(desc));
    msc_class_driver_set_config(DADDR, 0);
}

static int dir(void)
{
    DIR d;
    FRESULT fr = f_opendir(&d, "MSC0:/");
    if (fr == FR_OK)
        f_closedir(&d);
    return (int)fr;
}

/* One monitor command can mount a volume twice, as changing to a drive does. */
static int dir_twice(void)
{
    dir();
    return dir();
}

static void idle(void)
{
    now_us += 2000000;
}

/* A formatted disk with one file, written through the driver. */
static void format(void)
{
    BYTE work[FF_MAX_SS];
    MKFS_PARM opt = {FM_FAT, 0, 0, 0, 0};
    FRESULT fr = f_mkfs("MSC0:", &opt, work, sizeof(work));
    if (fr != FR_OK)
        fprintf(stderr, "f_mkfs %d\n", fr);
    FIL f;
    UINT bw;
    f_open(&f, "MSC0:/FLOPPY.TXT", FA_WRITE | FA_CREATE_ALWAYS);
    f_write(&f, "ka-chunk", 8, &bw);
    f_close(&f);
    idle();
}

static unsigned steps_for(int (*access)(void), int *fr)
{
    unsigned const before = drive.steps;
    *fr = access();
    return drive.steps - before;
}

/* The TEAC answers the first command after a disk goes in with no medium. */
UTEST(msc, teac_reads_a_disk_on_the_first_access_after_insert)
{
    plug_in(MODEL_TEAC, true);
    format();
    ASSERT_EQ(dir(), FR_OK);
    idle();
    drive_eject();
    int fr;
    EXPECT_LE(steps_for(dir, &fr), 2u);
    ASSERT_EQ(fr, FR_NOT_READY);
    idle();
    EXPECT_LE(steps_for(dir, &fr), 2u);
    ASSERT_EQ(fr, FR_NOT_READY);
    idle();
    drive_insert();
    idle();
    ASSERT_EQ(dir(), FR_OK);
}

/* A removal the TEAC first answers with no medium rather than a media change. */
UTEST(msc, teac_removal_without_unit_attention)
{
    plug_in(MODEL_TEAC, false);
    format();
    ASSERT_EQ(dir(), FR_OK);
    idle();
    drive_eject();
    int fr;
    EXPECT_LE(steps_for(dir, &fr), 2u);
    ASSERT_EQ(fr, FR_NOT_READY);
    idle();
    drive_insert();
    ASSERT_EQ(dir(), FR_OK);
}

/* Out and back in while idle: the TEAC's first answer says no medium. */
UTEST(msc, teac_swap_while_idle)
{
    plug_in(MODEL_TEAC, false);
    format();
    ASSERT_EQ(dir(), FR_OK);
    idle();
    drive_eject();
    drive_insert();
    ASSERT_EQ(dir(), FR_OK);
}

UTEST(msc, one_command_mounting_twice_steps_at_most_twice)
{
    plug_in(MODEL_TEAC, true);
    format();
    ASSERT_EQ(dir(), FR_OK);
    idle();
    drive_eject();
    int fr;
    EXPECT_LE(steps_for(dir_twice, &fr), 2u);
    ASSERT_EQ(fr, FR_NOT_READY);
    idle();
    EXPECT_LE(steps_for(dir_twice, &fr), 2u);
    ASSERT_EQ(fr, FR_NOT_READY);
    idle();
    drive_insert();
    ASSERT_EQ(dir_twice(), FR_OK);
}

UTEST(msc, ye_eject_and_insert)
{
    plug_in(MODEL_YE, false);
    format();
    ASSERT_EQ(dir(), FR_OK);
    idle();
    drive_eject();
    int fr;
    EXPECT_LE(steps_for(dir, &fr), 2u);
    ASSERT_EQ(fr, FR_NOT_READY);
    idle();
    drive_insert();
    ASSERT_EQ(dir(), FR_OK);
}

/* STATUS mounts the drive without FatFs. A disk pulled after it is found by a
 * TUR, never by a READ, which an empty TEAC holds for seconds. */
UTEST(msc, status_then_eject_does_not_read_an_empty_drive)
{
    plug_in(MODEL_TEAC, true);
    format();
    msc_drive_reenumerate(0);
    char buf[80];
    msc_status_response(buf, sizeof(buf), 0, 80);
    idle();
    drive_eject();
    ASSERT_EQ(dir(), FR_NOT_READY);
    ASSERT_EQ(drive.reads_empty, 0u);
}

/* A CBI command's status comes from its interrupt data, so a command that
 * passes is not followed by REQUEST SENSE. */
UTEST(msc, cbi_passes_without_request_sense)
{
    plug_in(MODEL_YE, false);
    format();
    ASSERT_EQ(dir(), FR_OK);
    idle();
    unsigned const before = drive.request_senses;
    FIL f;
    char text[9] = {0};
    UINT br;
    ASSERT_EQ((int)f_open(&f, "MSC0:/FLOPPY.TXT", FA_READ), FR_OK);
    ASSERT_EQ((int)f_read(&f, text, 8, &br), FR_OK);
    f_close(&f);
    ASSERT_STREQ(text, "ka-chunk");
    ASSERT_EQ(drive.request_senses, before);
}

/* A drive may halt bulk IN after the last data of a READ that failed, so the
 * pipe is cleared before REQUEST SENSE (CBI 2.4.3.1.3). */
UTEST(msc, failed_read_clears_bulk_in_before_request_sense)
{
    plug_in(MODEL_YE, false);
    format();
    ASSERT_EQ(dir(), FR_OK);
    drive.bad_lba = 100;
    BYTE buf[512];
    ASSERT_EQ((int)disk_read(0, buf, 100, 1), RES_ERROR);
    EXPECT_EQ(drive.sense_stalled, 0u);
    EXPECT_GE(drive.clears[EP_IN & 0x0F], 1u);
}

/* A Command Block Reset leaves every data toggle undefined (CBI 2.2), so the
 * status pipe is cleared with the bulk pipes. */
UTEST(msc, timed_out_read_resets_and_clears_every_pipe)
{
    plug_in(MODEL_YE, false);
    format();
    ASSERT_EQ(dir(), FR_OK);
    drive.hang_reads = true;
    BYTE buf[512];
    ASSERT_EQ((int)disk_read(0, buf, 100, 1), RES_NOTRDY);
    drive.hang_reads = false;
    for (int i = 0; i < 100; i++)
        sys_task();
    EXPECT_GE(drive.clears[EP_IN & 0x0F], 1u);
    EXPECT_GE(drive.clears[EP_OUT & 0x0F], 1u);
    EXPECT_GE(drive.clears[EP_STATUS & 0x0F], 1u);
    ASSERT_EQ((int)disk_read(0, buf, 100, 1), RES_OK);
}

UTEST_MAIN();
