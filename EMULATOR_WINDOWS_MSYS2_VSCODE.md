# Building/running the desktop emulator on Windows via MSYS2 (UCRT64)

`README.md` says nobody was working on the Windows emulator port. This document covers a
second, working route that doesn't need Visual Studio: MSYS2's UCRT64 GCC toolchain. It does
not replace or modify the MSVC path `README.md` describes — that's still unimplemented.

Status: builds and runs natively, all 20 `ctest` suites pass, the D3D11 window renders.

## Prerequisites

- MSYS2 installed (default location `C:\msys64`).
- The **UCRT64** environment with `gcc`, `cmake`, and `ninja` installed
  (`C:\msys64\ucrt64\bin\gcc.exe`, `g++.exe`, `cmake.exe`, `ninja.exe`). Install via the
  MSYS2 shell if missing:
  ```
  pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
  ```

## Command-line build

From a shell with `C:\msys64\ucrt64\bin` **prepended** to `PATH` (this matters — see
Troubleshooting below):

```
git submodule update --init --depth 1 vendor/chips vendor/sokol vendor/imgui vendor/cppdap vendor/wingetopt
git -C vendor/cppdap submodule update --init --depth 1 third_party/json

cmake -S src/emu -B build/emulator/win-release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/emulator/win-release
ctest --test-dir build/emulator/win-release --output-on-failure
```

**Use Release, not Debug, for anything but active debugging.** Measured on this machine:
rendering 600 emulated frames took 31.6s with a Debug build vs. 1.25s with Release — an
optimized build is roughly 25x faster.

## VS Code CMake Tools

`src/emu/CMakePresets.json` puts the **emu** folder into CMake's *presets mode*. In that
mode, VS Code CMake Tools hides the classic Kit-selection command
(`cmake.selectKit`'s command-palette entry has `"when": "... && !useCMakePresets"` in the
extension's own `package.json`) — there's no "Select a Kit" to reach for. The root
**rp6502** folder has no `CMakePresets.json`, so it stays in Kit+Variant mode and "Select a
Kit" works fine there (for the Pico/ARM firmware toolchain) — the two folders behave
differently on purpose.

In presets mode the compiler has to come from a preset's `cacheVariables`, and the
project's own `src/emu/CMakePresets.json` (`debug`/`release`) doesn't set one — that's why
plain configure falls through to PATH-searching and can pick up the wrong compiler (see
Troubleshooting). The fix is a **`src/emu/CMakeUserPresets.json`** — CMake's standard,
git-ignored, machine-local override file that gets merged with `CMakePresets.json`
automatically:

```json
{
    "version": 3,
    "configurePresets": [
        {
            "name": "win-debug",
            "displayName": "Debug (MSYS2 UCRT64)",
            "inherits": "debug",
            "environment": {
                "PATH": "C:/msys64/ucrt64/bin;$penv{PATH}"
            },
            "cacheVariables": {
                "CMAKE_C_COMPILER": "C:/msys64/ucrt64/bin/gcc.exe",
                "CMAKE_CXX_COMPILER": "C:/msys64/ucrt64/bin/g++.exe"
            }
        },
        {
            "name": "win-release",
            "displayName": "Release (MSYS2 UCRT64)",
            "inherits": "release",
            "environment": {
                "PATH": "C:/msys64/ucrt64/bin;$penv{PATH}"
            },
            "cacheVariables": {
                "CMAKE_C_COMPILER": "C:/msys64/ucrt64/bin/gcc.exe",
                "CMAKE_CXX_COMPILER": "C:/msys64/ucrt64/bin/g++.exe"
            }
        }
    ],
    "buildPresets": [
        { "name": "win-debug", "configurePreset": "win-debug" },
        { "name": "win-release", "configurePreset": "win-release" }
    ]
}
```

`environment.PATH` is not optional even with an explicit `CMAKE_C_COMPILER` path: `gcc.exe`
spawns `cc1.exe` (the actual compiler) as a subprocess, and `cc1.exe` needs
`ucrt64/bin` on `PATH` to resolve its own runtime DLLs (`libgcc_s_seh-1.dll` etc., which live
in `ucrt64/bin`, not next to `cc1.exe`). Without it, `gcc.exe` fails with **zero diagnostic
output** — confirmed while diagnosing this exact setup.

This file is already in `.gitignore` — it's meant to stay local to each machine (adjust the
`C:/msys64/...` paths if your MSYS2 lives elsewhere).

To use it: in VS Code's CMake side panel, set Folder to **emu**, then Configure Preset →
**"Debug (MSYS2 UCRT64)"** or **"Release (MSYS2 UCRT64)"**.

## F5 debugging ("Emulator Debug") on Windows

`.vscode/launch.json`'s **"Emulator Debug"** config (`type: cppdbg`, `MIMode: gdb`) was
written for Linux/macOS and needed three fixes to work on Windows/MSYS2:

1. **Install gdb** — only `gcc`/`cmake`/`ninja` are covered by the Prerequisites above;
   the debugger is a separate package:
   ```
   pacman -S mingw-w64-ucrt-x86_64-gdb
   ```
2. **Wrong program filename.** The top-level `"program"` field points at
   `build/emulator/debug/rp6502-emu` (no extension, correct for Linux/macOS). Windows needs
   `rp6502-emu.exe`. Fixed with a `"windows"` platform-override block — VS Code merges this
   in only when running on Windows, so the Linux/macOS fields are untouched:
   ```jsonc
   "windows": {
       "program": "${workspaceFolder}/build/emulator/debug/rp6502-emu.exe",
       "miDebuggerPath": "C:/msys64/ucrt64/bin/gdb.exe"
   }
   ```
   `miDebuggerPath` is set explicitly (rather than relying on `gdb` being found via `PATH`)
   so this works even before the `PATH` fix below takes effect.
3. **PATH, again.** The `preLaunchTask` (`emu: Build (Debug)`) runs
   `cmake --build --preset debug` — the *base* preset from the committed
   `CMakePresets.json`, not the `win-debug` preset from `CMakeUserPresets.json` that carries
   the `environment.PATH` fix (task and preset are independent; there's no way to make a
   shared, committed task reference a gitignored, machine-local preset name without breaking
   Linux/macOS/other Windows setups). Since this is really the same "process spawned without
   `ucrt64/bin` on `PATH`" problem from the Troubleshooting section, but now hitting *every*
   entry point VS Code has (tasks, launch configs, and CMake Tools alike) rather than just
   configure, the durable fix is to add `C:\msys64\ucrt64\bin` to the **user's permanent
   Windows PATH** once, outside VS Code:
   ```
   setx PATH "%PATH%;C:\msys64\ucrt64\bin"
   ```
   Requires restarting VS Code (and any open terminals) to take effect. After that, every
   `process`-type task, launch config, and CMake Tools action inherits it automatically —
   no more per-tool PATH workarounds needed (the `CMakeUserPresets.json` `environment.PATH`
   entry becomes redundant belt-and-suspenders at that point, but harmless to leave in place).

With all three in place, F5 → **Emulator Debug** builds, launches
`rp6502-emu.exe --debug <rom>` under gdb, and stops/breaks normally.

## Running from the Windows desktop (no MSYS2 shell needed)

`src/emu/CMakeLists.txt` has a `WIN32 AND NOT MSVC` block that statically links the MinGW
runtime into `rp6502-emu.exe`:

```cmake
if(WIN32 AND NOT MSVC)
    target_link_options(rp6502-emu PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
```

This removes the executable's dependency on `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, and
`libstdc++-6.dll` (everything else it needs — `d3d11.dll`, `USER32.dll`, the Universal CRT,
etc. — already ships with Windows 10/11). The result runs standalone: point a desktop
shortcut straight at `rp6502-emu.exe <rom.rp6502>`, no MSYS2 install or `PATH` changes
required at runtime.

## Troubleshooting

- **Configure grabs `C:\llvm-mos\bin\clang.exe` and fails with `ld.lld: unable to find
  library -lkernel32` (etc.)** — PATH/kit resolution found the 6502 cross-compiler instead
  of a native one; it has no host CRT or link libraries. Use the `win-debug`/`win-release`
  presets above (or pass `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++` on the command
  line from a shell with `ucrt64/bin` on `PATH`).
- **`gcc.exe` fails with no error text at all, build just stops** — `ucrt64/bin` isn't on
  `PATH`, so `cc1.exe` can't load its own DLLs. Fixed by the preset's `environment.PATH`
  entry (command line: `export PATH="/c/msys64/ucrt64/bin:$PATH"` first).
- **`[saudio][error][id:13][line:1721]` at runtime** — a non-fatal stock-sokol WASAPI
  init failure (`IAudioClient::Initialize`, `vendor/sokol/sokol_audio.h`). The emulator keeps
  running without sound. Pass `--mute` to skip trying, or check that your default playback
  device isn't locked to exclusive mode by another app.
- **F5 "Emulator Debug" fails to start / can't find the program / can't start gdb** — see
  the dedicated section below; it's the same missing-`ucrt64/bin`-on-`PATH` issue plus a
  Linux/macOS-only program path and a missing `gdb` package.
- **F5 session starts, then gdb reports `error return .../windows-nat.c:2916 was 5: Odmowa
  dostępu` (Access Denied), the program "exits" with a bogus code (e.g. 42), then
  `... was 6: Invalid Handle`** — this isn't the emulator crashing or
  returning that exit code itself (`src/emu/app/main.c` only ever returns 0/1/2); it's gdb's
  Windows-native backend losing a `ContinueDebugEvent`/`WaitForDebugEvent` call, which happens
  when antivirus behavior-monitoring intercepts the Windows debug APIs a native debugger
  needs. Confirmed cause on this machine: **Avast**. Fix: add two exclusions in Avast
  (Settings → General → Exceptions):
  - `C:\msys64\ucrt64\bin\gdb.exe` — the debugger itself.
  - `build\` (as a **folder** exclusion) — covers every
    build output dir (`debug`/`release`/`win`/`win-release`) so new build dirs don't need a
    fresh exclusion each time.
  If exclusions alone don't fix it, temporarily disable Avast's "Self-Defense"/"Behavior
  Shield" to confirm the hypothesis, then re-enable it with the exclusions in place. (Any
  other antivirus with similar self-defense/behavior-monitoring features can cause the same
  symptom — the fix is the same shape: exclude `gdb.exe` and the build output folder.)
- **`[sapp][error][id:22] ... WIN32_D3D11_CREATE_DEVICE_AND_SWAPCHAIN_WITH_DEBUG_FAILED` at
  startup** — harmless, Debug-build-only noise, unrelated to the above. `SOKOL_DEBUG` (auto-on
  for Debug builds, since only Release defines `NDEBUG`) makes `sokol_app.h` request the D3D11
  SDK debug layer (`D3D11_CREATE_DEVICE_DEBUG`), which needs the Windows optional "Graphics
  Tools" feature that isn't installed by default. `sokol_app.h:8804-8850` already detects the
  failure, strips the flag, and retries successfully — the window still opens fine. Ignore it,
  or install "Graphics Tools" (Settings → Apps → Optional Features) purely to silence it.

## What changed in the source tree

MinGW's C library is close to POSIX but not complete; these files got small, mostly
`#ifdef _WIN32`-guarded fixes for the real gaps (not a full inventory — see `git log`/
`git diff` for exact lines):

- `src/emu/posix/dir.c`, `src/emu/posix/fs.c` — mingw's `dirent` has no `d_type` (stat the
  joined path instead), no `<sys/statvfs.h>` (use `GetDiskFreeSpaceExA`), `mkdir()` takes one
  argument not two, and a new `fs_realpath()` (mingw has no `realpath()`) with `\`→`/`
  normalization (mingw's `getcwd()`/`_fullpath()` return backslash paths).
- `src/emu/plat.h` — added the `fs_realpath()` declaration to the portable seam.
- `src/emu/api/clk.c` — POSIX-2008 `locale_t`/`newlocale`/`strftime_l` don't exist on mingw;
  added the `_create_locale`/`_strftime_l` equivalents, including honoring `LC_ALL`/`LANG`
  from the environment (mingw's `_create_locale(LC_ALL, "")` ignores them, unlike POSIX).
- `src/emu/api/pro.c` — replaced the direct `realpath()` call with the new `fs_realpath()`.
- `src/emu/host/msc.c`, `src/emu/mon/rom.c` — mingw has no `pread()` (added a small
  `lseek`+`read` shim); `open()` needs `O_BINARY` on Windows or the CRT's text-mode read
  stops dead at a `0x1A` byte (Ctrl-Z-as-EOF) — this was silently truncating file reads.
- Several `src/emu/tests/*.c` files — `_WIN32` guards for `mkdir`/`realpath`/`setenv`
  differences, a portable temp-directory helper (Windows has no `/tmp`), and updated
  expected-path strings to account for Windows drive-letter paths (the existing
  `MSC0://C/...` mapping in `host/msc.c` was already correct — only the tests' own
  hand-built expectation strings needed fixing).
