/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine under test.
 *
 * A machine test is written once and runs against whichever machine its tree
 * builds: emu_core in an emulator root, the verilated RP6502 in the fpga one.
 * This is what the two have in common — boot a program, take a frame, ask the
 * canvas — declared so a suite can be written to it instead of to one of them.
 *
 * It is deliberately small. Everything a machine can be asked that the other
 * cannot answer stays behind its own seam: mut_measure is the render budget,
 * which a C renderer has no line to be late on, and it says so rather than
 * pretending.
 *
 * mut_emu.c and mut_rtl.cpp are the two implementations; a suite links
 * whichever its tree registered and never names either.
 */

#ifndef _TESTS_BENCH_MUT_H_
#define _TESTS_BENCH_MUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Stand the machine up, and take it down. MUT_MAIN() does both around
     * the suite; a test that needs neither does not call them. */
    void mut_init(int argc, const char *const argv[]);
    void mut_free(void);

    /* Load a .rp6502, start it, and let it settle. False if the file will
     * not load. */
    bool mut_boot(const char *rom);

    /* The next whole frame, canvas-native RGBA8 0xAABBGGRR — the emulator's
     * present buffer and the fabric's composed pixel stream through the same
     * conversion, which is what lets one expectation cover both. The pointer
     * is the machine's own and is good until the next call, so a suite that
     * wants two frames copies the first.
     *
     * Two frames compared is what says a picture settled rather than merely
     * arrived once.
     *
     * The geometry is the caller's because the corpus states its own shape and
     * that is better evidence than asking the machine: a machine rendering the
     * wrong canvas is caught by the frame not matching, rather than by
     * agreeing with itself. The fabric scans exactly canvas-many pixels, so
     * this is also the only sync a capture needs. */
    const uint32_t *mut_frame(int w, int h);

    /* What the render spent against the beam, where the machine has a beam to
     * be late against. NONE is not a failure — it is a machine that cannot be
     * asked, and a suite skips the claim rather than inventing one. */
    typedef enum
    {
        MUT_BUDGET_NONE,
        MUT_BUDGET_UNDER,
        MUT_BUDGET_OVER,
    } mut_budget_t;

    /* Measures one frame and reports where its worst line finished. `name` is
     * for the machine's own report, printed where a machine has numbers worth
     * reading. */
    mut_budget_t mut_measure(const char *name);

#ifdef __cplusplus
}
#endif

/* Replaces UTEST_MAIN(): the machine is stood up once for the whole binary,
 * because both implementations cost too much to build per case. */
#define MUT_MAIN()                                   \
    UTEST_STATE();                                   \
    int main(int argc, const char *const argv[])     \
    {                                                \
        mut_init(argc, argv);                        \
        int rc = utest_main(argc, argv);             \
        mut_free();                                  \
        return rc;                                   \
    }

#endif /* _TESTS_BENCH_MUT_H_ */
