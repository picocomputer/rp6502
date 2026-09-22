# The RP6502 project tools: rp6502_executable(), rp6502_asset(),
# rp6502_xram(), rp6502_byproducts(), and the fetch that keeps this
# directory current.
#
# Update with:  cmake -P tools/rp6502.cmake
#
cmake_minimum_required(VERSION 3.21)

set(RP6502_TOOLS_REPO "picocomputer/rp6502")
set(RP6502_TOOLS_REF "main")
set(RP6502_EMU_RELEASE "latest")

set(RP6502_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "RP6502 tools directory")
get_filename_component(RP6502_PROJECT_DIR "${RP6502_TOOLS_DIR}" DIRECTORY)

# Where find_package(cc65) looks.
set(cc65_DIR "${RP6502_TOOLS_DIR}")

# Rename over the target, so a dead network leaves the working tool in place.
function(rp6502_fetch_tool name hash)
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

# Both lists this file reads are sha256sum(1) output: the tools in the
# repository and the assets on a release. Returns name=hash pairs.
function(rp6502_read_sums file out_var)
    file(STRINGS "${file}" lines)
    set(entries)
    foreach(line IN LISTS lines)
        if(line MATCHES "^([0-9a-fA-F]+)[ \t]+([^ \t/\\\\]+)$")
            list(APPEND entries "${CMAKE_MATCH_2}=${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out_var} "${entries}" PARENT_SCOPE)
endfunction()

# What to fetch, and what it should hash to, comes from the server.
function(rp6502_fetch_sums out_var)
    rp6502_fetch_tool(SHA256SUMS "")
    rp6502_read_sums("${RP6502_TOOLS_DIR}/SHA256SUMS" files)
    file(REMOVE "${RP6502_TOOLS_DIR}/SHA256SUMS")
    if(NOT files)
        message(FATAL_ERROR "tools/SHA256SUMS lists nothing to fetch.")
    endif()
    set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

# One release archive, reduced to the executable it carries.
function(rp6502_fetch_emu suffix member exe)
    if(RP6502_EMU_RELEASE STREQUAL "latest")
        set(base "https://github.com/${RP6502_TOOLS_REPO}/releases/latest/download")
    else()
        set(base "https://github.com/${RP6502_TOOLS_REPO}/releases/download/${RP6502_EMU_RELEASE}")
    endif()
    set(tmp "${RP6502_TOOLS_DIR}/${exe}.tmp")
    file(REMOVE_RECURSE "${tmp}")
    file(MAKE_DIRECTORY "${tmp}")
    file(DOWNLOAD "${base}/SHA256SUMS" "${tmp}/SHA256SUMS"
        STATUS status
        TLS_VERIFY ON
        INACTIVITY_TIMEOUT 30
    )
    list(GET status 0 code)
    list(GET status 1 text)
    if(NOT code EQUAL 0)
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: cannot fetch ${base}/SHA256SUMS\n${text}")
        return()
    endif()
    rp6502_read_sums("${tmp}/SHA256SUMS" assets)
    set(name)
    foreach(asset IN LISTS assets)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${asset}")
        # Read out before the next match overwrites them.
        set(asset_name "${CMAKE_MATCH_1}")
        set(asset_hash "${CMAKE_MATCH_2}")
        if(asset_name MATCHES "-${suffix}$")
            set(name "${asset_name}")
            set(hash "${asset_hash}")
        endif()
    endforeach()
    if(NOT name)
        file(REMOVE_RECURSE "${tmp}")
        message(NOTICE "No emulator: release ${RP6502_EMU_RELEASE} has no ${suffix}")
        return()
    endif()
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
endfunction()

function(rp6502_fetch_emulator)
    cmake_host_system_information(RESULT host QUERY OS_NAME)
    if(host STREQUAL "Windows")
        rp6502_fetch_emu("windows.zip" "rp6502-emu.exe" "rp6502-emu.exe")
        return()
    endif()
    if(host STREQUAL "Darwin")
        rp6502_fetch_emu("macos.zip"
            "rp6502-emu.app/Contents/MacOS/rp6502-emu" "rp6502-emu")
        return()
    endif()
    if(NOT host STREQUAL "Linux")
        return()
    endif()
    cmake_host_system_information(RESULT release QUERY OS_RELEASE)
    cmake_host_system_information(RESULT machine QUERY OS_PLATFORM)
    # WSL runs the Windows build through interop.
    if(release MATCHES "[Mm]icrosoft")
        rp6502_fetch_emu("windows.zip" "rp6502-emu.exe" "rp6502-emu.exe")
    endif()
    if(machine STREQUAL "x86_64" OR machine STREQUAL "aarch64")
        rp6502_fetch_emu("linux-${machine}.tar.gz" "rp6502-emu" "rp6502-emu")
    endif()
endfunction()

# Hooks patch config files.
function(rp6502_hook_tasks_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/tasks.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" json)
    if(json MATCHES "RP6502: update tools")
        return()
    endif()
    # Spliced as text, not through string(JSON), which rejects the trailing
    # commas VS Code allows and drops every comment on rewrite.
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

if(CMAKE_SCRIPT_MODE_FILE AND NOT RP6502_TOOLS_RELOADED)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_before)
    rp6502_fetch_sums(rp6502_tools_files)
    foreach(entry IN LISTS rp6502_tools_files)
        string(REGEX MATCH "^(.+)=([0-9a-fA-F]+)$" ignored "${entry}")
        rp6502_fetch_tool("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endforeach()
    set(RP6502_TOOLS_FETCHED TRUE)
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_after)
    if(NOT rp6502_tools_after STREQUAL rp6502_tools_before)
        set(RP6502_TOOLS_RELOADED TRUE)
        include("${CMAKE_CURRENT_LIST_FILE}")
        return()
    endif()
endif()

if(RP6502_TOOLS_FETCHED)
    rp6502_hook_tasks_json()
    rp6502_fetch_emulator()
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

if(DEFINED CC65_TARGET_SYSTEM)
    find_package(cc65 REQUIRED)
elseif(DEFINED LLVM_MOS_PLATFORM)
    find_package(llvm-mos-sdk REQUIRED)
else()
    message(FATAL_ERROR
        "No compiler selected.\n"
        "Configure with a CMake preset; cmake --list-presets shows them. "
        "Without presets, set CC65_TARGET_SYSTEM or LLVM_MOS_PLATFORM.")
endif()

# cc65 links a flat image at a fixed address;
# llvm-mos writes the address into the start of its output file.
function(rp6502_default_address var)
    if(CMAKE_C_COMPILER_ID STREQUAL "cc65")
        set(${var} 0x200 PARENT_SCOPE)
    else()
        set(${var} file PARENT_SCOPE)
    endif()
endfunction()

# Package the target as an RP6502 ROM.
#
# RP6502 Executable ROM
# ^^^^^^^^^^^^^^^^^^^^^
#
#  rp6502_executable(<name>
#                    [DATA <addr>]
#                    [NMI <addr>]
#                    [RESET <addr>]
#                    [IRQ <addr>]
#                    roms...)
#
# Packages the executable produced by ``<name>`` into RP6502 ROM format.
# Merges with specified ``roms...`` and those created with rp6502_asset().
# When ``DATA`` is omitted, the linker output of ``<name>`` is not
# included in the merge; only the asset/extra ROMs are bundled.
# The word `file` may be used for any <addr> indicating the
# address is to be read from the linker output in this order:
# ``DATA <addr>`` Starting memory address to load data.
# ``NMI <addr>`` Address for NMI to be stored at $FFFA-$FFFB.
# ``RESET <addr>`` Address for RESET to be stored at $FFFC-$FFFD.
# ``IRQ <addr>`` Address for IRQ to be stored at $FFFE-$FFFF.
# The word `default` may be used for any <addr> to take the compiler's
# own convention for where its linker output loads.
#
function(rp6502_executable name)
    # Parse args
    set(data_addr "none")
    set(nmi_addr "none")
    set(reset_addr "none")
    set(irq_addr "none")
    set(extra_roms)
    foreach(X IN LISTS ARGN)
        if (NOT data_addr)
            set(data_addr ${X})
        elseif (NOT reset_addr)
            set(reset_addr ${X})
        elseif (NOT irq_addr)
            set(irq_addr ${X})
        elseif (NOT nmi_addr)
            set(nmi_addr ${X})
        elseif (X STREQUAL "DATA")
            set(data_addr FALSE)
        elseif (X STREQUAL "RESET")
            set(reset_addr FALSE)
        elseif (X STREQUAL "IRQ")
            set(irq_addr FALSE)
        elseif (X STREQUAL "NMI")
            set(nmi_addr FALSE)
        else ()
            list(APPEND extra_roms ${X})
        endif ()
    endforeach()
    rp6502_default_address(default_addr)
    foreach(V data_addr nmi_addr reset_addr irq_addr)
        if (${V} STREQUAL "default")
            set(${V} "${default_addr}")
        endif()
    endforeach()
    # Resolve relative extra_roms against current source dir
    set(all_extra_roms)
    foreach(rom IN LISTS extra_roms)
        if (IS_ABSOLUTE "${rom}")
            list(APPEND all_extra_roms "${rom}")
        else()
            list(APPEND all_extra_roms "${CMAKE_CURRENT_SOURCE_DIR}/${rom}")
        endif()
    endforeach()
    # Collect asset ROMs registered by rp6502_asset()
    get_target_property(asset_roms ${name} RP6502_ASSET_ROMS)
    if (asset_roms)
        list(APPEND all_extra_roms ${asset_roms})
    endif()
    # Build the rp6502.py merge command
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(rom_file "${CMAKE_CURRENT_BINARY_DIR}/${name}.rp6502")
    set(tool_command "${Python3_EXECUTABLE}"
        "${RP6502_TOOLS_DIR}/rp6502.py"
    )
    set(executable_inputs)
    if (NOT data_addr STREQUAL "none")
        list(APPEND tool_command -a "${data_addr}")
        list(APPEND executable_inputs "$<TARGET_FILE:${name}>")
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
    # The ROM is its own buildable artifact, depending on the executable
    # (when DATA is given) plus every asset rom.
    add_custom_command(
        OUTPUT "${rom_file}"
        DEPENDS ${executable_inputs} ${all_extra_roms}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${rom_file}"
        COMMAND ${tool_command}
        VERBATIM
    )
    add_custom_target(${name}_rp6502 ALL DEPENDS "${rom_file}")
    # A layout that fails its checks must not produce a ROM.
    get_directory_property(xram_checks RP6502_XRAM_CHECKS)
    if (xram_checks)
        add_dependencies(${name}_rp6502 ${xram_checks})
    endif()
    set_property(DIRECTORY APPEND PROPERTY RP6502_ROM_TARGETS "${name}_rp6502")
    # Mark that rp6502_executable has been called for this target
    set_property(TARGET ${name} PROPERTY RP6502_EXECUTABLE_CALLED TRUE)
endfunction()

# Package anything as an RP6502 asset ROM.
#
# RP6502 Asset ROM
# ^^^^^^^^^^^^^^^^
#
#  rp6502_asset(<name> <address> <in_file>)
#
# If the address is numeric, the in_file will be loaded into
# RAM ($0-FFFF) or XRAM ($10000-1FFFF) when the ROM is loaded.
# Non-numeric addresses become filenames that can be opened
# with "ROM:filename" from a micro filesystem in the ROM.
# Writing the address as RAM(<x>) or XRAM(<x>) checks that it is in range,
# and XRAM() sets the bit that tells XRAM from RAM, so an offset from
# rp6502_xram() loads into XRAM. Inside the parentheses, <x> is a number
# or the name of a CMake variable.
#
function(rp6502_asset name)
    get_target_property(executable_called ${name} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_asset(${name} ...) must be registered BEFORE calling rp6502_executable()."
        )
    endif()
    # CMake gives every parenthesis to a command as an argument of its own,
    # so RAM(<x>) arrives here as four arguments and nothing named RAM or
    # XRAM is ever defined.
    set(args ${ARGN})
    list(LENGTH args argc)
    list(GET args 0 addr)
    if (addr STREQUAL "RAM" OR addr STREQUAL "XRAM")
        set(form "${addr}")
        if (NOT argc EQUAL 5)
            message(FATAL_ERROR "rp6502_asset(${name} ${form}(<address>) <in_file>)")
        endif()
        list(GET args 1 opened)
        list(GET args 3 closed)
        if (NOT opened STREQUAL "(" OR NOT closed STREQUAL ")")
            message(FATAL_ERROR "rp6502_asset(${name} ${form}(<address>) <in_file>)")
        endif()
        list(GET args 2 value)
        if (DEFINED ${value})
            set(value "${${value}}")
        endif()
        set(written "${value}")
        # A leading $ is how a 6502 program writes hex, which rp6502.py
        # takes as well.
        string(REGEX REPLACE "^\\$" "0x" value "${value}")
        if (NOT value MATCHES "^[-+]?(0[xX][0-9a-fA-F]+|[0-9]+)$")
            message(FATAL_ERROR
                "rp6502_asset(${name} ${form}(...)): ${written} is not a number.")
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
                "rp6502_asset(${name} ${form}(...)): ${written} is outside ${form}, which ends at ${ends}.")
        endif()
        if (form STREQUAL "XRAM")
            math(EXPR value "${value} | 0x10000")
        endif()
        math(EXPR addr "${value}" OUTPUT_FORMAT HEXADECIMAL)
        list(GET args 4 in_file)
    else()
        if (NOT argc EQUAL 2)
            message(FATAL_ERROR "rp6502_asset(<name> <address> <in_file>)")
        endif()
        list(GET args 1 in_file)
    endif()
    get_filename_component(src_file "${in_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH rel_path "${CMAKE_SOURCE_DIR}" "${src_file}")
    if (rel_path MATCHES "^\\.\\.")
        get_filename_component(rel_path "${src_file}" NAME)
    endif()
    string(MAKE_C_IDENTIFIER "${addr}" key)
    set(out_file "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.rp6502/${key}/${rel_path}")
    get_filename_component(out_dir "${out_file}" DIRECTORY)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT "${out_file}"
        DEPENDS "${src_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND "${Python3_EXECUTABLE}"
                "${RP6502_TOOLS_DIR}/rp6502.py"
                -a "${addr}"
                -o "${out_file}"
                create "${src_file}"
        VERBATIM
    )
    set_property(TARGET ${name} APPEND PROPERTY
        RP6502_ASSET_ROMS "${out_file}"
    )
endfunction()

# Give CMake the addresses a header defines.
#
# RP6502 XRAM Layout
# ^^^^^^^^^^^^^^^^^^
#
#  rp6502_xram(<header> <regex> [<unaligned_regex>])
#
# Reads the ``#define`` lines of ``<header>`` whose name matches
# ``<regex>`` and sets each name as a variable holding the value the
# header computes. The names then work as rp6502_asset() addresses.
# Names matching ``<unaligned_regex>`` are exempt from 16-bit alignment.
# A commented out define, a define with no value, and a function-like
# macro are all skipped.
#
# The layout is checked while the project builds rather than while it
# configures, so a header that will not compile still leaves a configured
# project behind, and every problem is reported by the compiler against
# the line in the header.
#
function(rp6502_xram header regex)
    set(unaligned "${ARGV2}")
    get_filename_component(header_file "${header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(header_name "${header_file}" NAME)
    get_filename_component(header_dir "${header_file}" DIRECTORY)
    get_filename_component(stem "${header_file}" NAME_WE)
    get_directory_property(rom_targets RP6502_ROM_TARGETS)
    if (rom_targets)
        message(FATAL_ERROR
            "rp6502_xram(${header}) must be registered BEFORE calling rp6502_executable()."
        )
    endif()
    # Editing the layout has to configure again, since these values are read
    # at configure time.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header_file}")

    # Read the header a line at a time. file(STRINGS), and every idiom that
    # escapes the text and splits it, join a line ending in a backslash to
    # the line after it, which moves every line number that follows.
    # Nothing here reads comments or conditionals, because each name is
    # written out under an #ifdef and the preprocessor settles what exists.
    file(READ "${header_file}" rest)
    string(REPLACE "\r\n" "\n" rest "${rest}")
    set(names)
    set(lines)
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

        # A continued definition is read under the number of its first line.
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
            if (cur MATCHES "^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)([^A-Za-z0-9_(].*)$")
                set(name "${CMAKE_MATCH_1}")
                string(STRIP "${CMAKE_MATCH_2}" value)
                # An include guard has no value and only integers are carried.
                if (NOT value STREQUAL "" AND NOT value MATCHES "^[\"']"
                        AND name MATCHES "^(${regex})$")
                    list(APPEND names "${name}")
                    list(APPEND lines "${cur_line}")
                    list(APPEND values "${value}")
                endif()
            endif()
        endif()
        if (last)
            break()
        endif()
    endwhile()

    # Every structure an offsetof names is probed below, whatever else is
    # written around it, and the first name that uses one carries its line.
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
                # Any one of them being defined is enough to probe with.
                list(GET probe_guards ${index} guard)
                list(REMOVE_AT probe_guards ${index})
                list(INSERT probe_guards ${index} "${guard} || defined(${name})")
            endif()
        endif()
    endforeach()

    set(dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${stem}.xram")
    string(REPLACE "\\" "/" header_c "${header_file}")

    # A program that prints the values. Everything is unsigned long, so
    # neither compiler's 16 bit size_t truncates what it prints. Included by
    # name with -I below, because cc65 cannot find a quoted include given as
    # an absolute path.
    set(stub "#include <stdio.h>\n#include \"${header_name}\"\n\nint main(void)\n{\n")
    foreach(name line IN ZIP_LISTS names lines)
        string(APPEND stub
            "#ifdef ${name}\n"
            "#line ${line} \"${header_c}\"\n"
            "    printf(\"${name} 0x%lX\\n\", (unsigned long)${name});\n"
            "#endif\n")
    endforeach()
    string(APPEND stub "    return 0;\n}\n")
    file(WRITE "${dir}/xram_stub.c" "${stub}")

    # A program of assertions, compiled but never run, so the compiler
    # reports a bad layout against the line in the header. Every name gets a
    # line of its own, since a name with no assertion would let a define
    # that will not compile through. Each declaration is one line, because
    # cc65 reports the line it finished reading rather than the one it
    # started on.
    set(check "#include <stddef.h>\n#include \"${header_name}\"\n\n")
    string(APPEND check
        "/* size_t is 16 bits, so a structure larger than 64K wraps instead of\n"
        "   being refused. An array of one is refused outright, because the\n"
        "   size of an array is checked against the largest object the target\n"
        "   allows and not against the truncated sizeof. */\n")
    # The leading underscore is the namespace C keeps for file scope, which
    # is the one place a name here cannot collide with the header's own.
    foreach(type line guard IN ZIP_LISTS probes probe_lines probe_guards)
        string(MAKE_C_IDENTIFIER "${type}" probe)
        string(APPEND check
            "\n#if ${guard}\n#line ${line} \"${header_c}\"\n"
            "extern ${type} _xram_fits_${probe}[1];\n#endif\n")
    endforeach()
    # An offset and arithmetic between offsets are size_t, so 16 bits, and
    # can never trip this. A number that does not fit is a long and does.
    foreach(name line IN ZIP_LISTS names lines)
        string(APPEND check
            "\n#ifdef ${name}\n#line ${line} \"${header_c}\"\n"
            "_Static_assert((${name}) < 0x10000L,"
            " \"${name} overflows 16 bits.\");\n")
        if (NOT unaligned OR NOT name MATCHES "^(${unaligned})$")
            string(APPEND check
                "#line ${line} \"${header_c}\"\n"
                "_Static_assert(!((${name}) & 1), \"${name} is unaligned."
                " To allow, use the [<unaligned_regex>] in rp6502_xram.\");\n")
        endif()
        string(APPEND check "#endif\n")
    endforeach()
    file(WRITE "${dir}/xram_check.c" "${check}")

    # cc65's CMAKE_C_COMPILER is a wrapper around cl65 that puts diagnostics
    # in the form an IDE matches, so both programs are built through it.
    set(compiler_args)
    if (CMAKE_C_COMPILER_ARG1)
        separate_arguments(compiler_args NATIVE_COMMAND "${CMAKE_C_COMPILER_ARG1}")
    endif()
    separate_arguments(flags NATIVE_COMMAND "${CMAKE_C_FLAGS}")

    set(failed FALSE)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" ${compiler_args} ${flags} -I "${header_dir}"
                -o "${dir}/xram_stub" "${dir}/xram_stub.c"
        WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    if (NOT result EQUAL 0)
        set(failed TRUE)
    endif()

    if (NOT failed)
        rp6502_default_address(load_addr)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${RP6502_TOOLS_DIR}/rp6502.py"
                    -a "${load_addr}" -r "${load_addr}"
                    -o "${dir}/xram_stub.rp6502" create "${dir}/xram_stub"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output
        )
        if (NOT result EQUAL 0)
            set(failed TRUE)
        endif()
    endif()

    if (NOT failed)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${RP6502_TOOLS_DIR}/rp6502.py"
                    -c "${RP6502_PROJECT_DIR}/.rp6502"
                    execute "${dir}/xram_stub.rp6502"
            TIMEOUT 60
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output
        )
        if (NOT result EQUAL 0)
            set(failed TRUE)
        endif()
    endif()

    # A failure here is the header's, and the build reports it in full, so
    # the configure finishes with every address at zero rather than leaving
    # the project unconfigured.
    string(REPLACE "\r" "" output "${output}")
    foreach(name IN LISTS names)
        if (failed)
            set(${name} 0 PARENT_SCOPE)
        elseif (output MATCHES "(^|\n)${name} (0x[0-9A-Fa-f]+)")
            # Out of range is zeroed, so it reaches rp6502_asset() as nothing
            # while the build reports it against the header.
            set(found "${CMAKE_MATCH_2}")
            math(EXPR numeric "${found}")
            if (numeric GREATER 65535)
                set(found 0)
            endif()
            set(${name} "${found}" PARENT_SCOPE)
        else()
            # The preprocessor skipped it, so it is not a name at all.
            unset(${name} PARENT_SCOPE)
        endif()
    endforeach()
    # A header that will not compile is reported by the compile below, but a
    # tool that did not run leaves a header that compiles and every address
    # at zero, which would otherwise build a ROM that loads everything over
    # the start of XRAM. So the reason is carried to the build and fails it.
    set(unread)
    if (failed)
        message(STATUS "rp6502_xram(${header}) read no addresses; the build reports why.")
        file(WRITE "${dir}/xram_unread.txt"
            "rp6502_xram(${header}) read no addresses, so every name is zero.\n${output}\n")
        set(unread
            COMMAND "${CMAKE_COMMAND}" -E cat "${dir}/xram_unread.txt"
            COMMAND "${CMAKE_COMMAND}" -E false)
    endif()

    # Target names are global, and one header can be shared by two
    # directories, so the name carries where it was called from.
    file(RELATIVE_PATH id "${CMAKE_SOURCE_DIR}" "${header_file}")
    if (id MATCHES "^\\.\\.")
        set(id "${header_file}")
    endif()
    string(MAKE_C_IDENTIFIER "${id}" id)
    string(MD5 hash "${CMAKE_CURRENT_BINARY_DIR}|${header_file}")
    string(SUBSTRING "${hash}" 0 8 hash)
    set(target "rp6502_xram_${id}_${hash}")
    add_custom_command(
        OUTPUT "${dir}/xram_check.stamp"
        DEPENDS "${header_file}" "${dir}/xram_check.c"
        COMMAND "${CMAKE_C_COMPILER}" ${compiler_args} ${flags} -I "${header_dir}"
                -c -o "${dir}/xram_check.o" "${dir}/xram_check.c"
        ${unread}
        COMMAND "${CMAKE_COMMAND}" -E touch "${dir}/xram_check.stamp"
        COMMENT "Checking ${header_name}"
        VERBATIM
    )
    add_custom_target(${target} ALL DEPENDS "${dir}/xram_check.stamp")
    set_property(DIRECTORY APPEND PROPERTY RP6502_XRAM_CHECKS "${target}")
endfunction()

# Declare files as byproducts of building <target>.
#
# RP6502 Byproducts
# ^^^^^^^^^^^^^^^^^
#
#  rp6502_byproducts(<target> <file>...)
#
# Some linker configurations write extra outputs alongside the main
# executable. CMake's add_executable() does not model these.
#
function(rp6502_byproducts target)
    add_custom_command(
        OUTPUT ${ARGN}
        DEPENDS ${target}
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${ARGN}
        VERBATIM
    )
endfunction()
