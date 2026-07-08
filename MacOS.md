# RP6502 on macOS: Firmware + Emulator Build Trees

This guide sets up both local build trees on macOS:
- `build/firmware` for RP2350 firmware targets
- `build/emulator` for the desktop emulator

The emulator path on macOS is new in this repo, so treat this as first bring-up.

## 1) Install prerequisites

Install Xcode command line tools:

```bash
xcode-select --install
```

Install Homebrew if needed:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Install required tools:

```bash
brew update
brew install cmake ninja pkg-config
```

## 2) Clone and initialize submodules

From repository root:

```bash
git submodule update --init
```

Initialize the nested cppdap JSON submodule used by the emulator debugger:

```bash
git -C vendor/cppdap submodule update --init third_party/json
```

If you plan to build web emulator output later, install/activate emsdk once:

```bash
./vendor/emsdk/emsdk install latest
./vendor/emsdk/emsdk activate latest
```

## 3) Create and build the firmware tree

From repository root:

```bash
cmake -S . -B build/firmware -G Ninja
cmake --build build/firmware
```

Notes:
- This project targets RP2350 boards (for example `pico2_w` by default).
- `cmake --build build/firmware` builds all firmware targets in one pass.

## 4) Create and build the emulator tree (macOS first bring-up)

Option A (using the emulator presets):

```bash
cmake -S src/emu -B build/emulator -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/emulator
```

Option B (equivalent preset flow, run from `src/emu`):

```bash
cmake --preset debug
cmake --build --preset debug
```

Expected macOS behavior:
- The build uses Metal/Cocoa frameworks for the windowed renderer.
- Objective-C compilation is enabled for Sokol implementation units.

## 5) Optional: run emulator tests

From repository root:

```bash
ctest --test-dir build/emulator --output-on-failure
```

## 6) VS Code task equivalents

If you prefer tasks:
- Firmware build: `Compile Project`
- Emulator configure: `emu: Configure (Debug)`
- Emulator build: `emu: Build (Debug)`
- Emulator tests: `emu: Test`

## 7) Clean rebuild when switching branches or after major pulls

From repository root:

```bash
rm -rf build
cmake -S . -B build/firmware -G Ninja
cmake --build build/firmware
cmake -S src/emu -B build/emulator -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/emulator
```

## 8) Troubleshooting

### Missing submodule sources
Symptoms:
- CMake errors referring to missing TinyUSB/littlefs/cppdap content

Fix:

```bash
git submodule update --init
git -C vendor/cppdap submodule update --init third_party/json
```

### Emulator builds but no window
On macOS this should not require extra GL/X11 packages. If a warning indicates a headless build anyway, reconfigure from a clean tree:

```bash
rm -rf build/emulator
cmake -S src/emu -B build/emulator -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/emulator
```

### Build tree contains stale artifacts
If old and new generated files are mixed after a pull, remove `build/` and regenerate both trees as shown in section 7.

## 9) macOS emulator code changes (first bring-up notes)

Two source-level compatibility updates were needed for current Xcode/macOS SDK builds:

1. `strftime_l` declaration visibility in the clock API:
- File: [src/emu/api/clk.c](src/emu/api/clk.c)
- Change: add an Apple-only forward declaration for `strftime_l(...)`.
- Why: with this SDK/header exposure, `strftime_l` may not be visible from the default include path used by this target, which caused an implicit declaration compile error.

2. `clock_nanosleep` absolute sleep fallback in the frame pacer:
- File: [src/emu/app/app_sokol.c](src/emu/app/app_sokol.c)
- Change: on macOS, use relative `nanosleep` with EINTR retry instead of `clock_nanosleep(..., TIMER_ABSTIME, ...)`.
- Why: `clock_nanosleep` and/or `TIMER_ABSTIME` were not available in this build mode on macOS, causing compile failures.

3. Backend-specific vertical flip for emulator video:
- File: [src/emu/app/app_sokol.c](src/emu/app/app_sokol.c)
- Change: apply the final texture V-flip only on GL-family backends; leave Metal unflipped.
- Why: the emulator framebuffer is stored with row 0 at the top, but GL backends sample it upside down while Metal presents it upright. The orientation difference is backend-specific, not ROM-specific.

After these changes, `cmake --build build/emulator` completed successfully on macOS.




