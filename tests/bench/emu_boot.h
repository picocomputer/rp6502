/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _EMU_TESTS_EMU_BOOT_H_
#define _EMU_TESTS_EMU_BOOT_H_

#include "core/vga/vga_emu.h"
#include "core/lifecycle.h"
#include "core/sys/sys.h"
#include "core/rom/rom.h"
#include "utest.h"

/* The emulator lifecycle is init-once + load/run/stop per program (see
 * the machine that ran it): the stop belongs to the program that ran, not to the one about
 * to. A test binary initializes the drivers exactly once, in a custom main(),
 * and each case ends the program the previous case left running before loading
 * its own. */

/* Replaces UTEST_MAIN(): declares the utest state and a main() that inits the
 * drivers once before running the cases. */
#define UTEST_MAIN_EMU()                             \
    UTEST_STATE();                                   \
    int main(int argc, const char *const argv[])     \
    {                                                \
        lifecycle_init();                                 \
        return utest_main(argc, argv);               \
    }

/* Program change: end the previous program, load rom, start it — what an exec
 * and a ROM drop do. The first call per process runs on the just-inited, not-yet-
 * running machine, so its lifecycle_stop is a harmless no-op on the idle drivers.
 * Committed on the spot, as the machine does it: the load writes the RAM the
 * outgoing program was running out of. */
static inline bool emu_restart(const char *rom)
{
    lifecycle_stop();
    lifecycle_commit();
    if (!rom_load(rom))
        return false;
    lifecycle_run();
    lifecycle_commit();
    return true;
}

/* Run n whole frames, the way every host does: pump the machine until video
 * says the frames went by. */
static inline void emu_frames(int n)
{
    const unsigned long want = vga_frame_count() + (unsigned long)n;
    while (vga_frame_count() < want)
        main_task();
}

#endif /* _EMU_TESTS_EMU_BOOT_H_ */
