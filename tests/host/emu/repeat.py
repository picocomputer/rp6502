#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
"""Runs with one seed answer cpu the same, though the program reads files as it
starts, because with no window a file transfer completes before the call
returns."""

import rp6502
from rp6502_rom import ADVENTURE, main


def test_repeat():
    answers = []
    # The answer to cpu ends with the seed, so the three runs use one seed.
    for _ in range(3):
        with rp6502.Emu("--seed", "1", ADVENTURE) as e:
            e.cmd("run 20")
            answers.append(e.cmd("cpu"))
    assert len(set(answers)) == 1, f"the runs differ: {answers}"


TESTS = {"repeat": test_repeat}
main(globals())
