/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Golden integration test: run hello.rp6502 through the whole headless core
 * (ROM load -> CPU -> RIA syscalls -> terminal -> framebuffer) and assert the
 * rendered framebuffer matches a known CRC, plus that the program exited
 * cleanly. One assertion that locks in the entire pipeline against regressions.
 */

#include "emu/mon/rom.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "utest.h"
#include <stdio.h>

static uint32_t fb[EMU_FB_WIDTH * EMU_FB_HEIGHT];

UTEST(golden, hello_world)
{
    ASSERT_TRUE(emu_rom_load(HELLO_ROM));
    emu_init();
    for (int i = 0; i < 2; i++)
        emu_run_frame();

    /* The program opens stdout, prints "Hello, world!", and exits. */
    ASSERT_TRUE(emu_cpu_halted);
    ASSERT_EQ(emu_exit_code, 0);

    emu_render(fb);
    uint32_t crc = emu_crc32(0, fb, sizeof(fb));
    printf("golden hello framebuffer crc = 0x%08X\n", crc);
    ASSERT_EQ(crc, (uint32_t)GOLDEN_HELLO_CRC);
}

UTEST_MAIN();
