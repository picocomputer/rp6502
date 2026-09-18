# tests/roms

This directory holds `adventure.rp6502`, the only `.rp6502` file in the
repository. Every other `.rp6502` program that a test runs is generated from
source.

Besides the tests, `src/host/itch.io` ships `adventure.rp6502` as the sample
program in the web bundle.

## Rebuilding it

It comes from the SDK examples, built with cc65:

    git clone https://github.com/picocomputer/examples
    cd examples
    git submodule update --init src/adventure/troglobit
    cmake --preset cc65/Release
    cmake --build --preset cc65/Release
    cp build/cc65/release/src/adventure/adventure.rp6502 <here>

Before the tests use it, check that it runs:

    rp6502-emu --headless build/cc65/release/src/adventure/adventure.rp6502

It prints the banner and then asks whether you would like instructions. The
first two `wait` commands in `tests/cpu/ria/adventure.txt` match text from the
banner and that question.
