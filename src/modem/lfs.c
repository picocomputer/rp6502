
#include "hardware/flash.h"
#include "littlefs/lfs.h"
#include "pico/printf.h"
#include "lfs.h"

static const char settings_fname[] = "settings.cfg";

lfs_t lfs_volume;
lfs_file_t lfs_file;

#define LFS_DISK_BLOCKS 128

#define LFS_DISK_SIZE (LFS_DISK_BLOCKS * FLASH_SECTOR_SIZE)
#define LFS_LOOKAHEAD_SIZE (LFS_DISK_BLOCKS / 8)

static char lfs_read_buffer[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
static char lfs_prog_buffer[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
static char lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE] __attribute__((aligned(4)));
static struct lfs_config cfg;

static int lfs_read(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, void *buffer, lfs_size_t size)
{
    (void)(c);
    memcpy(buffer,
           (void *)XIP_NOCACHE_NOALLOC_BASE +
               (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) +
               (block * FLASH_SECTOR_SIZE) +
               off,
           size);
    return LFS_ERR_OK;
}

static int lfs_prog(const struct lfs_config *c, lfs_block_t block,
                    lfs_off_t off, const void *buffer, lfs_size_t size)
{
    (void)(c);
    uint32_t flash_offs = (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) +
                          (block * FLASH_SECTOR_SIZE) +
                          off;
    flash_range_program(flash_offs, (const uint8_t *)buffer, size);
    return LFS_ERR_OK;
}

static int lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)(c);
    uint32_t flash_offs = (PICO_FLASH_SIZE_BYTES - LFS_DISK_SIZE) +
                          (block * FLASH_SECTOR_SIZE);
    flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    return LFS_ERR_OK;
}

static int lfs_sync(const struct lfs_config *c)
{
    (void)(c);
    return LFS_ERR_OK;
}

void initLFS(void)
{
    memset((void *)&cfg, 0, sizeof(cfg));
    cfg = (struct lfs_config){
        .read = lfs_read,
        .prog = lfs_prog,
        .erase = lfs_erase,
        .sync = lfs_sync,
        .read_size = 1,
        .prog_size = FLASH_PAGE_SIZE,
        .block_size = FLASH_SECTOR_SIZE,
        .block_count = LFS_DISK_SIZE / FLASH_SECTOR_SIZE,
        .block_cycles = 100,
        .cache_size = FLASH_PAGE_SIZE,
        .lookahead_size = LFS_LOOKAHEAD_SIZE,
        .read_buffer = lfs_read_buffer,
        .prog_buffer = lfs_prog_buffer,
        .lookahead_buffer = lfs_lookahead_buffer,
    };
    int err = lfs_mount(&lfs_volume, &cfg);
    if (err)
    {
        // Maybe first boot. Attempt format.
        // lfs_format returns -84 here, but still succeeds
        printf("Formatting - please wait\n");
        lfs_format(&lfs_volume, &cfg);
        err = lfs_mount(&lfs_volume, &cfg);
        if (err)
            printf("?Unable to format lfs (%d)", err);
        else
            printf("Formatting done\n");
    }
}

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
