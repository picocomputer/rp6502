/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

    void mut_init(int argc, const char *const argv[]);
    void mut_free(void);

    bool mut_boot(const char *rom);

    void mut_xram(uint32_t addr, uint8_t *dst, size_t len);

    /* The buffer mut_frame returns is valid only until the next call to
     * mut_frame or mut_boot, so a suite that compares two frames copies the
     * first. */
    const uint32_t *mut_frame(int w, int h);

    /* mut_console_start is called before mut_boot, because mut_boot starts
     * the program whose output is captured. */
    void mut_console_start(void);

    const char *mut_console(size_t *len);

    typedef enum
    {
        MUT_BUDGET_NONE,
        MUT_BUDGET_UNDER,
        MUT_BUDGET_OVER,
    } mut_budget_t;

    mut_budget_t mut_measure(const char *name);

#ifdef __cplusplus
}
#endif

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
