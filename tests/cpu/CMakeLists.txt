# The machine's own tests, whichever machine this tree builds.
#
# A test here is written once and asks about the RP6502, not about a program
# or a board. Where the claim can be made of both implementations, one source
# makes it of both: the corpora in wdc/ run against chips_dut.c or
# rtl_dut.cpp depending on the tree, and the frame suites boot the same
# generated ROMs on emu_core or on the verilated machine.
#
# Nothing here names a tree. A registration that needs the simulator guards on
# the helper that only exists where rp6502_verilate.cmake was included; one
# that needs the shipped binary guards on that target; one that needs the
# staged assets guards on the asset. So the desktop roots get what a C
# compiler can answer and the fpga root gets all of it, without either being
# told which it is.

add_subdirectory(wdc)
add_subdirectory(vga)
add_subdirectory(mem)
add_subdirectory(ria)
add_subdirectory(aud)
add_subdirectory(fs)
add_subdirectory(hid)
