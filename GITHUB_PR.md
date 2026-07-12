## Windows emulator support (MSYS2/UCRT64)

Ports `src/emu` to build and run natively on Windows via MSYS2's UCRT64 GCC toolchain
(previously unsupported — README said "nobody is working on the emulator for Windows").

- Close mingw's POSIX gaps (no `d_type`, `<sys/statvfs.h>`, `pread`, POSIX locale API,
  1-arg `mkdir`) in `posix/dir.c`, `posix/fs.c`, `api/clk.c`, `api/pro.c`, `host/msc.c`,
  `mon/rom.c`; add `fs_realpath()` to the `plat.h` seam.
- Statically link the MinGW runtime so `rp6502-emu.exe` runs standalone (no MSYS2 needed at
  runtime).
- Fix Windows gaps in the test suite (`mkdir`/`realpath`/`setenv`, no `/tmp`).
- Fix Windows F5 debugging (`launch.json`).

All 20 `ctest` suites pass; D3D11 window renders. Setup/troubleshooting:
`EMULATOR_WINDOWS_MSYS2_VSCODE.md`.
