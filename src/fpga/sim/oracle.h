/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The reference oracle: the emulator's machine core (emu_core) driven headless,
 * the same way tests/emu_boot.h drives it. RTL tests run a .rp6502 here and on
 * the verilated core, then compare console bytes, framebuffers or audio.
 */

#ifndef _FPGA_SIM_ORACLE_H_
#define _FPGA_SIM_ORACLE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Cold boot the drivers. Once per process, before any oracle_restart. */
    void oracle_init(void);

    /* Program change: stop the previous program, load rom, start it. */
    bool oracle_restart(const char *rom);

    void oracle_run_frames(int count);

    /* The present buffer (RGBA8 0xAABBGGRR, canvas-native stride). */
    const uint32_t *oracle_framebuffer(void);
    void oracle_canvas_size(int *width, int *height);

    unsigned long oracle_frame_count(void);

#ifdef __cplusplus
}
#endif

#endif /* _FPGA_SIM_ORACLE_H_ */
