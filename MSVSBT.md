# Installing Visual Studio Build Tools + VS Code for the "emu" folder (MSVC route)

This is the *other* Windows route besides MSYS2/UCRT64 (see
`EMULATOR_WINDOWS_MSYS2_VSCODE.md`) — the one `README.md` actually describes ("with MSVC
configure from an x64 Native Tools prompt"). This document only covers **installation and
VS Code wiring**. It does not install anything by itself — follow the steps manually.

## Important caveat before you start

Installing Build Tools alone will **not** make `rp6502-emu.exe` build successfully yet.
`src/emu/win/dir.c` and `src/emu/win/fs.c` — the Win32 bodies of the portable
`dir_*`/`fs_*` seam (`src/emu/plat.h`) that MSVC needs (mingw doesn't need them; it has its
own POSIX-ish headers) — are still skeletons: every function is a `TODO(win)` comment plus
`errno = ENOSYS; return false;` (confirmed: 27 such stub markers across the two files).
Configure will succeed once a compiler is found, but the actual build will fail deep inside
`emu_core` until those Win32 bodies are implemented (each `TODO(win)` comment names the exact
Win32 API to call — `FindFirstFileW`, `GetFileAttributesExW`, `_wmkdir`, etc.). Treat this
guide as "get MSVC recognized and configuring," not "get the emulator running."

## 1. Install Visual Studio Build Tools

**Option A — winget (fastest):**
```
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

**Option B — manual installer:**
1. Download from https://visualstudio.microsoft.com/downloads/ → scroll to "Tools for Visual
   Studio" → **Build Tools for Visual Studio 2022**.
2. Run it. In the workload picker, check **"Desktop development with C++"**.
3. On the right-hand summary pane, confirm these are included (default): MSVC v143 build
   tools, Windows 11 SDK, C++ CMake tools for Windows (optional — the project uses its own
   MSYS2/CMake or a separate CMake install, not required), C++ core features.
4. Install (~2-3 GB). No reboot needed.

This installs **no IDE** — just `cl.exe`, `link.exe`, the Windows SDK, and MSBuild, plus a
**"Developer Command Prompt for VS 2022"** / **"x64 Native Tools Command Prompt for VS
2022"** shortcut in the Start Menu (added by the installer).

## 2. Verify the install

Open **"x64 Native Tools Command Prompt for VS 2022"** from the Start Menu and run:
```
cl
```
You should see `Microsoft (R) C/C++ Optimizing Compiler Version ...` (not "command not
found"). This confirms `cl.exe`/`link.exe`/the Windows SDK are on `PATH` **inside this
specific shell only** — a plain `cmd.exe`/PowerShell/VS Code terminal opened normally will
*not* have them, which is the crux of step 3.

## 3. Launch VS Code with the MSVC environment loaded

`cl.exe` isn't self-contained like `gcc.exe` — it also needs `INCLUDE`/`LIB`/`LIBPATH`
environment variables set (for the CRT and Windows SDK headers/libs), which only the
Developer Command Prompt's `vcvarsall.bat` sets up. The simplest way to get this into VS
Code, **matching what `README.md` already recommends**, is to launch VS Code *from* that
prompt rather than from the Start Menu/taskbar:

1. Open **"x64 Native Tools Command Prompt for VS 2022"**.
2. `cd` to the repo root:
   ```
   cd /d C:\@prg\@picocomputer\@firmware\rp6502
   ```
3. Launch VS Code from inside it:
   ```
   code .
   ```

VS Code (and everything it spawns — CMake Tools, integrated terminals, tasks, the debugger)
inherits that environment for the lifetime of this window. **You need to repeat this every
time you want to work on the MSVC build** — a normally-launched VS Code window won't have
`cl.exe` on `PATH`.

(A shortcut to make this less tedious: create a `.bat` file with those two commands and pin
it to the Start Menu/taskbar instead of VS Code's own icon, for whenever you want the MSVC
session specifically. Keep using the normal VS Code icon — or MSYS2 — for the MinGW route.)

## 4. Configure the "emu" folder

With VS Code launched per step 3, no CMakeUserPresets.json or kit selection should be
needed — `cmake -S src/emu -B build/emulator/debug -G Ninja` (what the `debug`/`release`
presets already do) will find `cl.exe` on `PATH` automatically, same as any other
Ninja+MSVC setup. In the CMake side panel: Folder **emu** → Configure Preset → **debug** or
**release** (the plain, existing presets — no MSYS2-specific override needed for this
route).

If configure instead complains it can't find a working C compiler, double check VS Code was
actually launched from the Native Tools prompt (step 3) and not from a normal shell/shortcut
— this is the most common mistake.

## 5. Debugging (once the build actually succeeds)

MSVC builds can use VS Code's native Windows debugger instead of gdb — add a
`"windows"` override (or a separate configuration) to `.vscode/launch.json` using
`"type": "cppvsdbg"` instead of `"cppdbg"`/`MIMode: gdb"`, pointing `program` at
`build/emulator/debug/rp6502-emu.exe`. `cppvsdbg` doesn't need MSYS2's gdb at all, and in
practice doesn't hit the antivirus/Windows-debug-API friction documented in
`EMULATOR_WINDOWS_MSYS2_VSCODE.md` (Microsoft's own debug engine, not a third-party
gdb.exe making raw Win32 debug API calls) — though this hasn't been verified end-to-end
since the win/dir.c-fs.c gap (see the caveat above) currently blocks getting that far.

## Coexisting with the MSYS2/UCRT64 setup

No conflict: Build Tools and MSYS2 are independent installs. Which one gets used depends
entirely on which VS Code window/terminal/preset you're working in — a normally-launched
VS Code window (or the MSYS2 shell) still resolves to MinGW/UCRT64 as before; only a VS
Code window launched per step 3 above sees `cl.exe`. `CMakeUserPresets.json`
(`win-debug`/`win-release`, MSYS2-specific) is untouched by any of this.
