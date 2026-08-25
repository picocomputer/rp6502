# The machine as a libretro core

Notes for working on this host. Nothing here is needed to use the core —
that is `src/dist/libretro/README.txt`.

## What this host is

A libretro frontend owns the loop, the window, the audio device and the
input hardware, and calls `retro_run` once per video frame. So this host
is `emu_core` and an ABI file, and none of `src/core/emu/app` — no sokol,
no command line, no script channel, no debugger, no `main()`.

The seams it needs were already there:

| | |
| --- | --- |
| a frame | `sys_run_frame`, which is exactly what `retro_run` is asked for |
| a picture | `vga_set_framebuffer` + `vga_canvas_size`, and `SET_GEOMETRY` when the canvas changes |
| sound | `aud_read`, native-rate stereo at the 48 kHz this core declares, so nothing is resampled |
| devices | the `kbd_` / `pad_` / `mou_` / `tab_` host entry points, the same ones the web host drives |
| a program | `rom_load`, `pro_set_argv`, `main_run` |

Two things are converted on the way out. The machine paints RGBA8 and
libretro asked for XRGB8888, so red and blue trade places — an exchange
that is its own inverse, which is why `tests/cpu/vid` answers here with
the same CRCs it answers everywhere. And the audio ring is floats, which
become the int16 pairs the batch callback takes.

There is no monitor on this host, and no debugger or scripting. A
`.rp6502` runs; when it stops, the core sends `RETRO_ENVIRONMENT_SHUTDOWN`
and is finished.

## Build and test

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`build/libretro/release/rp6502_libretro.so` is the core. The suite is
two halves: `tests/cpu` is the machine, answering through the shipped
library via `tests/bench/mut_libretro.c`, and `tests/host/libretro` is
this core as a libretro citizen. Both open the `.so` rather than linking
its objects, because the export list and the version script are exactly
what a pile of objects cannot be wrong about.

```
nm -D --defined-only build/libretro/release/rp6502_libretro.so
```

should print `retro_*` and nothing else.

## Running it in a frontend

```
retroarch -v -L build/libretro/release/rp6502_libretro.so tests/roms/adventure.rp6502
```

Headless, for a smoke check — `--max-frames` runs N frames and exits:

```
printf 'video_driver = "null"\naudio_driver = "null"\ninput_driver = "null"\nmenu_driver = "null"\n' > /tmp/headless.cfg
retroarch --appendconfig /tmp/headless.cfg --max-frames 600 -v \
    -L build/libretro/release/rp6502_libretro.so tests/roms/adventure.rp6502
```

That only says the library loads and does not crash, which is why it is
not a ctest: a frontend agreeing with us is not evidence about the
machine, and making it one would be handing the oracle back to something
outside this repository. The suite holds the core to the contract;
RetroArch is where a person looks at it.

### What to look at

- a program's picture, and that a canvas change resizes rather than crops
- the keyboard, with Game Focus on (Scroll Lock)
- a gamepad, and the button labels under Controls
- sound
- Load Content twice in a row, and Restart
- Quit, and that nothing was written into the directory RetroArch was
  started from

## Getting it into the Online Updater

Not done yet, and not something this repository can do on its own. The
steps, for when it is:

1. PR `src/dist/libretro/rp6502_libretro.info` to
   [libretro-super](https://github.com/libretro/libretro-super/tree/master/dist/info)
   as `dist/info/rp6502_libretro.info`.
2. Ask the libretro team — an issue on libretro-super, or Discord — to
   mirror this repository on git.libretro.com and enable its pipeline.
   `.gitlab-ci.yml` at the top of this repository is what their buildbot
   reads; it names `src/host/libretro` as the CMake root and builds the
   `rp6502_libretro` target, which is why that target has exactly that
   name.
3. The buildbot's nightlies then appear under Online Updater / Core
   Downloader.

Afterwards, if it seems worth it: an icon in
[retroarch-assets](https://github.com/libretro/retroarch-assets), entries
in [libretro-database](https://github.com/libretro/libretro-database) so
programs land in a playlist, and a page in
[libretro-docs](https://github.com/libretro/docs).
