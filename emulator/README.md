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
