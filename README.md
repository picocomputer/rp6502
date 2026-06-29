# Rumbledethumps' Picocomputer 6502

The Picocomputer 6502 is a real 6502 computer built from a WDC 65C02, a couple
of Raspberry Pi Picos, and very little else. This repository holds everything
that runs on the Picos plus a desktop/web emulator of the whole machine.

The main documentation starts here:<br>
https://picocomputer.github.io/

Pre-built `.uf2` firmware images for Pi Pico 2 boards:<br>
https://github.com/picocomputer/rp6502/releases

## Layout

* [firmware/](firmware/) — the RP6502-RIA, RP6502-RIA-W, and RP6502-VGA
  firmware that runs on the Pico 2 boards. Built with the Pico SDK.
* [emulator/](emulator/) — a desktop and WebAssembly emulator of the machine
  (CPU + RIA + VGA), sharing the firmware's own rendering and audio code.
* [src/](src/) — shared source: the firmware (`ria/`, `vga/`) and the
  emulator (`emu/`).
* [vendor/](vendor/) — third-party dependencies. Most are git submodules;
  run `git submodule update --init` to populate them.

The firmware and the emulator are separate CMake projects with incompatible
toolchains (Pico cross-build vs. host/Emscripten), so each builds from its own
directory. Open [rp6502.code-workspace](rp6502.code-workspace) in VS Code to
get both as selectable folders, or build either from its own README.

## License

BSD 3-Clause. See [LICENSE](LICENSE); every source file carries an
`SPDX-License-Identifier`. The emulator additionally bundles third-party code
under permissive licenses — see
[emulator/THIRD-PARTY-NOTICES.md](emulator/THIRD-PARTY-NOTICES.md).
