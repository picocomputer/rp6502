# Global volume and balance — plan

Status (2026-11-21): in progress on branch `feature/volume`.

## Behaviour
- Volume in dB, 0 to -63 in 1 dB steps; -64 is silence. Balance -63 to +63:
  +n takes n dB off the left channel, -n off the right; 0 leaves both whole.
- User: `SET VOL (0..-64)` and `SET BAL (-63..63)` in the monitor, saved in the
  config file.
- Program: XREG device 0, channel 1, index 2 = volume (int16, 0..-64), index 3 =
  balance (int16, -63..63). Both return to 0 when the program stops. Saved in
  savestates.
- User and program values add in dB (the program can only go quieter than the
  user's setting); the sums saturate at -64 and +/-63.
- The bell (BEL, 0x07) follows only the user's VOL and BAL, never the program's.
- Keyboard: Volume Up/Down step the user's VOL by 3 dB, Mute toggles silence.
  The file is written about 2 s after the last key, to spare the flash. The
  program still sees the keys.

## Mechanics
- `src/core/aud/vol.c`: table of 65 Q15 gains (round(32768 * 10^(-dB/20)), 0 at
  -64 dB), the config rows, the xreg handlers, the keys, savestate rows. It
  computes four gains — engines L/R and bell L/R — and hands them to the
  machine's mixer through `aud_set_gain`.
- The PSG returns the bell separately from its eight voices (`psg.c` and
  `psg.sv` in step), so a mixer can scale the two apart.
- Mixers apply the gains to the unclamped sum before the 16-bit clamp; unity
  gain is bit-exact with the mix as it was.
  - Pico: `aud_irq` in `src/host/pico/ria/aud/mix.c`.
  - Software machines: `aud_mix` in `src/core/aud/mix.c`.
  - Pocket: `wiring.sv` mix, gains from two new registers in the audio window
    (0x38 engines, 0x3C bell; `{right, left}` Q15), written by `sw/aud.c`.

## Verification
- Emulator ctest (`.claude/scripts/emu_test.bat`), new unit tests for the gain
  table, the sums and the bell's exemption.
- Firmware build `cmake --build build`.
- RTL tests need Verilator, which is not installed on this machine: the
  `psg.sv`/`wiring.sv` changes rely on upstream CI.
- Docs: `ria.rst` (XREG), monitor SET help, in picocomputer.github.io.
