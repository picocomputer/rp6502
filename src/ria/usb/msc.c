/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "tusb.h"
#include "str/str.h"
#include "usb/msc.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "pico/aon_timer.h"
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

#if defined(DEBUG_RIA_USB) || defined(DEBUG_RIA_USB_MSC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define DBG_VOL(vol, fmt, ...)                                \
    DBG("MSC:%lums vol %u: " fmt,                             \
        (unsigned long)to_ms_since_boot(get_absolute_time()), \
        (unsigned)(vol),                                      \
        ##__VA_ARGS__)

#define DBG_CMD(vol, cmd, status)                                           \
    DBG_VOL(vol, cmd " (status=0x%02x sk=0x%02x asc=0x%02x ascq=0x%02x)\n", \
            (unsigned)(status),                                             \
            msc_vol[vol].sense_key, msc_vol[vol].sense_asc, msc_vol[vol].sense_ascq)

// File descriptor pool for open files
#define MSC_STD_FIL_MAX 8
static FIL msc_std_fil_pool[MSC_STD_FIL_MAX];

// Timeout for read/write/sync SCSI commands and
// anything that might interact with motors.
// Needs headroom for 3.5" floppy disk drives.
#define MSC_SCSI_RW_TIMEOUT_MS 2500

// Time budget for SCSI commands which do
// not need to account for mechanical delay.
#define MSC_SCSI_OP_TIMEOUT_MS 250

// disk_status() issues a TUR on removable volumes only when this many
// milliseconds have elapsed since the last successful SCSI command.
// This detects media removal without adding overhead during active I/O.
#define MSC_DISK_STATUS_TIMEOUT_MS 250

// Validate essential settings from ffconf.h
static_assert(sizeof(TCHAR) == sizeof(char));
static_assert(FF_CODE_PAGE == RP6502_CODE_PAGE);
static_assert(FF_FS_EXFAT == RP6502_EXFAT);
static_assert(FF_LBA64 == RP6502_EXFAT);
static_assert(FF_USE_STRFUNC == 1);
static_assert(FF_USE_LFN == 1);
static_assert(FF_MAX_LFN == 255);
static_assert(FF_LFN_UNICODE == 0);
static_assert(FF_LFN_BUF == 255);
static_assert(FF_SFN_BUF == 12);
static_assert(FF_FS_RPATH == 2);
static_assert(FF_MULTI_PARTITION == 0);
static_assert(FF_FS_LOCK == 8);
static_assert(FF_FS_NORTC == 0);
static_assert(FF_USE_TRIM == 1);
static_assert(FF_VOLUMES == 10);
static_assert(FF_STR_VOLUME_ID == 1);
#ifdef FF_VOLUME_STRS
#error FF_VOLUME_STRS must not be defined
#endif

// Place volume strings in flash
static const char __in_flash("fatfs_vol") VolumeStrMSC0[] = "MSC0";
static const char __in_flash("fatfs_vol") VolumeStrMSC1[] = "MSC1";
static const char __in_flash("fatfs_vol") VolumeStrMSC2[] = "MSC2";
static const char __in_flash("fatfs_vol") VolumeStrMSC3[] = "MSC3";
static const char __in_flash("fatfs_vol") VolumeStrMSC4[] = "MSC4";
static const char __in_flash("fatfs_vol") VolumeStrMSC5[] = "MSC5";
static const char __in_flash("fatfs_vol") VolumeStrMSC6[] = "MSC6";
static const char __in_flash("fatfs_vol") VolumeStrMSC7[] = "MSC7";
static const char __in_flash("fatfs_vol") VolumeStrMSC8[] = "MSC8";
static const char __in_flash("fatfs_vol") VolumeStrMSC9[] = "MSC9";
const char __in_flash("fatfs_vols") * VolumeStr[FF_VOLUMES] = {
    VolumeStrMSC0, VolumeStrMSC1, VolumeStrMSC2, VolumeStrMSC3,
    VolumeStrMSC4, VolumeStrMSC5, VolumeStrMSC6, VolumeStrMSC7,
    VolumeStrMSC8, VolumeStrMSC9};

// Build a FatFS volume path like "MSC0:" for volume.
static inline void msc_vol_path(TCHAR buf[6], uint8_t vol)
{
    static_assert(FF_VOLUMES <= 10);
    memcpy(buf, "MSC0:", 6);
    buf[3] += vol;
}

typedef enum
{
    msc_volume_free = 0,
    msc_volume_registered,
    msc_volume_mounted,
    msc_volume_ejected,
} msc_volume_status_t;

typedef struct
{
    msc_volume_status_t status;
    uint8_t dev_addr;
    uint8_t lun;
    FATFS fatfs;
    bool removable;
    uint8_t spc_version; // 0x05=SPC-3, 0x06=SPC-4, 0x07=SPC-5
    uint64_t block_count;
    uint32_t block_size;
    uint8_t sense_key;
    uint8_t sense_asc;
    uint8_t sense_ascq;
    bool write_prot;
    bool unmap_supported;
    bool sync_cache_supressed;
    bool lbpme;
    absolute_time_t last_ok;
} msc_vol_t;

static msc_vol_t msc_vol[FF_VOLUMES];

// This driver requires our custom TinyUSB: src/tinyusb_rp6502/msc_host.c
// It will not work with upstream: src/tinyusb/src/class/msc/msc_host.c
// Additional SCSI commands and interfaces are not in upstream TinyUSB.

typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;
    uint8_t : 3;
    bool disable_block_descriptor : 1;
    uint8_t : 4;
    uint8_t page_code : 6;
    uint8_t page_control : 2;
    uint8_t subpage_code;
    uint8_t reserved[3];
    uint16_t alloc_length; // big-endian
    uint8_t control;
} scsi_mode_sense10_t;
TU_VERIFY_STATIC(sizeof(scsi_mode_sense10_t) == 10, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint16_t data_len; // big-endian
    uint8_t medium_type;
    uint8_t : 7;
    bool write_protected : 1; // bit 7: write protect
    uint8_t long_lba_bit;
    uint8_t reserved;
    uint16_t block_descriptor_len; // big-endian
} scsi_mode_sense10_resp_t;
TU_VERIFY_STATIC(sizeof(scsi_mode_sense10_resp_t) == 8, "size is not correct");

// SBC-3 §5.15.2: READ CAPACITY(16) (SERVICE ACTION IN, opcode 0x9E, SA 0x10).
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code;       // 0x9E
    uint8_t service_action; // 0x10
    uint8_t reserved1[8];
    uint32_t alloc_length; // big-endian
    uint8_t reserved2;
    uint8_t control;
} scsi_read_capacity16_t;
TU_VERIFY_STATIC(sizeof(scsi_read_capacity16_t) == 16, "size is not correct");

typedef struct TU_ATTR_PACKED
{
    uint32_t last_lba_hi; // big-endian, bytes 0-3
    uint32_t last_lba_lo; // big-endian, bytes 4-7
    uint32_t block_size;  // big-endian, bytes 8-11
    uint8_t prot;         // byte 12
    uint8_t lbppbe;       // byte 13
    uint8_t lbpme_byte;   // byte 14: bit 7 = LBPME, bit 6 = LBPRZ
    uint8_t reserved[17]; // bytes 15-31
} scsi_read_capacity16_resp_t;
TU_VERIFY_STATIC(sizeof(scsi_read_capacity16_resp_t) == 32, "size is not correct");

// SBC-3 §5.6: READ(16) (opcode 0x88)
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code; // 0x88
    uint8_t flags;
    uint32_t lba_hi;      // big-endian, bytes 2-5
    uint32_t lba_lo;      // big-endian, bytes 6-9
    uint32_t block_count; // big-endian, bytes 10-13
    uint8_t group_number; // byte 14
    uint8_t control;      // byte 15
} scsi_read16_t;
TU_VERIFY_STATIC(sizeof(scsi_read16_t) == 16, "size is not correct");

// SBC-3 §5.24: WRITE(16) (opcode 0x8A)
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code; // 0x8A
    uint8_t flags;
    uint32_t lba_hi;      // big-endian, bytes 2-5
    uint32_t lba_lo;      // big-endian, bytes 6-9
    uint32_t block_count; // big-endian, bytes 10-13
    uint8_t group_number; // byte 14
    uint8_t control;      // byte 15
} scsi_write16_t;
TU_VERIFY_STATIC(sizeof(scsi_write16_t) == 16, "size is not correct");

// SPC-4 §7.8.16: Logical Block Provisioning VPD page (page code 0xB2).
// We only need the first 6 bytes.
typedef struct TU_ATTR_PACKED
{
    uint8_t peripheral_device_type : 5;
    uint8_t peripheral_qualifier : 3;
    uint8_t page_code;    // 0xB2
    uint16_t page_length; // big-endian, min 0x0004
    uint8_t threshold_exponent;
    // Byte 5 — bit 7 = LBPU, bit 6 = LBPWS, bit 5 = LBPWS10, bits 2:0 = LBPRZ
    uint8_t lbprz : 3;
    uint8_t : 2;
    bool lbpws10 : 1;
    bool lbpws : 1;
    bool lbpu : 1;
} scsi_vpd_lbp_t;
TU_VERIFY_STATIC(sizeof(scsi_vpd_lbp_t) == 6, "size is not correct");

// SBC-3 §5.25: UNMAP command (opcode 0x42)
typedef struct TU_ATTR_PACKED
{
    uint8_t cmd_code; // 0x42
    uint8_t anchor : 1;
    uint8_t : 7;
    uint8_t reserved[4];
    uint8_t group_number : 5;
    uint8_t : 3;
    uint16_t param_list_length; // big-endian
    uint8_t control;
} scsi_unmap_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_t) == 10, "size is not correct");

// SBC-3 §5.25.2: UNMAP block descriptor
typedef struct TU_ATTR_PACKED
{
    uint32_t lba_hi;      // big-endian, upper 32 bits
    uint32_t lba_lo;      // big-endian, lower 32 bits
    uint32_t block_count; // big-endian
    uint8_t reserved[4];
} scsi_unmap_block_desc_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_block_desc_t) == 16, "size is not correct");

// SBC-3 §5.25.1: UNMAP parameter list header + one block descriptor
typedef struct TU_ATTR_PACKED
{
    uint16_t data_length;       // big-endian (total bytes - 2)
    uint16_t block_desc_length; // big-endian
    uint8_t reserved[4];
    scsi_unmap_block_desc_t desc;
} scsi_unmap_param_t;
TU_VERIFY_STATIC(sizeof(scsi_unmap_param_t) == 24, "size is not correct");

// Superset of msc_csw_status_t (defined in msc_host.c) with a timeout value.
typedef enum
{
    MSC_STATUS_PASSED,      // == MSC_CSW_STATUS_PASSED
    MSC_STATUS_FAILED,      // == MSC_CSW_STATUS_FAILED
    MSC_STATUS_PHASE_ERROR, // == MSC_CSW_STATUS_PHASE_ERROR
    MSC_STATUS_TIMED_OUT,   // returned on I/O timeout
} msc_status_t;

uint8_t tuh_msc_protocol(uint8_t dev_addr);
msc_status_t tuh_msc_scsi_sync(uint8_t dev_addr, msc_cbw_t *cbw,
                               const void *data, uint32_t timeout_ms);

// Override of the weak tuh_msc_pump() default in msc_host.c.
// Pumps USB events and all application tasks during blocking I/O.
// FatFs re-entry would be a problem so main_task() never calls FatFs
// but it does call the required tuh_task().
void tuh_msc_pump(void) { main_task(); }

// Initialize a CBW for a volume's LUN.
// Signature and tag are stamped by tuh_msc_scsi_submit().
static inline void msc_cbw_init(msc_cbw_t *cbw, uint8_t vol,
                                uint32_t total_bytes, uint8_t dir,
                                uint8_t cmd_len, const void *cmd)
{
    memset(cbw, 0, sizeof(msc_cbw_t));
    cbw->lun = msc_vol[vol].lun;
    cbw->total_bytes = total_bytes;
    cbw->dir = dir;
    cbw->cmd_len = cmd_len;
    memcpy(cbw->command, cmd, cmd_len);
}

// Core SCSI helper with autosense.
static msc_status_t msc_scsi_command(uint8_t vol, msc_cbw_t *cbw,
                                     const void *data, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t dev_addr = msc_vol[vol].dev_addr;
    msc_status_t status = tuh_msc_scsi_sync(dev_addr, cbw, data, timeout_ms);
    if (status == MSC_STATUS_TIMED_OUT)
        return status;
    if (status == MSC_STATUS_PHASE_ERROR)
    {
        msc_vol[vol].sense_key = SCSI_SENSE_NONE;
        msc_vol[vol].sense_asc = 0;
        msc_vol[vol].sense_ascq = 0;
        return status;
    }
    if (status == MSC_STATUS_PASSED &&
        tuh_msc_protocol(dev_addr) != MSC_PROTOCOL_CBI_NO_INTERRUPT)
    {
        msc_vol[vol].last_ok = get_absolute_time();
        return status;
    }
    scsi_sense_fixed_resp_t sense_resp;
    memset(&sense_resp, 0, sizeof(sense_resp));
    scsi_request_sense_t const sense_cmd = {
        .cmd_code = SCSI_CMD_REQUEST_SENSE,
        .alloc_length = sizeof(scsi_sense_fixed_resp_t)};
    msc_cbw_t sense_cbw;
    msc_cbw_init(&sense_cbw, vol, sizeof(scsi_sense_fixed_resp_t), TUSB_DIR_IN_MASK,
                 sizeof(sense_cmd), &sense_cmd);
    int64_t remaining_ms = absolute_time_diff_us(get_absolute_time(), deadline) / 1000;
    uint32_t sense_timeout = remaining_ms > MSC_SCSI_OP_TIMEOUT_MS
                                 ? (uint32_t)remaining_ms
                                 : MSC_SCSI_OP_TIMEOUT_MS;
    msc_status_t sense_status = tuh_msc_scsi_sync(
        dev_addr, &sense_cbw, &sense_resp, sense_timeout);
    bool sense_data_valid = (sense_status == MSC_STATUS_PASSED) ||
                            (sense_status != MSC_STATUS_TIMED_OUT &&
                             sense_resp.response_code != 0);
    if (sense_data_valid && sense_resp.response_code)
    {
        msc_vol[vol].sense_key = sense_resp.sense_key;
        msc_vol[vol].sense_asc = sense_resp.add_sense_code;
        msc_vol[vol].sense_ascq = sense_resp.add_sense_qualifier;
    }
    else
    {
        msc_vol[vol].sense_key = SCSI_SENSE_NONE;
        msc_vol[vol].sense_asc = 0;
        msc_vol[vol].sense_ascq = 0;
    }
    if (tuh_msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
    {
        // CB: sense data is the only outcome indicator (no transport status).
        // CBI: sense data overrides the interrupt status to handle recovered errors.
        if (!sense_data_valid || sense_status == MSC_STATUS_TIMED_OUT)
        {
            status = MSC_STATUS_TIMED_OUT;
        }
        else if (msc_vol[vol].sense_key == SCSI_SENSE_NONE ||
                 msc_vol[vol].sense_key == SCSI_SENSE_RECOVERED_ERROR)
        {
            status = MSC_STATUS_PASSED;
            msc_vol[vol].last_ok = get_absolute_time();
        }
        else
        {
            status = MSC_STATUS_FAILED;
        }
    }
    return status;
}

static msc_status_t msc_scsi_inquiry(uint8_t vol,
                                     scsi_inquiry_resp_t *resp)
{
    scsi_inquiry_t const cmd = {
        .cmd_code = SCSI_CMD_INQUIRY,
        .alloc_length = sizeof(scsi_inquiry_resp_t)};
    memset(resp, 0, sizeof(*resp));
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_inquiry_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_RW_TIMEOUT_MS);
    // Per SPC-4 §6.4.1, INQUIRY is one of the few commands that executes in any
    // device state and explicitly does not clear a UNIT ATTENTION condition.
    if (msc_vol[vol].sense_key == SCSI_SENSE_UNIT_ATTENTION &&
        resp->response_data_format != 0)
        status = MSC_STATUS_PASSED;
    DBG_CMD(vol, "INQUIRY", status);
    return status;
}

static msc_status_t msc_scsi_test_unit_ready(uint8_t vol)
{
    scsi_test_unit_ready_t const cmd = {.cmd_code = SCSI_CMD_TEST_UNIT_READY};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "TUR", status);
    return status;
}

static msc_status_t msc_scsi_read_capacity10(uint8_t vol, scsi_read_capacity10_resp_t *resp)
{
    scsi_read_capacity10_t const cmd = {.cmd_code = SCSI_CMD_READ_CAPACITY_10};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_capacity10_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "READ CAPACITY(10)", status);
    return status;
}

static msc_status_t msc_scsi_read_capacity16(uint8_t vol, scsi_read_capacity16_resp_t *resp)
{
    scsi_read_capacity16_t const cmd = {
        .cmd_code = 0x9E,
        .service_action = 0x10,
        .alloc_length = tu_htonl(sizeof(scsi_read_capacity16_resp_t)),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_capacity16_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "READ CAPACITY(16)", status);
    return status;
}

#if FF_LBA64
static msc_status_t msc_scsi_read16(uint8_t vol,
                                    void *buff, uint64_t lba, uint16_t block_count,
                                    uint32_t block_size)
{
    scsi_read16_t const cmd = {
        .cmd_code = 0x88,
        .lba_hi = tu_htonl((uint32_t)(lba >> 32)),
        .lba_lo = tu_htonl((uint32_t)lba),
        .block_count = tu_htonl(block_count),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, (uint32_t)block_count * block_size, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "READ(16)", status);
    return status;
}

static msc_status_t msc_scsi_write16(uint8_t vol,
                                     const void *buff, uint64_t lba, uint16_t block_count,
                                     uint32_t block_size)
{
    scsi_write16_t const cmd = {
        .cmd_code = 0x8A,
        .lba_hi = tu_htonl((uint32_t)(lba >> 32)),
        .lba_lo = tu_htonl((uint32_t)lba),
        .block_count = tu_htonl(block_count),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, (uint32_t)block_count * block_size, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "WRITE(16)", status);
    return status;
}
#endif // FF_LBA64

static msc_status_t msc_scsi_read_format_capacities(uint8_t vol, void *resp)
{
    scsi_read_format_capacity_t const cmd = {
        .cmd_code = SCSI_CMD_READ_FORMAT_CAPACITY,
        .alloc_length = tu_htons(sizeof(scsi_read_format_capacity_data_t))};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_read_format_capacity_data_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "READ FORMAT CAPACITIES", status);
    return status;
}

static msc_status_t msc_scsi_mode_sense6(uint8_t vol, uint8_t page_code, scsi_mode_sense6_resp_t *resp)
{
    scsi_mode_sense6_t const cmd = {
        .cmd_code = SCSI_CMD_MODE_SENSE_6,
        .disable_block_descriptor = 1,
        .page_code = page_code,
        .alloc_length = sizeof(scsi_mode_sense6_resp_t),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_mode_sense6_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "MODE SENSE(6)", status);
    return status;
}

static msc_status_t msc_scsi_mode_sense10(uint8_t vol,
                                          uint8_t page_code, scsi_mode_sense10_resp_t *resp)
{
    scsi_mode_sense10_t const cmd = {
        .cmd_code = 0x5A,
        .disable_block_descriptor = 1,
        .page_code = page_code,
        .alloc_length = tu_htons(sizeof(scsi_mode_sense10_resp_t)),
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(scsi_mode_sense10_resp_t), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "MODE SENSE(10)", status);
    return status;
}

static msc_status_t msc_scsi_sync_cache10(uint8_t vol)
{
    uint8_t cmd[10] = {0x35}; // SYNCHRONIZE CACHE (10)
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, 0, TUSB_DIR_OUT, 10, cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, NULL, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "SYNC CACHE(10)", status);
    return status;
}

static msc_status_t msc_scsi_unmap(uint8_t vol, LBA_t lba, uint32_t block_count)
{
    scsi_unmap_t const cmd = {
        .cmd_code = 0x42,
        .param_list_length = tu_htons(sizeof(scsi_unmap_param_t)),
    };
    scsi_unmap_param_t param = {
        .data_length = tu_htons(sizeof(scsi_unmap_param_t) - 2),
        .block_desc_length = tu_htons(sizeof(scsi_unmap_block_desc_t)),
        .desc = {
#if FF_LBA64
            .lba_hi = tu_htonl((uint32_t)((uint64_t)lba >> 32)),
#endif
            .lba_lo = tu_htonl((uint32_t)lba),
            .block_count = tu_htonl(block_count),
        },
    };
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(param), TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, &param, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "UNMAP", status);
    return status;
}

static msc_status_t msc_scsi_read10(uint8_t vol,
                                    void *buff, uint32_t lba, uint16_t block_count,
                                    uint32_t block_size)
{
    scsi_read10_t const cmd = {
        .cmd_code = SCSI_CMD_READ_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, block_count * block_size, TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "READ(10)", status);
    return status;
}

static msc_status_t msc_scsi_write10(uint8_t vol,
                                     const void *buff, uint32_t lba, uint16_t block_count,
                                     uint32_t block_size)
{
    scsi_write10_t const cmd = {
        .cmd_code = SCSI_CMD_WRITE_10,
        .lba = tu_htonl(lba),
        .block_count = tu_htons(block_count)};
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, block_count * block_size, TUSB_DIR_OUT, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, buff, MSC_SCSI_RW_TIMEOUT_MS);
    DBG_CMD(vol, "WRITE(10)", status);
    return status;
}

// Read device capacity.
// CBI: READ FORMAT CAPACITIES (UFI mandatory command).
// BOT: tries READ CAPACITY(16) first (also captures LBPME for TRIM gating);
//      falls back to READ CAPACITY(10) for older SBC-2 devices.
// Returns true on success, populating block_count and block_size.
static bool msc_read_capacity(uint8_t vol)
{
    uint8_t dev_addr = msc_vol[vol].dev_addr;

    if (tuh_msc_protocol(dev_addr) != MSC_PROTOCOL_BOT)
    {
        // CBI: READ FORMAT CAPACITIES
        scsi_read_format_capacity_data_t rfc = {0};
        if (msc_scsi_read_format_capacities(vol, &rfc) != MSC_STATUS_PASSED)
            return false;
        if (rfc.list_length < 8 || (rfc.list_length % 8) != 0)
            return false;
        uint32_t blocks = tu_ntohl(rfc.block_num);
        uint32_t bsize = ((uint32_t)rfc.reserved2 << 16) | tu_ntohs(rfc.block_size_u16);
        if (blocks == 0 || bsize == 0 ||
            (bsize & (bsize - 1)) != 0 || bsize > 4096)
            return false;
        msc_vol[vol].block_count = blocks;
        msc_vol[vol].block_size = bsize;
    }
    else
    {
        // BOT: try READ CAPACITY(16) on SPC-3+ devices (INQUIRY version >= 0x05).
        scsi_read_capacity16_resp_t cap16 = {0};
        if (msc_vol[vol].spc_version >= 0x05 &&
            msc_scsi_read_capacity16(vol, &cap16) == MSC_STATUS_PASSED)
        {
            uint64_t last_lba64 = ((uint64_t)tu_ntohl(cap16.last_lba_hi) << 32) |
                                  (uint64_t)tu_ntohl(cap16.last_lba_lo);
            uint32_t bsize16 = tu_ntohl(cap16.block_size);
            if (bsize16 == 0 || (bsize16 & (bsize16 - 1)) != 0 || bsize16 > 4096)
                return false;
#if !FF_LBA64
            if (last_lba64 > UINT32_MAX)
                return false; // >2TB not supported without FF_LBA64
#endif
            msc_vol[vol].block_count = last_lba64 + 1;
            msc_vol[vol].block_size = bsize16;
            msc_vol[vol].lbpme = (cap16.lbpme_byte >> 7) & 1;
            DBG_VOL(vol, "READ CAPACITY(16): %llu blocks, %lu bytes/block, LBPME=%d\n",
                    (unsigned long long)msc_vol[vol].block_count,
                    (unsigned long)bsize16, msc_vol[vol].lbpme);
            return true;
        }
        // BOT: READ CAPACITY(10) — mandatory SBC command, works on all devices.
        scsi_read_capacity10_resp_t cap10 = {0};
        if (msc_scsi_read_capacity10(vol, &cap10) != MSC_STATUS_PASSED)
            return false;
        uint32_t last_lba = tu_ntohl(cap10.last_lba);
        if (last_lba == 0xFFFFFFFF)
            return false; // >2TB sentinel without FF_LBA64 (or RC(16) already failed)
        uint32_t bsize = tu_ntohl(cap10.block_size);
        if (bsize == 0 || (bsize & (bsize - 1)) != 0 || bsize > 4096)
            return false;
        msc_vol[vol].block_count = last_lba + 1;
        msc_vol[vol].block_size = bsize;
    }

    return true;
}

// Determine write protection via MODE SENSE.
// CBI: MODE SENSE(10) (UFI mandatory command, opcode 0x5A).
// BOT: MODE SENSE(6) (SBC mandatory command, opcode 0x1A).
// Non-fatal: defaults to not protected on failure.
static void msc_sense_write_protect(uint8_t vol)
{
    uint8_t dev_addr = msc_vol[vol].dev_addr;
    if (tuh_msc_protocol(dev_addr) == MSC_PROTOCOL_BOT)
    {
        scsi_mode_sense6_resp_t ms6;
        if (msc_scsi_mode_sense6(vol, 0x3F, &ms6) == MSC_STATUS_PASSED)
        {
            DBG_VOL(vol, "MODE SENSE(6) WP=%d\n", ms6.write_protected);
            msc_vol[vol].write_prot = ms6.write_protected;
        }
    }
    else
    {
        scsi_mode_sense10_resp_t ms10;
        if (msc_scsi_mode_sense10(vol, 0x3F, &ms10) == MSC_STATUS_PASSED)
        {
            DBG_VOL(vol, "MODE SENSE(10) WP=%d\n", ms10.write_protected);
            msc_vol[vol].write_prot = ms10.write_protected;
        }
    }
}

// Probe VPD page B2 to check whether the device supports SCSI UNMAP.
static bool msc_probe_unmap(uint8_t vol)
{
    scsi_inquiry_t const cmd = {
        .cmd_code = SCSI_CMD_INQUIRY,
        .reserved1 = 1, // EVPD = 1 (bit 0 of byte 1)
        .page_code = 0xB2,
        .alloc_length = sizeof(scsi_vpd_lbp_t),
    };
    scsi_vpd_lbp_t resp;
    memset(&resp, 0, sizeof(resp));
    msc_cbw_t cbw;
    msc_cbw_init(&cbw, vol, sizeof(resp), TUSB_DIR_IN_MASK, sizeof(cmd), &cmd);
    msc_status_t status = msc_scsi_command(vol, &cbw, &resp, MSC_SCSI_OP_TIMEOUT_MS);
    DBG_CMD(vol, "INQUIRY VPD B2", status);
    if (status != MSC_STATUS_PASSED || resp.page_code != 0xB2)
        return false;
    DBG_VOL(vol, "VPD B2: LBPU=%d\n", resp.lbpu);
    return resp.lbpu;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const max_lun = tuh_msc_get_maxlun(dev_addr);
    for (uint8_t lun = 0; lun <= max_lun; lun++)
    {
        // Find a free FatFS volume slot.
        uint8_t vol = FF_VOLUMES;
        for (uint8_t v = 0; v < FF_VOLUMES; v++)
        {
            if (msc_vol[v].status == msc_volume_free)
            {
                vol = v;
                break;
            }
        }
        if (vol == FF_VOLUMES)
        {
            DBG("MSC mount: no free vol for dev %d LUN %d\n", dev_addr, lun);
            continue;
        }
        msc_vol[vol].dev_addr = dev_addr;
        msc_vol[vol].lun = lun;
        msc_vol[vol].status = msc_volume_registered;
        TCHAR volstr[6];
        msc_vol_path(volstr, vol);
        f_mount(&msc_vol[vol].fatfs, volstr, 0);
        DBG_VOL(vol, "mount dev_addr %d LUN %d\n", dev_addr, lun);
    }
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_vol[vol].dev_addr == dev_addr &&
            msc_vol[vol].status != msc_volume_free)
        {
            TCHAR volstr[6];
            msc_vol_path(volstr, vol);
            f_unmount(volstr);
            msc_vol[vol].status = msc_volume_free;
            msc_vol[vol].dev_addr = 0;
            msc_vol[vol].lun = 0;
            msc_vol[vol].block_count = 0;
            msc_vol[vol].block_size = 0;
            msc_vol[vol].sense_key = 0;
            msc_vol[vol].sense_asc = 0;
            msc_vol[vol].sense_ascq = 0;
            msc_vol[vol].write_prot = false;
            msc_vol[vol].unmap_supported = false;
            msc_vol[vol].sync_cache_supressed = false;
            msc_vol[vol].lbpme = false;
            msc_vol[vol].removable = false;
            msc_vol[vol].spc_version = 0;
            DBG_VOL(vol, "unmounted (dev_addr %d)\n", dev_addr);
        }
    }
}

DWORD get_fattime(void)
{
    struct timespec ts;
    struct tm tm;
    if (aon_timer_get_time(&ts))
    {
        time_t t = (time_t)ts.tv_sec;
        localtime_r(&t, &tm);
        if (tm.tm_year + 1900 >= 1980 && tm.tm_year + 1900 <= 2107)
            return ((DWORD)(tm.tm_year + 1900 - 1980) << 25) |
                   ((DWORD)(tm.tm_mon + 1) << 21) |
                   ((DWORD)tm.tm_mday << 16) |
                   ((WORD)tm.tm_hour << 11) |
                   ((WORD)tm.tm_min << 5) |
                   ((WORD)(tm.tm_sec >> 1));
    }
    return ((DWORD)0 << 25 | (DWORD)1 << 21 | (DWORD)1 << 16);
}

DSTATUS disk_status(BYTE pdrv)
{
    // We only support partition 0. One vol per physical drive.
    uint8_t vol = pdrv;
    if (msc_vol[vol].status != msc_volume_mounted)
    {
        DBG_VOL(vol, "disk_status, not mounted, status=%d\n", msc_vol[vol].status);
        return STA_NOINIT;
    }
    // Test for removed media if we haven't used the drive in a while.
    if (msc_vol[vol].removable &&
        time_reached(delayed_by_ms(msc_vol[vol].last_ok, MSC_DISK_STATUS_TIMEOUT_MS)))
    {
        DBG_VOL(vol, "disk_status, issuing TUR\n");
        if (msc_scsi_test_unit_ready(vol) == MSC_STATUS_FAILED)
        {
            uint8_t asc = msc_vol[vol].sense_asc;
            if (asc == 0x3A || asc == 0x28) // MEDIUM NOT PRESENT or MAY HAVE CHANGED
            {
                msc_vol[vol].status = msc_volume_ejected;
                msc_vol[vol].block_count = 0;
                msc_vol[vol].block_size = 0;
                msc_vol[vol].write_prot = false;
                msc_vol[vol].sense_key = SCSI_SENSE_NONE;
                msc_vol[vol].sense_asc = 0;
                msc_vol[vol].sense_ascq = 0;
                return STA_NOINIT;
            }
        }
    }
    return msc_vol[vol].write_prot ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    uint8_t vol = pdrv;
    DBG_VOL(vol, "disk_initialize, status=%d\n", msc_vol[vol].status);

    if (msc_vol[vol].status == msc_volume_registered ||
        msc_vol[vol].status == msc_volume_ejected)
    {
        // ---- INQUIRY (first mount only) ----
        if (msc_vol[vol].status == msc_volume_registered)
        {
            scsi_inquiry_resp_t inq;
            if (msc_scsi_inquiry(vol, &inq) != MSC_STATUS_PASSED)
                return STA_NOINIT;
            msc_vol[vol].removable = inq.is_removable;
            msc_vol[vol].spc_version = inq.version;
        }

        // ---- TUR ----
        absolute_time_t tur_deadline = make_timeout_time_ms(MSC_SCSI_RW_TIMEOUT_MS);
        int tur_count = 0;
        bool tur_ok;
        do
        {
            tur_count++;
            tur_ok = msc_scsi_test_unit_ready(vol) == MSC_STATUS_PASSED;
        } while (!tur_ok && !time_reached(tur_deadline) &&
                 // Only one retry for media sense. Need for TEAC floppy.
                 ((tur_count == 1 && msc_vol[vol].sense_key == SCSI_SENSE_NOT_READY) ||
                  // Normal UA clearing per the specs.
                  (msc_vol[vol].sense_key == SCSI_SENSE_UNIT_ATTENTION)));
        if (!tur_ok)
        {
            if (msc_vol[vol].removable)
                msc_vol[vol].status = msc_volume_ejected;
        }
        // ---- CAPACITY ----
        else if (msc_read_capacity(vol))
        {
            // ---- WRITE PROTECTION ----
            msc_sense_write_protect(vol);
            // ---- UNMAP SUPPORT ----
            if (!msc_vol[vol].write_prot &&
                tuh_msc_protocol(msc_vol[vol].dev_addr) == MSC_PROTOCOL_BOT &&
                msc_vol[vol].lbpme)
                msc_vol[vol].unmap_supported = msc_probe_unmap(vol);
            msc_vol[vol].status = msc_volume_mounted;
        }
    }

    if (msc_vol[vol].status == msc_volume_ejected)
        return STA_NODISK;

    if (msc_vol[vol].status != msc_volume_mounted)
        return STA_NOINIT;

    return msc_vol[vol].write_prot ? STA_PROTECT : 0;
}

static DRESULT msc_status_to_dresult(uint8_t vol, msc_status_t status)
{
    if (status == MSC_STATUS_PASSED)
        return RES_OK;
    if (status == MSC_STATUS_TIMED_OUT)
        return RES_NOTRDY;
    uint8_t sk = msc_vol[vol].sense_key;
    if (sk == SCSI_SENSE_NOT_READY)
        return RES_NOTRDY;
    if (sk == SCSI_SENSE_DATA_PROTECT)
        return RES_WRPRT;
    if (sk == SCSI_SENSE_ILLEGAL_REQUEST)
        return RES_PARERR;
    return RES_ERROR;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t vol = pdrv;
    uint32_t const block_size = msc_vol[vol].block_size;
    uint8_t const dev_addr = msc_vol[vol].dev_addr;
    if (block_size == 0 || dev_addr == 0)
        return RES_NOTRDY;
    // Clamp each transfer so total_bytes fits the USB host transfer
    // length limit (uint16_t).
    uint16_t const max_blocks = (uint16_t)(UINT16_MAX / block_size);
    while (count > 0)
    {
        uint16_t n = (count > max_blocks) ? max_blocks : (uint16_t)count;
        msc_status_t status;
#if FF_LBA64
        if (sector > UINT32_MAX)
            status = msc_scsi_read16(vol, buff, (uint64_t)sector, n, block_size);
        else
#endif
            status = msc_scsi_read10(vol, buff, (uint32_t)sector, n, block_size);
        if (status != MSC_STATUS_PASSED)
            return msc_status_to_dresult(vol, status);
        buff += (uint32_t)n * block_size;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t vol = pdrv;
    if (msc_vol[vol].write_prot)
        return RES_WRPRT;
    uint32_t const block_size = msc_vol[vol].block_size;
    uint8_t const dev_addr = msc_vol[vol].dev_addr;
    if (block_size == 0 || dev_addr == 0)
        return RES_NOTRDY;
    uint16_t const max_blocks = (uint16_t)(UINT16_MAX / block_size);
    while (count > 0)
    {
        uint16_t n = (count > max_blocks) ? max_blocks : (uint16_t)count;
        msc_status_t status;
#if FF_LBA64
        if (sector > UINT32_MAX)
            status = msc_scsi_write16(vol, buff, (uint64_t)sector, n, block_size);
        else
#endif
            status = msc_scsi_write10(vol, buff, (uint32_t)sector, n, block_size);
        if (status != MSC_STATUS_PASSED)
            return msc_status_to_dresult(vol, status);
        buff += (uint32_t)n * block_size;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    uint8_t vol = pdrv;
    switch (cmd)
    {
    case CTRL_SYNC:
    {
        if (msc_vol[vol].dev_addr == 0)
            return RES_NOTRDY;
        if (msc_vol[vol].write_prot)
            return RES_OK;
        if (tuh_msc_protocol(msc_vol[vol].dev_addr) != MSC_PROTOCOL_BOT)
            return RES_OK;
        if (msc_vol[vol].sync_cache_supressed)
            return RES_OK;
        msc_status_t status = msc_scsi_sync_cache10(vol);
        if (status == MSC_STATUS_FAILED &&
            msc_vol[vol].sense_key == SCSI_SENSE_ILLEGAL_REQUEST)
        {
            msc_vol[vol].sync_cache_supressed = true;
            return RES_OK;
        }
        return msc_status_to_dresult(vol, status);
    }
    case GET_SECTOR_COUNT:
#if FF_LBA64
        *((LBA_t *)buff) = msc_vol[vol].block_count;
#else
        *((DWORD *)buff) = (DWORD)msc_vol[vol].block_count;
#endif
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)msc_vol[vol].block_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *((DWORD *)buff) = 1;
        return RES_OK;
    case CTRL_TRIM:
    {
        if (!msc_vol[vol].unmap_supported)
            return RES_OK;
        LBA_t *rt = (LBA_t *)buff;
        LBA_t start = rt[0];
        LBA_t end = rt[1];
        if (start > end)
            return RES_PARERR;
        // UNMAP block descriptor block_count is 32-bit.
        if ((end - start) >= (LBA_t)UINT32_MAX)
            return RES_PARERR;
        msc_status_t status = msc_scsi_unmap(vol, start, (uint32_t)(end - start + 1));
        return status == MSC_STATUS_TIMED_OUT ? RES_NOTRDY : RES_OK;
    }
    default:
        return RES_PARERR;
    }
}

int msc_status_count(void)
{
    int count = 0;
    for (uint8_t vol = 0; vol < FF_VOLUMES; vol++)
    {
        if (msc_vol[vol].status == msc_volume_registered ||
            msc_vol[vol].status == msc_volume_mounted ||
            msc_vol[vol].status == msc_volume_ejected)
            count++;
    }
    return count;
}

// Some vendors pad their strings with spaces, others with zeros.
// This will ensure zeros, which prints better.
static void msc_inquiry_rtrims(uint8_t *s, size_t l)
{
    while (l--)
    {
        if (s[l] == ' ')
            s[l] = '\0';
        else
            break;
    }
}

int msc_status_response(char *buf, size_t buf_size, int state)
{
    if (state < 0 || state >= FF_VOLUMES)
        return -1;
    uint8_t vol = state;
    if (msc_vol[vol].status == msc_volume_registered ||
        msc_vol[vol].status == msc_volume_mounted ||
        msc_vol[vol].status == msc_volume_ejected)
    {
        // Refresh or init media status
        if (disk_status(vol) == STA_NOINIT)
            disk_initialize(vol);

        char sizebuf[24];
        if (msc_vol[vol].status != msc_volume_mounted)
        {
            snprintf(sizebuf, sizeof(sizebuf), "%s", STR_PARENS_NO_MEDIA);
        }
        else
        {
            // Floppy-era media (under 5 MB): raw/1024/1000
            // Everything else: pure decimal raw/1000/1000
            const char *xb;
            double size;
            uint64_t raw = (uint64_t)msc_vol[vol].block_count *
                           (uint64_t)msc_vol[vol].block_size;
            if (raw < 5000000ULL)
            {
                xb = "KB";
                size = raw / 1024.0;
                if (size >= 1000)
                {
                    xb = "MB";
                    size /= 1000;
                }
                // no %g, strip zeros manually
                char num[16];
                snprintf(num, sizeof(num), "%.3f", size);
                char *p = num + strlen(num) - 1;
                while (*p == '0')
                    *p-- = '\0';
                if (*p == '.')
                    *p = '\0';
                snprintf(sizebuf, sizeof(sizebuf), "%s %s", num, xb);
            }
            else
            {
                xb = "MB";
                size = raw / 1e6;
                if (size >= 1000)
                {
                    xb = "GB";
                    size /= 1000;
                }
                if (size >= 1000)
                {
                    xb = "TB";
                    size /= 1000;
                }
                snprintf(sizebuf, sizeof(sizebuf), "%.1f %s", size, xb);
            }
        }
        scsi_inquiry_resp_t inq;
        msc_status_t csw = msc_scsi_inquiry(vol, &inq);
        if (csw == MSC_STATUS_PASSED)
        {
            msc_inquiry_rtrims(inq.vendor_id, 8);
            msc_inquiry_rtrims(inq.product_id, 16);
            msc_inquiry_rtrims(inq.product_rev, 4);
            snprintf(buf, buf_size, STR_STATUS_MSC,
                     VolumeStr[vol],
                     sizebuf,
                     inq.vendor_id,
                     inq.product_id,
                     inq.product_rev);
        }
        else
        {
            snprintf(buf, buf_size, STR_STATUS_MSC,
                     VolumeStr[vol],
                     sizebuf,
                     STR_PARENS_NONE, STR_PARENS_NONE, "");
        }
    }
    return state + 1;
}

static FIL *msc_std_validate_fil(int desc)
{
    if (desc < 0 || desc >= MSC_STD_FIL_MAX)
        return NULL;
    if (!msc_std_fil_pool[desc].obj.fs)
        return NULL;
    return &msc_std_fil_pool[desc];
}

bool msc_std_handles(const char *path)
{
    (void)path;
    // MSC/FatFS is the catch-all handler
    return true;
}

int msc_std_open(const char *path, uint8_t flags, api_errno *err)
{
    static_assert(FA_READ == 0x01);
    static_assert(FA_WRITE == 0x02);
    const uint8_t RDWR = 0x03;
    const uint8_t CREAT = 0x10;
    const uint8_t TRUNC = 0x20;
    const uint8_t APPEND = 0x40;
    const uint8_t EXCL = 0x80;

    uint8_t mode = flags & RDWR;
    if (flags & CREAT)
    {
        if (flags & EXCL)
            mode |= FA_CREATE_NEW;
        else if (flags & TRUNC)
            mode |= FA_CREATE_ALWAYS;
        else if (flags & APPEND)
            mode |= FA_OPEN_APPEND;
        else
            mode |= FA_OPEN_ALWAYS;
    }

    FIL *fp = NULL;
    for (int i = 0; i < MSC_STD_FIL_MAX; i++)
    {
        if (!msc_std_fil_pool[i].obj.fs)
        {
            fp = &msc_std_fil_pool[i];
            break;
        }
    }
    if (!fp)
    {
        *err = API_EMFILE;
        return -1;
    }

    FRESULT fresult = f_open(fp, path, mode);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }

    return (int)(fp - msc_std_fil_pool);
}

int msc_std_close(int desc, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FRESULT fresult = f_close(fp);
    fp->obj.fs = NULL;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}

std_rw_result msc_std_read(int desc, char *buf, uint32_t count, uint32_t *bytes_read, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *bytes_read = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT br;
    FRESULT fresult = f_read(fp, buf, count, &br);
    *bytes_read = br;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

std_rw_result msc_std_write(int desc, const char *buf, uint32_t count, uint32_t *bytes_written, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *bytes_written = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    UINT bw;
    FRESULT fresult = f_write(fp, buf, count, &bw);
    *bytes_written = bw;
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return STD_ERROR;
    }
    return STD_OK;
}

int msc_std_lseek(int desc, int8_t whence, int32_t offset, int32_t *pos, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FSIZE_t absolute_offset;
    if (whence == SEEK_SET)
    {
        if (offset < 0)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = offset;
    }
    else if (whence == SEEK_CUR)
    {
        FSIZE_t current_pos = f_tell(fp);
        if (offset < 0 && (FSIZE_t)(-offset) > current_pos)
        {
            *err = API_EINVAL;
            return -1;
        }
        if (offset > 0 && (FSIZE_t)offset > (~(FSIZE_t)0) - current_pos)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = current_pos + offset;
    }
    else if (whence == SEEK_END)
    {
        FSIZE_t file_size = f_size(fp);
        if (offset < 0 && (FSIZE_t)(-offset) > file_size)
        {
            *err = API_EINVAL;
            return -1;
        }
        if (offset > 0 && (FSIZE_t)offset > (~(FSIZE_t)0) - file_size)
        {
            *err = API_EINVAL;
            return -1;
        }
        absolute_offset = file_size + offset;
    }
    else
    {
        *err = API_EINVAL;
        return -1;
    }
    FRESULT fresult = f_lseek(fp, absolute_offset);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    FSIZE_t fpos = f_tell(fp);
    if (fpos > 0x7FFFFFFF)
        fpos = 0x7FFFFFFF;
    *pos = fpos;
    return 0;
}

int msc_std_sync(int desc, api_errno *err)
{
    FIL *fp = msc_std_validate_fil(desc);
    if (!fp)
    {
        *err = API_EBADF;
        return -1;
    }
    FRESULT fresult = f_sync(fp);
    if (fresult != FR_OK)
    {
        *err = api_errno_from_fatfs(fresult);
        return -1;
    }
    return 0;
}
