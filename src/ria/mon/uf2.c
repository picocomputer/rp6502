/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// References:
// https://github.com/microsoft/uf2
// https://github.com/raspberrypi/picotool

#include "mon/mon.h"
#include "mon/uf2.h"
#include "str/str.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/vga.h"
#include <boot/uf2.h>
#include <fatfs/ff.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#include <pico/binary_info/defs.h>
#include <pico/binary_info/structure.h>
#include <pico/bootrom.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_UF2)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define UF2_MAP_TABLE_MAX 10 // small arbitrary cap; picotool also uses one
#define UF2_NAME_READ_MAX 32
// Fault-detection ceiling. Typical SPI NOR: 4K erase ~45ms + 16x page
// program ~8ms = ~55ms. Worst-case datasheet: erase up to 400ms + programs
// up to 48ms. Success path returns as soon as the ack lands.
#define UF2_VGA_FLASH_ACK_TIMEOUT_MS 500

static enum {
    UF2_IDLE,
    UF2_WRITE,
    UF2_REBOOT,
    UF2_FAILED,
    UF2_VGA_STREAM,   // read next UF2 block, push its payload into VGA xram
    UF2_VGA_WAIT_ACK, // sent $1:F:05; polling pix_wait_poll()
    UF2_VGA_SUCCESS,  // send $1:F:06 word=0 then watchdog_reboot RIA
    UF2_VGA_LOCKUP,   // send $1:F:06 word=1 then spin RIA forever
} uf2_state;

static FIL uf2_fil;
static uint32_t uf2_num_blocks;
static uint32_t uf2_block_idx;
static uint32_t uf2_first_target;
static uint16_t uf2_payload_size;
static int uf2_last_percent;
static FSIZE_t uf2_main_start;
// RIA path: page accumulator progress.
static uint32_t uf2_cur_page;   // last page flash offset in accumulator; -1u invalid
static uint32_t uf2_cur_sector; // last erased sector index; -1u invalid
// VGA path: sector index most recently sent to VGA for flashing, and the
// resume flag used when a sector-transition deferral interrupts a block
// mid-processing.
static uint32_t uf2_vga_last_sector;
static bool uf2_vga_has_deferred_block;

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
// Assumes the main firmware region is a contiguous run of blocks starting at
// uf2_first_target with stride uf2_payload_size. If future tooling emits
// non-contiguous main firmware, name lookups through this helper can miss;
// the write path does not depend on this helper.
static int32_t uf2_addr_to_file_off(uint32_t stored_addr, uint32_t needed)
{
    if (stored_addr < uf2_first_target)
        return -1;
    uint32_t image_offset = stored_addr - uf2_first_target;
    uint32_t block_no = image_offset / uf2_payload_size;
    uint32_t in_payload = image_offset % uf2_payload_size;
    if (in_payload + needed > uf2_payload_size)
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
// find_binary_info() / id_and_string path.
// Picotool scans the first 1024 B of the main image on RP2350; we match
// that by assembling the payloads of the first main-firmware UF2 blocks
// that together cover at least 1024 B (capped at MBUF_SIZE and num_blocks).
static const char *uf2_find_program_name(void)
{
    uint32_t want_blocks = MBUF_SIZE / uf2_payload_size;
    if (want_blocks == 0)
        want_blocks = 1;
    if (want_blocks > uf2_num_blocks)
        want_blocks = uf2_num_blocks;
    for (uint32_t i = 0; i < want_blocks; i++)
    {
        FSIZE_t off = uf2_main_start + (FSIZE_t)(i * 512 + 32);
        if (f_lseek(&uf2_fil, off) != FR_OK)
            return NULL;
        UINT br;
        if (f_read(&uf2_fil, mbuf + i * uf2_payload_size,
                   uf2_payload_size, &br) != FR_OK ||
            br != uf2_payload_size)
            return NULL;
    }
    uint32_t scan_words = want_blocks * uf2_payload_size / 4;

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
            (unsigned long)(want_blocks * uf2_payload_size));
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

// Validate a UF2 block header already in mbuf (or at any address). The
// pointer argument lets callers validate blocks read into scratch areas
// other than mbuf (e.g. the write-phase page accumulator layout). Captures
// num_blocks, first_target, and payload_size when block_no==0.
static bool uf2_check_block_at(const void *blk, uint32_t block_no)
{
    const struct uf2_block *b = (const struct uf2_block *)blk;
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
    if (b->block_no != block_no)
    {
        DBG("UF2 block_no=%lu expected %lu\n",
            (unsigned long)b->block_no, (unsigned long)block_no);
        return false;
    }
    if (block_no == 0)
    {
        if (b->payload_size < 1 || b->payload_size > 476)
        {
            DBG("UF2 block 0 payload_size=%lu out of spec range 1..476\n",
                (unsigned long)b->payload_size);
            return false;
        }
        uf2_num_blocks = b->num_blocks;
        uf2_first_target = b->target_addr;
        uf2_payload_size = (uint16_t)b->payload_size;
        DBG("UF2 block 0 OK: num_blocks=%lu first_target=0x%08lX payload_size=%u\n",
            (unsigned long)uf2_num_blocks, (unsigned long)uf2_first_target,
            (unsigned)uf2_payload_size);
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
        if (b->payload_size != uf2_payload_size)
        {
            DBG("UF2 block %lu payload_size=%lu expected %u\n",
                (unsigned long)block_no, (unsigned long)b->payload_size,
                (unsigned)uf2_payload_size);
            return false;
        }
    }
    return true;
}

static inline bool uf2_check_block(uint32_t block_no)
{
    return uf2_check_block_at(mbuf, block_no);
}

// Emit a progress line when the percent ticks.
static void uf2_progress(void)
{
    int pct = (int)((uint64_t)uf2_block_idx * 100 / uf2_num_blocks);
    if (pct != uf2_last_percent)
    {
        uf2_last_percent = pct;
        printf(STR_UF2_FLASHING, pct);
    }
}

// mbuf layout during RIA write phase:
//   [0..FLASH_PAGE_SIZE)                 — page accumulator (persisted)
//   [FLASH_PAGE_SIZE .. FLASH_PAGE_SIZE+512) — 512-B block read
// (MBUF_SIZE == 1024 B; these are disjoint. See src/ria/sys/mem.h.)
#define UF2_RIA_PAGE_BUF (mbuf)
#define UF2_RIA_BLOCK_BUF (mbuf + FLASH_PAGE_SIZE)

// Stream n bytes of src to flash_addr through the page accumulator. Erases
// each new sector on first visit; programs each page when the write pointer
// moves out of it. Returns false if the incoming address would revisit an
// already-programmed sector (not supported — would require re-erasing a
// sector we've already burned into).
static bool uf2_ria_write(uint32_t flash_addr, const uint8_t *src, uint32_t n)
{
    while (n)
    {
        uint32_t page = flash_addr & ~(FLASH_PAGE_SIZE - 1);
        uint32_t off = flash_addr & (FLASH_PAGE_SIZE - 1);
        if (page != uf2_cur_page)
        {
            if (uf2_cur_page != (uint32_t)-1)
                flash_range_program(uf2_cur_page, UF2_RIA_PAGE_BUF, FLASH_PAGE_SIZE);
            uint32_t sec = page / FLASH_SECTOR_SIZE;
            if (sec != uf2_cur_sector)
            {
                if (uf2_cur_sector != (uint32_t)-1 && sec <= uf2_cur_sector)
                {
                    DBG("UF2 sector revisit rejected: cur=%lu new=%lu\n",
                        (unsigned long)uf2_cur_sector, (unsigned long)sec);
                    return false;
                }
                DBG("UF2 erase sector @0x%08lX (block %lu/%lu)\n",
                    (unsigned long)(sec * FLASH_SECTOR_SIZE),
                    (unsigned long)uf2_block_idx,
                    (unsigned long)uf2_num_blocks);
                flash_range_erase(sec * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
                uf2_cur_sector = sec;
            }
            memset(UF2_RIA_PAGE_BUF, 0xFF, FLASH_PAGE_SIZE);
            uf2_cur_page = page;
        }
        uint32_t chunk = FLASH_PAGE_SIZE - off;
        if (chunk > n)
            chunk = n;
        memcpy(UF2_RIA_PAGE_BUF + off, src, chunk);
        src += chunk;
        flash_addr += chunk;
        n -= chunk;
    }
    return true;
}

// Program the in-flight page. Called at end of image.
static void uf2_ria_flush(void)
{
    if (uf2_cur_page != (uint32_t)-1)
    {
        flash_range_program(uf2_cur_page, UF2_RIA_PAGE_BUF, FLASH_PAGE_SIZE);
        uf2_cur_page = (uint32_t)-1;
    }
}

static void uf2_do_write(void)
{
    UINT br;
    FRESULT fr = f_read(&uf2_fil, UF2_RIA_BLOCK_BUF, 512, &br);
    if (fr != FR_OK || br != 512 ||
        !uf2_check_block_at(UF2_RIA_BLOCK_BUF, uf2_block_idx))
    {
        DBG("UF2 write-phase read/validate failed at block %lu (fr=%d br=%u)\n",
            (unsigned long)uf2_block_idx, (int)fr, (unsigned)br);
        uf2_state = UF2_FAILED;
        return;
    }
    struct uf2_block *b = (struct uf2_block *)UF2_RIA_BLOCK_BUF;
    uint32_t flash_addr = b->target_addr - XIP_BASE;
#ifdef DEBUG_RIA_MON_UF2_SIMULATE_FAILURE
    // Corrupt sector 0 so the flash is demonstrably broken after reboot —
    // lets us exercise the full failure-UX path end to end, including
    // what happens after a manual power-cycle on a half-written image.
    if (uf2_block_idx == 0)
    {
        for (uint32_t i = 0; i < uf2_payload_size; i++)
            b->data[i] ^= 0xFF;
        uf2_ria_write(flash_addr, b->data, uf2_payload_size);
        uf2_ria_flush();
        uf2_block_idx++;
        uf2_progress();
        uf2_state = UF2_FAILED;
        return;
    }
#endif
    if (!uf2_ria_write(flash_addr, b->data, uf2_payload_size))
    {
        uf2_state = UF2_FAILED;
        return;
    }

    uf2_block_idx++;
    uf2_progress();

    if (uf2_block_idx >= uf2_num_blocks)
    {
        uf2_ria_flush();
        putchar('\n');
        uf2_state = UF2_REBOOT;
    }
}

// Stream one UF2 block's payload into VGA xram, using xram as a 4 KB sector
// accumulator. A sector transition (next block targets a different sector,
// or image ends) triggers a $1:F:05 to VGA and a wait for ack/nak. When a
// transition happens mid-stream, the just-read block is deferred: it is
// stored in mbuf and processed after the ack returns.
static void uf2_do_vga_stream_block(void)
{
    if (!uf2_vga_has_deferred_block)
    {
        UINT br;
        FRESULT fr = f_read(&uf2_fil, mbuf, 512, &br);
        if (fr != FR_OK || br != 512 || !uf2_check_block(uf2_block_idx))
        {
            DBG("UF2 VGA read/validate failed at block %lu (fr=%d br=%u)\n",
                (unsigned long)uf2_block_idx, (int)fr, (unsigned)br);
            uf2_state = UF2_VGA_LOCKUP;
            return;
        }
    }
    struct uf2_block *b = (struct uf2_block *)mbuf;
    uint32_t flash_addr = b->target_addr - XIP_BASE;
    uint32_t sector = flash_addr / FLASH_SECTOR_SIZE;
    uint32_t off_in_sector = flash_addr % FLASH_SECTOR_SIZE;

    if (off_in_sector + uf2_payload_size > FLASH_SECTOR_SIZE)
    {
        DBG("UF2 VGA block %lu straddles sector boundary — unsupported layout\n",
            (unsigned long)uf2_block_idx);
        uf2_state = UF2_VGA_LOCKUP;
        return;
    }

    // Sector transition: flush the in-flight sector first, defer this block.
    if (uf2_cur_sector != (uint32_t)-1 && sector != uf2_cur_sector)
    {
        DBG("UF2 VGA flush sector %lu (about to start %lu)\n",
            (unsigned long)uf2_cur_sector, (unsigned long)sector);
        uf2_vga_last_sector = uf2_cur_sector;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x05, (uint16_t)uf2_cur_sector);
        uf2_cur_sector = (uint32_t)-1;
        uf2_vga_has_deferred_block = true;
        pix_wait_begin(UF2_VGA_FLASH_ACK_TIMEOUT_MS);
        uf2_state = UF2_VGA_WAIT_ACK;
        return;
    }
    uf2_vga_has_deferred_block = false;

    // New sector: pre-fill xram with 0xFF so gaps read as erased flash.
    if (sector != uf2_cur_sector)
    {
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++)
            pix_send_blocking(PIX_DEVICE_XRAM, 0, 0xFF, (uint16_t)i);
        uf2_cur_sector = sector;
    }

#ifdef DEBUG_RIA_MON_UF2_SIMULATE_FAILURE
    // Corrupt every byte in every block of sector 0 so the resulting
    // flash is demonstrably broken — lets us exercise the full
    // failure-UX path end to end, including what the VGA looks like
    // after a manual power-cycle on a half-written image.
    if (sector == 0)
    {
        for (uint32_t i = 0; i < uf2_payload_size; i++)
            b->data[i] ^= 0xFF;
    }
#endif

    for (uint32_t i = 0; i < uf2_payload_size; i++)
        pix_send_blocking(PIX_DEVICE_XRAM, 0, b->data[i],
                          (uint16_t)(off_in_sector + i));

    uf2_block_idx++;
    uf2_progress();

    // End of image: flush the in-flight sector.
    if (uf2_block_idx >= uf2_num_blocks)
    {
        DBG("UF2 VGA final flush sector %lu\n", (unsigned long)uf2_cur_sector);
        uf2_vga_last_sector = uf2_cur_sector;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x05, (uint16_t)uf2_cur_sector);
        uf2_cur_sector = (uint32_t)-1;
        pix_wait_begin(UF2_VGA_FLASH_ACK_TIMEOUT_MS);
        uf2_state = UF2_VGA_WAIT_ACK;
    }
}

static void uf2_do_vga_wait(void)
{
    switch (pix_wait_poll())
    {
    case 0:
        return;
    case 1:
#ifdef DEBUG_RIA_MON_UF2_SIMULATE_FAILURE
        // Belt-and-suspenders: if VGA unexpectedly ack'd sector 0 after
        // the payload was corrupted, force lockup.
        if (uf2_vga_last_sector == 0)
        {
            uf2_state = UF2_VGA_LOCKUP;
            return;
        }
#endif
        if (uf2_block_idx >= uf2_num_blocks)
        {
            putchar('\n');
            uf2_state = UF2_VGA_SUCCESS;
        }
        else
        {
            uf2_state = UF2_VGA_STREAM;
        }
        return;
    default:
        DBG("UF2 VGA flash failed at sector %lu\n",
            (unsigned long)uf2_vga_last_sector);
        uf2_state = UF2_VGA_LOCKUP;
        return;
    }
}

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
        break;
    case UF2_FAILED:
        printf(STR_UF2_FLASH_FAILED);
        stdio_flush();
        reset_usb_boot(0, 0);
        break;
    case UF2_VGA_STREAM:
        uf2_do_vga_stream_block();
        break;
    case UF2_VGA_WAIT_ACK:
        uf2_do_vga_wait();
        break;
    case UF2_VGA_SUCCESS:
        stdio_flush();
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x06, 0);
        // Let the PIX FIFO drain and VGA begin its reboot before we go.
        busy_wait_ms(50);
        watchdog_reboot(0, 0, 0);
        break;
    case UF2_VGA_LOCKUP:
        printf(STR_UF2_FLASH_FAILED);
        stdio_flush();
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x06, 1);
        for (;;)
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

    if (!strcmp(name, STR_UF2_PROG_NAME_VGA))
    {
        if (!vga_connected())
        {
            uf2_close();
            mon_add_response_str(STR_ERR_VGA_NOT_CONNECTED);
            return;
        }
        fr = f_lseek(&uf2_fil, uf2_main_start);
        if (fr != FR_OK)
        {
            DBG("UF2 VGA rewind f_lseek failed fr=%d\n", (int)fr);
            uf2_close();
            mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
            return;
        }
        uf2_block_idx = 0;
        uf2_last_percent = -1;
        uf2_cur_sector = (uint32_t)-1;
        uf2_vga_has_deferred_block = false;
        DBG("UF2 entering VGA stream phase, %lu blocks from offset %lu\n",
            (unsigned long)uf2_num_blocks, (unsigned long)uf2_main_start);
        uf2_state = UF2_VGA_STREAM;
        return;
    }

    if (strcmp(name, STR_UF2_PROG_NAME_RIA))
    {
        DBG("UF2 program_name \"%s\" does not match \"%s\"\n",
            name, STR_UF2_PROG_NAME_RIA);
        uf2_close();
        mon_add_response_str(STR_ERR_INVALID_UF2_FILE);
        return;
    }

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
    uf2_cur_page = (uint32_t)-1;
    uf2_cur_sector = (uint32_t)-1;
    DBG("UF2 entering write phase, %lu blocks from offset %lu\n",
        (unsigned long)uf2_num_blocks, (unsigned long)uf2_main_start);
    uf2_state = UF2_WRITE;
}
