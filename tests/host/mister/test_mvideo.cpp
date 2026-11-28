/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * mister_video behind mister_bars, read the way sys/ reads emu's video.
 *
 * Every clock of every output frame is held to a model of the raster
 * written from the spec: CE, DE, HS, VS, the aspect and the colour. The
 * video is also measured the way Template_MiSTer's sys/hps_io.sv does
 * for Main's UIO_GET_VRES, its video_calc copied line for line, with the
 * clk_100 timers counted in clk_sys. Main resyncs HDMI when the count it
 * reports, nres, or the aspect changes. A program that selects the
 * canvas it already has, to clear the scanline table, must change
 * neither, nor anything else Main measures, so those tests ask that the
 * measurements stay bit for bit the same while only the colour changes.
 *
 * An output frame is the 840000 clocks from the fourth after vid_frame,
 * the first clock whose outputs come from the new frame's raster.
 */

#include "Vtb_mvideo.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>

static Vtb_mvideo *dut;

static const long FRAME = 840000;

static void fresh(uint8_t canvas, bool frame_en)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vtb_mvideo;
    dut->clk_sys = 0;
    dut->canvas = canvas;
    dut->frame_en = frame_en;
    dut->rows_en = 1;
    dut->blank = 0;
    dut->eval();
}

static void tick()
{
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static int canvas_w(int cv)
{
    return cv == 1 || cv == 2 || cv == 5 ? 320 : 640;
}

static int canvas_h(int cv)
{
    switch (cv)
    {
    case 1:
        return 240;
    case 2:
        return 180;
    case 4:
        return 360;
    case 5:
        return 200;
    case 6:
        return 400;
    default:
        return 480;
    }
}

/* What Main is told, by canvas: README.md's table. */
struct Want
{
    int hcnt, vcnt, ccnt, pixrep, de_h, de_v, vs_top, vs_lines;
    int line, arx, ary;
};

static Want want(int cv)
{
    switch (cv)
    {
    case 1:
        return {320, 240, 2560, 8, 326, 16, 244, 3, 3206, 4, 3};
    case 2:
        return {320, 180, 2560, 8, 326, 46, 214, 3, 3206, 16, 9};
    case 4:
        return {640, 360, 1280, 2, 96, 93, 430, 2, 1600, 16, 9};
    case 5:
        return {320, 200, 2560, 8, 326, 36, 224, 3, 3206, 16, 10};
    case 6:
        return {640, 400, 1280, 2, 96, 73, 450, 2, 1600, 16, 10};
    default:
        return {640, 480, 1280, 2, 96, 33, 490, 2, 1600, 4, 3};
    }
}

/* mister_bars's card, an RGB555 pixel with alpha in bit 5. */
static uint16_t bars(int cv, int col, int row)
{
    bool dbl = canvas_w(cv) == 320;
    int bar = ((dbl ? col << 1 : col) / 80) & 7;
    if (row < canvas_h(cv) / 2)
        return (uint16_t)((bar & 4 ? 0x1F << 11 : 0) | (bar & 2 ? 0x1F << 6 : 0) |
                          (bar & 1 ? 0x1F : 0));
    return (uint16_t)(((row & 0x1F) << 11) | (((col >> 4) & 0x1F) << 6) |
                      ((col & 1) << 5) | (col & 0x1F));
}

static uint32_t rgb888(uint16_t p)
{
    uint32_t r = ((p & 0x1F) << 3) | ((p >> 2) & 7);
    uint32_t g = (((p >> 6) & 0x1F) << 3) | ((p >> 8) & 7);
    uint32_t b = (((p >> 11) & 0x1F) << 3) | ((p >> 13) & 7);
    return (r << 16) | (g << 8) | b;
}

/* sys/hps_io.sv's video_calc, the clk_vid block, line for line. Each
 * clock computes the next registers from the old ones. f1 and new_vmode
 * are RP6502.sv's: tied low. sys_top.v's sync_fix leaves HS and VS as
 * they are, for they are short high pulses, and its scanlines delays
 * them all alike. */
struct VideoCalc
{
    uint32_t vid_hcnt = 0, vid_vcnt = 0, vid_ccnt = 0;
    uint8_t vid_nres = 0, vid_int = 0, vid_pixrep = 0, vid_de_v = 0;
    uint16_t vid_de_h = 0;
    uint32_t vid_fclks = 0;
    uint32_t hcnt = 0, vcnt = 0, ccnt = 0;
    uint8_t pcnt = 0, de_v = 0;
    uint16_t de_h = 0;
    bool old_vs = 0, old_hs = 0, old_hs_vclk = 0, old_de = 0, old_de_vclk = 0,
         old_de1 = 0, old_vmode = 0;
    uint8_t resto = 0;
    bool calch = 0;

    void clock(bool ce_pix, bool de, bool hs, bool vs)
    {
        const bool f1 = false, new_vmode = false;
        VideoCalc n = *this;
        if (resto == 1)
            n.vid_fclks = (vid_fclks + 1) & 0xFFFFFF;
        if (calch && de)
            n.ccnt = ccnt + 1;
        n.pcnt = (uint8_t)(pcnt + 1);
        n.old_hs_vclk = hs;
        n.de_h = (uint16_t)(de_h + 1);
        if (old_hs_vclk && !hs)
            n.de_h = 1;
        n.old_de_vclk = de;
        if (calch && !old_de_vclk && de)
            n.vid_de_h = de_h;
        if (ce_pix)
        {
            n.old_vs = vs;
            n.old_hs = hs;
            n.old_de = de;
            n.old_de1 = old_de;
            n.pcnt = 1;
            if (!vs && !old_de && de)
                n.vcnt = vcnt + 1;
            if (calch && de)
                n.hcnt = hcnt + 1;
            if (old_de && !de)
                n.calch = 0;
            if (!old_de1 && old_de)
                n.vid_pixrep = pcnt;
            if (old_hs && !hs)
                n.de_v = (uint8_t)(de_v + 1);
            if (calch && !old_de && de)
                n.vid_de_v = de_v;
            if (old_vs && !vs)
            {
                n.vid_int = (uint8_t)(((vid_int << 1) | f1) & 3);
                if (!f1)
                {
                    if (hcnt && vcnt)
                    {
                        n.old_vmode = new_vmode;
                        if (resto)
                            n.resto = (resto + 1) & 15;
                        if (vid_hcnt != hcnt || vid_vcnt != vcnt ||
                            old_vmode != new_vmode)
                        {
                            n.resto = 1;
                            n.vid_fclks = 0;
                        }
                        if (resto == 15)
                            n.vid_nres = (uint8_t)(vid_nres + 1);
                        n.vid_hcnt = hcnt;
                        n.vid_vcnt = vcnt;
                        n.vid_ccnt = ccnt;
                    }
                    n.vcnt = 0;
                    n.hcnt = 0;
                    n.ccnt = 0;
                    n.calch = 1;
                    n.de_v = 0;
                }
            }
        }
        *this = n;
    }
};

/* What Main reads back, with hps_io's clk_100 timers (VS rise to rise,
 * HS rise to rise, DE on the first line after VS) counted in clk_sys. */
struct Snap
{
    uint32_t hcnt, vcnt, ccnt, fclks, vtime, ptime;
    uint16_t de_h;
    uint8_t pixrep, de_v, nres, f1;
    uint16_t arx, ary;
    bool operator==(const Snap &o) const
    {
        return hcnt == o.hcnt && vcnt == o.vcnt && ccnt == o.ccnt &&
               fclks == o.fclks && vtime == o.vtime && ptime == o.ptime &&
               de_h == o.de_h && pixrep == o.pixrep && de_v == o.de_v &&
               nres == o.nres && f1 == o.f1 && arx == o.arx && ary == o.ary;
    }
};

static void print_snap(const char *what, const Snap &s)
{
    printf("mvideo: %s: %ux%u ccnt %u pixrep %u de_h %u de_v %u fclks %u "
           "vtime %u ptime %u nres %u f1 %u ar %u:%u\n",
           what, s.hcnt, s.vcnt, s.ccnt, s.pixrep, s.de_h, s.de_v, s.fclks,
           s.vtime, s.ptime, s.nres, s.f1, s.arx, s.ary);
}

/* The raster from the spec, one output clock at a time. pos is the
 * clock within the raster's frame whose strobe would show at t = pos. */
struct Ref
{
    int cv = 0;
    bool dbl = false;
    int h = 0, line = 0;
    bool ce = 0, de = 0, hs = 0, vs = 0;
    int x = 0, row = 0;

    void start(int c)
    {
        cv = c;
        dbl = canvas_w(cv) == 320;
        h = line = 0;
    }

    /* Outputs at the next t; they hold the last strobe's values. */
    void step()
    {
        int len = dbl ? (line == 261 ? 3234 : 3206) : 1600;
        ce = dbl ? (h & 7) == 7 : (h & 1);
        if (ce)
        {
            Want w = want(cv);
            int ch = canvas_h(cv);
            x = dbl ? h >> 3 : h >> 1;
            row = dbl ? line - 1 : line;
            de = x < canvas_w(cv) && row >= 0 && row < ch;
            hs = dbl ? x >= 330 && x < 360 : x >= 656 && x < 752;
            vs = line >= w.vs_top && line < w.vs_top + w.vs_lines;
        }
        if (++h == len)
        {
            h = 0;
            line++;
        }
    }
};

enum Check
{
    CHECK_BARS, /* the card, or black where the frame is not live */
    CHECK_NONE, /* the colour is the test's to change */
};

/* Follows the output by frame. The test says what the next frame's
 * colour should be; the canvas and liveness come from the stimulus. */
struct Mon
{
    long cyc = 0;
    long start_at = 0; /* the raster's power-on frame starts at once */
    long anchor = -1, frame_due = -1;
    long t = 0;
    long frames = 0;   /* frames started */
    long rephases = 0; /* frames cut short by vid_frame */
    bool in_frame = false, live = false, after_cut = false;
    int cv = 0;
    Check check = CHECK_BARS, next_check = CHECK_BARS;
    Ref ref;
    VideoCalc vc;
    uint64_t sig = 0;
    uint16_t arx_q = 0, ary_q = 0; /* the frame's last clock's */
    long lit = 0, de_px = 0;

    /* clk_sys timers and per-frame records */
    long vs_rise = -1, hs_rise = -1, vs_period = 0, de_run = 0;
    bool vs_q = 0, hs_q = 0, de_q = 0, ptime_on = false;
    uint32_t vtime = 0, ptime = 0;
    long hs_short = 0, hs_long = 0, hs_other = 0;
    long vs_periods_bad = 0;
    bool vs_check = false;

    bool fail(const char *what, long got, long want)
    {
        printf("mvideo: %s in frame %ld (cv %d) at t=%ld: got %ld, want %ld\n",
               what, frames, cv, t, got, want);
        return false;
    }

    Snap snap() const
    {
        return {vc.vid_hcnt, vc.vid_vcnt, vc.vid_ccnt, vc.vid_fclks, vtime,
                ptime, vc.vid_de_h, vc.vid_pixrep, vc.vid_de_v, vc.vid_nres,
                (uint8_t)(vc.vid_int & 1), arx_q, ary_q};
    }

    /* The frame just ended; the caller looks at it. */
    bool (*on_end)(Mon &, void *) = nullptr;
    void *on_end_arg = nullptr;

    bool begin_frame()
    {
        if (in_frame)
        {
            if (t != FRAME)
            {
                rephases++;
                after_cut = true;
            }
            else
            {
                after_cut = false;
                if (on_end && !on_end(*this, on_end_arg))
                    return false;
            }
        }
        live = frame_due == cyc;
        anchor = cyc;
        cv = dut->canvas;
        if (frames == 0)
            cv = 0; /* the raster wakes on canvas 0 */
        check = next_check;
        ref.start(cv);
        t = 0;
        sig = 1469598103934665603ull;
        hs_short = hs_long = hs_other = 0;
        in_frame = true;
        frames++;
        return true;
    }

    /* One clock of output, after its rising edge. */
    bool clock()
    {
        if (cyc == start_at || cyc == frame_due)
            if (!begin_frame())
                return false;
        if (dut->tb_mvideo_frame)
            frame_due = cyc + 4;

        bool ce = dut->tb_mvideo_ce, de = dut->tb_mvideo_de,
             hs = dut->tb_mvideo_hs, vs = dut->tb_mvideo_vs;
        vc.clock(ce, de, hs, vs);

        /* hps_io's clk_100 timers, in clk_sys */
        if (vs && !vs_q)
        {
            if (vs_rise >= 0)
            {
                vtime = (uint32_t)(cyc - vs_rise);
                if (vs_check && vtime != FRAME)
                    vs_periods_bad++;
            }
            vs_rise = cyc;
        }
        if (!vs && vs_q)
            ptime_on = true, de_run = 0;
        if (ptime_on && de)
            de_run++;
        if (ptime_on && de_q && !de)
            ptime = (uint32_t)de_run, ptime_on = false;
        if (hs && !hs_q)
        {
            if (hs_rise >= 0)
            {
                long ht = cyc - hs_rise;
                if (ht == want(cv).line)
                    hs_short++;
                else if (ht == want(cv).line + 28)
                    hs_long++;
                else
                    hs_other++;
            }
            hs_rise = cyc;
        }
        vs_q = vs;
        hs_q = hs;
        de_q = de;

        uint32_t s = (uint32_t)ce | (uint32_t)de << 1 | (uint32_t)hs << 2 |
                     (uint32_t)vs << 3 | (uint32_t)dut->tb_mvideo_arx << 4 |
                     (uint32_t)dut->tb_mvideo_ary << 17;
        sig = (sig ^ s) * 1099511628211ull;

        ref.step();
        /* vid_frame moves the raster at once, a few clocks before the
         * frame it starts; only lock does that. */
        bool cutting = frame_due > cyc && t + frame_due - cyc != FRAME;
        bool skip = cutting || (after_cut && t < 8);
        if (!skip)
        {
            if (ce != ref.ce)
                return fail("CE_PIXEL", ce, ref.ce);
            if (de != ref.de)
                return fail("DE", de, ref.de);
            if (hs != ref.hs)
                return fail("HS", hs, ref.hs);
            if (vs != ref.vs)
                return fail("VS", vs, ref.vs);
        }
        Want w = want(cv);
        if (dut->tb_mvideo_arx != w.arx)
            return fail("ARX", dut->tb_mvideo_arx, w.arx);
        if (dut->tb_mvideo_ary != w.ary)
            return fail("ARY", dut->tb_mvideo_ary, w.ary);

        uint32_t rgb = ((uint32_t)dut->tb_mvideo_r << 16) |
                       ((uint32_t)dut->tb_mvideo_g << 8) | dut->tb_mvideo_b;
        if (!skip && check == CHECK_BARS)
        {
            uint32_t want_rgb =
                live && ref.de ? rgb888(bars(cv, ref.x, ref.row)) : 0;
            if (rgb != want_rgb)
                return fail("RGB", rgb, want_rgb);
        }
        arx_q = dut->tb_mvideo_arx;
        ary_q = dut->tb_mvideo_ary;
        if (ce && de)
        {
            de_px++;
            lit += rgb != 0;
        }
        t++;
        cyc++;
        if (t == FRAME)
            start_at = cyc;
        return true;
    }
};

/* Runs until `frames` frames have ended, calling at(frame index) at the
 * start of line 100 of each. */
template <typename F>
static bool run(Mon &m, long frames, F at)
{
    long end = m.frames + frames;
    long limit = m.cyc + (frames + 2) * FRAME;
    while (m.frames <= end)
    {
        if (m.cyc > limit)
        {
            printf("mvideo: frames do not end\n");
            return false;
        }
        tick();
        if (!m.clock())
            return false;
        if (m.t == 100L * want(m.cv).line)
            at(m.frames);
    }
    return true;
}

/* The measurements of a canvas held a while, against the table. */
static bool snap_is(const Snap &s, int cv)
{
    Want w = want(cv);
    bool ok = (int)s.hcnt == w.hcnt && (int)s.vcnt == w.vcnt &&
              (int)s.ccnt == w.ccnt && s.pixrep == w.pixrep &&
              s.de_h == w.de_h && s.de_v == w.de_v && s.fclks == FRAME &&
              s.vtime == FRAME && (int)s.ptime == w.ccnt && s.f1 == 0 &&
              s.arx == w.arx && s.ary == w.ary;
    if (!ok)
    {
        printf("mvideo: canvas %d: want %dx%d ccnt %d pixrep %d de_h %d de_v %d\n",
               cv, w.hcnt, w.vcnt, w.ccnt, w.pixrep, w.de_h, w.de_v);
        print_snap("got", s);
    }
    return ok;
}

/* The lines of a frame by HS: all one length, but the 320 raster's long
 * line. */
static bool lines_ok(const Mon &m)
{
    bool dbl = canvas_w(m.cv) == 320;
    bool ok = m.hs_other == 0 && m.hs_long == (dbl ? 1 : 0) &&
              m.hs_short + m.hs_long == (dbl ? 262 : 525);
    if (!ok)
        printf("mvideo: canvas %d lines: %ld short, %ld long, %ld other\n",
               m.cv, m.hs_short, m.hs_long, m.hs_other);
    return ok;
}

/* Every frame after lock: the measurements are the table's, the
 * signature is the same, nres moved once, at lock, and never again. */
struct Steady
{
    int cv;
    long from;
    long checked = 0;
    uint64_t sig = 0;
    Snap ref{};
    bool have_ref = false;
};

static bool steady_end(Mon &m, void *arg)
{
    Steady &st = *(Steady *)arg;
    if (m.frames < st.from)
        return true;
    if (!lines_ok(m))
        return false;
    Snap s = m.snap();
    if (!snap_is(s, st.cv) || s.nres != 1 || m.vc.resto != 0)
    {
        printf("mvideo: frame %ld: nres %u resto %u\n", m.frames, s.nres,
               m.vc.resto);
        return false;
    }
    if (!st.have_ref)
    {
        st.ref = s;
        st.sig = m.sig;
        st.have_ref = true;
    }
    else if (!(s == st.ref) || m.sig != st.sig)
    {
        printf("mvideo: frame %ld changed what Main measures\n", m.frames);
        print_snap("was", st.ref);
        print_snap("now", s);
        return false;
    }
    st.checked++;
    return true;
}

static void canvas_case(int *utest_result, uint8_t cv)
{
    fresh(cv, true);
    Mon m;
    Steady st{cv, 20};
    m.on_end = steady_end;
    m.on_end_arg = &st;
    m.vs_check = true;
    ASSERT_TRUE(run(m, 24, [](long) {}));
    ASSERT_EQ(m.rephases, 1L);
    ASSERT_GE(st.checked, 3L);
    ASSERT_EQ(m.vs_periods_bad, 0L);
    ASSERT_GT(m.lit, m.de_px / 2);
}

UTEST(mvideo, canvas0_console)
{
    canvas_case(utest_result, 0);
}

UTEST(mvideo, canvas1_320x240)
{
    canvas_case(utest_result, 1);
}

UTEST(mvideo, canvas2_320x180)
{
    canvas_case(utest_result, 2);
}

UTEST(mvideo, canvas3_640x480)
{
    canvas_case(utest_result, 3);
}

UTEST(mvideo, canvas4_640x360)
{
    canvas_case(utest_result, 4);
}

UTEST(mvideo, canvas5_320x200)
{
    canvas_case(utest_result, 5);
}

UTEST(mvideo, canvas6_640x400)
{
    canvas_case(utest_result, 6);
}

/* The canvas changes on line 100 of a frame. The machine takes it as its
 * next frame starts and the output as its next frame does, so the frame
 * being shown keeps its canvas to the end and the next is whole in the
 * new one. Frames stay 840000 clocks apart and nres moves once a change,
 * to the new canvas's measurements. */
struct Switch
{
    long hold;
    long checked = 0;
};

static bool switch_end(Mon &m, void *arg)
{
    Switch &sw = *(Switch *)arg;
    long in_seg = (m.frames - 1) % sw.hold;
    long seg = (m.frames - 1) / sw.hold;
    if (m.frames < 3) /* lock */
        return true;
    /* A change's first frame has its first line measured from the old
     * canvas's last. */
    if (in_seg > 0 && !lines_ok(m))
        return false;
    /* nres moved once at lock and once for each change before. Its
     * measurements reach the table within the 16 frames resto waits. */
    if (in_seg >= sw.hold - 2)
    {
        Snap s = m.snap();
        if (!snap_is(s, m.cv) || s.nres != 1 + seg || m.vc.resto != 0)
        {
            printf("mvideo: frame %ld segment %ld: nres %u resto %u\n",
                   m.frames, seg, s.nres, m.vc.resto);
            return false;
        }
        sw.checked++;
    }
    return true;
}

UTEST(mvideo, canvas_switch)
{
    static const uint8_t order[] = {0, 1, 6, 2, 4, 5, 3, 1};
    const long hold = 20;
    fresh(order[0], true);
    Mon m;
    Switch sw{hold};
    m.on_end = switch_end;
    m.on_end_arg = &sw;
    ASSERT_TRUE(run(m, hold * (long)sizeof order, [&](long f) {
        long seg = f / hold;
        if (f % hold == 0 && seg < (long)sizeof order)
            dut->canvas = order[seg];
    }));
    ASSERT_EQ(m.rephases, 1L);
    ASSERT_EQ(sw.checked, 2L * (long)sizeof order);
}

/* A program selects the canvas it has, as it does to clear the scanline
 * table between scenes. On line 100 of frame f the canvas is written
 * again and the table cleared; f + 1 has no rows at all; on f + 2 the
 * picture comes back; the console and 640x480, which decode alike, swap
 * as a program's exit does; and a frame goes by with no vid_frame. All
 * of it may change the colour and nothing else. */
static void reselect_case(int *utest_result, uint8_t cv)
{
    const long f = 21;
    fresh(cv, true);
    Mon m;
    Steady st{cv, 20};
    m.on_end = steady_end;
    m.on_end_arg = &st;
    m.vs_check = true;
    uint8_t other = cv == 0 ? 3 : cv == 3 ? 0 : cv;
    ASSERT_TRUE(run(m, f + 30, [&](long n) {
        if (n == f)
        {
            dut->canvas = cv;
            dut->blank = 1;
            m.next_check = CHECK_NONE;
            m.check = CHECK_NONE;
        }
        else if (n == f + 1)
            dut->rows_en = 0;
        else if (n == f + 2)
        {
            dut->rows_en = 1;
            dut->blank = 0;
            m.next_check = CHECK_BARS;
        }
        else if (n == f + 3)
            dut->canvas = other;
        else if (n == f + 4)
            dut->canvas = cv;
        else if (n == f + 5)
            dut->frame_en = 0;
        else if (n == f + 6)
            dut->frame_en = 1;
    }));
    ASSERT_EQ(m.rephases, 1L);
    ASSERT_EQ(m.vs_periods_bad, 0L);
    ASSERT_GE(st.checked, 30L);
    ASSERT_GT(m.lit, 0L);
}

UTEST(mvideo, reselect0_console)
{
    reselect_case(utest_result, 0);
}

UTEST(mvideo, reselect1_320x240)
{
    reselect_case(utest_result, 1);
}

UTEST(mvideo, reselect2_320x180)
{
    reselect_case(utest_result, 2);
}

UTEST(mvideo, reselect3_640x480)
{
    reselect_case(utest_result, 3);
}

UTEST(mvideo, reselect4_640x360)
{
    reselect_case(utest_result, 4);
}

UTEST(mvideo, reselect5_320x200)
{
    reselect_case(utest_result, 5);
}

UTEST(mvideo, reselect6_640x400)
{
    reselect_case(utest_result, 6);
}

/* With no vid_frame ever, sync is the same and the picture is black. */
UTEST(mvideo, no_frame_black)
{
    for (uint8_t cv : {0, 1, 6})
    {
        fresh(cv, false);
        Mon m;
        ASSERT_TRUE(run(m, 3, [](long) {}));
        ASSERT_EQ(m.rephases, 0L);
        ASSERT_GT(m.de_px, 0L);
        ASSERT_EQ(m.lit, 0L);
    }
}

/* When the frames stop, the frames after the last one are black and sync
 * carries on as it was; when they come back, so does the picture, and
 * vid_frame finds the raster where it left it. */
UTEST(mvideo, frames_stop_then_resume)
{
    for (uint8_t cv : {1, 3})
    {
        fresh(cv, true);
        Mon m;
        Steady st{cv, 20};
        m.on_end = steady_end;
        m.on_end_arg = &st;
        m.vs_check = true;
        long dark = 0;
        long lit_before = 0;
        ASSERT_TRUE(run(m, 30, [&](long n) {
            if (n == 21)
                dut->frame_en = 0;
            else if (n == 22)
                lit_before = m.lit;
            else if (n == 23)
            {
                dut->frame_en = 1;
                dark = m.lit - lit_before;
            }
        }));
        ASSERT_EQ(m.rephases, 1L);
        ASSERT_EQ(dark, 0L);
        ASSERT_GE(st.checked, 8L);
    }
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    int rc = utest_main(argc, argv);
    if (dut)
    {
        dut->final();
        delete dut;
    }
    return rc;
}
