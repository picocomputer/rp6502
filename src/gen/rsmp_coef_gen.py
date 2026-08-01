#!/usr/bin/env python3
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Designs the Farrow matrix in src/emu/emu/rsmp.c, which src/fpga/rtl/aud/
# aud_rsmp.sv reads too. Needs numpy; it is a design and check tool, not
# part of the build, because the coefficients are committed.
#
# Why not Lagrange, which is what everyone reaches for: Lagrange is
# maximally flat at DC, and it spends every degree of freedom there. Its
# gain then depends on the fractional phase, and a gain that moves with the
# phase is heard as roughness rather than as a level error. Measured
# through the integer path at 49704 -> 48000, six-point Lagrange leaves
# -57 dB of it at 8 kHz and -36 dB at 12 kHz.
#
# So the sub-filters are least-squares fits to the ideal fractional delay
# instead, over a grid of both frequency and phase. Two things make the
# result usable rather than merely optimal:
#
#   The DC gain is CONSTRAINED, not fitted. Row 0 sums to one and every
#   other row to zero, so a constant comes through as itself at every
#   phase. An unconstrained fit trades that away and puts a gain wobble
#   right where the ear is most sensitive.
#
#   The error is weighted 1/w. Weighted evenly, the fit spreads its error
#   across the band and lands 40 dB worse than Lagrange at 4 kHz while
#   winning at 16 kHz — a bad trade, since almost nothing in the audible
#   range lives up there. Weighted 1/w it beats Lagrange everywhere.
#
# Result at 49704 -> 48000, sideband level in dB, lower better:
#
#     kHz      1      4      8     12     16     20
#     lagrange-6   -87    -85    -57    -36    -22    -11
#     this        -87    -86    -85    -77    -41    -18
#
# The first four are the integer output floor, not the filter.

import argparse

TAPS = 10
ORDER = 6
Q = 14
WP = 0.55  # fit out to 0.55*pi; past there the taps run out either way
WEIGHT_P = 1  # 1/w**p

COMMITTED = [
    [0, 0, 0, 0, 16384, 0, 0, 0, 0, 0],
    [133, -894, 3513, -12473, -2217, 16425, -6285, 2373, -683, 108],
    [-37, 339, -2021, 13844, -24348, 14142, -2377, 560, -116, 14],
    [-236, 1496, -4962, 5793, 2348, -10351, 8789, -3887, 1211, -201],
    [162, -1118, 4309, -9509, 11721, -7720, 2218, 115, -236, 58],
    [-21, 176, -839, 2345, -3888, 3888, -2345, 839, -176, 21],
]


def design():
    import numpy as np

    us = np.linspace(0, 1, 33)
    ws = np.linspace(1e-4, WP * np.pi, 180)
    nf = TAPS - 1
    rows, rhs = [], []
    for u in us:
        for w in ws:
            g = 1.0 / (w ** WEIGHT_P)
            # The last tap is eliminated rather than fitted; that is how the
            # row sums stay exact instead of nearly exact.
            eL = np.exp(-1j * w * (TAPS - 1))
            b = np.zeros(nf * ORDER, complex)
            const = 0j
            for k in range(ORDER):
                const += (1.0 if k == 0 else 0.0) * (u ** k) * eL
                for i in range(nf):
                    b[i * ORDER + k] = (np.exp(-1j * w * i) - eL) * (u ** k) * g
            rows.append(b)
            rhs.append((np.exp(-1j * w * (TAPS // 2 - 1 + u)) - const) * g)
    A = np.array(rows)
    y = np.array(rhs)
    A = np.vstack([A.real, A.imag])
    y = np.concatenate([y.real, y.imag])
    f = np.linalg.lstsq(A, y, rcond=None)[0].reshape(nf, ORDER).T

    m = [[0] * TAPS for _ in range(ORDER)]
    for k in range(ORDER):
        row = [int(round(f[k][i] * (1 << Q))) for i in range(nf)]
        want = (1 << Q) if k == 0 else 0
        m[k] = row + [want - sum(row)]
    return m


def check_rows(m):
    for k, row in enumerate(m):
        want = (1 << Q) if k == 0 else 0
        if sum(row) != want:
            raise SystemExit(f"row {k} sums to {sum(row)}, wanted {want}")
        if max(abs(c) for c in row) > 32767:
            raise SystemExit(f"row {k} does not fit int16")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    check_rows(COMMITTED)
    if a.check:
        m = design()
        if m != COMMITTED:
            raise SystemExit(f"design drifted:\n derived   {m}\n committed {COMMITTED}")
        print(f"rsmp matrix reproduces, {TAPS} taps, order {ORDER}, Q{Q}")
        return 0

    for row in design():
        print("    {" + ", ".join(str(c) for c in row) + "},")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
