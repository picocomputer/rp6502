#include "hardware/flash.h"
#include "littlefs/lfs.h"
#include "pico/printf.h"
#include "sys/lfs.h"
#include "lfs.h"

static const char settings_fname[] = "settings.cfg";

// lfs_t lfs_volume;
lfs_file_t lfs_file;

bool readSettings(SETTINGS_T *p)
{
    bool ok = false;
    uint8_t file_buffer[FLASH_PAGE_SIZE];
    struct lfs_file_config file_config = {
        .buffer = file_buffer,
    };
    if (lfs_file_opencfg(&lfs_volume, &lfs_file, settings_fname, LFS_O_RDONLY, &file_config) == LFS_ERR_OK)
    {
        if (lfs_file_read(&lfs_volume, &lfs_file, p, sizeof(SETTINGS_T)) == sizeof(SETTINGS_T))
            ok = true;
        lfs_file_close(&lfs_volume, &lfs_file);
    }
    return ok;
}

bool writeSettings(SETTINGS_T *p)
{
    bool ok = false;
    uint8_t file_buffer[FLASH_PAGE_SIZE];
    struct lfs_file_config file_config = {
        .buffer = file_buffer,
    };
    if (lfs_file_opencfg(&lfs_volume, &lfs_file, settings_fname, LFS_O_RDWR | LFS_O_CREAT, &file_config) == LFS_ERR_OK)
    {
        if (lfs_file_write(&lfs_volume, &lfs_file, p, sizeof(SETTINGS_T)) == sizeof(SETTINGS_T))
        {
            ok = true;
        }
        lfs_file_close(&lfs_volume, &lfs_file);
    }
    return ok;
}
