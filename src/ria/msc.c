/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "ff.h"
#include "diskio.h"

static scsi_inquiry_resp_t inquiry_resp;

static FATFS fatfs[CFG_TUH_DEVICE_MAX]; // for simplicity only support 1 LUN per device
static volatile bool _disk_busy[CFG_TUH_DEVICE_MAX];

bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    msc_cbw_t const *cbw = cb_data->cbw;
    msc_csw_t const *csw = cb_data->csw;

    if (csw->status != 0)
    {
        printf("USB mass storage device inquiry failed\n");
        return false;
    }

    // Print out Vendor ID, Product ID and Rev
    printf("%.8s %.16s rev %.4s\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev);

    // Get capacity of device
    uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
    uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);
    printf("Disk Size: %lu MB\r\n", block_count / ((1024 * 1024) / block_size));
    // printf("Block Count = %lu, Block Size: %lu\r\n", block_count, block_size);

    uint8_t const drive_num = dev_addr - 1;
    char drive_path[3] = "0:";
    drive_path[0] += drive_num;

    if (f_mount(&fatfs[drive_num], drive_path, 1) != FR_OK)
    {
        puts("mount failed");
    }

    // change to newly mounted drive
    f_chdir(drive_path);
    f_chdrive(drive_path);

    //  char label[34];
    //  if ( FR_OK == f_getlabel(drive_path, label, NULL) )
    //  {
    //    puts(label);
    //  }

    printf("MSC mount: address = %d, drive_path = %s\n", dev_addr, drive_path);

    return true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    uint8_t const lun = 0;
    tuh_msc_inquiry(dev_addr, lun, &inquiry_resp, inquiry_complete_cb, 0);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    (void)dev_addr;
    printf("USB mass storage device unmount\n");

    uint8_t const drive_num = dev_addr - 1;
    char drive_path[3] = "0:";
    drive_path[0] += drive_num;

    f_unmount(drive_path);
}

static void wait_for_disk_io(BYTE pdrv)
{
    while (_disk_busy[pdrv])
    {
        // TODO all USB tasks (hid)
        tuh_task();
    }
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    (void)dev_addr;
    (void)cb_data;
    _disk_busy[dev_addr - 1] = false;
    return true;
}

DSTATUS disk_status(BYTE pdrv)
{
    uint8_t dev_addr = pdrv + 1;
    return tuh_msc_mounted(dev_addr) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return 0; // nothing to do
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t const dev_addr = pdrv + 1;
    uint8_t const lun = 0;

    _disk_busy[pdrv] = true;
    tuh_msc_read10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0);
    wait_for_disk_io(pdrv);

    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint8_t const dev_addr = pdrv + 1;
    uint8_t const lun = 0;

    _disk_busy[pdrv] = true;
    tuh_msc_write10(dev_addr, lun, buff, sector, (uint16_t)count, disk_io_complete, 0);
    wait_for_disk_io(pdrv);

    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    uint8_t const dev_addr = pdrv + 1;
    uint8_t const lun = 0;
    switch (cmd)
    {
    case CTRL_SYNC:
        // nothing to do since we do blocking
        return RES_OK;

    case GET_SECTOR_COUNT:
        *((DWORD *)buff) = (WORD)tuh_msc_get_block_count(dev_addr, lun);
        return RES_OK;

    case GET_SECTOR_SIZE:
        *((WORD *)buff) = (WORD)tuh_msc_get_block_size(dev_addr, lun);
        return RES_OK;

    case GET_BLOCK_SIZE:
        *((DWORD *)buff) = 1; // erase block size in units of sector size
        return RES_OK;

    default:
        return RES_PARERR;
    }

    return RES_OK;
}

void msc_ls(const uint8_t *args)
{
    // TODO I think the monitor should work in TCHARs
    assert(sizeof TCHAR == sizeof uint8_t);

    const uint8_t *dpath = ".";
    if (args[0])
        dpath = args;

    DIR dir;
    if (FR_OK != f_opendir(&dir, dpath))
    {
        printf("?cannot access '%s': No such file or directory\n", dpath);
        return;
    }

    FILINFO fno;
    while ((f_readdir(&dir, &fno) == FR_OK) && (fno.fname[0] != 0))
    {
        if (fno.fname[0] != '.')
        {
            if (fno.fattrib & AM_DIR)
                printf("<DIR> %s\n", fno.fname);
            else
                printf("      %s\n", fno.fname);
        }
    }

    f_closedir(&dir);
}

void msc_cd(const uint8_t *args)
{
    if (!args[0])
    {
        // TODO print current directory
        printf("?invalid arguments\n");
        return;
    }
    if ((FR_OK != f_chdir(args)) || (FR_OK != f_chdrive(args)))
    {
        printf("?No such file or directory\n");
        return;
    }
}
