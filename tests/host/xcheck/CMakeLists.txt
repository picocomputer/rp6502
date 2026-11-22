# xcheck: the console's terminal built as the Pico VGA keeps it, in mode 0's
# cells with a spare row and its jobs done two rows a pass, and as the RIA
# keeps it, in text cells with none and its jobs done at once, both at the
# VGA's 32 rows with the alternate screen (machine.h). xcheck.py feeds both
# the same stream and holds the text they keep against each other.
foreach(_kind text mode0)
    set(_target xcheck_${_kind})
    add_executable(${_target} xcheck.c
        ${RP6502_SRC}/core/term/term.c ${RP6502_SRC}/core/term/grid.c
        ${RP6502_SRC}/core/term/screen.c ${RP6502_SRC}/core/term/vterm.c
        ${RP6502_SRC}/core/term/color.c ${RP6502_SRC}/core/term/cell_${_kind}.c
        ${RP6502_SRC}/core/sys/crc32.c ${RP6502_BENCH}/tb_version.c)
    if(COMMAND rp6502_msvc_dialect)
        rp6502_msvc_dialect(${_target})
    endif()
    target_include_directories(${_target} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR} ${RP6502_SRC} ${RP6502_GEN})
    if(_kind STREQUAL text)
        target_compile_definitions(${_target} PRIVATE TERM_CELL_TEXT=1 TERM_SPARE_ROWS=0)
    else()
        target_compile_definitions(${_target} PRIVATE TERM_JOB_ROWS=2)
    endif()
    rp6502_log_definitions(${_target} PRIVATE)
endforeach()

# xcheck_test(<name> <arg>...): xcheck.py runs both builds with the args.
function(xcheck_test name)
    add_test(NAME xcheck_${name}
        COMMAND ${RP6502_PYTHON} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/xcheck.py
            $<TARGET_FILE:xcheck_text> $<TARGET_FILE:xcheck_mode0> ${ARGN})
    set_tests_properties(xcheck_${name} PROPERTIES
        LABELS "c;host.xcheck" SKIP_RETURN_CODE 77)
endfunction()

# Seeded streams weighted to what moves cells about: the margins, the
# shifts, the inserts and deletes, the rectangles, the guards and the
# selective erases, REP, the alternate screen, smooth scroll, line sizes
# and changes of height.
foreach(_seed 1 2 3 4)
    xcheck_test(fuzz_${_seed} fuzz ${_seed} 50000)
endforeach()
