# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The machine's half of the register layout version, which every FPGA
# host's gen/version.py builds its own from. It writes no file.
#
# MACHINE_VERSION covers what every FPGA host's firmware shares with the
# bitstream: src/core/sys/wiring.sv's decode, src/core/riscv/soc.sv's 0xF
# map, src/core/riscv/mem_pkg.sv, src/host/fpga/sw/mmio.h and
# src/host/fpga/sw/link.ld. Raise it when any of those changes
# in a way the firmware sees. Every host's version changes with it.

MACHINE_VERSION = 2

# Each host's id is the high byte of its version, so a firmware built for
# one host stops on another's bitstream. A new host takes its own initial.
POCKET = 0x50  # 'P'


def reg_version(host_id, host_version):
    """The value a host's bitstream shows at 0x80030000 and its firmware
    carries after its reset jump: the host id, the machine version and the
    host version, in 8, 12 and 12 bits."""
    return host_id << 24 | MACHINE_VERSION << 12 | host_version
