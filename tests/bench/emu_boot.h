/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _EMU_TESTS_EMU_BOOT_H_
#define _EMU_TESTS_EMU_BOOT_H_

#include "core/aud/mix.h"
#include "core/sys/proc.h"
#include "core/vga/vga_emu.h"
#include "core/sys/sys.h"
#include "core/rom/rom.h"
#include "utest.h"

#define UTEST_MAIN_EMU()                             \
    UTEST_STATE();                                   \
    int main(int argc, const char *const argv[])     \
    {                                                \
        sys_init();                                 \
        return utest_main(argc, argv);               \
    }

static inline bool emu_restart(const char *rom)
{
    if (!proc_boot(rom, 0, NULL, 0))
        return false;
    sys_commit();
    return true;
}

static inline void emu_frames(int n)
{
    while (n-- > 0 && vga_run_frame())
        ;
}

#endif /* _EMU_TESTS_EMU_BOOT_H_ */
