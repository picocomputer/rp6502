/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// References:
// https://github.com/microsoft/uf2
// https://github.com/raspberrypi/picotool

#include "ria/mon/mon.h"
#include "ria/mon/uf2.h"
#include "core/str/str.h"
#include "ria/sys/com.h"
#include "core/sys/debug_log.h"
#include "ria/sys/mbuf.h"
#include "ria/sys/pix.h"
#include "ria/sys/vga.h"
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

#define UF2_MAP_TABLE_MAX 10 // small arbitrary cap; picotool also uses one
#define UF2_NAME_READ_MAX 32
// Fault-detection ceiling for one $1:F:07, which programs a page and erases
// its sector first when the page is not blank. Worst case in a SPI NOR
// datasheet is a 400ms erase plus a 3ms program, and the VGA holds its reply
// out of a 2ms window around each VSYNC. The success path returns as soon as
// the reply arrives.
#define UF2_VGA_ACK_TIMEOUT_MS 500

static enum {
    UF2_IDLE,
    UF2_VALIDATE,  // walk every block once, before anything is erased
    UF2_IDENTIFY,  // read the program name, choose a target
    UF2_WRITE,     // program this board's flash
    UF2_REBOOT,
    UF2_FAILED,
    UF2_VGA_WRITE, // stage a page in VGA xram, then send $1:F:07
    UF2_VGA_WAIT,  // polling pix_wait_poll() for the reply to $1:F:07
    UF2_VGA_SUCCESS, // $1:F:06 word=0 then watchdog_reboot RIA
    UF2_VGA_LOCKUP,  // $1:F:06 word=1 then spin RIA forever
} uf2_state;

// A block's first 32 bytes. Reading only these locates a block without
// pulling the payload behind it.
struct uf2_header
{
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size;
};
static_assert(sizeof(struct uf2_header) == 32, "uf2_header is not a block header");

static FIL uf2_fil;
static FSIZE_t uf2_main_start;
static uint32_t uf2_num_blocks;
static uint32_t uf2_block_idx;
static uint32_t uf2_low_addr;
static uint32_t uf2_stride;
static uint32_t uf2_high_addr;
static uint32_t uf2_page_idx;
static uint32_t uf2_page_count;
static int uf2_last_percent;
static bool uf2_to_vga;

// mbuf carries the 512-byte block being read and the 256-byte page built from
// it, 768 of MBUF_SIZE. They are live at the same time and nothing else runs
// while FLASH does. See src/host/pico/ria/sys/mbuf.h.
#define UF2_BLOCK ((const struct uf2_block *)mbuf)
#define UF2_PAGE (mbuf + 512)

static void uf2_close(void)
{
    if (uf2_fil.obj.fs)
    {
        f_close(&uf2_fil);
        uf2_fil.obj.fs = NULL;
    }
    uf2_state = UF2_IDLE;
}

static void uf2_invalid(void)
{
    uf2_close();
    mon_add_response_utf8(S(STR_ERR_INVALID_UF2_FILE));
}

// Firmware for this chip, in main flash. A block of another family, which
// includes the RP2350-E10 absolute block picotool writes first, is skipped
// rather than refused. See check_abs_block in picotool's elf2uf2.cpp.
static bool uf2_is_ours(const struct uf2_header *h)
{
    return h->magic_start0 == UF2_MAGIC_START0 &&
           h->magic_start1 == UF2_MAGIC_START1 &&
           (h->flags & UF2_FLAG_FAMILY_ID_PRESENT) &&
           !(h->flags & (UF2_FLAG_NOT_MAIN_FLASH | UF2_FLAG_FILE_CONTAINER)) &&
           h->file_size == RP2350_ARM_S_FAMILY_ID;
}

// Read one block header by index. Every block has been walked by the time
// this runs, so only the seek and the read can fail.
static bool uf2_read_header(uint32_t block_no, struct uf2_header *h)
{
    if (block_no >= uf2_num_blocks)
        return false;
    if (f_lseek(&uf2_fil, uf2_main_start + (FSIZE_t)block_no * 512) != FR_OK)
        return false;
    UINT br;
    return f_read(&uf2_fil, h, sizeof *h, &br) == FR_OK && br == sizeof *h;
}

// File offset of a stored flash address, with the bytes left in its payload.
// A linker emits blocks ascending by payload_size from the lowest address, so
// the index that stride predicts is tried and then checked against the block
// it lands on. A layout that stride does not describe falls back to a walk.
static int32_t uf2_find_block(uint32_t addr, uint32_t *avail)
{
    struct uf2_header h;
    if (addr >= uf2_low_addr && uf2_stride)
    {
        uint32_t guess = (addr - uf2_low_addr) / uf2_stride;
        if (guess >= uf2_num_blocks)
            guess = uf2_num_blocks - 1;
        if (uf2_read_header(guess, &h) &&
            addr >= h.target_addr && addr - h.target_addr < h.payload_size)
        {
            uint32_t in = addr - h.target_addr;
            *avail = h.payload_size - in;
            return (int32_t)(uf2_main_start + (FSIZE_t)guess * 512 + 32 + in);
        }
    }
    for (uint32_t i = 0; i < uf2_num_blocks; i++)
    {
        if (!uf2_read_header(i, &h))
            return -1;
        if (addr >= h.target_addr && addr - h.target_addr < h.payload_size)
        {
            uint32_t in = addr - h.target_addr;
            *avail = h.payload_size - in;
            return (int32_t)(uf2_main_start + (FSIZE_t)i * 512 + 32 + in);
        }
    }
    return -1;
}

// Read n bytes of the stored image from a flash address, block by block so a
// range crossing the end of a payload still reads. Returns the count read,
// which is short at the end of the image.
static uint32_t uf2_read_addr(uint32_t addr, void *dest, uint32_t n)
{
    uint8_t *out = dest;
    uint32_t got = 0;
    while (n)
    {
        uint32_t avail;
        int32_t off = uf2_find_block(addr, &avail);
        if (off < 0)
            break;
        uint32_t chunk = avail < n ? avail : n;
        if (f_lseek(&uf2_fil, (FSIZE_t)off) != FR_OK)
            break;
        UINT br;
        if (f_read(&uf2_fil, out, chunk, &br) != FR_OK || br != chunk)
            break;
        out += chunk;
        addr += chunk;
        got += chunk;
        n -= chunk;
    }
    return got;
}

struct uf2_map
{
    uint32_t source;
    uint32_t dest_start;
    uint32_t dest_end;
};

// Read n bytes from a runtime pointer, resolving through the binary_info copy
// table one chunk at a time so a range crossing a mapped region still reads.
static uint32_t uf2_read_ptr(const struct uf2_map *map, int map_count,
                             uint32_t ptr, void *dest, uint32_t n)
{
    uint8_t *out = dest;
    uint32_t got = 0;
    while (n)
    {
        uint32_t addr = ptr, chunk = n;
        for (int i = 0; i < map_count; i++)
            if (ptr >= map[i].dest_start && ptr < map[i].dest_end)
            {
                addr = map[i].source + (ptr - map[i].dest_start);
                if (map[i].dest_end - ptr < chunk)
                    chunk = map[i].dest_end - ptr;
                break;
            }
        uint32_t did = uf2_read_addr(addr, out, chunk);
        out += did;
        ptr += did;
        got += did;
        n -= did;
        if (did != chunk)
            break;
    }
    return got;
}

// Locate the program_name in binary_info, mirroring picotool's
// find_binary_info, which reads 256 words from the image base on RP2350. This
// clobbers mbuf, and on success the NUL-terminated string is left at its
// start.
static const char *uf2_find_program_name(void)
{
    if (uf2_read_addr(uf2_low_addr, mbuf, MBUF_SIZE) != MBUF_SIZE)
        return NULL;

    const uint32_t *words = (const uint32_t *)mbuf;
    uint32_t scan = MBUF_SIZE / 4;
    uint32_t bi_start = 0, bi_end = 0, map_ptr = 0;
    bool found = false;
    for (uint32_t i = 0; i + 4 < scan; i++)
    {
        if (words[i] != BINARY_INFO_MARKER_START)
            continue;
        if (words[i + 4] != BINARY_INFO_MARKER_END)
            continue;
        if (words[i + 2] <= words[i + 1])
            continue;
        if ((words[i + 1] & 3) || (words[i + 2] & 3))
            continue;
        bi_start = words[i + 1];
        bi_end = words[i + 2];
        map_ptr = words[i + 3];
        found = true;
        break;
    }
    if (!found)
    {
        RP6502_LOG(uf2, WARN, "no binary_info marker in first %u bytes", MBUF_SIZE);
        return NULL;
    }

    struct uf2_map map[UF2_MAP_TABLE_MAX];
    int map_count = 0;
    while (map_ptr && map_count < UF2_MAP_TABLE_MAX)
    {
        struct uf2_map m;
        if (uf2_read_ptr(map, map_count, map_ptr, &m, sizeof m) != sizeof m)
            break;
        if (!m.source)
            break;
        map[map_count++] = m;
        map_ptr += sizeof m;
    }

    for (uint32_t slot = bi_start; slot < bi_end; slot += 4)
    {
        uint32_t entry;
        if (uf2_read_ptr(map, map_count, slot, &entry, 4) != 4)
            return NULL;
        struct
        {
            uint16_t type;
            uint16_t tag;
            uint32_t id;
            uint32_t value;
        } core;
        if (uf2_read_ptr(map, map_count, entry, &core, sizeof core) != sizeof core)
            continue;
        if (core.type != BINARY_INFO_TYPE_ID_AND_STRING ||
            core.tag != BINARY_INFO_TAG_RASPBERRY_PI ||
            core.id != BINARY_INFO_ID_RP_PROGRAM_NAME)
            continue;
        uint32_t got = uf2_read_ptr(map, map_count, core.value, mbuf, UF2_NAME_READ_MAX);
        if (!got || !memchr(mbuf, 0, got))
        {
            RP6502_LOG(uf2, WARN, "program_name not terminated within %lu bytes",
                       (unsigned long)got);
            return NULL;
        }
        return (const char *)mbuf;
    }
    RP6502_LOG(uf2, WARN, "no program_name entry in binary_info");
    return NULL;
}

static void uf2_progress(void)
{
    int pct = (int)((uint64_t)uf2_block_idx * 100 / uf2_num_blocks);
    if (pct != uf2_last_percent)
    {
        uf2_last_percent = pct;
        com_printf_utf8(STR_UF2_FLASHING, pct);
    }
}

// Walk the file once: skip whatever is not ours to find the first block that
// is, then check every block of the image parses and note how far it reaches.
// One block per call, because a blocking pass over a megabyte would stall the
// terminal, PIX and USB. Nothing is erased until this finishes.
static void uf2_do_validate(void)
{
    UINT br;
    if (f_read(&uf2_fil, mbuf, 512, &br) != FR_OK || br != 512)
    {
        RP6502_LOG(uf2, WARN, "short read at block %lu of %lu",
                   (unsigned long)uf2_block_idx, (unsigned long)uf2_num_blocks);
        uf2_invalid();
        return;
    }
    const struct uf2_block *b = UF2_BLOCK;
    const struct uf2_header *h = (const struct uf2_header *)mbuf;

    if (!uf2_num_blocks)
    {
        if (!uf2_is_ours(h) || b->magic_end != UF2_MAGIC_END)
        {
            uf2_main_start += 512;
            return;
        }
        if (!h->num_blocks)
        {
            RP6502_LOG(uf2, WARN, "block 0 claims no blocks");
            uf2_invalid();
            return;
        }
        uf2_num_blocks = h->num_blocks;
        uf2_low_addr = h->target_addr;
        uf2_stride = h->payload_size;
        RP6502_LOG(uf2, DEBUG, "image at offset %lu: %lu blocks from 0x%08lX",
                   (unsigned long)uf2_main_start, (unsigned long)uf2_num_blocks,
                   (unsigned long)uf2_low_addr);
    }

    const char *why = NULL;
    if (!uf2_is_ours(h) || b->magic_end != UF2_MAGIC_END)
        why = "not a block of this image";
    else if (h->payload_size > sizeof b->data)
        why = "payload larger than a block";
    else if (h->target_addr < XIP_BASE)
        why = "target below flash";
    if (why)
    {
        RP6502_LOG(uf2, WARN, "block %lu: %s", (unsigned long)uf2_block_idx, why);
        uf2_invalid();
        return;
    }

    uint32_t end = h->target_addr - XIP_BASE + h->payload_size;
    if (end > uf2_high_addr)
        uf2_high_addr = end;
    if (h->target_addr < uf2_low_addr)
        uf2_low_addr = h->target_addr;

    if (++uf2_block_idx >= uf2_num_blocks)
        uf2_state = UF2_IDENTIFY;
}

// Name the image and pick its target. The flash size is checked here because
// the SDK answers an address past the part with an assert that does not
// compile out, which would panic with nothing on screen.
static void uf2_do_identify(void)
{
    const char *name = uf2_find_program_name();
    if (!name)
    {
        uf2_invalid();
        return;
    }
    RP6502_LOG(uf2, INFO, "program_name=\"%s\" reaching 0x%08lX",
               name, (unsigned long)uf2_high_addr);

    if (!strcmp(name, STR_UF2_PROG_NAME_VGA))
        uf2_to_vga = true;
    else if (!strcmp(name, STR_UF2_PROG_NAME_RIA))
        uf2_to_vga = false;
    else
    {
        RP6502_LOG(uf2, WARN, "\"%s\" is neither \"%s\" nor \"%s\"",
                   name, STR_UF2_PROG_NAME_RIA, STR_UF2_PROG_NAME_VGA);
        uf2_invalid();
        return;
    }

    // Only the VGA knows how much flash it has, and it naks a page past it.
    if (!uf2_to_vga && uf2_high_addr > PICO_FLASH_SIZE_BYTES)
    {
        RP6502_LOG(uf2, WARN, "image reaches 0x%08lX, flash is 0x%08X",
                   (unsigned long)uf2_high_addr, PICO_FLASH_SIZE_BYTES);
        uf2_invalid();
        return;
    }

    // A page index is carried by a 16-bit PIX word, so that is as far as the
    // VGA can be addressed. Without this the index would wrap onto a page that
    // exists and the wrong one would be programmed.
    if (uf2_to_vga && (uf2_high_addr - 1) / FLASH_PAGE_SIZE > UINT16_MAX)
    {
        RP6502_LOG(uf2, WARN, "image reaches 0x%08lX, past what a page index carries",
                   (unsigned long)uf2_high_addr);
        uf2_invalid();
        return;
    }

    if (uf2_to_vga && !vga_connected())
    {
        uf2_close();
        mon_add_response_utf8(S(STR_ERR_VGA_NOT_CONNECTED));
        return;
    }

    if (f_lseek(&uf2_fil, uf2_main_start) != FR_OK)
    {
        uf2_invalid();
        return;
    }
    uf2_block_idx = 0;
    uf2_page_idx = 0;
    uf2_page_count = 0;
    uf2_last_percent = -1;
    uf2_state = uf2_to_vga ? UF2_VGA_WRITE : UF2_WRITE;
}

// Build one page of the block in mbuf, padded with 0xFF. Programming only
// clears bits, so the padding writes nothing and a page assembled from more
// than one block ends up the union of them.
static void uf2_build_page(uint32_t page)
{
    const struct uf2_block *b = UF2_BLOCK;
    uint32_t base = b->target_addr - XIP_BASE;
    uint32_t page_base = page * FLASH_PAGE_SIZE;
    uint32_t from = base > page_base ? base : page_base;
    uint32_t to = base + b->payload_size;
    if (to > page_base + FLASH_PAGE_SIZE)
        to = page_base + FLASH_PAGE_SIZE;
    memset(UF2_PAGE, 0xFF, FLASH_PAGE_SIZE);
    memcpy(UF2_PAGE + (from - page_base), b->data + (from - base), to - from);
}

// Read the next block and count the pages its payload reaches into.
static bool uf2_next_block(void)
{
    UINT br;
    if (f_read(&uf2_fil, mbuf, 512, &br) != FR_OK || br != 512)
    {
        RP6502_LOG(uf2, ERROR, "read failed at block %lu", (unsigned long)uf2_block_idx);
        return false;
    }
    const struct uf2_block *b = UF2_BLOCK;
    uint32_t base = b->target_addr - XIP_BASE;
    uint32_t first = base / FLASH_PAGE_SIZE;
    uint32_t last = b->payload_size
                        ? (base + b->payload_size - 1) / FLASH_PAGE_SIZE
                        : first;
    uf2_page_idx = first;
    uf2_page_count = last - first + 1;
    return true;
}

static void uf2_do_write(void)
{
    if (!uf2_page_count && !uf2_next_block())
    {
        uf2_state = UF2_FAILED;
        return;
    }
    while (uf2_page_count)
    {
        uf2_build_page(uf2_page_idx);
        uint32_t offs = uf2_page_idx * FLASH_PAGE_SIZE;
        const uint8_t *dest = (const uint8_t *)(XIP_BASE + offs);
        for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++)
            if (dest[i] != 0xFF)
            {
                flash_range_erase(offs & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
                break;
            }
        flash_range_program(offs, UF2_PAGE, FLASH_PAGE_SIZE);
        if (memcmp(dest, UF2_PAGE, FLASH_PAGE_SIZE))
        {
            RP6502_LOG(uf2, ERROR, "page 0x%08lX did not take", (unsigned long)offs);
            uf2_state = UF2_FAILED;
            return;
        }
        uf2_page_idx++;
        uf2_page_count--;
    }

    uf2_block_idx++;
    uf2_progress();
    if (uf2_block_idx >= uf2_num_blocks)
    {
        putchar('\n');
        uf2_state = UF2_REBOOT;
    }
}

static void uf2_do_vga_write(void)
{
    if (!uf2_page_count && !uf2_next_block())
    {
        uf2_state = UF2_VGA_LOCKUP;
        return;
    }
    uf2_build_page(uf2_page_idx);
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++)
        pix_send_blocking(PIX_DEVICE_XRAM, 0, UF2_PAGE[i], (uint16_t)i);
    // Armed before the request, because a reply is only kept while waiting.
    pix_wait_begin(UF2_VGA_ACK_TIMEOUT_MS);
    pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x07, (uint16_t)uf2_page_idx);
    uf2_state = UF2_VGA_WAIT;
}

static void uf2_do_vga_wait(void)
{
    switch (pix_wait_poll())
    {
    case 0:
        return;
    case 1:
        break;
    default:
        RP6502_LOG(uf2, ERROR, "VGA refused page %lu of block %lu",
                   (unsigned long)uf2_page_idx, (unsigned long)uf2_block_idx);
        uf2_state = UF2_VGA_LOCKUP;
        return;
    }
    uf2_page_idx++;
    if (--uf2_page_count)
    {
        uf2_state = UF2_VGA_WRITE;
        return;
    }
    uf2_block_idx++;
    uf2_progress();
    if (uf2_block_idx >= uf2_num_blocks)
    {
        putchar('\n');
        uf2_state = UF2_VGA_SUCCESS;
    }
    else
        uf2_state = UF2_VGA_WRITE;
}

void uf2_task(void)
{
    switch (uf2_state)
    {
    case UF2_IDLE:
        break;
    case UF2_VALIDATE:
        uf2_do_validate();
        break;
    case UF2_IDENTIFY:
        uf2_do_identify();
        break;
    case UF2_WRITE:
        uf2_do_write();
        break;
    case UF2_REBOOT:
        stdio_flush();
        watchdog_reboot(0, 0, 0);
        break;
    case UF2_FAILED:
        com_printf_utf8(STR_UF2_FLASH_FAILED);
        stdio_flush();
        reset_usb_boot(0, 0);
        break;
    case UF2_VGA_WRITE:
        uf2_do_vga_write();
        break;
    case UF2_VGA_WAIT:
        uf2_do_vga_wait();
        break;
    case UF2_VGA_SUCCESS:
        stdio_flush();
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x06, 0);
        // The PIX FIFO has to drain and the VGA has to start its reboot
        // before this board resets.
        busy_wait_ms(50);
        watchdog_reboot(0, 0, 0);
        break;
    case UF2_VGA_LOCKUP:
        com_printf_utf8(STR_UF2_FLASH_FAILED);
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
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    RP6502_LOG(uf2, INFO, "FLASH \"%s\"", path);

    FRESULT fr = f_open(&uf2_fil, path, FA_READ);
    if (fr != FR_OK)
    {
        uf2_fil.obj.fs = NULL;
        mon_add_response_fatfs(fr);
        return;
    }
    uf2_main_start = 0;
    uf2_num_blocks = 0;
    uf2_block_idx = 0;
    uf2_stride = 0;
    uf2_low_addr = 0;
    uf2_high_addr = 0;
    uf2_state = UF2_VALIDATE;
}
