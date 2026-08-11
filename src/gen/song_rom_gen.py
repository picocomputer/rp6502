#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# A .rp6502 that claims the OPL, programs one voice into its page and
# then does nothing at all, forever.
#
# It exists for the half of a restore that no register comparison can
# reach. The blob carries XRAM, so the device's page comes back by
# itself; what does not come back is the chip, whose register file is
# write-only fabric. aud_restore's answer is to write the page over
# itself so the chip hears it again -- and aud_opl.sv's sequencer takes
# a write only while it is idle, spends four clocks on each one, and
# drops in silence anything that arrives between. A replay that runs
# ahead of it therefore restores a song to a chip that heard a tenth of
# it, which sounds like a few notes and then nothing.
#
# Sleeping on a programmed page is what puts a bench in front of that.
# The loop at the end is the point: the machine has to be alive and
# holding the page when the freeze lands.

import argparse

from rp6502_rom import Asm, image

PAGE = 0xFE00
READY = b"song ok\r\n"

# Channel 0's modulator is slot 0 and its carrier slot 3 -- the same
# voice tests/aud/test_opl.cpp sounds, so a page that fails to come back
# fails against registers whose effect is already known.
REGS = (
    (0x20, 0x01),  # modulator: mult 1
    (0x23, 0x01),  # carrier:   mult 1
    (0x40, 0x10),  # modulator: a little attenuated
    (0x43, 0x00),  # carrier:   full volume
    (0x60, 0xF0),  # fast attack, no decay
    (0x63, 0xF0),
    (0x80, 0x77),  # sustain high, slow release
    (0x83, 0x77),
    (0xC0, 0x0E),  # feedback, both operators out
    (0xA0, 0x98),  # f-number low
    (0xB0, 0x31),  # key on, block 4, f-number high
)


def prog():
    p = Asm()

    # Device 0, channel 1, address 1 is the OPL's page pointer.
    p.xreg(0, 1, 1, PAGE)
    for reg, val in REGS:
        p.poke(PAGE | reg, val)

    for c in READY:
        p.lda(c)
        p.putc_a()

    # Alive and holding the page. Not stp(): a stopped machine has no
    # session to save and the sleep under test is a running one's.
    here = p.here()
    p.jmp(here)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--print-ready", action="store_true")
    a = ap.parse_args()
    if a.print_ready:
        print(READY.decode(), end="")
    if a.emit:
        print(f"song.rp6502 {image(prog()).write(a.emit)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
