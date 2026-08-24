# The suite that runs on this workstation's own machine.
#
# Every host that builds rp6502-emu adds this directory, so what is here needs
# a C compiler and nothing else: no Verilator, no RISC-V toolchain, no Quartus.
# Working on C never elaborates a line of RTL.
#
# The other half is tests/rtl, a project root of its own. The two are filed by
# subsystem the same way, and where a claim can be made about both machines it
# is made twice against one set of evidence — tests/rtl/wdc compiles the
# corpora and the interface in emu/wdc against the fabric's implementation
# rather than keeping a second copy of them.

include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

add_subdirectory(aud)
add_subdirectory(vid)
add_subdirectory(ria)
add_subdirectory(wdc)
add_subdirectory(fs)
add_subdirectory(dbg)
add_subdirectory(hid)

# The emulator driven from outside, as an application project drives it.
add_subdirectory(app)
