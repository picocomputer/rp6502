# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The CMake helpers that the projects which build and test a Picocomputer
# share. Every top-level CMakeLists.txt includes it first, and a file it
# includes may include it again.
#
# The build runs on Linux, macOS and Windows hosts, so the helpers here hold
# whatever differs between them. Reach for one before writing a path, an
# object name or a Python command by hand.

include_guard(DIRECTORY)

get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

# ---- Python ---------------------------------------------------------------
#
# Every script runs as ${RP6502_PYTHON} SCRIPT ARGS..., through build.py,
# which gives it build.py, rp6502.py and the test libraries to import. The interpreter is found rather than named python3,
# which a Windows host may not have. -X utf8 reads and writes text as UTF-8
# whatever the host's code page, and -B leaves no __pycache__ in the tree.

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(RP6502_PYTHON ${Python3_EXECUTABLE} -X utf8 -B ${CMAKE_CURRENT_LIST_DIR}/build.py)
# What every script run through RP6502_PYTHON may import, for DEPENDS.
set(RP6502_PYTHON_LIBS
    ${CMAKE_CURRENT_LIST_DIR}/build.py
    ${RP6502_ROOT}/tools/rp6502.py
    ${RP6502_ROOT}/tests/lib/rp6502_asm.py
    ${RP6502_ROOT}/tests/lib/rp6502_rom.py)

# A script run by the build, after it has run once and whenever it or what it
# reads changes:
#
#   rp6502_python_step(NAME SCRIPT script [ARGS ...] [DEPENDS ...]
#                      [OUTPUT ...] [COMMENT text])
#
# adds the target NAME. A script that writes files names them in OUTPUT; one
# that only checks gets a stamp, written when it passes.
function(rp6502_python_step NAME)
    cmake_parse_arguments(S "" "SCRIPT;COMMENT" "ARGS;DEPENDS;OUTPUT" ${ARGN})
    set(touch)
    if(NOT S_OUTPUT)
        set(S_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${NAME}.stamp)
        set(touch COMMAND ${CMAKE_COMMAND} -E touch ${S_OUTPUT})
    endif()
    if(NOT S_COMMENT)
        set(S_COMMENT "Running ${NAME}")
    endif()
    add_custom_command(OUTPUT ${S_OUTPUT}
        COMMAND ${RP6502_PYTHON} ${S_SCRIPT} ${S_ARGS}
        ${touch}
        DEPENDS ${S_SCRIPT} ${RP6502_PYTHON_LIBS} ${S_DEPENDS}
        COMMENT "${S_COMMENT}"
        VERBATIM)
    add_custom_target(${NAME} DEPENDS ${S_OUTPUT})
endfunction()

# ---- objects and linker scripts -------------------------------------------
#
# A linker script places code and data by its section, and a section of its
# own comes with every function and variable (-ffunction-sections and
# -fdata-sections), named after it: .bss.font8 is font8, wherever it was
# compiled. A rule naming such a section works on every host. A rule naming
# an object file is for code that cannot be marked, such as the SDK's, and
# goes through rp6502_object_patterns.

# Firmware objects are named NAME.c.o on every host. The Pico SDK asks for
# .o, but CMake names objects .obj on a Windows host after the SDK's
# toolchain file has run, and only a rules override comes later. Their
# directories keep CMake's full layout, which rp6502_object_patterns follows,
# rather than CMake 4.2's SHORT hashes. Call before project().
macro(rp6502_portable_objects)
    set(_rules ${CMAKE_BINARY_DIR}/rp6502_rules.cmake)
    file(WRITE ${_rules}
        "foreach(lang IN ITEMS C CXX ASM)\n"
        "    set(CMAKE_\${lang}_OUTPUT_EXTENSION .o)\n"
        "endforeach()\n")
    set(CMAKE_USER_MAKE_RULES_OVERRIDE ${_rules})
    set(CMAKE_INTERMEDIATE_DIR_STRATEGY FULL CACHE STRING
        "Object directories in CMake's full layout, which linker scripts name" FORCE)
endmacro()

# Sources a linker script names, by their symbols' sections or by their
# objects, build outside LTO. LTO would compile them again into objects of its
# own, under other names, where no rule finds them. A rule that then finds
# nothing is caught by the ASSERT or the check that goes with it.
function(rp6502_linker_named TARGET)
    set_property(SOURCE ${ARGN} TARGET_DIRECTORY ${TARGET}
        APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
endfunction()

# The linker's pattern for each source's object, as rp6502_portable_objects
# names them: a source is compiled into a directory named for its path from
# the current source directory, with ".." turned into "__", and the pattern
# keeps the last directory and the name.
function(rp6502_object_patterns out)
    set(patterns "")
    foreach(src ${ARGN})
        file(RELATIVE_PATH dir ${CMAKE_CURRENT_SOURCE_DIR} ${src})
        get_filename_component(dir ${dir} DIRECTORY)
        get_filename_component(dir ${dir} NAME)
        if(dir STREQUAL "..")
            set(dir "__")
        endif()
        get_filename_component(name ${src} NAME)
        string(APPEND patterns " */${dir}/${name}.o")
    endforeach()
    set(${out} "${patterns}" PARENT_SCOPE)
endfunction()
