# The soft CPU: the Hazard3 SoC the firmware runs on.
#
# There is no emulator counterpart — this machine is the only one with a soft
# CPU — so, like mem, the whole subsystem lives in this tree.

# Its microsecond clock against clk_sys wall time, which has now been
# wrong twice.
rp6502_add_machine_test(mtime TIMEOUT 120)

# A load of the word a store just wrote, which the AHB overlaps onto
# one edge and the block RAM answers stale.
rp6502_add_machine_test(tcm TIMEOUT 120)

# Halt, take everything, put it in a core that has just come out of
# reset, and carry on: the soft CPU resuming rather than booting.
rp6502_add_machine_test(resume TIMEOUT 300)

# The engine reading the stopped machine out, against the same bytes
# fetched straight from the model.
rp6502_add_machine_test(sst TIMEOUT 600)
