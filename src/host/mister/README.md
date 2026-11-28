# Porting to MiSTer

These notes are for working on this core. For now the core shows colour
bars, `mister_bars`, a test card in the shape of the machine's pixel
stream, through `core/mister_video.sv`, the path the machine's picture
will take. The OSD's Test canvas option picks the canvas.

## Video

`core/mister_video.sv` hands `sys/` each canvas as it is, and MiSTer's
scaler, shaders and scanline filters do the rest. The rules:

- **Native pixels.** A canvas goes out at its own width and height, with
  no letterbox, no pillarbox, and no pixel or line doubling. A 320x240
  canvas is 320 pixels on 240 lines.
- **True aspect.** Pixels are square, so `VIDEO_ARX:ARY` is the canvas's
  own shape: 4:3 for 640x480, 320x240 and the console, 16:10 for
  640x400 and 320x200, 16:9 for 640x360 and 320x180.
- **The machine's 60 Hz.** Every frame is 840000 clk_sys, the machine's
  800x525 frame at 25.2 MHz, so `vsync_adjust` can lock to it.
  Progressive; `VGA_F1` and `VGA_SL` are 0.
- **A new canvas may resync HDMI.** Main re-programs HDMI when what it
  measures changes and `vsync_adjust` is 1 or 2.
- **The same canvas never does.** Programs select the canvas they have
  to clear the scanline table between scenes. Nothing Main measures may
  change then, not for one frame.

Main reads the video through `UIO_GET_VRES` and `UIO_GET_FB_PAR`
(`Main_MiSTer/video.cpp`, `get_video_info`), and resyncs on a new
`nres` or a new aspect. `sys/hps_io.sv`'s `video_calc` measures the
core's CE_PIXEL, DE, HS and VS after `sys_top.v`'s `sync_fix` and
`scanlines`, which change none of their timing. At each fall of VS it
compares the DE pixels of the first line and the DE lines of the frame,
and `new_vmode`, with the last frame's, and any change bumps `nres`
about 15 frames later. It also keeps the clocks from HS to DE (de_h),
the lines from VS to DE (de_v), the clocks between strobes (pixrep),
the DE clocks of a line, and the frame's clocks; the clk_100 timers
keep the HS and VS periods.

| canvas | line clocks | lines | CE | DE lines | VS lines | de_h | de_v | ARX:ARY |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 640x480, console | 1600 | 525 | every 2nd | 0-479 | 490-491 | 96 | 33 | 4:3 |
| 640x400 | 1600 | 525 | every 2nd | 0-399 | 450-451 | 96 | 73 | 16:10 |
| 640x360 | 1600 | 525 | every 2nd | 0-359 | 430-431 | 96 | 93 | 16:9 |
| 320x240 | 3206, line 261 3234 | 262 | every 8th | 1-240 | 244-246 | 326 | 16 | 4:3 |
| 320x200 | 3206, line 261 3234 | 262 | every 8th | 1-200 | 224-226 | 326 | 36 | 16:10 |
| 320x180 | 3206, line 261 3234 | 262 | every 8th | 1-180 | 214-216 | 326 | 46 | 16:9 |

A line is active, front porch, HS, back porch; HS is pixels 656-751 of
the 640 raster and 330-359 of the 320 one. The 320 raster is 240p at
15.7 kHz with its picture 80% of a line, as the 640 one has. No 262
equal lines make 840000 clocks, so one line in the back porch is 28
clocks longer. VS sits in the middle of the lines below the picture, so
a CRT centres it.

How the same canvas keeps all of it:

- Everything but the colour is a function of the raster's counters and
  of the canvas's width and height, and the canvas is taken only at the
  end of a frame from the machine's, which changes only when a program
  writes a different one. The console and 640x480 decode alike, so a
  program's exit back to the console changes nothing.
- The raster never stops. vid_frame only sets its phase, and both
  rasters are as long as the machine's frame, so after the first frame
  it finds the raster already there. A new canvas starts a whole frame.
- A cleared table, a frame with no rows, a held or reset machine: these
  reach only the colour. Black is the colour, never DE dropped.
- `new_vmode` is tied low.

The firmware's `vga_canvas_select` clears the table even for the same
canvas; that is what the call is for, so it has no same-canvas check.

A 320 wide canvas is 15 kHz, so `VGA_SCALER` follows MiSTer.ini's
`forced_scandoubler`, unless `direct_video`: a 31 kHz-only analog
monitor gets the scaler's picture and a 15 kHz CRT the core's own.

## The tree

MiSTer's maintainers rebuild a core from its repository with Quartus
17.0.2 and a fresh `sys/`, so the build exports a tree in the shape of
Template_MiSTer, <https://github.com/MiSTer-devel/Template_MiSTer>, to
`build/mister/RP6502_MiSTer/`:

| | from |
| --- | --- |
| `sys/` | upstream, the tree pinned in `sys.pin`, never edited |
| `RP6502.qpf`, `.qsf`, `.sdc`, `.sv` | `tree/`, written by hand |
| `rtl/pll.v`, `pll.qip`, `pll/` | `tree/rtl/`, the PLL wizard's output |
| `rtl/*` | the machine's RTL, the soft CPU's hazard3 modules and the MiSTer glue, in one folder |
| `files.qip` | generated: the RTL and includes in `rtl/`, then `RP6502.sdc` and `RP6502.sv`; the PLL comes in through `sys.qip` |
| `releases/RP6502_<date>.rbf` | the package step |

`RP6502.qsf` is Template.qsf with `TIMEQUEST_MULTICORNER_ANALYSIS ON`,
and our assignments after it. Its `LAST_QUARTUS_VERSION` stays "17.0.2
Standard Edition", the line Template has, although Quartus rewrites it
in any project it opens; the build only ever opens copies.

`sys/` comes, when CMake configures, from the branch `mister` of this
repository, which publishes the core, or else from a Template_MiSTer
clone at the pinned commit:

    cmake --preset release \
        -DRP6502_MISTER_SYS_REPO=<a Template_MiSTer clone>

Either way its git tree id must be the one in `sys.pin`. Without it there
are no MiSTer targets, and tests/rtl skips `sys_sdc_names`.

## Building

| target | does |
| --- | --- |
| `mister_export` | writes the tree; `sdc_gate.py` checks its SDC |
| `mister_elab` | analysis and elaboration of a copy of the tree with 17.0.2 and with 25.1; seconds, worth running after every RTL edit |
| `rbf17`, `rbf25` | the stock flow, `quartus_sh --flow compile RP6502`, in `build/mister/q17` or `q25`, then the gates every FPGA host's fit passes (`src/host/fpga/README.md`) |
| `package` | `build/mister/package/_Computer/RP6502_<date>.rbf`, `RP6502_MiSTer.zip`, and the rbf in the tree's `releases/` |
| `mister` (all) | both elaborations, `rbf17` and `package` |

Releases are built with 17.0.2. 25.1 builds the same tree as a check,
asked for by name with `rbf25`, at every milestone and before every
release. `sys.qip` picks the PLL's qip by Quartus's major version and
`sys/` has one only for 13 and 17, so a 25.1 copy gets `sys/pll_q25.qip`,
a copy of `pll_q17.qip`. `sys/` itself is never changed.

`q17` and `q25` keep their `db/` between builds, and every file of the
export is copied over them before each one, the qsf Quartus rewrote
among them.

The `<date>` of a release is the `BUILD_DATE` that `sys/build_id.tcl`
wrote for the fit, the date the OSD's V line shows.

The gates read these, in `quartus/`:

- `drc_baseline17.txt`, `drc_baseline25.txt`: the Design Assistant's
  findings each version may report, seeded from this core's own fits.
  `rbf17` and `rbf25` wait for theirs; the target `q17/fit.stamp` or
  `q25/fit.stamp` runs the fit and the Design Assistant without the
  gates, and leaves `output_files/RP6502.drc.rpt` to seed it from.
- `wiring_sdc_counts.txt`: the registers each collection in
  `src/core/sys/wiring.sdc` matches. The bars have no machine, so
  `files.qip` leaves `wiring.sdc` out and each count is 0.
- `sys_ignored.txt`: the assignments of `sys/` Quartus leaves unapplied.
- `sys_filters.txt`: the filters of `sys/`'s SDC that match nothing in
  this core, because they belong to a `sys/` feature it does not use.

## The PLL

`tree/rtl/pll.v` is the wizard's: a 50 MHz reference and three outputs,
clk_sys at 50.4 MHz, clk_a2 at 100.8 MHz shifted 8990 ps and clk_ph at
50.4 MHz shifted 12400 ps. The VCO runs at 403.2 MHz, so a phase step is
310 ps, and the shifts are 29 and 40 steps. To change it, edit the
`Retrieval info` lines of `pll.v` in a scratch copy, run
`qmegawiz -silent pll.v` there with Quartus 17.0.2, and copy back only
`pll.v`, `pll.qip` and `pll/pll_0002.v` and `.qip`, the four files
Template keeps. Run headless, the wizard records the device as "Unknown"
and adds a speed grade 2 line to `pll.qip`. Neither reaches the
hardware: the fitter sets the PLL's counters for the qsf's device.

## Tests

In tests/rtl, under the label `host.mister`:

- `mvideo`: `mister_bars` into `mister_video`. Every clock against a
  model of the table above, and `hps_io.sv`'s `video_calc` copied into
  the test to measure it as `sys/` does: every canvas, a change of
  canvas, and the same canvas selected again, which may change only
  the colour.
- `sdc_filters_mister`: `RP6502.sdc` and `wiring.sdc` name only what the
  MiSTer RTL has.
- `sys_sdc_names`: none of `sys_top.sdc`'s bare name patterns can match
  this core's registers (`gen/sys_sdc_gate.py`).
- `mister_tree`: `tree/` is still what 17.0.2 and a fresh `sys/` build,
  and `sys.pin` is whole (`gen/tree_gate.py`).
