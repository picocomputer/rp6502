/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chips_dut.h"
#include "lockstep.h"
#include "lockstep_scen.h"
#include "utest.h"
#include "rtl_dut.h"

#include <cstdio>
#include <cstring>

static uint8_t image[0x10000];

static bool run(const lockstep_scen_t *scen)
{
    lockstep_scen_image(image, scen->entry);
    lockstep_result_t r;
    bool ok = lockstep_run(&chips_dut, &rtl_dut, image, scen->evs,
                           scen->n_evs, scen->cycles, &r);
    if (!ok)
        printf("%s\n", r.detail);
    return ok;
}

#define LOCKSTEP(name)                                    \
    UTEST(lockstep, name)                                 \
    {                                                     \
        ASSERT_TRUE(run(&lockstep_scen_##name));          \
    }

LOCKSTEP_SCENARIOS(LOCKSTEP)

UTEST(lockstep, pin_fuzz)
{
    lockstep_scen_image(image, LOCKSTEP_FUZZ_ENTRY);

    static lockstep_ev_t evs[LOCKSTEP_FUZZ_EVENTS];
    lockstep_scen_fuzz(evs);

    lockstep_result_t r;
    bool ok = lockstep_run(&chips_dut, &rtl_dut, image, evs,
                           LOCKSTEP_FUZZ_EVENTS, LOCKSTEP_FUZZ_CYCLES, &r);
    if (!ok)
        printf("%s\n", r.detail);
    ASSERT_TRUE(ok);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    rtl_dut_init(argc, argv);
    int rc = utest_main(argc, argv);
    rtl_dut_free();
    return rc;
}
