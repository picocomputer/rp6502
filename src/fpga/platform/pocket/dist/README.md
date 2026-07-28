# Pocket distribution tree

Everything the SD card needs except the binaries:

- `Cores/Rumbledethumps.RP6502/bitstream.rbf_r` comes from the Quartus
  build through `src/fpga/codegen/rbf_r_gen.py` (byte-wise bit reversal).
- `Cores/Rumbledethumps.RP6502/icon.bin` (36x36, 16-bit brightness in the
  high byte, rotated 90 CCW) and `Platforms/_images/rp6502.bin` (521x165)
  are produced with agg23's Analogue-Pocket-Image-Process converter; the
  platform image byte format is not publicly documented, so the converter
  is authoritative.

Zip the three top directories at the archive root as
`Rumbledethumps.RP6502_<version>_<date>.zip`.
