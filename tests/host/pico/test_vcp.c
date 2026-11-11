/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The VCP that pairs with the NFC reader, against a fake CDC host. The port is
 * reserved for the reader only while NFC is on. */
#include "tusb.h"
#include "ria/usb/vcp.h"
#include "ria/usb/usb.h"
#include "core/sys/config.h"
#include "utest.h"
#include <string.h>

#define PAIRED_HASH "0403:6001:0600:FTDI:FT232R:A1"
static char hash_cfg[VCP_NFC_HASH_SIZE];
static uint8_t nfc_cfg;
const char *vcp_get_nfc_device_hash(void) { return hash_cfg; }
bool vcp_set_nfc_device_hash(const char *v) { strcpy(hash_cfg, v); vcp_apply_nfc_device_hash(v, true); return true; }
uint8_t nfc_get_enabled(void) { return nfc_cfg; }
bool nfc_set_enabled(uint8_t v) { nfc_cfg = v; return true; }
const char STR_STATUS_CDC[] = "%s: %s %s\n";
const char STR_STATUS_NFC[] = " (NFC)\n";

/* A fake TinyUSB CDC host. Its requests complete when pumped. */
static tuh_xfer_cb_t pend_cb; static uintptr_t pend_ud;
static bool queue(tuh_xfer_cb_t cb, uintptr_t ud) { pend_cb = cb; pend_ud = ud; return true; }
static void pump(void) { while (pend_cb) { tuh_xfer_t x = {.result = XFER_RESULT_SUCCESS, .user_data = pend_ud}; tuh_xfer_cb_t cb = pend_cb; pend_cb = NULL; cb(&x);} }
bool tuh_cdc_itf_get_info(uint8_t idx, tuh_itf_info_t *info) { memset(info, 0, sizeof *info); info->daddr = idx + 1; return true; }
bool tuh_vid_pid_get(uint8_t d, uint16_t *v, uint16_t *p) { (void)d; *v = 0x0403; *p = 0x6001; return true; }
bool tuh_cdc_read_clear(uint8_t i) { (void)i; return true; }
bool tuh_cdc_write_clear(uint8_t i) { (void)i; return true; }
bool tuh_cdc_get_line_coding_local(uint8_t i, cdc_line_coding_t *c) { (void)i; (void)c; return false; }
bool tuh_cdc_set_line_coding(uint8_t i, cdc_line_coding_t const *c, tuh_xfer_cb_t cb, uintptr_t ud) { (void)i; (void)c; return queue(cb, ud); }
bool tuh_cdc_set_control_line_state(uint8_t i, uint16_t s, tuh_xfer_cb_t cb, uintptr_t ud) { (void)i; (void)s; return queue(cb, ud); }
uint32_t tuh_cdc_read(uint8_t i, void *b, uint32_t n) { (void)i; (void)b; (void)n; return 0; }
uint32_t tuh_cdc_write(uint8_t i, void const *b, uint32_t n) { (void)i; (void)b; return n; }
uint32_t tuh_cdc_write_flush(uint8_t i) { (void)i; return 0; }
bool usb_device_id_hash(uint8_t d, char *buf, size_t n) { (void)d; snprintf(buf, n, "%s", PAIRED_HASH); return true; }
static const uint8_t str_desc[USB_DESC_STRING_BUF_SIZE] = {4, 3, 'X', 0};
const void *usb_string_fetch_manufacturer(uint8_t d) { (void)d; return str_desc; }
const void *usb_string_fetch_product(uint8_t d) { (void)d; return str_desc; }
void usb_desc_string_to_oem(const void *s, size_t ss, char *d, size_t ds) { (void)s; (void)ss; snprintf(d, ds, "X"); }

static void setup(uint8_t enabled)
{
    strcpy(hash_cfg, PAIRED_HASH);
    nfc_cfg = enabled;
    vcp_apply_nfc_device_hash(hash_cfg, false);
    tuh_cdc_mount_cb(0);
    vcp_task(); /* pairs VCP0 */
}

UTEST(vcp, nfc_off_frees_paired_port)
{
    setup(0);
    api_errno err = 0;
    int fd = vcp_std_open("VCP0:", 0, &err);
    ASSERT_GE(fd, 0);
    pump();
    ASSERT_EQ(STD_OK, vcp_std_opening(fd, &err));
    vcp_std_close(fd, &err); pump();
    tuh_cdc_umount_cb(0);
}

UTEST(vcp, nfc_on_holds_paired_port)
{
    setup(1);
    api_errno err = 0;
    ASSERT_EQ(-1, vcp_std_open("VCP0:", 0, &err));
    ASSERT_EQ(API_EBUSY, err);
    int fd = vcp_nfc_open();
    ASSERT_GE(fd, 0);
    pump();
    vcp_std_close(fd, &err); pump();
    tuh_cdc_umount_cb(0);
}

UTEST(vcp, app_keeps_port_when_nfc_turns_on)
{
    setup(0);
    api_errno err = 0;
    int fd = vcp_std_open("VCP0:", 0, &err);
    ASSERT_GE(fd, 0); pump();
    nfc_cfg = 1;                      /* SET NFC 1 */
    ASSERT_EQ(-1, vcp_nfc_open());    /* reader waits for the program */
    ASSERT_EQ(STD_OK, vcp_std_opening(fd, &err));
    vcp_std_close(fd, &err); pump();
    int nfd = vcp_nfc_open();         /* then takes the port */
    ASSERT_GE(nfd, 0); pump();
    ASSERT_EQ(-1, vcp_std_open("VCP0:", 0, &err));
    ASSERT_EQ(API_EBUSY, err);
    vcp_std_close(nfd, &err); pump();
    tuh_cdc_umount_cb(0);
}

/* STATUS marks the paired port as the reader's only while NFC is on. */
UTEST(vcp, status_marks_paired_port_while_nfc_is_on)
{
    char buf[80];
    setup(1);
    vcp_status_response(buf, sizeof buf, 0, 0);
    ASSERT_TRUE(strstr(buf, "(NFC)") != NULL);
    tuh_cdc_umount_cb(0);
    setup(0);
    vcp_status_response(buf, sizeof buf, 0, 0);
    ASSERT_TRUE(strstr(buf, "(NFC)") == NULL);
    tuh_cdc_umount_cb(0);
}

UTEST_MAIN();
