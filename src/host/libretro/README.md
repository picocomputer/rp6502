# The machine as a libretro core

Notes for working on this host. Nothing here is needed to use the core —
that is `src/host/libretro/dist/README.txt`.

## What this host is

A libretro frontend owns the loop, the window, the audio device and the
input hardware, and calls `retro_run` once per video frame. So this host
is `emu_core` and an ABI file, and none of `src/host/sokol` — no sokol,
no command line, no script channel, no debugger, no `main()`.

The seams it needs were already there:

| | |
| --- | --- |
| a frame | `vga_run_frame`, which is exactly what `retro_run` is asked for |
| a picture | `vga_set_framebuffer` + `vga_canvas_size`, and `SET_GEOMETRY` when the canvas changes |
| sound | `aud_render`, which fills a buffer at the 48 kHz this core declares — most voices are generated at it already, and the OPL2 is resampled because a YM3812 runs at 49716 Hz |
| devices | the `keyboard_` / `gamepad_` / `mouse_` / `tablet_` host entry points, the same ones the web host drives |
| a program | `rom_load`, `proc_set_argv`, `main_run` |

Two things are converted on the way out. The machine paints RGBA8 and
libretro asked for XRGB8888, so red and blue trade places — an exchange
that is its own inverse, which is why `tests/cpu/vga` answers here with
the same CRCs it answers everywhere. And what `aud_render` fills is
floats, which become the int16 pairs the batch callback takes.

There is no monitor on this host, and no debugger or scripting. A
`.rp6502` runs; when it stops, the core sends `RETRO_ENVIRONMENT_SHUTDOWN`
and is finished.

## Build and test

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The same three commands build it on Windows, from a Visual Studio x64
developer prompt, and on macOS — this is one root on every OS, and the
seam under it is `host/posix` or `host/windows` depending on which one you
are. The core is `rp6502_libretro.so`, `.dll`, or `.dylib`. The suite is
two halves: `tests/cpu` is the machine, answering through the shipped
library via `tests/bench/mut_libretro.c`, and `tests/host/libretro` is
this core as a libretro citizen. Both open the `.so` rather than linking
its objects, because the export list and the version script are exactly
what a pile of objects cannot be wrong about.

```
nm -D --defined-only build/libretro/release/rp6502_libretro.so   # ELF
nm -gU build/libretro/release/rp6502_libretro.dylib              # Mach-O, _retro_*
dumpbin /exports build\libretro\release\rp6502_libretro.dll    # PE
```

should print `retro_*` and nothing else. CI checks all three, which is the
only continuous proof of the two nobody here can run.

Each platform builds in its own CI job — Linux x86_64 and aarch64,
Windows, macOS, Android — and one `libretro-bundle` job collects them into
a single `rp6502-<version>-libretro.zip`, a folder per platform. A core is one thing, and a release page listing it once per
machine would bury everything else — libretro builds for twenty-odd
platforms, and this shape does not grow a row for each.

## Running it in a frontend

From VS Code, F5 on **RetroArch Debug** builds the debug core, launches
RetroArch on it, and stops at breakpoints in `retro.c`. It picks a ROM from
`tests/roms`; **RetroArch Debug (path…)** takes any path you type. Both
carry the WSL workaround below, so nobody has to remember it.

By hand:

```
retroarch -v -L build/libretro/release/rp6502_libretro.so tests/roms/adventure.rp6502
```

Headless, for a smoke check — `--max-frames` runs N frames and exits:

```
cat > /tmp/headless.cfg <<'EOF'
video_driver = "null"
audio_driver = "null"
input_driver = "null"
menu_driver = "null"
config_save_on_exit = "false"
EOF
retroarch --appendconfig /tmp/headless.cfg --max-frames 600 -v \
    -L build/libretro/release/rp6502_libretro.so tests/roms/adventure.rp6502
```

`config_save_on_exit` is not optional there. RetroArch saves its
configuration when it quits, appended files included, so without it those
four null drivers become permanent and every later run is headless with no
window and no sound.

That only says the library loads and does not crash, which is why it is
not a ctest: a frontend agreeing with us is not evidence about the
machine, and making it one would be handing the oracle back to something
outside this repository. The suite holds the core to the contract;
RetroArch is where a person looks at it.

### Typing

A frontend binds the keyboard to its own gamepad and hotkeys — in RetroArch
Enter is Start, `p` pauses, and `x z s a q w` are face and shoulder
buttons — so on a machine that is a computer the keyboard looks dead. The
player turns that off with **Game Focus**, which is Scroll Lock by
default, and the core says so on screen when a program loads.

Settings > Input > "Auto Enable Game Focus" set to **Detect** makes it
automatic for cores that register a keyboard callback, which this one
does.

### Under WSL

WSLg's compositor does not advertise `zxdg_decoration_manager_v1`, and
RetroArch's Wayland backend draws no decorations of its own, so the window
arrives with no title bar and no way to move or close it. Nothing is wrong
with the install — server-side decorations are optional in Wayland and
several compositors decline them. Hide the Wayland socket and X11 gets
used instead, which under WSLg means a real Windows window:

```
WAYLAND_DISPLAY=nonexistent-0 retroarch -L … game.rp6502
```

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

1. PR the `rp6502_libretro.info` out of a tagged build to
   [libretro-super](https://github.com/libretro/libretro-super/tree/master/dist/info)
   as `dist/info/rp6502_libretro.info`. It is already inside the release
   zip — the template is `src/host/libretro/dist/rp6502_libretro.info.in` and
   the build fills its version in from `version.cmake`, so the file to
   send is the generated one and never the template.
2. Ask the libretro team — an issue on libretro-super, or Discord — to
   mirror this repository on git.libretro.com and enable its pipeline.
   `.gitlab-ci.yml` at the top of this repository is what their buildbot
   reads; it names `src/host/libretro` as the CMake root and builds the
   `rp6502_libretro` target, which is why that target has exactly that
   name. Its Windows job asks for their MSVC template rather than the
   mingw one, because MSVC is the Windows toolchain this repository
   builds and tests.
3. The buildbot's nightlies then appear under Online Updater / Core
   Downloader.

Afterwards, if it seems worth it: an icon in
[retroarch-assets](https://github.com/libretro/retroarch-assets), entries
in [libretro-database](https://github.com/libretro/libretro-database) so
programs land in a playlist, and a page in
[libretro-docs](https://github.com/libretro/docs).
