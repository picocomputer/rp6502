# tests/roms

One program, `adventure.rp6502`, which no generator writes. Everything else a
test drives is generated from source under `tests/gen` and lands in the build
directory, where it cannot drift from the generator that describes it.

Adventure stays because it is a whole application rather than a fixture shaped
like one: a dozen tests read its console text, its ROM assets, its savestate
and its boot, and `src/host/itch.io` ships it as the sample the web bundle
plays.

## Rebuilding it

It comes from the SDK examples, built with cc65:

    git clone https://github.com/picocomputer/examples
    cd examples
    git submodule update --init src/adventure/troglobit
    cmake --preset cc65/Release
    cmake --build --preset cc65/Release
    cp build/cc65/release/src/adventure/adventure.rp6502 <here>

Then prove the copy before the suite runs on it:

    rp6502-emu --headless build/cc65/release/src/adventure/adventure.rp6502

It should print the banner and ask whether you would like instructions, which
is the first thing `tests/cpu/ria/adventure.txt` waits for.
