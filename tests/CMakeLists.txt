# One entry point for both build trees.
#
# The directories below are the machine's subsystems, not its hosts: aud, vid,
# mem, ria, rv and cpu65 are the same names src/rtl uses. Each one holds
# whatever evidence exists for its subsystem — the C suites, the RTL suites,
# and where they can be, the same suite against both — because a reader
# looking for "how is the PSG proven" should find it all in one place rather
# than half of it under an emulator directory and half under an FPGA one.
#
# Platform-specific tests are the exception and live under host/, since a
# Pocket bridge FIFO genuinely is about the host and not about the machine.
#
# src/emu adds this with nothing set, and gets the C suites.
# src/rtl adds it with RP6502_TESTS_RTL, and gets those plus the RTL. It adds
# it only when RP6502_RTL_SIM is on; a Pocket bitstream needs the RTL source
# list, which lives in src/rtl/machine.cmake, and none of what is here.

include(${CMAKE_CURRENT_LIST_DIR}/rp6502_tests.cmake)

if(RP6502_TESTS_RTL)
    include(${CMAKE_CURRENT_LIST_DIR}/rp6502_verilate.cmake)
    add_subdirectory(bench)
endif()

add_subdirectory(cpu65)
add_subdirectory(aud)
add_subdirectory(vid)
add_subdirectory(mem)
add_subdirectory(ria)
add_subdirectory(rv)
add_subdirectory(fs)
add_subdirectory(dbg)
add_subdirectory(hid)

if(RP6502_VERILATE)
    add_subdirectory(host/pocket)
endif()
