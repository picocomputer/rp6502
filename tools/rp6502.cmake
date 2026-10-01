#[=======================================================================[.rst:
rp6502
------

The CMake tools of a Picocomputer 6502 project. ``CMakeLists.txt``
includes this file before ``project()``:

.. code-block:: cmake

  include(${CMAKE_CURRENT_LIST_DIR}/tools/rp6502.cmake)
  project(hello C ASM)

The include loads the compiler for the variable that the configure preset
sets: cc65 for ``CC65_TARGET_SYSTEM``, llvm-mos for ``LLVM_MOS_PLATFORM``,
or none for ``RP6502_BASIC``, a project of BASIC programs only. The include
also adds BASIC as a language for ``project()``, fetches the emulator when
``tools/`` lacks it, and sets ``RP6502_TOOLS_DIR`` to the ``tools/``
folder.

Commands
^^^^^^^^

* :command:`rp6502_map` reads asset addresses from a C header.
* :command:`rp6502_asset` adds a file to a ROM.
* :command:`rp6502_executable` packages the linker output as a ROM.
* :command:`rp6502_basic` packages BASIC programs as a ROM.
* :command:`rp6502_web` packages a ROM as a web page.
* :command:`rp6502_byproducts` declares the extra files that a link writes.

Functions named ``_rp6502_*`` are internal.

Update
^^^^^^

``cmake -P tools/rp6502.cmake`` replaces each file that ``tools/SHA256SUMS``
of ``RP6502_TOOLS_REPO`` lists, after a check of the hash, then fetches the
emulator, and adds an update task and a web launch entry to
``.vscode/tasks.json`` and ``.vscode/launch.json`` when those files exist.
Commit the tools; the first configure of a clone fetches the emulator.
When the release has no emulator for the host,
``tools/rp6502-emu.unsupported`` holds the reason until the next update.
#]=======================================================================]

cmake_minimum_required(VERSION 3.21)

set(RP6502_TOOLS_REPO "picocomputer/rp6502")
set(RP6502_TOOLS_REF "main")
set(RP6502_EMU_RELEASE "latest")
set(RP6502_BASIC_REPO "picocomputer/msbasic")

set(RP6502_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "RP6502 tools directory")
get_filename_component(RP6502_PROJECT_DIR "${RP6502_TOOLS_DIR}" DIRECTORY)

# The folder of cc65-config.cmake, for find_package(cc65).
set(cc65_DIR "${RP6502_TOOLS_DIR}")

#[=======================================================================[.rst:
.. command:: rp6502_map

  Reads asset addresses from a C header.

  .. code-block:: cmake

    rp6502_map(<target> <header> <regex> [<unaligned_regex>])

  Reads each ``#define`` of ``<header>`` whose whole name matches
  ``<regex>``, and records the name on ``<target>`` with the value that the
  compiler of the project computes, for ``RAM()`` and ``XRAM()`` in
  :command:`rp6502_asset` calls. A define with no value, a string or
  character constant, a function-like macro, and a define that the
  preprocessor skips are left out. A target can read several headers, one
  call each, but two of the headers cannot define the same name. Call this
  after ``add_executable(<target>)`` and before the assets that use the
  names.

  The build of ``<target>`` compiles a check of the header before the ROM.
  The check fails for a value that does not fit in 16 bits, for a structure
  in an ``offsetof()`` that is larger than 64K, or for an odd value unless
  the whole name matches ``<unaligned_regex>``. The compiler reports each
  failure at the line of the define. With a header that does not compile,
  the project still configures, and the build reports the error. A change
  to the header, or to a header that it includes, configures the project
  again.
#]=======================================================================]
function(rp6502_map target)
    # The arguments after <target> are counted rather than named, so a wrong
    # count gets the usage message and not a CMake error.
    if (ARGC LESS 3 OR ARGC GREATER 4)
        message(FATAL_ERROR
            "rp6502_map(<target> <header> <regex> [<unaligned_regex>])")
    endif()
    if (NOT TARGET ${target})
        message(FATAL_ERROR
            "rp6502_map(${target} ...): ${target} is not a target. "
            "Call rp6502_map() after add_executable(${target}).")
    endif()
    set(header "${ARGV1}")
    set(regex "${ARGV2}")
    set(unaligned)
    if (ARGC EQUAL 4)
        set(unaligned "${ARGV3}")
    endif()
    get_target_property(executable_called ${target} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_map(${target} ...) must be registered BEFORE calling rp6502_executable()."
        )
    endif()
    get_filename_component(header_file "${header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(header_name "${header_file}" NAME)
    get_filename_component(header_dir "${header_file}" DIRECTORY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header_file}")

    # The header is read a line at a time, because file(STRINGS) joins a line
    # ending in a backslash to the next one, which shifts the line numbers.
    # Comments and #if are left to the preprocessor, since each name in the
    # stub is under an #ifdef.
    file(READ "${header_file}" rest)
    string(REPLACE "\r\n" "\n" rest "${rest}")
    set(names)
    set(lines)
    set(columns)
    set(values)
    set(lineno 0)
    set(pending "")
    set(pending_line 0)
    while(TRUE)
        string(FIND "${rest}" "\n" pos)
        if (pos LESS 0)
            set(line "${rest}")
            set(rest "")
            set(last TRUE)
        else()
            string(SUBSTRING "${rest}" 0 ${pos} line)
            math(EXPR pos "${pos}+1")
            string(SUBSTRING "${rest}" ${pos} -1 rest)
            set(last FALSE)
        endif()
        math(EXPR lineno "${lineno}+1")

        # A continued definition is read under the number of the first line.
        if (pending STREQUAL "")
            set(cur "${line}")
            set(cur_line ${lineno})
        else()
            set(cur "${pending}${line}")
            set(cur_line ${pending_line})
        endif()
        if (cur MATCHES "\\\\$")
            string(REGEX REPLACE "\\\\$" " " pending "${cur}")
            set(pending_line ${cur_line})
        else()
            set(pending "")
            if (cur MATCHES "^([ \t]*#[ \t]*define[ \t]+)([A-Za-z_][A-Za-z0-9_]*)([^A-Za-z0-9_(].*)$")
                string(LENGTH "${CMAKE_MATCH_1}" column)
                set(name "${CMAKE_MATCH_2}")
                string(STRIP "${CMAKE_MATCH_3}" value)
                # An include guard has no value, and a string is no address.
                if (NOT value STREQUAL "" AND NOT value MATCHES "^[\"']"
                        AND name MATCHES "^(${regex})$")
                    list(APPEND names "${name}")
                    list(APPEND lines "${cur_line}")
                    list(APPEND columns "${column}")
                    list(APPEND values "${value}")
                endif()
            endif()
        endif()
        if (last)
            break()
        endif()
    endwhile()

    # Each structure that an offsetof names gets one size probe.
    set(probes)
    set(probe_lines)
    set(probe_guards)
    foreach(name value line IN ZIP_LISTS names values lines)
        if (value MATCHES "offsetof[ \t]*\\(([^,]+),")
            string(STRIP "${CMAKE_MATCH_1}" type)
            list(FIND probes "${type}" index)
            if (index LESS 0)
                list(APPEND probes "${type}")
                list(APPEND probe_lines "${line}")
                list(APPEND probe_guards "defined(${name})")
            else()
                list(GET probe_guards ${index} guard)
                list(REMOVE_AT probe_guards ${index})
                list(INSERT probe_guards ${index} "${guard} || defined(${name})")
            endif()
        endif()
    endforeach()

    # Each target and header path gets a folder, since file names can repeat.
    file(RELATIVE_PATH id "${CMAKE_SOURCE_DIR}" "${header_file}")
    if (id MATCHES "^\\.\\.")
        # A name from the whole path can pass the 260-character Windows limit.
        get_filename_component(stem "${header_file}" NAME_WE)
        string(SHA1 hash "${header_file}")
        string(SUBSTRING "${hash}" 0 8 hash)
        set(id "${stem}_${hash}")
    endif()
    string(MAKE_C_IDENTIFIER "${id}" id)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.map/${id}")
    set(stamp "${dir}/map_check.stamp")
    get_property(checks TARGET ${target} PROPERTY RP6502_MAP_CHECKS)
    if (stamp IN_LIST checks)
        message(FATAL_ERROR
            "rp6502_map(${target} ${header}): ${header} is already mapped for ${target}.")
    endif()
    string(REPLACE "\\" "/" header_c "${header_file}")

    # The compiler writes each value of the stub as a constant in assembly,
    # with the structure layout of the program. The constants are unsigned
    # long, because size_t is 16 bits with both compilers and would truncate
    # a value. The constants are numbered and the header is found through -I,
    # because cc65 cuts identifiers at 64 characters and cannot open a quoted
    # include with an absolute path.
    set(prelude "#include <stddef.h>\n#include \"${header_name}\"\n")
    set(stub "${prelude}")
    set(index 0)
    foreach(name IN LISTS names)
        string(APPEND stub
            "\n#ifdef ${name}\n"
            "const unsigned long _map_${index} = (unsigned long)(${name});\n"
            "#endif\n")
        math(EXPR index "${index} + 1")
    endforeach()

    # The check is compiled and never run. Each condition is on a line that
    # #line numbers as the define, because cc65 reports the line where a
    # declaration ends, and is indented to the column of the name for a
    # compiler that reports columns. Every name has an assertion, so a define
    # that does not compile also fails.
    set(check "${prelude}\n")
    string(APPEND check
        "#define _map_fits(x) ((unsigned long)(x) < 0x10000UL)\n"
        "#define _map_even(x) (!((x) & 1))\n")
    string(APPEND check
        "/* size_t is 16 bits, so a structure larger than 64K wraps instead of\n"
        "   being refused. An array of one is refused outright, because the\n"
        "   size of an array is checked against the largest object the target\n"
        "   allows and not against the truncated sizeof. */\n")
    # C reserves a leading underscore at file scope, so no name collides.
    foreach(type line guard IN ZIP_LISTS probes probe_lines probe_guards)
        string(MAKE_C_IDENTIFIER "${type}" probe)
        string(APPEND check
            "\n#if ${guard}\n#line ${line} \"${header_c}\"\n"
            "extern ${type} _map_fits_${probe}[1];\n#endif\n")
    endforeach()
    # An offset always fits, but a large or a negative constant does not.
    foreach(name line column IN ZIP_LISTS names lines columns)
        string(REPEAT " " ${column} indent)
        string(APPEND check
            "\n#ifdef ${name}\n_Static_assert(\n#line ${line} \"${header_c}\"\n"
            "${indent}_map_fits(${name}), \"${name} does not fit in 16 bits.\");\n")
        if (NOT unaligned OR NOT name MATCHES "^(${unaligned})$")
            string(APPEND check
                "_Static_assert(\n#line ${line} \"${header_c}\"\n"
                "${indent}_map_even(${name}), \"${name} is unaligned."
                " To allow, use the [<unaligned_regex>] in rp6502_map.\");\n")
        endif()
        string(APPEND check "#endif\n")
    endforeach()
    # The files are rewritten only on a change, so a configure alone does not
    # relink. file(CONFIGURE) would expand an @name@ in the header path.
    foreach(part stub check)
        file(WRITE "${dir}/map_${part}.new" "${${part}}")
        file(COPY_FILE "${dir}/map_${part}.new" "${dir}/map_${part}.c" ONLY_IF_DIFFERENT)
    endforeach()

    # The check builds through the cc65 wrapper for IDE diagnostics. cl65
    # compiles the stub, as CMake 3.21 takes a -S after -P as a CMake option.
    set(compiler_args)
    set(asm_flags -S)
    if (CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(compiler_args -P "${RP6502_TOOLS_DIR}/cc65-toolchain.cmake" -- "${CC65_C_COMPILER}")
        set(stub_compiler "${CC65_C_COMPILER}")
    else()
        if (CMAKE_C_COMPILER_ARG1)
            separate_arguments(compiler_args NATIVE_COMMAND "${CMAKE_C_COMPILER_ARG1}")
        endif()
        set(stub_compiler "${CMAKE_C_COMPILER}" ${compiler_args})
        # llvm-mos defaults to LTO, where -S writes LLVM IR, not assembly.
        list(PREPEND asm_flags -fno-lto)
    endif()
    separate_arguments(flags NATIVE_COMMAND "${CMAKE_C_FLAGS}")
    # clang does not escape spaces in -MT, so the target is a plain name.
    separate_arguments(dep_flags NATIVE_COMMAND "${CMAKE_DEPFILE_FLAGS_C}")
    string(REPLACE "<DEP_TARGET>" "map_stub" dep_flags "${dep_flags}")
    string(REPLACE "<DEP_FILE>" "${dir}/map_stub.d" dep_flags "${dep_flags}")

    set(failed FALSE)
    execute_process(
        COMMAND ${stub_compiler} ${flags} -I "${header_dir}"
                ${dep_flags} ${asm_flags} -o "${dir}/map_stub.s" "${dir}/map_stub.c"
        WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    if (NOT result EQUAL 0)
        set(failed TRUE)
    endif()

    # Every header that the stub includes is a configure dependency. A failed
    # cc65 compile keeps the old list, which can name a deleted header.
    set(headers)
    if (EXISTS "${dir}/map_stub.d")
        file(READ "${dir}/map_stub.d" deps)
        # The list is Make syntax, where a name ending in a colon is a target.
        string(REGEX REPLACE "\\\\\r?\n" " " deps "${deps}")
        string(REGEX REPLACE "([^\\\\])[ \t\r\n]+" "\\1;" deps "${deps}")
        foreach(dep IN LISTS deps)
            if (dep STREQUAL "" OR dep MATCHES ":$")
                continue()
            endif()
            string(REPLACE "\\ " " " dep "${dep}")
            string(REPLACE "\\#" "#" dep "${dep}")
            string(REPLACE "$$" "$" dep "${dep}")
            cmake_path(ABSOLUTE_PATH dep BASE_DIRECTORY "${dir}" NORMALIZE)
            if (EXISTS "${dep}")
                list(APPEND headers "${dep}")
            endif()
        endforeach()
    endif()
    # The old list lacks newer includes, so a failed compile also adds the
    # file of each file:line: error.
    if (failed)
        string(REGEX MATCHALL "\n([A-Za-z]:)?[^: \n][^:\n]*:[0-9]+:" named "\n${output}")
        foreach(file IN LISTS named)
            string(REGEX REPLACE "^\n(.*):[0-9]+:$" "\\1" file "${file}")
            cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY "${dir}" NORMALIZE)
            if (EXISTS "${file}")
                list(APPEND headers "${file}")
            endif()
        endforeach()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${headers})

    # A failed read records every name as 0xFFFFFFFF, which fails the build of
    # any asset that uses it.
    set(asm)
    if (NOT failed)
        file(READ "${dir}/map_stub.s" asm)
        string(REPLACE "\r" "" asm "${asm}")
    endif()
    set(index -1)
    foreach(name IN LISTS names)
        math(EXPR index "${index} + 1")
        set(found "")
        if (failed)
            set(found 0xFFFFFFFF)
        elseif (asm MATCHES "(^|\n)_?_map_${index}:")
            # cc65 writes .dword $<hex> and llvm-mos writes .long <decimal>.
            # Anything else, or 16 bits or more, stays unread.
            set(found 0xFFFFFFFF)
            if (asm MATCHES "(^|\n)_?_map_${index}:[ \t]*\n[ \t]*\\.(dword|long)[ \t]+(\\$[0-9A-Fa-f]+|[0-9]+)")
                string(REPLACE "$" "0x" value "${CMAKE_MATCH_3}")
                math(EXPR value "${value}")
                if (value LESS 65536)
                    math(EXPR found "${value}" OUTPUT_FORMAT HEXADECIMAL)
                endif()
            endif()
        endif()
        # The preprocessor skipped this define.
        if (found STREQUAL "")
            continue()
        endif()
        # A name can repeat under #if within a header, but not across headers.
        # A failed read records no origins, since a name can be in a false #if.
        if (NOT failed)
            get_target_property(from ${target} RP6502_MAP_FROM_${name})
            if (NOT from MATCHES "-NOTFOUND$" AND NOT from STREQUAL header_file)
                file(RELATIVE_PATH from "${CMAKE_CURRENT_SOURCE_DIR}" "${from}")
                message(FATAL_ERROR
                    "rp6502_map(${target} ${header}): ${name} is already defined by "
                    "rp6502_map(${target} ${from}).")
            endif()
            set_property(TARGET ${target} PROPERTY RP6502_MAP_FROM_${name} "${header_file}")
        endif()
        set_property(TARGET ${target} PROPERTY RP6502_MAP_NAME_${name} "${found}")
    endforeach()
    if (failed)
        message(STATUS "rp6502_map(${target} ${header}) read no addresses; the build reports why.")
        # Without the stamp, the check runs and reports the cause even when
        # no dependency is newer.
        file(REMOVE "${stamp}")
    endif()

    # The check is a source and a link input of the target, so a failed
    # check stops the ROM, and an IDE lists no extra target.
    add_custom_command(
        OUTPUT "${stamp}"
        DEPENDS "${header_file}" "${dir}/map_check.c" ${headers}
        COMMAND "${CMAKE_C_COMPILER}" ${compiler_args} ${flags} -I "${header_dir}"
                -c -o "${dir}/map_check.o" "${dir}/map_check.c"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        COMMENT "Checking ${header_name}"
        VERBATIM
    )
    set_source_files_properties("${stamp}" PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(${target} PRIVATE "${stamp}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${stamp}")
    set_property(TARGET ${target} APPEND PROPERTY RP6502_MAP_CHECKS "${stamp}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_asset

  Adds ``<file>`` to the ROM of ``<target>``.

  .. code-block:: cmake

    rp6502_asset(<target> <address> <file>)
    rp6502_asset(<target> RAM(<x>) <file>)
    rp6502_asset(<target> XRAM(<x>) <file>)

  A numeric ``<address>``, decimal or hex that starts with ``0x`` or ``$``,
  loads ``<file>`` into RAM ($0-$FFFF) or XRAM ($10000-$1FFFF) when the ROM
  loads. Any other ``<address>`` is a file name in the ROM, which a program
  opens as ``ROM:<address>``. A relative ``<file>`` is a path from the
  current source folder. A generated file, such as the output of a custom
  command or of :command:`rp6502_byproducts`, is named by the full path, as
  in ``${CMAKE_CURRENT_BINARY_DIR}/logo.bin``. Call this before
  :command:`rp6502_executable` or :command:`rp6502_basic`.

  ``RAM(<x>)`` and ``XRAM(<x>)`` check that the address is in range, and
  ``XRAM()`` sets bit 16, so an offset from :command:`rp6502_map` loads
  into XRAM. ``<x>`` is a number in the same forms, a name that
  :command:`rp6502_map` read for ``<target>``, or the name of a CMake
  variable. A name that :command:`rp6502_map` could not read fails the
  build.
#]=======================================================================]
function(rp6502_asset target)
    get_target_property(executable_called ${target} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_asset(${target} ...) must be registered BEFORE calling rp6502_executable() or rp6502_basic()."
        )
    endif()
    # CMake passes RAM(<x>) as the four arguments RAM, (, <x> and ).
    set(args ${ARGN})
    list(LENGTH args argc)
    list(GET args 0 addr)
    set(unread FALSE)
    if (addr STREQUAL "RAM" OR addr STREQUAL "XRAM")
        set(form "${addr}")
        if (NOT argc EQUAL 5)
            message(FATAL_ERROR "rp6502_asset(${target} ${form}(<address>) <in_file>)")
        endif()
        list(GET args 1 opened)
        list(GET args 3 closed)
        if (NOT opened STREQUAL "(" OR NOT closed STREQUAL ")")
            message(FATAL_ERROR "rp6502_asset(${target} ${form}(<address>) <in_file>)")
        endif()
        list(GET args 2 value)
        set(token "${value}")
        get_target_property(read ${target} RP6502_MAP_NAME_${value})
        if (NOT read MATCHES "-NOTFOUND$")
            set(value "${read}")
        elseif (DEFINED ${value})
            set(value "${${value}}")
        endif()
        set(written "${value}")
        # rp6502_map() records 0xFFFFFFFF for a name it could not read.
        if (value STREQUAL "0xFFFFFFFF")
            set(unread TRUE)
            set(value 0)
        endif()
        # A leading $ is 6502 hex, which rp6502.py also accepts.
        string(REGEX REPLACE "^\\$" "0x" value "${value}")
        if (NOT value MATCHES "^[-+]?(0[xX][0-9a-fA-F]+|[0-9]+)$")
            message(FATAL_ERROR
                "rp6502_asset(${target} ${form}(...)): ${written} is not a number,"
                " or a name from rp6502_map(${target} ...).")
        endif()
        if (form STREQUAL "RAM")
            set(limit 65535)
            set(ends "0xFFFF")
        else()
            set(limit 131071)
            set(ends "0x1FFFF")
        endif()
        math(EXPR value "(${value})")
        if (value LESS 0 OR value GREATER ${limit})
            message(FATAL_ERROR
                "rp6502_asset(${target} ${form}(...)): ${written} is outside ${form}, which ends at ${ends}.")
        endif()
        if (form STREQUAL "XRAM")
            math(EXPR value "${value} | 0x10000")
        endif()
        math(EXPR addr "${value}" OUTPUT_FORMAT HEXADECIMAL)
        if (unread)
            # Two unread addresses must not share an output path.
            set(addr "${form}_${token}")
        endif()
        list(GET args 4 in_file)
    else()
        if (NOT argc EQUAL 2)
            message(FATAL_ERROR "rp6502_asset(<target> <address> <file>)")
        endif()
        list(GET args 1 in_file)
    endif()
    get_filename_component(src_file "${in_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH rel_path "${CMAKE_SOURCE_DIR}" "${src_file}")
    if (rel_path MATCHES "^\\.\\.")
        get_filename_component(rel_path "${src_file}" NAME)
    endif()
    string(MAKE_C_IDENTIFIER "${addr}" key)
    set(out_file "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.rp6502/${key}/${rel_path}")
    get_filename_component(out_dir "${out_file}" DIRECTORY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(create
        COMMAND "${Python3_EXECUTABLE}"
                "${RP6502_TOOLS_DIR}/rp6502.py"
                -a "${addr}"
                -o "${out_file}"
                create "${src_file}")
    set(checks)
    # The command runs after the map checks, which report the header error.
    if (unread)
        get_property(checks TARGET ${target} PROPERTY RP6502_MAP_CHECKS)
        set(create
            COMMAND ${CMAKE_COMMAND} -E echo
                "rp6502_asset(${target} ${form}(${token})): rp6502_map() did not read this address when CMake last configured. Configure again."
            COMMAND ${CMAKE_COMMAND} -E false)
    endif()
    set(create COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}" ${create})
    add_custom_command(
        OUTPUT "${out_file}"
        DEPENDS "${src_file}" ${checks}
        ${create}
        VERBATIM
    )
    # The asset is never compiled as a source, whatever the file name.
    set_source_files_properties("${out_file}" PROPERTIES HEADER_FILE_ONLY TRUE)
    get_target_property(roms ${target} RP6502_ASSET_ROMS)
    list(LENGTH roms index)
    if (NOT roms)
        set(index 0)
    endif()
    set_target_properties(${target} PROPERTIES
        RP6502_ASSET_SRC_${index} "${src_file}"
        RP6502_ASSET_CMD_${index} "${create}")
    set_property(TARGET ${target} APPEND PROPERTY
        RP6502_ASSET_ROMS "${out_file}"
    )
    set_property(TARGET ${target} APPEND PROPERTY RP6502_ASSET_NAMES "${addr}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_executable

  Packages the linker output of a target as an RP6502 ROM.

  .. code-block:: cmake

    rp6502_executable(<target> [DATA <addr>] [NMI <addr>]
                      [RESET <addr>] [IRQ <addr>] [<rom>...])

  Building ``<target>`` also writes ``<target>.rp6502`` in the current build
  folder. The ROM merges the linker output, the assets that
  :command:`rp6502_asset` adds, and each ``<rom>``, a path from the
  current source folder. Call :command:`rp6502_asset`,
  :command:`rp6502_map` and :command:`rp6502_byproducts` for the target
  before this command.

  ``DATA <addr>``
    Where the linker output loads. Without ``DATA``, the ROM leaves the
    linker output out.

  ``NMI <addr>``, ``RESET <addr>``, ``IRQ <addr>``
    The vectors at $FFFA, $FFFC and $FFFE. ``RESET`` is required.

  An ``<addr>`` of ``file`` reads a 16-bit address from the start of the
  linker output, in the order of the keywords above. An ``<addr>`` of
  ``default`` is 0x200 for cc65 and ``file`` for llvm-mos.
#]=======================================================================]
function(rp6502_executable target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "DATA;RESET;IRQ;NMI" "")
    if (arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR
            "rp6502_executable(${target} ...): ${arg_KEYWORDS_MISSING_VALUES} needs an address.")
    endif()
    # An address of 0 is valid, so a keyword counts as given when it is
    # defined, whatever its value.
    foreach(key DATA RESET IRQ NMI)
        string(TOLOWER "${key}_addr" var)
        if (DEFINED arg_${key})
            set(${var} "${arg_${key}}")
        else()
            set(${var} "none")
        endif()
    endforeach()
    set(extra_roms ${arg_UNPARSED_ARGUMENTS})
    _rp6502_default_address(default_addr)
    foreach(V data_addr nmi_addr reset_addr irq_addr)
        if (${V} STREQUAL "default")
            set(${V} "${default_addr}")
        endif()
    endforeach()
    set(all_extra_roms)
    foreach(rom IN LISTS extra_roms)
        if (IS_ABSOLUTE "${rom}")
            list(APPEND all_extra_roms "${rom}")
        else()
            list(APPEND all_extra_roms "${CMAKE_CURRENT_SOURCE_DIR}/${rom}")
        endif()
    endforeach()
    # An asset made from a byproduct of the link is made after the link.
    get_target_property(asset_roms ${target} RP6502_ASSET_ROMS)
    get_target_property(byproducts ${target} RP6502_BYPRODUCTS)
    set(pre_link)
    set(post_link)
    set(post_link_roms)
    if (asset_roms)
        list(APPEND all_extra_roms ${asset_roms})
        set(index 0)
        foreach(rom IN LISTS asset_roms)
            get_target_property(src ${target} RP6502_ASSET_SRC_${index})
            if (byproducts AND src IN_LIST byproducts)
                get_target_property(cmd ${target} RP6502_ASSET_CMD_${index})
                list(APPEND post_link ${cmd})
                list(APPEND post_link_roms "${rom}")
            else()
                list(APPEND pre_link "${rom}")
            endif()
            math(EXPR index "${index} + 1")
        endforeach()
    endif()
    # The merge command of rp6502.py
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(rom_file "${CMAKE_CURRENT_BINARY_DIR}/${target}.rp6502")
    set(tool_command "${Python3_EXECUTABLE}"
        "${RP6502_TOOLS_DIR}/rp6502.py"
    )
    set(executable_inputs)
    if (NOT data_addr STREQUAL "none")
        list(APPEND tool_command -a "${data_addr}")
        list(APPEND executable_inputs "$<TARGET_FILE:${target}>")
    endif ()
    if (NOT nmi_addr STREQUAL "none")
        list(APPEND tool_command -n "${nmi_addr}")
    endif ()
    if (NOT reset_addr STREQUAL "none")
        list(APPEND tool_command -r "${reset_addr}")
    else ()
        message (FATAL_ERROR "rp6502_executable RESET address missing")
    endif ()
    if (NOT irq_addr STREQUAL "none")
        list(APPEND tool_command -i "${irq_addr}")
    endif ()
    list(APPEND tool_command
        -o "${rom_file}"
        create ${executable_inputs}
        -- ${all_extra_roms}
    )
    # As sources, the assets are built before the link, and a change to one
    # links the target again. The ROM is a post-build step, so any build of
    # the target writes it.
    if (pre_link)
        target_sources(${target} PRIVATE ${pre_link})
    endif()
    if (post_link_roms)
        list(REMOVE_ITEM all_extra_roms ${post_link_roms})
    endif()
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${all_extra_roms})
    add_custom_command(TARGET ${target} POST_BUILD
        ${post_link}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${rom_file}"
        COMMAND ${tool_command}
        BYPRODUCTS "${rom_file}" ${post_link_roms}
        VERBATIM
    )
    set_target_properties(${target} PROPERTIES
        RP6502_EXECUTABLE_CALLED TRUE
        RP6502_ROM "${rom_file}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_basic

  Packages BASIC programs as an RP6502 ROM.

  .. code-block:: cmake

    rp6502_basic(<target> [BASIC <spec>] [<autorun>])

  Building the executable target ``<target>`` writes ``<target>.rp6502``,
  which holds BASIC and the assets of ``<target>``. The programs are assets
  that :command:`rp6502_asset` adds before this call, as in
  ``rp6502_asset(<target> game.bas src/game.bas)``. One program starts
  another with ``RUN "ROM:<file>"``. The project lists BASIC as a
  language, as in ``project(<name> BASIC)`` or
  ``project(<name> C ASM BASIC)``, and a project with no compiler sets
  ``RP6502_BASIC`` in the configure preset.

  ``BASIC <spec>``
    The BASIC ROM: ``owner/repo/ref``, ``owner/repo`` for the latest
    release, a ref of ``picocomputer/msbasic``, or a ``.rp6502`` file of
    the project. The default is the latest release of
    ``picocomputer/msbasic``. A ref other than a release tag needs a GitHub
    token in ``GITHUB_TOKEN`` or ``GH_TOKEN``.

  ``<autorun>``
    The asset that BASIC runs at start, through an added ``autorun.bas``
    asset. Without an ``autorun.bas`` asset, BASIC starts at the prompt.
#]=======================================================================]
function(rp6502_basic target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "BASIC" "")
    set(caller "rp6502_basic(${target})")
    list(LENGTH arg_UNPARSED_ARGUMENTS count)
    if (count GREATER 1 OR NOT TARGET ${target})
        message(FATAL_ERROR
            "rp6502_basic(<target> [BASIC <spec>] [<autorun>]), after add_executable(<target>) "
            "and its rp6502_asset() calls")
    endif()
    if (NOT CMAKE_BASIC_COMPILER_LOADED)
        message(FATAL_ERROR
            "${caller}: BASIC is not a language of the project. List it in "
            "project(), as in project(<name> BASIC) or project(<name> C ASM BASIC).")
    endif()
    if (NOT arg_BASIC AND EXISTS "${RP6502_TOOLS_DIR}/basic.rp6502")
        message(FATAL_ERROR
            "${caller}: tools/basic.rp6502 is no longer used by default. Name it "
            "with rp6502_basic(${target} BASIC tools/basic.rp6502 ...), or delete it "
            "for the latest release.")
    endif()
    _rp6502_fetch("${caller}" BASIC "${arg_BASIC}" basic source)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.basic")
    # The file changes only with BASIC, so a Makefile relinks for a new BASIC
    # that is older than the ROM.
    file(CONFIGURE OUTPUT "${dir}/basic.txt" CONTENT "${basic}\n")
    if (count EQUAL 1)
        # The ROM: drive ignores case, and BASIC is written in capitals.
        string(TOUPPER "${arg_UNPARSED_ARGUMENTS}" autorun)
        get_target_property(names ${target} RP6502_ASSET_NAMES)
        string(TOUPPER "${names}" names)
        if (NOT autorun IN_LIST names)
            message(FATAL_ERROR
                "${caller}: no rp6502_asset(${target} ${arg_UNPARSED_ARGUMENTS} ...) "
                "comes before it.")
        endif()
        # file(CONFIGURE) keeps the ROM from rebuilding at every configure.
        file(CONFIGURE OUTPUT "${dir}/autorun.bas" CONTENT "10 RUN \"ROM:${autorun}\"\n")
        rp6502_asset(${target} autorun.bas "${dir}/autorun.bas")
    endif()
    get_target_property(assets ${target} RP6502_ASSET_ROMS)
    if (NOT assets)
        set(assets)
    endif()
    # As sources, the assets are built before the link that merges them.
    target_sources(${target} PRIVATE ${assets})
    set(rom "${CMAKE_CURRENT_BINARY_DIR}/${target}.rp6502")
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE BASIC SUFFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    # The link writes the ROM, and Ninja requires a rule that declares it.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E true
        BYPRODUCTS "${rom}"
        VERBATIM)
    target_link_options(${target} PRIVATE "${basic}" ${assets})
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${basic}" "${dir}/basic.txt" ${assets})
    set_target_properties(${target} PROPERTIES
        RP6502_EXECUTABLE_CALLED TRUE
        RP6502_ROM "${rom}")
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_web

  Packages a ROM as a web page that runs the ROM in the emulator.

  .. code-block:: cmake

    rp6502_web(<target> [OUTPUT <name>.zip] [EMULATOR <spec>]
               [PAGE <file> | <folder>] [CONFIG <text>])

  ``<target>`` must already have a ROM from :command:`rp6502_executable`
  or :command:`rp6502_basic`. Building ``<target>`` also writes
  ``web/<name>.zip`` in the build folder, with the same files unpacked in
  ``web/<name>/``. At the root are the ROM, the page as ``index.html``,
  and ``rp6502.js`` and ``rp6502.wasm`` from the web zip of the emulator.

  ``OUTPUT <name>.zip``
    The name of the zip, by default ``<target>.zip``.

  ``EMULATOR <spec>``
    The web zip: ``owner/repo/ref``, ``owner/repo`` for the latest
    release, a ref of ``picocomputer/rp6502``, or a ``.zip`` file of the
    project. The default is the latest release of ``picocomputer/rp6502``.
    A ref other than a release tag needs a GitHub token in ``GITHUB_TOKEN``
    or ``GH_TOKEN``. ``RP6502_WEB_EMULATOR``, when set, replaces
    ``EMULATOR`` in every call.

  ``PAGE <file> | <folder>``
    The page file, or a folder copied with all subfolders, where the root
    ``index.html`` is the page. The default is the ``index.html`` of the
    web zip. The page must load the emulator with
    ``<script src="rp6502.js">``, and a script with the settings of
    ``CONFIG`` is inserted just before that tag.

  ``CONFIG <text>``
    JavaScript, the keys and values of an object, which replace or add
    keys of ``CONFIG`` in the page. ``CONFIG.rom`` is always the ROM.
    ``CONFIG.github``, for the links under a footer, is the GitHub
    repository of the git remote ``origin`` unless ``<text>`` names another.
    With the default page, ``CONFIG.title`` is empty unless ``<text>`` sets
    it.
#]=======================================================================]
function(rp6502_web target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "OUTPUT;EMULATOR;PAGE;CONFIG" "")
    set(caller "rp6502_web(${target})")
    if (arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "${caller}: ${arg_UNPARSED_ARGUMENTS} follows no keyword. rp6502_web(<target> "
            "[OUTPUT <name>.zip] [EMULATOR <spec>] [PAGE <file> | <folder>] [CONFIG <text>])")
    endif()
    if (TARGET ${target})
        get_target_property(rom_file ${target} RP6502_ROM)
    endif()
    if (NOT rom_file)
        message(FATAL_ERROR
            "${caller}: ${target} makes no ROM. Call rp6502_executable(${target} ...) "
            "or rp6502_basic(${target} ...) before rp6502_web(${target} ...).")
    endif()
    set(zip "${target}.zip")
    if (arg_OUTPUT)
        set(zip "${arg_OUTPUT}")
    endif()
    if (NOT zip MATCHES "^[A-Za-z0-9_-][A-Za-z0-9._-]*\\.zip$")
        message(FATAL_ERROR "${caller}: OUTPUT ${zip} is not a file name ending in .zip.")
    endif()
    string(REGEX REPLACE "\\.zip$" "" name "${zip}")
    get_property(zips GLOBAL PROPERTY RP6502_WEB_ZIPS)
    if (zip IN_LIST zips)
        message(FATAL_ERROR "${caller}: two rp6502_web() calls make ${zip}.")
    endif()
    set_property(GLOBAL APPEND PROPERTY RP6502_WEB_ZIPS "${zip}")
    set(spec "${arg_EMULATOR}")
    if (RP6502_WEB_EMULATOR)
        set(spec "${RP6502_WEB_EMULATOR}")
    endif()
    _rp6502_fetch("${caller}" EMULATOR "${spec}" web_zip source)

    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.web")
    file(REMOVE_RECURSE "${dir}/emulator")
    file(ARCHIVE_EXTRACT INPUT "${web_zip}" DESTINATION "${dir}/emulator"
        PATTERNS rp6502.js rp6502.wasm index.html)
    foreach(part rp6502.js rp6502.wasm index.html)
        if (NOT EXISTS "${dir}/emulator/${part}")
            message(FATAL_ERROR "${caller}: ${web_zip} has no ${part} at its root.")
        endif()
    endforeach()

    set(page "${dir}/emulator/index.html")
    set(folder)
    if (arg_PAGE)
        get_filename_component(path "${arg_PAGE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if (IS_DIRECTORY "${path}")
            set(folder "${path}")
            if (EXISTS "${path}/index.html")
                set(page "${path}/index.html")
            endif()
        elseif (EXISTS "${path}")
            set(page "${path}")
        else()
            message(FATAL_ERROR "${caller}: PAGE ${arg_PAGE} is not a file or a folder.")
        endif()
    endif()
    set(folder_files)
    set(folder_entries)
    if (folder)
        file(GLOB_RECURSE found LIST_DIRECTORIES false CONFIGURE_DEPENDS "${folder}/*")
        foreach(file IN LISTS found)
            file(RELATIVE_PATH relative "${folder}" "${file}")
            if (relative STREQUAL "${target}.rp6502" OR relative STREQUAL "rp6502.js"
                    OR relative STREQUAL "rp6502.wasm")
                message(FATAL_ERROR "${caller}: ${arg_PAGE}/${relative} has the name of a file rp6502_web() makes.")
            endif()
            list(APPEND folder_files "${file}")
        endforeach()
        file(GLOB folder_entries LIST_DIRECTORIES true RELATIVE "${folder}" "${folder}/*")
        list(REMOVE_ITEM folder_entries index.html)
    endif()
    if (NOT page STREQUAL "${dir}/emulator/index.html")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${page}")
    endif()

    # The settings are a separate script, before rp6502.js reads CONFIG.
    file(READ "${page}" html)
    string(FIND "${html}" "<script src=\"rp6502.js\"" at)
    if (at LESS 0)
        message(FATAL_ERROR "${caller}: ${page} has no <script src=\"rp6502.js\">.")
    endif()
    set(script "<script>\n")
    if (page STREQUAL "${dir}/emulator/index.html")
        string(APPEND script "CONFIG.title = '';\n")
    endif()
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" remote get-url origin
        OUTPUT_VARIABLE origin OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (origin MATCHES "github\\.com[:/]([^/]+/[^/]+)$")
        string(REGEX REPLACE "\\.git$" "" repo "${CMAKE_MATCH_1}")
        string(APPEND script "CONFIG.github = '${repo}';\n")
    endif()
    if (DEFINED arg_CONFIG)
        string(APPEND script "Object.assign(CONFIG, {\n${arg_CONFIG}\n});\n")
    endif()
    string(APPEND script "CONFIG.rom = '${target}.rp6502';\n</script>\n")
    string(SUBSTRING "${html}" 0 ${at} head)
    string(SUBSTRING "${html}" ${at} -1 tail)
    set(html "${head}${script}${tail}")
    # index.html is rewritten only on a change, so the zip is not rebuilt.
    set(written)
    if (EXISTS "${dir}/index.html")
        file(READ "${dir}/index.html" written)
    endif()
    if (NOT written STREQUAL html)
        file(WRITE "${dir}/index.html" "${html}")
    endif()
    file(SHA256 "${web_zip}" web_hash)
    string(REPLACE ";" "\n" listed "${folder_files}")
    file(CONFIGURE OUTPUT "${dir}/sources.txt"
        CONTENT "${web_zip} ${web_hash}\n${listed}\n" @ONLY)
    set(out "${CMAKE_BINARY_DIR}/web")
    file(CONFIGURE OUTPUT "${out}/${name}.emulator" CONTENT "${source}\n" @ONLY)

    set(stage "${out}/${name}")
    set(copy_folder)
    if (folder)
        set(copy_folder COMMAND "${CMAKE_COMMAND}" -E copy_directory "${folder}" "${stage}")
    endif()
    # The zip is a post-build step of the ROM target, and a change to the
    # page, the folder or the web zip links the target again.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${dir}/index.html" "${dir}/sources.txt" ${folder_files})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${stage}" "${out}/${zip}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage}"
        ${copy_folder}
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${dir}/emulator/rp6502.js" "${dir}/emulator/rp6502.wasm" "${dir}/index.html"
            "${stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${rom_file}" "${stage}/${target}.rp6502"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${stage}"
            "${CMAKE_COMMAND}" -E tar cf "${out}/${zip}" --format=zip --
            index.html rp6502.js rp6502.wasm ${target}.rp6502 ${folder_entries}
        BYPRODUCTS "${out}/${zip}"
        COMMENT "Packaging web/${zip}"
        VERBATIM
    )
endfunction()

#[=======================================================================[.rst:
.. command:: rp6502_byproducts

  Declares files that the link of a target also writes.

  .. code-block:: cmake

    rp6502_byproducts(<target> <file>...)

  Some linker configurations write files beside the executable, which
  ``add_executable()`` does not model. A relative ``<file>`` is a path
  from the current build folder. An :command:`rp6502_asset` made from one
  of these files is made after the link. Call this before
  :command:`rp6502_executable`.
#]=======================================================================]
function(rp6502_byproducts target)
    add_custom_command(
        OUTPUT ${ARGN}
        DEPENDS ${target}
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${ARGN}
        VERBATIM
    )
    foreach(file IN LISTS ARGN)
        get_filename_component(file "${file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        set_property(TARGET ${target} APPEND PROPERTY RP6502_BYPRODUCTS "${file}")
    endforeach()
endfunction()

# Internal helpers of the commands above

# cc65 links a flat image at a fixed address;
# llvm-mos writes the address into the start of the output file.
function(_rp6502_default_address var)
    if(CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(${var} 0x200 PARENT_SCOPE)
    else()
        set(${var} file PARENT_SCOPE)
    endif()
endfunction()

# Fetches the BASIC ROM or web zip that <spec> names into the build folder,
# and sets <out_var> to the file and <source_var> to the origin of the file,
# the text of web/<name>.emulator. A ref is a release tag, or else a commit
# with a CI artifact. Tags and commits are fetched once per build folder. The
# latest release and branches are looked up at each configure, or taken from
# the last lookup offline.
function(_rp6502_fetch caller keyword spec out_var source_var)
    if(keyword STREQUAL "BASIC")
        set(repo "${RP6502_BASIC_REPO}")
        set(pattern "^basic\\.rp6502$")
        set(wanted "basic.rp6502")
        set(offline_fix "BASIC tools/basic.rp6502")
    else()
        set(repo "${RP6502_TOOLS_REPO}")
        set(pattern "-web\\.zip$")
        set(wanted "file ending in -web.zip")
        set(offline_fix "EMULATOR tools/rp6502-web.zip")
    endif()
    set(offline_fix "To work offline, commit a copy of the file and name it, such as ${offline_fix}.")
    if(spec MATCHES "\\.(zip|rp6502)$")
        get_filename_component(file "${spec}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${file}" OR IS_DIRECTORY "${file}")
            message(FATAL_ERROR "${caller}: ${keyword} ${spec} is not a file.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
        set(${out_var} "${file}" PARENT_SCOPE)
        set(${source_var} "file ${file}" PARENT_SCOPE)
        return()
    endif()
    set(ref latest)
    if(spec MATCHES "^([^/]+/[^/]+)(/(.+))?$")
        set(repo "${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_3)
            set(ref "${CMAKE_MATCH_3}")
        endif()
    elseif(NOT spec STREQUAL "")
        set(ref "${spec}")
    endif()
    set(what "${keyword} ${repo}/${ref}")
    set(cache "${CMAKE_BINARY_DIR}/rp6502/${repo}")
    string(MAKE_C_IDENTIFIER "${ref}" key)
    set(record "${cache}/${key}.ref")
    set(tmp "${cache}/${key}.tmp")

    # A tag or a full commit hash names the same file forever.
    if(EXISTS "${record}")
        file(STRINGS "${record}" lines)
        list(GET lines 0 kind)
        list(GET lines 1 file)
        list(GET lines 2 source)
        if(EXISTS "${file}" AND (kind STREQUAL "tag" OR ref MATCHES "^[0-9a-f]{40}$"))
            set(${out_var} "${file}" PARENT_SCOPE)
            set(${source_var} "${source}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # A release, where SHA256SUMS lists the file and the hash.
    if(ref STREQUAL "latest")
        set(base "https://github.com/${repo}/releases/latest/download")
    else()
        set(base "https://github.com/${repo}/releases/download/${ref}")
    endif()
    _rp6502_fetch_url("${base}/SHA256SUMS" "${tmp}" result text)
    if(result STREQUAL "ok")
        _rp6502_read_sums("${tmp}" assets)
        file(REMOVE "${tmp}")
        set(name)
        foreach(asset IN LISTS assets)
            string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${asset}")
            # The matches are read out before the next match overwrites them.
            set(asset_name "${CMAKE_MATCH_1}")
            set(asset_hash "${CMAKE_MATCH_2}")
            if(asset_name MATCHES "${pattern}")
                set(name "${asset_name}")
                string(TOLOWER "${asset_hash}" hash)
            endif()
        endforeach()
        if(NOT name)
            message(FATAL_ERROR "${caller}: the SHA256SUMS of ${what} lists no ${wanted}.")
        endif()
        set(file "${cache}/${hash}/${name}")
        if(NOT EXISTS "${file}")
            message(STATUS "Fetching ${what}: ${name}")
            _rp6502_fetch_url("${base}/${name}" "${file}.tmp" result text ${hash})
            if(NOT result STREQUAL "ok")
                message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}")
            endif()
            file(RENAME "${file}.tmp" "${file}")
        endif()
        if(ref STREQUAL "latest")
            set(kind latest)
        else()
            set(kind tag)
        endif()
        set(source "release ${repo} ${ref}")
    elseif(result STREQUAL "failed")
        message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}")
    elseif(result STREQUAL "offline")
        _rp6502_fetch_last("${record}" "${caller}"
            "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}" file source)
        set(${out_var} "${file}" PARENT_SCOPE)
        set(${source_var} "${source}" PARENT_SCOPE)
        return()
    else()
        # Not a release with SHA256SUMS: a commit, a short hash, or a branch.
        set(api "https://api.github.com/repos/${repo}")
        _rp6502_fetch_url("${api}/releases/tags/${ref}" "${tmp}" release text)
        file(REMOVE "${tmp}")
        if(release STREQUAL "ok")
            message(FATAL_ERROR "${caller}: the release ${ref} of ${repo} has no SHA256SUMS.")
        endif()
        if("$ENV{GITHUB_TOKEN}" STREQUAL "" AND "$ENV{GH_TOKEN}" STREQUAL "")
            message(FATAL_ERROR
                "${caller}: ${what} is not a release with SHA256SUMS. A commit is "
                "fetched from its CI run, which needs a GitHub token in GITHUB_TOKEN "
                "or GH_TOKEN.")
        endif()
        _rp6502_fetch_url("${api}/commits/${ref}" "${tmp}" result text)
        if(result STREQUAL "missing")
            message(FATAL_ERROR "${caller}: ${repo} has no release or commit named ${ref}.")
        elseif(NOT result STREQUAL "ok")
            _rp6502_fetch_last("${record}" "${caller}"
                "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}" file source)
            set(${out_var} "${file}" PARENT_SCOPE)
            set(${source_var} "${source}" PARENT_SCOPE)
            return()
        endif()
        file(READ "${tmp}" json)
        string(JSON sha GET "${json}" sha)
        set(file)
        file(GLOB found "${cache}/${sha}/*")
        foreach(candidate IN LISTS found)
            get_filename_component(candidate_name "${candidate}" NAME)
            if(candidate_name MATCHES "${pattern}")
                set(file "${candidate}")
            endif()
        endforeach()
        if(file AND EXISTS "${cache}/${sha}/run")
            file(READ "${cache}/${sha}/run" source)
        else()
            _rp6502_fetch_url("${api}/actions/runs?head_sha=${sha}&per_page=100"
                "${tmp}" result text)
            if(NOT result STREQUAL "ok")
                message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}")
            endif()
            file(READ "${tmp}" json)
            string(JSON count LENGTH "${json}" workflow_runs)
            # Pull request runs go last; they build a merge, not the commit.
            set(first)
            set(later)
            if(count GREATER 0)
                math(EXPR last "${count} - 1")
                foreach(i RANGE ${last})
                    string(JSON id GET "${json}" workflow_runs ${i} id)
                    string(JSON event GET "${json}" workflow_runs ${i} event)
                    if(event STREQUAL "pull_request")
                        list(APPEND later ${id})
                    else()
                        list(APPEND first ${id})
                    endif()
                endforeach()
            endif()
            set(runs ${first} ${later})
            set(source)
            foreach(run IN LISTS runs)
                _rp6502_fetch_url("${api}/actions/runs/${run}/artifacts?per_page=100" "${tmp}" result text)
                if(NOT result STREQUAL "ok")
                    continue()
                endif()
                file(READ "${tmp}" json)
                string(JSON count LENGTH "${json}" artifacts)
                if(count EQUAL 0)
                    continue()
                endif()
                math(EXPR last "${count} - 1")
                foreach(i RANGE ${last})
                    string(JSON name GET "${json}" artifacts ${i} name)
                    string(JSON expired GET "${json}" artifacts ${i} expired)
                    if(NOT name MATCHES "${pattern}" OR expired)
                        continue()
                    endif()
                    string(JSON id GET "${json}" artifacts ${i} id)
                    string(JSON digest ERROR_VARIABLE no_digest GET "${json}" artifacts ${i} digest)
                    set(check)
                    if(NOT no_digest AND digest MATCHES "^sha256:([0-9a-f]+)$")
                        set(check ${CMAKE_MATCH_1})
                    endif()
                    set(file "${cache}/${sha}/${name}")
                    message(STATUS "Fetching ${what}: ${name} from CI run ${run}")
                    _rp6502_fetch_url("${api}/actions/artifacts/${id}/zip" "${file}.tmp" result text ${check})
                    if(NOT result STREQUAL "ok")
                        message(FATAL_ERROR "${caller}: cannot fetch ${what}.\n${text}\n${offline_fix}")
                    endif()
                    file(RENAME "${file}.tmp" "${file}")
                    set(source "run ${repo} ${run}")
                    file(WRITE "${cache}/${sha}/run" "${source}")
                    break()
                endforeach()
                if(source)
                    break()
                endif()
            endforeach()
            file(REMOVE "${tmp}")
            if(NOT source)
                message(FATAL_ERROR
                    "${caller}: the CI runs of ${repo} commit ${sha} have no "
                    "${wanted} that has not expired. Artifacts expire after 90 days.")
            endif()
        endif()
        set(kind commit)
    endif()
    file(WRITE "${record}" "${kind}\n${file}\n${source}\n")
    set(${out_var} "${file}" PARENT_SCOPE)
    set(${source_var} "${source}" PARENT_SCOPE)
endfunction()

# One GitHub request. Sets <out_var> to ok, missing for HTTP 404 or 422,
# failed for another HTTP error or a wrong [<sha256>], or offline, and
# <text_var> to the reason. The token goes only to api.github.com; curl
# drops it on a redirect to storage.
function(_rp6502_fetch_url url file out_var text_var)
    set(headers)
    if(url MATCHES "^https://api\\.github\\.com/")
        set(headers
            HTTPHEADER "Accept: application/vnd.github+json"
            HTTPHEADER "X-GitHub-Api-Version: 2022-11-28")
        if(NOT "$ENV{GITHUB_TOKEN}" STREQUAL "")
            list(APPEND headers HTTPHEADER "Authorization: Bearer $ENV{GITHUB_TOKEN}")
        elseif(NOT "$ENV{GH_TOKEN}" STREQUAL "")
            list(APPEND headers HTTPHEADER "Authorization: Bearer $ENV{GH_TOKEN}")
        endif()
    endif()
    file(DOWNLOAD "${url}" "${file}"
        STATUS status
        LOG log
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
        ${headers}
    )
    list(GET status 0 code)
    list(GET status 1 text)
    # The last status line is the answer after any redirects.
    string(REGEX MATCHALL "HTTP/[0-9.]+ [0-9][0-9][0-9]" answers "${log}")
    set(answer)
    if(answers)
        list(GET answers -1 answer)
        string(REGEX REPLACE "^HTTP/[0-9.]+ " "" answer "${answer}")
    endif()
    set(result ok)
    if(NOT code EQUAL 0)
        file(REMOVE "${file}")
        # curl reports an error answer as 22; anything else is the network.
        if(NOT code EQUAL 22)
            set(result offline)
        elseif(answer STREQUAL "404" OR answer STREQUAL "422")
            set(result missing)
        else()
            set(result failed)
            set(text "HTTP ${answer}")
        endif()
    elseif(ARGC GREATER 4)
        file(SHA256 "${file}" got)
        string(TOLOWER "${ARGV4}" expected)
        if(NOT got STREQUAL expected)
            file(REMOVE "${file}")
            set(result failed)
            set(text "wrong contents, SHA256 ${got} where ${expected} was expected")
        endif()
    endif()
    set(${out_var} ${result} PARENT_SCOPE)
    set(${text_var} "${url}: ${text}" PARENT_SCOPE)
endfunction()

# Offline, uses the file that <record> names, or stops with <message>.
function(_rp6502_fetch_last record caller message file_var source_var)
    if(EXISTS "${record}")
        file(STRINGS "${record}" lines)
        list(GET lines 1 file)
        list(GET lines 2 source)
        if(EXISTS "${file}")
            message(NOTICE "${caller}: no network, so the file fetched before is used.")
            set(${file_var} "${file}" PARENT_SCOPE)
            set(${source_var} "${source}" PARENT_SCOPE)
            return()
        endif()
    endif()
    message(FATAL_ERROR "${message}")
endfunction()

# Used by the commands and the tool update

# Reads sha256sum(1) output as a list of name=hash.
function(_rp6502_read_sums file out_var)
    file(STRINGS "${file}" lines)
    set(entries)
    foreach(line IN LISTS lines)
        if(line MATCHES "^([0-9a-fA-F]+)[ \t]+([^ \t/\\\\]+)$")
            list(APPEND entries "${CMAKE_MATCH_2}=${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out_var} "${entries}" PARENT_SCOPE)
endfunction()

# Internal helpers of the tool update and the emulator fetch

# Fetches tools/<name>, and checks the file against <hash> when <hash> is not
# empty. The rename comes last, so a failed fetch leaves the working tool in
# place.
function(_rp6502_fetch_tool name hash)
    set(url "https://raw.githubusercontent.com/${RP6502_TOOLS_REPO}/${RP6502_TOOLS_REF}/tools/${name}")
    set(out "${RP6502_TOOLS_DIR}/${name}")
    message(STATUS "Fetching tools/${name}")
    file(DOWNLOAD "${url}" "${out}.tmp"
        STATUS status
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
    )
    list(GET status 0 code)
    list(GET status 1 text)
    file(SIZE "${out}.tmp" size)
    if(NOT code EQUAL 0 OR size EQUAL 0)
        file(REMOVE "${out}.tmp")
        message(FATAL_ERROR "Cannot fetch ${url}\n${text}")
    endif()
    if(hash)
        file(SHA256 "${out}.tmp" got)
        string(TOLOWER "${hash}" hash)
        if(NOT got STREQUAL hash)
            file(REMOVE "${out}.tmp")
            message(FATAL_ERROR
                "Wrong contents for tools/${name}\n"
                "expected ${hash}\n"
                "     got ${got}")
        endif()
    endif()
    file(RENAME "${out}.tmp" "${out}")
endfunction()

# Sets <out_var> to the name=hash list of tools/SHA256SUMS in the tools
# repository.
function(_rp6502_fetch_sums out_var)
    _rp6502_fetch_tool(SHA256SUMS "")
    _rp6502_read_sums("${RP6502_TOOLS_DIR}/SHA256SUMS" files)
    file(REMOVE "${RP6502_TOOLS_DIR}/SHA256SUMS")
    if(NOT files)
        message(FATAL_ERROR "tools/SHA256SUMS lists nothing to fetch.")
    endif()
    set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

# Fetches the emulator for the host. With MISSING, the fetch skips what
# tools/ has, and skips everything while tools/rp6502-emu.unsupported exists,
# so no configure on a host with no build repeats the fetch.
function(_rp6502_fetch_emulator)
    # Each build is suffix|member|exe. Windows and Apple hosts are tested with
    # CMAKE_HOST_WIN32 and CMAKE_HOST_APPLE, because OS_NAME is macOS on a Mac
    # and not Darwin.
    set(builds)
    if(CMAKE_HOST_WIN32)
        list(APPEND builds "windows.zip|rp6502-emu.exe|rp6502-emu.exe")
    elseif(CMAKE_HOST_APPLE)
        list(APPEND builds "macos.zip|rp6502-emu.app/Contents/MacOS/rp6502-emu|rp6502-emu")
    else()
        cmake_host_system_information(RESULT host QUERY OS_NAME)
        cmake_host_system_information(RESULT release QUERY OS_RELEASE)
        cmake_host_system_information(RESULT machine QUERY OS_PLATFORM)
        if(host STREQUAL "Linux")
            # WSL runs the Windows build through interop.
            if(release MATCHES "[Mm]icrosoft")
                list(APPEND builds "windows.zip|rp6502-emu.exe|rp6502-emu.exe")
            endif()
            list(APPEND builds "linux-${machine}.tar.gz|rp6502-emu|rp6502-emu")
        endif()
    endif()
    set(sentinel "${RP6502_TOOLS_DIR}/rp6502-emu.unsupported")
    if(NOT "MISSING" IN_LIST ARGN)
        file(REMOVE "${sentinel}")
    elseif(EXISTS "${sentinel}")
        message(STATUS "No emulator: tools/rp6502-emu.unsupported has the reason, "
            "and cmake -P tools/rp6502.cmake tries again.")
        return()
    endif()
    set(assets)
    foreach(build IN LISTS builds)
        string(REPLACE "|" ";" fields "${build}")
        list(GET fields 0 suffix)
        list(GET fields 1 member)
        list(GET fields 2 exe)
        if("MISSING" IN_LIST ARGN AND EXISTS "${RP6502_TOOLS_DIR}/${exe}")
            continue()
        endif()
        if(NOT assets)
            if(RP6502_EMU_RELEASE STREQUAL "latest")
                set(base "https://github.com/${RP6502_TOOLS_REPO}/releases/latest/download")
            else()
                set(base "https://github.com/${RP6502_TOOLS_REPO}/releases/download/${RP6502_EMU_RELEASE}")
            endif()
            message(STATUS "Fetching the emulator list")
            set(sums "${RP6502_TOOLS_DIR}/rp6502-emu.SHA256SUMS.tmp")
            # TIMEOUT limits the wait on a network that drops packets.
            file(DOWNLOAD "${base}/SHA256SUMS" "${sums}"
                STATUS status
                TLS_VERIFY ON
                TIMEOUT 30
            )
            list(GET status 0 code)
            list(GET status 1 text)
            if(code EQUAL 0)
                _rp6502_read_sums("${sums}" assets)
            endif()
            file(REMOVE "${sums}")
            if(NOT assets)
                message(NOTICE "No emulator: cannot fetch ${base}/SHA256SUMS\n${text}")
                break()
            endif()
        endif()
        _rp6502_fetch_emu("${base}" "${assets}" "${suffix}" "${member}" "${exe}" result)
        if(result STREQUAL "unsupported")
            file(APPEND "${sentinel}"
                "Release ${RP6502_EMU_RELEASE} has no ${suffix} for tools/${exe}.\n")
        endif()
    endforeach()
endfunction()

# Fetches the asset ending in <suffix> and unpacks <member> as tools/<exe>.
# Sets <result_var> to ok, unsupported for no such asset, or failed.
function(_rp6502_fetch_emu base assets suffix member exe result_var)
    set(${result_var} failed PARENT_SCOPE)
    set(name)
    foreach(asset IN LISTS assets)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${asset}")
        # The matches are read out before the next match overwrites them.
        set(asset_name "${CMAKE_MATCH_1}")
        set(asset_hash "${CMAKE_MATCH_2}")
        if(asset_name MATCHES "-${suffix}$")
            set(name "${asset_name}")
            set(hash "${asset_hash}")
        endif()
    endforeach()
    if(NOT name)
        message(NOTICE "No emulator: release ${RP6502_EMU_RELEASE} has no ${suffix}")
        set(${result_var} unsupported PARENT_SCOPE)
        return()
    endif()
    set(tmp "${RP6502_TOOLS_DIR}/${exe}.tmp")
    file(REMOVE_RECURSE "${tmp}")
    file(MAKE_DIRECTORY "${tmp}")
    message(STATUS "Fetching tools/${exe}")
    file(DOWNLOAD "${base}/${name}" "${tmp}/${name}"
        STATUS status
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
    )
    list(GET status 0 code)
    list(GET status 1 text)
    if(NOT code EQUAL 0)
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: cannot fetch ${base}/${name}\n${text}")
        return()
    endif()
    file(SHA256 "${tmp}/${name}" got)
    string(TOLOWER "${hash}" hash)
    if(NOT got STREQUAL hash)
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: wrong contents for ${name}")
        return()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${name}"
        WORKING_DIRECTORY "${tmp}"
        RESULT_VARIABLE result
        ERROR_VARIABLE error
    )
    if(NOT result STREQUAL "0")
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: cannot unpack ${name}\n${error}")
        return()
    endif()
    file(RENAME "${tmp}/${member}" "${RP6502_TOOLS_DIR}/${exe}" RESULT result)
    file(REMOVE_RECURSE "${tmp}")
    if(NOT result STREQUAL "0")
        message(NOTICE "No emulator: cannot replace tools/${exe}, close it first\n${result}")
        return()
    endif()
    if(NOT CMAKE_HOST_WIN32)
        file(CHMOD "${RP6502_TOOLS_DIR}/${exe}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endif()
    set(${result_var} ok PARENT_SCOPE)
endfunction()

# Adds the update task to .vscode/tasks.json.
function(_rp6502_hook_tasks_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/tasks.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" json)
    if(json MATCHES "RP6502: update tools")
        return()
    endif()
    # The task is spliced in as text, because string(JSON) rejects the trailing
    # commas that VS Code accepts and drops every comment on a rewrite.
    set(task [==[

        {
            "label": "RP6502: update tools",
            "type": "process",
            "command": "cmake",
            "args": [
                "-P",
                "${workspaceFolder}/tools/rp6502.cmake"
            ],
            "presentation": {
                "reveal": "always",
                "panel": "dedicated"
            },
            "problemMatcher": []
        },]==])
    string(FIND "${json}" "\"tasks\"" tasks_at)
    if(tasks_at LESS 0)
        message(NOTICE "Add an \"RP6502: update tools\" task to .vscode/tasks.json by hand.")
        return()
    endif()
    string(SUBSTRING "${json}" ${tasks_at} -1 tail)
    string(FIND "${tail}" "[" bracket_at)
    if(bracket_at LESS 0)
        message(NOTICE "Add an \"RP6502: update tools\" task to .vscode/tasks.json by hand.")
        return()
    endif()
    math(EXPR cut "${tasks_at} + ${bracket_at} + 1")
    string(SUBSTRING "${json}" 0 ${cut} head)
    string(SUBSTRING "${json}" ${cut} -1 rest)
    file(WRITE "${file}" "${head}${task}${rest}")
    message(STATUS "Added the update task to .vscode/tasks.json")
endfunction()

# Renames the old entries of .vscode/launch.json and adds RP6502-WEB.
function(_rp6502_hook_launch_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/launch.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" before)
    string(REPLACE "\"RP6502 (Emulator)\"" "\"RP6502-EMU\"" json "${before}")
    string(REPLACE "\"RP6502 (Hardware)\"" "\"RP6502-PICO\"" json "${json}")
    string(REPLACE "\"RP6502 (Web)\"" "\"RP6502-WEB\"" json "${json}")
    if(NOT json STREQUAL before)
        file(WRITE "${file}" "${json}")
        message(STATUS "Renamed the entries of .vscode/launch.json")
    endif()
    if(json MATCHES "\"RP6502-WEB\"")
        return()
    endif()
    # The entry is inserted as text at the end of the configurations, so the
    # entry that F5 starts does not change.
    set(entry [==[
        {
            "name": "RP6502-WEB",
            "type": "debugpy",
            "request": "launch",
            "console": "integratedTerminal",
            "program": "${workspaceFolder}/tools/rp6502.py",
            "args": [
                "web",
                "${command:cmake.launchTargetPath}"
            ],
        },
]==])
    # The scan finds the bracket that closes the configurations, outside
    # strings and comments, and the last character before it outside comments.
    set(end_at -1)
    string(FIND "${json}" "\"configurations\"" at)
    if(at GREATER_EQUAL 0)
        string(SUBSTRING "${json}" ${at} -1 tail)
        string(FIND "${tail}" "[" open_at)
        if(open_at GREATER_EQUAL 0)
            math(EXPR last_at "${at} + ${open_at}")
            math(EXPR i "${last_at} + 1")
            string(LENGTH "${json}" length)
            set(depth 1)
            set(state code)
            while(i LESS length)
                string(SUBSTRING "${json}" ${i} 2 pair)
                string(SUBSTRING "${pair}" 0 1 c)
                if(state STREQUAL "string")
                    if(c STREQUAL "\\")
                        math(EXPR i "${i} + 1")
                    elseif(c STREQUAL "\"")
                        set(state code)
                        set(last_at ${i})
                    endif()
                elseif(state STREQUAL "line")
                    if(c STREQUAL "\n")
                        set(state code)
                    endif()
                elseif(state STREQUAL "block")
                    if(pair STREQUAL "*/")
                        set(state code)
                        math(EXPR i "${i} + 1")
                    endif()
                elseif(c STREQUAL "\"")
                    set(state string)
                elseif(pair STREQUAL "//")
                    set(state line)
                elseif(pair STREQUAL "/*")
                    set(state block)
                elseif(c STREQUAL "[")
                    math(EXPR depth "${depth} + 1")
                    set(last_at ${i})
                elseif(c STREQUAL "]")
                    math(EXPR depth "${depth} - 1")
                    if(depth EQUAL 0)
                        set(end_at ${i})
                        break()
                    endif()
                    set(last_at ${i})
                elseif(NOT c MATCHES "^[ \t\r\n]$")
                    set(last_at ${i})
                endif()
                math(EXPR i "${i} + 1")
            endwhile()
        endif()
    endif()
    if(end_at LESS 0)
        message(NOTICE "Add an \"RP6502-WEB\" entry to .vscode/launch.json by hand.")
        return()
    endif()
    math(EXPR cut "${last_at} + 1")
    string(SUBSTRING "${json}" 0 ${cut} head)
    string(SUBSTRING "${json}" ${cut} -1 rest)
    if(head MATCHES "}$")
        string(APPEND head ",")
    endif()
    string(REGEX REPLACE "^[ \t]*\n" "" rest "${rest}")
    if(rest MATCHES "^]")
        string(PREPEND rest "    ")
    endif()
    file(WRITE "${file}" "${head}\n${entry}${rest}")
    message(STATUS "Added the web entry to .vscode/launch.json")
endfunction()

# This code runs at include, after every function above is defined.

# cmake -P updates the tools and includes the new file when it changed.
if(CMAKE_SCRIPT_MODE_FILE AND NOT RP6502_TOOLS_RELOADED)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_before)
    _rp6502_fetch_sums(rp6502_tools_files)
    foreach(entry IN LISTS rp6502_tools_files)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${entry}")
        _rp6502_fetch_tool("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endforeach()
    set(RP6502_TOOLS_FETCHED TRUE)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_after)
    if(NOT rp6502_tools_after STREQUAL rp6502_tools_before)
        set(RP6502_TOOLS_RELOADED TRUE)
        include("${CMAKE_CURRENT_LIST_FILE}")
        return()
    endif()
endif()

# RP6502_TOOLS_FETCHED is set by the update above or by the bootstrap.
if(RP6502_TOOLS_FETCHED)
    _rp6502_hook_tasks_json()
    _rp6502_hook_launch_json()
    _rp6502_fetch_emulator()
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

# A clone lacks the emulator, which is not committed.
if(NOT RP6502_TOOLS_FETCHED)
    _rp6502_fetch_emulator(MISSING)
endif()

if(DEFINED CC65_TARGET_SYSTEM)
    find_package(cc65 REQUIRED)
elseif(DEFINED LLVM_MOS_PLATFORM)
    find_package(llvm-mos-sdk REQUIRED)
elseif(NOT RP6502_BASIC)
    message(FATAL_ERROR
        "No compiler selected.\n"
        "Configure with a CMake preset; cmake --list-presets shows them. "
        "Without presets, set CC65_TARGET_SYSTEM or LLVM_MOS_PLATFORM, "
        "or RP6502_BASIC for a BASIC project.")
endif()

# BASIC is a CMake language in every project, so a BASIC program is an
# executable target that cmake.launchTargetPath in VS Code resolves.
set(rp6502_basic_dir "${CMAKE_BINARY_DIR}/CMakeFiles/rp6502-basic")
file(WRITE "${rp6502_basic_dir}/CMakeDetermineBASICCompiler.cmake" [=[
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_BASIC_COMPILER "${Python3_EXECUTABLE}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/CMakeBASICCompiler.cmake.in"
    "${CMAKE_PLATFORM_INFO_DIR}/CMakeBASICCompiler.cmake" @ONLY)
set(CMAKE_BASIC_COMPILER_ENV_VAR "")
]=])
file(WRITE "${rp6502_basic_dir}/CMakeBASICCompiler.cmake.in" [=[
set(CMAKE_BASIC_COMPILER "@CMAKE_BASIC_COMPILER@")
set(CMAKE_BASIC_COMPILER_LOADED 1)
set(CMAKE_BASIC_SOURCE_FILE_EXTENSIONS "")
set(CMAKE_BASIC_OUTPUT_EXTENSION .rp6502)
set(CMAKE_BASIC_COMPILER_ENV_VAR "")
]=])
# The executable is an empty file. The ROM is <TARGET>.rp6502, the name that
# a launch configuration makes from the target path, as for C. BASIC comes
# first, so a help asset of the program replaces the help of BASIC.
file(WRITE "${rp6502_basic_dir}/CMakeBASICInformation.cmake"
"set(CMAKE_BASIC_LINK_EXECUTABLE \"<CMAKE_BASIC_COMPILER> \\\"${RP6502_TOOLS_DIR}/rp6502.py\\\" -o <TARGET>.rp6502 create --replace help <LINK_FLAGS> <OBJECTS>\" \"<CMAKE_COMMAND> -E touch <TARGET>\")
set(CMAKE_BASIC_INFORMATION_LOADED 1)
")
file(WRITE "${rp6502_basic_dir}/CMakeTestBASICCompiler.cmake" "set(CMAKE_BASIC_COMPILER_WORKS 1 CACHE INTERNAL \"\")\n")
list(APPEND CMAKE_MODULE_PATH "${rp6502_basic_dir}")
unset(rp6502_basic_dir)
