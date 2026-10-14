/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/hid/vtkeys.h"
#include "core/ria/ria.h"
#include "emu_boot.h"

UTEST(features, ria_tick_holds_irq_through_ack)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));

    ria_reg_write(0xFFF0, 0x40);
    vtkeys_ctrl_letter('c');
    ASSERT_TRUE(ria_irq_asserted());

    uint8_t data = 0;
    ASSERT_TRUE(ria_tick(0xFFF0, true, &data));
    ASSERT_TRUE((data & 0x40) != 0);

    ASSERT_FALSE(ria_tick(0x0000, true, &data));
}

static float g_out[800 * 2];

static bool rendered_audio(int frames)
{
    for (int p = 0; p < frames; p++)
    {
        emu_frames(1);
        const int n = aud_render(g_out, 800);
        for (int i = 0; i < n * 2; i++)
            if (g_out[i] != 0.0f)
                return true;
    }
    return false;
}

UTEST(features, audio_disable)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    ASSERT_TRUE(aud_enabled());

    aud_set_enabled(false);
    ASSERT_FALSE(aud_enabled());

    bel_add(&bel_teletype);
    ASSERT_FALSE(rendered_audio(8));

    aud_set_enabled(true);
    emu_frames(60);
}

UTEST_MAIN_EMU()
