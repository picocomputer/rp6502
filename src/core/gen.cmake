include_guard(GLOBAL)

# The interpreter is found rather than named python3 because on Windows it is
# python.exe, and the libretro buildbot runs the generators on Windows.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Every generator takes the command line in core/gen.py, so a new one only
# needs adding here.
set(RP6502_GENERATORS
    aud/opl.py
    aud/psg.py
    aud/rsmp.py
    hid/layout.py
    str/unicode.py
    term/color.py
    term/font.py
    wdc/cpu.py)
list(TRANSFORM RP6502_GENERATORS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)

set(RP6502_GEN_PY ${CMAKE_CURRENT_LIST_DIR}/gen.py)
get_filename_component(RP6502_GEN_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
include(${RP6502_GEN_ROOT}/submodules.cmake)

# The C files, made at build time.
set(RP6502_GEN ${CMAKE_BINARY_DIR}/gen)
# The SystemVerilog packages and the images the Pocket loads.
set(RP6502_ASSETS ${CMAKE_BINARY_DIR}/assets)

# Sets ins to the files gen reads and outs to the names of the files of
# kind it writes.
function(rp6502_gen_list gen kind ins outs)
    get_property(lines GLOBAL PROPERTY RP6502_GEN_LIST_${gen})
    if(NOT lines)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${gen} --list
            OUTPUT_VARIABLE lines RESULT_VARIABLE rc)
        if(rc)
            message(FATAL_ERROR "${gen} --list failed")
        endif()
        string(STRIP "${lines}" lines)
        string(REGEX REPLACE "\r?\n" ";" lines "${lines}")
        set_property(GLOBAL PROPERTY RP6502_GEN_LIST_${gen} "${lines}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${gen} ${RP6502_GEN_PY})
    endif()
    set(i)
    set(o)
    foreach(line IN LISTS lines)
        if(line MATCHES "^in (.+)$")
            list(APPEND i ${RP6502_GEN_ROOT}/${CMAKE_MATCH_1})
        elseif(line MATCHES "^${kind} (.+)$")
            list(APPEND o ${CMAKE_MATCH_1})
        endif()
    endforeach()
    set(${ins} ${i} PARENT_SCOPE)
    set(${outs} ${o} PARENT_SCOPE)
endfunction()

# Adds the target rp6502_gen_c, which writes the C files into RP6502_GEN.
function(rp6502_gen_c)
    if(TARGET rp6502_gen_c)
        return()
    endif()
    file(MAKE_DIRECTORY ${RP6502_GEN})
    set(all)
    foreach(gen IN LISTS RP6502_GENERATORS)
        rp6502_gen_list(${gen} c ins names)
        if(NOT names)
            continue()
        endif()
        set(outs ${names})
        list(TRANSFORM outs PREPEND ${RP6502_GEN}/)
        list(JOIN names " " text)
        add_custom_command(OUTPUT ${outs}
            COMMAND ${Python3_EXECUTABLE} ${gen} --c ${RP6502_GEN}
            DEPENDS ${gen} ${RP6502_GEN_PY} ${ins}
            COMMENT "Generating ${text}"
            VERBATIM)
        list(APPEND all ${outs})
    endforeach()
    add_custom_target(rp6502_gen_c DEPENDS ${all})
endfunction()

# Adds the target rp6502_gen_assets, which writes the SystemVerilog packages
# and the images into RP6502_ASSETS.
#
# verilate() runs Verilator over its sources when CMake configures, so a
# missing output is generated at configure time as well as by the build rule.
# The generator runs at configure time only when an output is missing,
# because each run gives its outputs new timestamps and the Pocket fit
# depends on the generated packages, so the fit would run again after every
# configure.
function(rp6502_gen_assets)
    if(TARGET rp6502_gen_assets)
        return()
    endif()
    rp6502_submodule(vendor/chips SENTINEL codegen/w65c02_gen.py
        WANTS "the w65c02 decode table generator")
    file(MAKE_DIRECTORY ${RP6502_ASSETS})
    set(all)
    foreach(gen IN LISTS RP6502_GENERATORS)
        set(args)
        set(outs)
        set(names)
        foreach(kind IN ITEMS sv bin)
            rp6502_gen_list(${gen} ${kind} ins kind_names)
            if(kind_names)
                list(APPEND args --${kind} ${RP6502_ASSETS})
                list(APPEND names ${kind_names})
            endif()
        endforeach()
        if(NOT names)
            continue()
        endif()
        set(outs ${names})
        list(TRANSFORM outs PREPEND ${RP6502_ASSETS}/)
        set(absent FALSE)
        foreach(out IN LISTS outs)
            if(NOT EXISTS ${out})
                set(absent TRUE)
            endif()
        endforeach()
        if(absent)
            execute_process(COMMAND ${Python3_EXECUTABLE} ${gen} ${args}
                RESULT_VARIABLE rc)
            if(rc)
                message(FATAL_ERROR "${gen} failed")
            endif()
        endif()
        list(JOIN names " " text)
        add_custom_command(OUTPUT ${outs}
            COMMAND ${Python3_EXECUTABLE} ${gen} ${args}
            DEPENDS ${gen} ${RP6502_GEN_PY} ${ins}
            COMMENT "Generating ${text}"
            VERBATIM)
        list(APPEND all ${outs})
    endforeach()
    add_custom_target(rp6502_gen_assets ALL DEPENDS ${all})
endfunction()
