# The machine as a libretro core

Notes for working on this host. Nothing here is needed to use the core —
that is `src/host/libretro/dist/README.txt`.

## What this host is

A libretro frontend owns the loop, the window, the audio device and the input
hardware, and it calls `retro_run` once per video frame. This host is
therefore `emu_core` plus one ABI file, and none of `src/host/sokol`. There is
no sokol, no command line, no script channel, no debugger and no `main()`.

`emu_core` already had every entry point it needs:

| | |
| --- | --- |
| a frame | `vga_run_frame`, which is what `retro_run` is asked for |
| a picture | `vga_set_framebuffer` and `vga_canvas_size`, plus `SET_GEOMETRY` when the canvas changes |
| sound | `aud_render`, which fills a buffer at the 48 kHz this core declares. Most voices are generated at that rate already, and the OPL2 is resampled because a YM3812 runs at 49716 Hz |
| devices | the `keyboard_`, `gamepad_`, `mouse_` and `tablet_` host entry points, the same ones the web host drives |
| a program | `rom_load`, `proc_set_argv`, `main_run` |

Two things are converted on the way out. The machine paints RGBA8 and libretro
asked for XRGB8888, so red and blue trade places. That exchange is its own
inverse, so `tests/cpu/vga` answers here with the same CRCs it answers
everywhere. `aud_render` fills floats, and those become the int16 pairs the
batch callback takes.

This host has no monitor, no debugger and no scripting. A `.rp6502` runs, and
when it stops the core sends `RETRO_ENVIRONMENT_SHUTDOWN` and finishes.

## Build and test

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The same three commands build it on Windows from a Visual Studio x64 developer
prompt, and on macOS. There is one CMake root for every operating system, and
the layer underneath is `osal/posix` or `osal/windows`. The core is
`rp6502_libretro.so`, `.dll` or `.dylib`.

The suite has two halves. `tests/cpu` tests the machine through the shipped
library, using `tests/bench/mut_libretro.c`. `tests/host/libretro` tests this
core as a libretro citizen. Both open the shared library rather than linking
its objects, because only the shipped library can prove the export list and
the version script are right.

```
nm -D --defined-only build/libretro/release/rp6502_libretro.so   # ELF
nm -gU build/libretro/release/rp6502_libretro.dylib              # Mach-O, _retro_*
dumpbin /exports build\libretro\release\rp6502_libretro.dll      # PE
```

Each should print `retro_*` symbols and nothing else. CI checks all three,
which is the only continuous check on the two nobody here can run.

Each platform builds in its own CI job — Linux x86_64 and aarch64, Windows,
macOS and Android — and a `libretro-bundle` job collects them into a single
`rp6502-<version>-libretro.zip` with a folder per platform. One zip keeps the
release page short. libretro builds for twenty-odd platforms, and a row per
machine would bury everything else on the page.

## Running it in a frontend

From VS Code, F5 on **RetroArch Debug** builds the debug core, launches
RetroArch on it, and stops at breakpoints in `retro.c`. It picks a ROM from
`tests/roms`. **RetroArch Debug (path…)** takes any path you type. Both carry
the WSL workaround below, so nobody has to remember it.

By hand:

```
retroarch -v -L build/libretro/release/rp6502_libretro.so tests/roms/adventure.rp6502
```

Headless, for a smoke check. `--max-frames` runs N frames and exits:

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

`config_save_on_exit` is not optional there. RetroArch saves its configuration
when it quits, including appended files, so without it those four null drivers
become permanent and every later run has no window and no sound.

A headless run only says that the library loads and does not crash, which is
why it is not a ctest. A frontend agreeing with the core is not evidence about
the machine, and treating it as evidence would put the oracle outside this
repository. The suite holds the core to the contract, and RetroArch is where a
person looks at it.

### Typing

A frontend binds the keyboard to its own gamepad and hotkeys. In RetroArch,
Enter is Start, `p` pauses, and `x z s a q w` are face and shoulder buttons.
It also keeps the mouse for its own cursor. On a machine that is a computer,
both therefore look dead. The player turns that off with **Game Focus**, which
is Scroll Lock by default, and the core says so on screen the first time a
program asks for the console, the keyboard or the mouse. A program that wants
only a gamepad or the tablet is never told.

Scroll Lock is only the default hotkey, and a handheld may not have the key at
all, in which case it needs remapping. Setting Settings > Input > "Auto Enable
Game Focus" to **Detect** (`input_auto_game_focus = "2"`) avoids the question,
because it turns Game Focus on automatically for cores that register a
keyboard callback, which this one does.

### Under WSL

WSLg's compositor does not advertise `zxdg_decoration_manager_v1`, and
RetroArch's Wayland backend draws no decorations of its own, so the window
arrives with no title bar and no way to move or close it. Nothing is wrong
with the install, because server-side decorations are optional in Wayland and
several compositors decline them. Hiding the Wayland socket makes RetroArch
use X11 instead, which under WSLg means a real Windows window:

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

Not done yet, and not something this repository can do on its own. The steps,
for when it is:

1. PR the `rp6502_libretro.info` from a tagged build to
   [libretro-super](https://github.com/libretro/libretro-super/tree/master/dist/info)
   as `dist/info/rp6502_libretro.info`. It is already inside the release zip.
   The template is `src/host/libretro/dist/rp6502_libretro.info.in` and the
   build fills in its version from `version.cmake`, so send the generated file
   and never the template.
2. Ask the libretro team, through an issue on libretro-super or on Discord, to
   mirror this repository on git.libretro.com and enable its pipeline.
   `.gitlab-ci.yml` at the top of this repository is what their buildbot
   reads. It names `src/host/libretro` as the CMake root and builds the
   `rp6502_libretro` target, which is why that target has exactly that name.
   Its Windows job asks for their MSVC template rather than the mingw one,
   because MSVC is the Windows toolchain this repository builds and tests.
3. The buildbot's nightlies then appear under Online Updater / Core Downloader.

Afterwards, if it seems worth it: an icon in
[retroarch-assets](https://github.com/libretro/retroarch-assets), entries in
[libretro-database](https://github.com/libretro/libretro-database) so programs
land in a playlist, and a page in
[libretro-docs](https://github.com/libretro/docs).

TODO: figure out versioning and release tagging.
