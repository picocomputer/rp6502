# The console on the board it was written for. A desktop tree adds this
# directory only when RP6502_DEVICE names the RIA's console port; RP6502_TELNET
# (host, or host:port) and RP6502_KEY add the second console. CI has no board
# and never sets them.
#
#   cmake -DRP6502_DEVICE=/dev/ttyACM0 -DRP6502_TELNET=192.168.1.89 -DRP6502_KEY=word build/sokol
#   ctest --test-dir build/sokol -R pico

set(_args --device ${RP6502_DEVICE})
if(RP6502_TELNET)
    list(APPEND _args --telnet ${RP6502_TELNET})
endif()
if(RP6502_KEY)
    list(APPEND _args --key ${RP6502_KEY})
endif()
add_test(NAME pico.console
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_LIST_DIR}/console.py ${_args})
set_tests_properties(pico.console PROPERTIES TIMEOUT 600 LABELS "pico")
