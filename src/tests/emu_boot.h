/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _EMU_TESTS_EMU_BOOT_H_
#define _EMU_TESTS_EMU_BOOT_H_

#include "emu/main.h"
#include "emu/sys/sys.h"
#include "emu/emu/rom.h"
#include "utest.h"

/* The emulator lifecycle is init-once + stop/run per program (see emu/main.c).
 * A test binary initializes the drivers exactly once, in a custom main(), then
 * each case (re)starts the machine on its ROM via a stop + load + run. */

/* Replaces UTEST_MAIN(): declares the utest state and a main() that inits the
 * drivers once before running the cases. */
#define UTEST_MAIN_EMU()                             \
    UTEST_STATE();                                   \
    int main(int argc, const char *const argv[])     \
    {                                                \
        main_init();                                 \
        return utest_main(argc, argv);               \
    }

/* Program change: stop the previous program, load rom, start it — what an exec
 * and a ROM drop do. The first call per process runs on the just-inited, not-yet-
 * running machine, so its main_stop is a harmless no-op on the idle drivers. */
static inline bool emu_restart(const char *rom)
{
    main_stop();
    if (!rom_load(rom))
        return false;
    main_run();
    return true;
}

#endif /* _EMU_TESTS_EMU_BOOT_H_ */
