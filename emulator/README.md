# Rumbledethumps' Picocomputer 6502 - Emulator

An emulator for Rumbledethumps' [Picocomputer 6502](https://picocomputer.github.io).
It runs a WDC 65C02 against an emulated RIA (registers + syscall API + XRAM)
and VGA graphics system, built from floooh's [chips](https://github.com/floooh/chips)
and [sokol](https://github.com/floooh/sokol) and the RP6502's own firmware.

Just like the firmware and the primary SDKs, this project is almost entirely C.
Only the Debug Adapter Protocol and Debug UI code is C++.

This emulator is new code relative to the mature and stable RP6502 firmware.
If emulator behavior differs, the firmware is usually correct.
Expect a few bumps. AI bug fixes are accepted, but not if they come with
a wall of AI text or excessive comments - a human needs to be in the loop to
filter the noise or the maintainer will burn out.


## WebAssembly / Emscripten

TODO Here are the tools you need to build.

## Linux

TODO Here are the tools you need to build.

## Windows

Windows is planned. Looking for a volunteer to drive.

## MacOS

MacOS stalled. Requires a developer with a Mac.


------- TODO clean up below. each platform will have different tools requirements.


## Development System Requirements

* CMake (>= 3.20) and Ninja (or Make)
* A C11 compiler (gcc or clang)
* Submodules: `git submodule update --init` from the repo root — **not**
  `--recursive`. The emulator needs the vendored deps under `vendor/` (chips,
  sokol, imgui, cppdap on the desktop; emsdk for the web build). The firmware
  sources it compiles live in this same repo under `src/`, and FatFs (its
  Unicode tables) is vendored at `vendor/fatfs`.
* For the live window (Linux): GL/X11 dev headers, e.g.
  `sudo apt-get install libgl-dev libx11-dev libxi-dev libxcursor-dev`.
  Without them the emulator still builds and renders via `--screenshot`.
* For the WebAssembly build: nothing extra to install — the [Emscripten
  SDK](https://emscripten.org) is the `vendor/emsdk` submodule (pinned in
  `emscripten-version.txt`); `build_web.sh` fetches the toolchain into it on
  first run.

## Build

From this `emulator/` directory (its `CMakePresets.json` defines `debug`,
`release`, and `wasm`):

```sh
cmake --preset release
cmake --build --preset release
```

## Run

```sh
# Open a window and run a ROM (keyboard input drives the program's stdin)
./build/rp6502-emu tests/roms/adventure.rp6502

# Headless: run a ROM and write the screen to a PNG (no display needed)
./build/rp6502-emu tests/roms/hello.rp6502 --screenshot hello.png

# Headless with scripted keystrokes ('\n' = Enter)
./build/rp6502-emu tests/roms/adventure.rp6502 \
    --input $'no\ntake lamp\n' --frames 200 --screenshot adv.png

# Files are read/written on the host through the MSC0: drive; --fs picks the
# starting directory for relative paths
./build/rp6502-emu tests/roms/adventure.rp6502 --fs ./saves
```


## License

This emulator is licensed under the BSD 3-Clause license — the same terms as the
RP6502 firmware. See [LICENSE](../LICENSE). Every source file carries an
`SPDX-License-Identifier`.

The one exception is the standalone web shell
[src/emu/web/index.html](../src/emu/web/index.html), which you redistribute
alongside a self-playing ROM: it is dual-licensed `BSD-3-Clause OR Unlicense`
(see [UNLICENSE](UNLICENSE)) so it can be modified, rebranded, or placed in the
public domain. The emulator itself stays BSD-3-Clause.

## Third-party software

The distributed executables statically include third-party code, all under
permissive licenses. **Every bundled component may be used commercially without
restriction.** Components and the copyright notices required for binary
distribution are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md);
each dependency's full license text also lives in its `src/<name>` directory.
The running emulator prints the same credits with `rp6502-emu --credits` (on the
web, append `?credits` to the page URL).

| Component | License | In |
|---|---|---|
| floooh's [chips](https://github.com/floooh/chips) (incl. the `w65c02.h` CPU fork) and [sokol](https://github.com/floooh/sokol) | Zlib | desktop, web |
| [RP6502 firmware](https://github.com/picocomputer/rp6502) subset (audio, terminal + font, VGA modes) | BSD-3-Clause | desktop, web |
| [emu8950](https://github.com/digital-sound-antiques/emu8950) OPL audio | MIT | desktop, web |
| FatFs Unicode tables ([ChaN](http://elm-chan.org/fsw/ff/)) | FatFs (BSD-1-Clause) | desktop, web |
| [Emscripten](https://github.com/emscripten-core/emscripten) runtime + musl libc | MIT | web |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | desktop (debugger) |
| [cppdap](https://github.com/google/cppdap) + bundled nlohmann/json | Apache-2.0, MIT | desktop (debugger) |
| [wingetopt](https://github.com/alex85k/wingetopt) | ISC + BSD-2-Clause | Windows only |
