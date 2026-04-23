/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/mon.h"
#include "mon/uf2.h"
#include "str/str.h"
#include "sys/mem.h"
#include <boot/uf2.h>
#include <fatfs/ff.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#include <pico/binary_info/defs.h>
#include <pico/binary_info/structure.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

// #define DEBUG_RIA_MON_UF2

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_UF2)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define UF2_PROG_NAME_RIA "RP6502-RIA"
#define UF2_PROG_NAME_RIA_W "RP6502-RIA-W"
#define UF2_PROG_NAME_VGA "RP6502-VGA"

#define UF2_MAP_TABLE_MAX 10 // matches picotool's arbitrary cap
#define UF2_NAME_READ_MAX 32

static enum {
    UF2_IDLE,
    UF2_WRITE,
    UF2_REBOOT,
    UF2_FAILED,
} uf2_state;

static FIL uf2_fil;
static uint32_t uf2_num_blocks;
static uint32_t uf2_block_idx;
static uint32_t uf2_first_target;
static int uf2_last_percent;
static FSIZE_t uf2_main_start; // file offset of main firmware's block 0

static struct
{
    uint32_t source;
    uint32_t dest_start;
    uint32_t dest_end;
} uf2_map_table[UF2_MAP_TABLE_MAX];
static int uf2_map_count;

static void uf2_close(void)
{
    if (uf2_fil.obj.fs)
    {
        f_close(&uf2_fil);
        uf2_fil.obj.fs = NULL;
    }
    uf2_state = UF2_IDLE;
}

// Detect the RP2350-E10 "absolute block" workaround that picotool's
// elf2uf2 injects at the start of RP2350 flash UF2s. Picotool skips
// these (see check_abs_block in elf2uf2.cpp and build_rmap_uf2 in
// main.cpp). Block must already be in mbuf.
static bool uf2_is_abs_block(void)
{
    struct uf2_block *b = (struct uf2_block *)mbuf;
    if (b->magic_start0 != UF2_MAGIC_START0)
        return false;
    if (b->magic_start1 != UF2_MAGIC_START1)
        return false;
    if (b->magic_end != UF2_MAGIC_END)
        return false;
    if ((b->flags & ~UF2_FLAG_EXTENSION_FLAGS_PRESENT) != UF2_FLAG_FAMILY_ID_PRESENT)
        return false;
    if (b->payload_size != 256)
        return false;
    if (b->num_blocks != 2)
        return false;
    if (b->file_size != ABSOLUTE_FAMILY_ID)
        return false;
    if (b->block_no != 0)
        return false;
    for (int i = 0; i < 256; i++)
        if (b->data[i] != 0xEF)
            return false;
    if (b->flags & UF2_FLAG_EXTENSION_FLAGS_PRESENT)
    {
        uint32_t ext;
        memcpy(&ext, b->data + 256, 4);
        if (ext != UF2_EXTENSION_RP2_IGNORE_BLOCK)
            return false;
    }
    return true;
}

// Stored flash address -> UF2 file offset (absolute; includes uf2_main_start).
static int32_t uf2_addr_to_file_off(uint32_t stored_addr, uint32_t needed)
{
    if (stored_addr < uf2_first_target)
        return -1;
    uint32_t image_offset = stored_addr - uf2_first_target;
    uint32_t block_no = image_offset / 256;
    uint32_t in_payload = image_offset % 256;
    if (in_payload + needed > 256)
        return -1;
    if (block_no >= uf2_num_blocks)
        return -1;
    return (int32_t)(uf2_main_start + block_no * 512 + 32 + in_payload);
}

// Runtime pointer -> UF2 file offset via mapping table, identity fallback.
static int32_t uf2_ptr_to_file_off(uint32_t ptr, uint32_t needed)
{
    for (int i = 0; i < uf2_map_count; i++)
    {
        if (ptr >= uf2_map_table[i].dest_start &&
            ptr < uf2_map_table[i].dest_end)
        {
            uint32_t stored = uf2_map_table[i].source +
                              (ptr - uf2_map_table[i].dest_start);
            return uf2_addr_to_file_off(stored, needed);
        }
    }
    return uf2_addr_to_file_off(ptr, needed);
}

static bool uf2_read_at(int32_t file_off, uint32_t n)
{
    if (file_off < 0 || n > MBUF_SIZE)
        return false;
    FRESULT fr = f_lseek(&uf2_fil, (FSIZE_t)file_off);
    if (fr != FR_OK)
        return false;
    UINT br;
    fr = f_read(&uf2_fil, mbuf, n, &br);
    return fr == FR_OK && br == n;
}

// Locate and extract the program_name from binary_info. Clobbers mbuf.
// On success, NUL-terminated string is at mbuf[0..]. Mirrors picotool's
// find_binary_info() / id_and_string path (see picotool main.cpp:2456).
// Picotool scans the first 256 uint32 words (1024 B) on RP2350; we match
// that by assembling the payloads of the first up to 4 main-firmware UF2
// blocks into mbuf.
static const char *uf2_find_program_name(void)
{
    uint32_t want_blocks = uf2_num_blocks < 4 ? uf2_num_blocks : 4;
    for (uint32_t i = 0; i < want_blocks; i++)
    {
        FSIZE_t off = uf2_main_start + (FSIZE_t)(i * 512 + 32);
        if (f_lseek(&uf2_fil, off) != FR_OK)
            return NULL;
        UINT br;
        if (f_read(&uf2_fil, mbuf + i * 256, 256, &br) != FR_OK || br != 256)
            return NULL;
    }
    uint32_t scan_words = want_blocks * 256 / 4;

    const uint32_t *payload = (const uint32_t *)mbuf;
    int found = -1;
    uint32_t bi_addr_start = 0, bi_addr_end = 0, map_tbl_ptr = 0;
    for (uint32_t i = 0; i + 4 < scan_words; i++)
    {
        if (payload[i] != BINARY_INFO_MARKER_START)
            continue;
        if (payload[i + 4] != BINARY_INFO_MARKER_END)
            continue;
        uint32_t from = payload[i + 1];
        uint32_t to = payload[i + 2];
        if (to <= from)
            continue;
        if ((from & 3) || (to & 3))
            continue;
        found = (int)i;
        bi_addr_start = from;
        bi_addr_end = to;
        map_tbl_ptr = payload[i + 3];
        break;
    }
    if (found < 0)
    {
        DBG("UF2 binary_info marker not found in first %lu bytes\n",
            (unsigned long)(want_blocks * 256));
        return NULL;
    }
    DBG("UF2 binary_info marker at image offset %lu\n",
        (unsigned long)(found * 4));
    DBG("UF2 bi_addr=[0x%08lX..0x%08lX) map=0x%08lX\n",
        (unsigned long)bi_addr_start, (unsigned long)bi_addr_end,
        (unsigned long)map_tbl_ptr);

    uf2_map_count = 0;
    while (map_tbl_ptr && uf2_map_count < UF2_MAP_TABLE_MAX)
    {
        uint32_t entry_ptr = map_tbl_ptr + (uint32_t)(uf2_map_count * 12);
        int32_t off = uf2_addr_to_file_off(entry_ptr, 12);
        if (off < 0)
            break;
        if (!uf2_read_at(off, 12))
            break;
        uint32_t src, ds, de;
        memcpy(&src, mbuf, 4);
        memcpy(&ds, mbuf + 4, 4);
        memcpy(&de, mbuf + 8, 4);
        if (src == 0)
            break;
        uf2_map_table[uf2_map_count].source = src;
        uf2_map_table[uf2_map_count].dest_start = ds;
        uf2_map_table[uf2_map_count].dest_end = de;
        DBG("UF2 map[%d] src=0x%08lX dst=[0x%08lX..0x%08lX)\n",
            uf2_map_count, (unsigned long)src,
            (unsigned long)ds, (unsigned long)de);
        uf2_map_count++;
    }

    for (uint32_t slot = bi_addr_start; slot < bi_addr_end; slot += 4)
    {
        int32_t slot_off = uf2_ptr_to_file_off(slot, 4);
        if (slot_off < 0)
        {
            DBG("UF2 bi slot 0x%08lX off image\n", (unsigned long)slot);
            return NULL;
        }
        if (!uf2_read_at(slot_off, 4))
            return NULL;
        uint32_t entry_ptr;
        memcpy(&entry_ptr, mbuf, 4);

        int32_t core_off = uf2_ptr_to_file_off(entry_ptr, 4);
        if (core_off < 0)
            continue;
        if (!uf2_read_at(core_off, 4))
            continue;
        uint16_t type, tag;
        memcpy(&type, mbuf, 2);
        memcpy(&tag, mbuf + 2, 2);
        if (type != BINARY_INFO_TYPE_ID_AND_STRING ||
            tag != BINARY_INFO_TAG_RASPBERRY_PI)
            continue;

        int32_t id_off = uf2_ptr_to_file_off(entry_ptr + 4, 8);
        if (id_off < 0)
            continue;
        if (!uf2_read_at(id_off, 8))
            continue;
        uint32_t id, value;
        memcpy(&id, mbuf, 4);
        memcpy(&value, mbuf + 4, 4);
        if (id != BINARY_INFO_ID_RP_PROGRAM_NAME)
            continue;

        int32_t str_off = uf2_ptr_to_file_off(value, 1);
        if (str_off < 0)
        {
            DBG("UF2 program_name ptr 0x%08lX off image\n",
                (unsigned long)value);
            return NULL;
        }
        if (!uf2_read_at(str_off, UF2_NAME_READ_MAX))
            return NULL;
        if (!memchr(mbuf, 0, UF2_NAME_READ_MAX))
        {
            DBG("UF2 program_name not NUL-terminated within %d bytes\n",
                UF2_NAME_READ_MAX);
            return NULL;
        }
        DBG("UF2 program_name=\"%s\"\n", (const char *)mbuf);
        return (const char *)mbuf;
    }
    DBG("UF2 no program_name entry in binary_info\n");
    return NULL;
}

// Validate a UF2 block header already in mbuf. Captures num_blocks and
// first_target when block_no==0.
static bool uf2_check_block(uint32_t block_no)
{
    struct uf2_block *b = (struct uf2_block *)mbuf;
    if (b->magic_start0 != UF2_MAGIC_START0)
    {
        DBG("UF2 block %lu bad magic_start0=0x%08lX\n",
            (unsigned long)block_no, (unsigned long)b->magic_start0);
        return false;
    }
    if (b->magic_start1 != UF2_MAGIC_START1)
    {
        DBG("UF2 block %lu bad magic_start1=0x%08lX\n",
            (unsigned long)block_no, (unsigned long)b->magic_start1);
        return false;
    }
    if (b->magic_end != UF2_MAGIC_END)
    {
        DBG("UF2 block %lu bad magic_end=0x%08lX\n",
            (unsigned long)block_no, (unsigned long)b->magic_end);
        return false;
    }
    if (!(b->flags & UF2_FLAG_FAMILY_ID_PRESENT))
    {
        DBG("UF2 block %lu no FAMILY_ID_PRESENT flag (flags=0x%08lX)\n",
            (unsigned long)block_no, (unsigned long)b->flags);
        return false;
    }
    if (b->flags & UF2_FLAG_NOT_MAIN_FLASH)
    {
        DBG("UF2 block %lu NOT_MAIN_FLASH flag set (flags=0x%08lX)\n",
            (unsigned long)block_no, (unsigned long)b->flags);
        return false;
    }
    if (b->file_size != RP2350_ARM_S_FAMILY_ID &&
        b->file_size != ABSOLUTE_FAMILY_ID)
    {
        DBG("UF2 block %lu family=0x%08lX (want RP2350_ARM_S 0x%08lX or ABSOLUTE 0x%08lX)\n",
            (unsigned long)block_no, (unsigned long)b->file_size,
            (unsigned long)RP2350_ARM_S_FAMILY_ID,
            (unsigned long)ABSOLUTE_FAMILY_ID);
        return false;
    }
    if (b->payload_size != 256)
    {
        DBG("UF2 block %lu payload_size=%lu (want 256)\n",
            (unsigned long)block_no, (unsigned long)b->payload_size);
        return false;
    }
    if (b->block_no != block_no)
    {
        DBG("UF2 block_no=%lu expected %lu\n",
            (unsigned long)b->block_no, (unsigned long)block_no);
        return false;
    }
    if (block_no == 0)
    {
        uf2_num_blocks = b->num_blocks;
        uf2_first_target = b->target_addr;
        DBG("UF2 block 0 OK: num_blocks=%lu first_target=0x%08lX\n",
            (unsigned long)uf2_num_blocks, (unsigned long)uf2_first_target);
    }
    else
    {
        if (b->num_blocks != uf2_num_blocks)
        {
            DBG("UF2 block %lu num_blocks=%lu expected %lu\n",
                (unsigned long)block_no, (unsigned long)b->num_blocks,
                (unsigned long)uf2_num_blocks);
            return false;
        }
        if (b->target_addr != uf2_first_target + block_no * 256)
        {
            DBG("UF2 block %lu target_addr=0x%08lX expected 0x%08lX\n",
                (unsigned long)block_no, (unsigned long)b->target_addr,
                (unsigned long)(uf2_first_target + block_no * 256));
            return false;
        }
    }
    return true;
}

static void __no_inline_not_in_flash_func(uf2_write_page)(
    uint32_t flash_offs, const uint8_t *data, bool erase_sector)
{
    if (erase_sector)
        flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offs, data, FLASH_PAGE_SIZE);
}

static void uf2_do_write(void)
{
    UINT br;
    FRESULT fr = f_read(&uf2_fil, mbuf, 512, &br);
    if (fr != FR_OK || br != 512 || !uf2_check_block(uf2_block_idx))
    {
        DBG("UF2 write-phase read/validate failed at block %lu (fr=%d br=%u)\n",
            (unsigned long)uf2_block_idx, (int)fr, (unsigned)br);
        uf2_state = UF2_FAILED;
        return;
    }
    struct uf2_block *b = (struct uf2_block *)mbuf;
    uint32_t flash_offs = b->target_addr - XIP_BASE;
    bool erase_sector = (uf2_block_idx % 16) == 0;
    if (erase_sector)
        DBG("UF2 erase+prog sector @0x%08lX (block %lu/%lu)\n",
            (unsigned long)flash_offs, (unsigned long)uf2_block_idx,
            (unsigned long)uf2_num_blocks);
    uf2_write_page(flash_offs, b->data, erase_sector);

    uf2_block_idx++;

    int pct = (int)((uint64_t)uf2_block_idx * 100 / uf2_num_blocks);
    if (pct != uf2_last_percent && pct < 100)
    {
        uf2_last_percent = pct;
        printf("\rFlashing: %d%%", pct);
        stdio_flush();
    }

    if (uf2_block_idx >= uf2_num_blocks)
    {
        printf("\rFlashing: 100%%\n");
        stdio_flush();
        uf2_state = UF2_REBOOT;
    }
}

// uf2_task uses direct printf+stdio_flush for its \r-repainted progress line
// and advisory/error messages; the line-oriented mon_response pipeline can't
// express a \r-updated counter. This is the only subsystem that does so.
void uf2_task(void)
{
    switch (uf2_state)
    {
    case UF2_IDLE:
        break;
    case UF2_WRITE:
        uf2_do_write();
        break;
    case UF2_REBOOT:
        stdio_flush();
        watchdog_reboot(0, 0, 0);
        while (1)
            tight_loop_contents();
    case UF2_FAILED:
        printf("\n?Flash failed, recover with BOOTSEL\n");
        stdio_flush();
        while (1)
            tight_loop_contents();
    }
}

bool uf2_active(void)
{
    return uf2_state != UF2_IDLE;
}

void uf2_mon_flash(const char *args)
{
    const char *path = str_parse_string(&args);
    if (!path || !str_parse_end(args))
    {
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    DBG("UF2 FLASH \"%s\"\n", path);

    FRESULT fr = f_open(&uf2_fil, path, FA_READ);
    if (fr != FR_OK)
    {
        DBG("UF2 f_open failed fr=%d\n", (int)fr);
        mon_add_response_fatfs(fr);
        uf2_fil.obj.fs = NULL;
        return;
    }
    FSIZE_t fsize = f_size(&uf2_fil);
    DBG("UF2 f_size=%lu\n", (unsigned long)fsize);

    // Scan past any leading RP2350-E10 abs blocks to find main firmware.
    uf2_main_start = 0;
    for (;;)
    {
        if (uf2_main_start + 512 > fsize)
        {
            DBG("UF2 no main-firmware block found before EOF\n");
            uf2_close();
            mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
            return;
        }
        if (f_lseek(&uf2_fil, uf2_main_start) != FR_OK)
        {
            uf2_close();
            mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
            return;
        }
        UINT br;
        fr = f_read(&uf2_fil, mbuf, 512, &br);
        if (fr != FR_OK || br != 512)
        {
            DBG("UF2 read at offset %lu failed (fr=%d br=%u)\n",
                (unsigned long)uf2_main_start, (int)fr, (unsigned)br);
            uf2_close();
            mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
            return;
        }
        if (!uf2_is_abs_block())
            break;
        DBG("UF2 skipping abs_block at offset %lu\n",
            (unsigned long)uf2_main_start);
        uf2_main_start += 512;
    }
    DBG("UF2 main firmware starts at file offset %lu\n",
        (unsigned long)uf2_main_start);

    if (!uf2_check_block(0))
    {
        DBG("UF2 main block 0 validate failed\n");
        uf2_close();
        mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
        return;
    }

    FSIZE_t main_end = uf2_main_start + (FSIZE_t)uf2_num_blocks * 512;
    if (main_end > fsize)
    {
        DBG("UF2 truncated: f_size=%lu need at least %lu\n",
            (unsigned long)fsize, (unsigned long)main_end);
        uf2_close();
        mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
        return;
    }

    const char *name = uf2_find_program_name();
    if (!name)
    {
        uf2_close();
        mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
        return;
    }

    if (!strcmp(name, UF2_PROG_NAME_VGA))
    {
        DBG("UF2 VGA target, stubbed\n");
        uf2_close();
        printf("?TODO\n");
        return;
    }

    if (strcmp(name, UF2_PROG_NAME_RIA) &&
        strcmp(name, UF2_PROG_NAME_RIA_W))
    {
        DBG("UF2 unrecognized program_name \"%s\"\n", name);
        uf2_close();
        mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
        return;
    }

    printf("A Pi Pico cannot be bricked. If flashing fails, recover via USB\n"
           "using the standard BOOTSEL method.\n");
    stdio_flush();

    fr = f_lseek(&uf2_fil, uf2_main_start);
    if (fr != FR_OK)
    {
        DBG("UF2 rewind f_lseek failed fr=%d\n", (int)fr);
        uf2_close();
        mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
        return;
    }
    uf2_block_idx = 0;
    uf2_last_percent = -1;
    DBG("UF2 entering write phase, %lu blocks from offset %lu\n",
        (unsigned long)uf2_num_blocks, (unsigned long)uf2_main_start);
    uf2_state = UF2_WRITE;
}
