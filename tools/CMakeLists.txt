# The RP6502 project tools, and the thing that keeps them current.
#
# This file lives in picocomputer/rp6502 and is copied into a project by the
# stub the template ships. Both copies carry the same fetch loop, so the stub
# replaces itself with this on the first configure and every later update is
# the same code path.
#
# Update from the command line with:
#
#   cmake -P tools/CMakeLists.txt
#
# which re-fetches everything and re-runs the config hooks. A plain configure
# only fetches what is missing, so a warm tree costs one stat per file and
# never touches the network.
#
# It is include()d rather than add_subdirectory()d because a toolchain file
# has to exist before project() runs, and add_subdirectory() cannot happen
# that early. Functions are global once defined, so a project still calling
# add_subdirectory(tools) keeps its rp6502_* commands — only the fetch is too
# late to help it.

cmake_minimum_required(VERSION 3.21)

# Where the tools come from, and the four files that make up the set.
# TODO: back to main once the centraltools work is tested.
set(RP6502_TOOLS_REPO "picocomputer/rp6502")
set(RP6502_TOOLS_REF "centraltools")
set(RP6502_TOOLS_FILES CMakeLists.txt cc65-config.cmake cc65.cmake rp6502.py)

# Cached, not scoped: the commands below need this at call time, and
# CMAKE_CURRENT_LIST_DIR inside a function is the caller's file, not ours. A
# plain variable would be invisible to the parent of a project that reaches
# this file through add_subdirectory(tools). INTERNAL rewrites every
# configure, so moving the project cannot leave a stale path behind.
set(RP6502_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "RP6502 tools directory")
get_filename_component(RP6502_PROJECT_DIR "${RP6502_TOOLS_DIR}" DIRECTORY)

# find_package(cc65) looks here. Saying it once means a project needs only the
# two lines that pick a compiler, with no third to point at this directory.
set(cc65_DIR "${RP6502_TOOLS_DIR}")

# Fetch to a temporary name and rename over the target, so a dead network or a
# 404 leaves a working tool in place instead of a truncated one.
function(rp6502_fetch_tool name)
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
    file(RENAME "${out}.tmp" "${out}")
endfunction()

# Add the update task to a project made before this file existed. Hooks patch
# what is there and never create it: a missing .vscode/tasks.json means it was
# deleted by someone not using VS Code, and putting it back would be this file
# arguing with them.
function(rp6502_hook_tasks_json)
    set(file "${RP6502_PROJECT_DIR}/.vscode/tasks.json")
    if(NOT EXISTS "${file}")
        return()
    endif()
    file(READ "${file}" json)
    if(json MATCHES "RP6502: update tools")
        return()
    endif()
    # Spliced in as text rather than through string(JSON), which would reject
    # the trailing commas VS Code allows and drop every comment on rewrite.
    set(task [==[

        {
            "label": "RP6502: update tools",
            "type": "process",
            "command": "cmake",
            "args": [
                "-P",
                "${workspaceFolder}/tools/CMakeLists.txt"
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

# Script mode is the explicit update, so everything comes down again. A plain
# configure only fills gaps. Not on the second pass below, where the fetch has
# already happened and repeating it would just cost another round trip.
if(CMAKE_SCRIPT_MODE_FILE AND NOT RP6502_TOOLS_RELOADED)
    set(RP6502_TOOLS_FORCE TRUE)
endif()

file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_before)
foreach(name IN LISTS RP6502_TOOLS_FILES)
    if(RP6502_TOOLS_FORCE OR NOT EXISTS "${RP6502_TOOLS_DIR}/${name}")
        rp6502_fetch_tool("${name}")
        set(RP6502_TOOLS_FETCHED TRUE)
    endif()
endforeach()

# We may have just replaced ourselves. CMake is running the copy it read at
# the top, so hand off to the one now on disk; it finds nothing missing and
# does not come back here.
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" rp6502_tools_after)
if(NOT rp6502_tools_after STREQUAL rp6502_tools_before)
    set(RP6502_TOOLS_FORCE FALSE)
    set(RP6502_TOOLS_RELOADED TRUE)
    include("${CMAKE_CURRENT_LIST_FILE}")
    return()
endif()

if(RP6502_TOOLS_FETCHED)
    rp6502_hook_tasks_json()
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    return()
endif()

# The compiler is chosen by the project, before project(), with one of:
#
#   set(CC65_TARGET_SYSTEM rp6502)
#   find_package(cc65 REQUIRED)
#
#   set(LLVM_MOS_PLATFORM rp6502)
#   find_package(llvm-mos-sdk REQUIRED)
#
function(rp6502_require_toolchain)
    message(FATAL_ERROR
        "No compiler selected.\n"
        "Select a CMake preset (cc65 or llvm-mos), or edit CMakeLists.txt and "
        "uncomment one of set(CC65_TARGET_SYSTEM rp6502) or "
        "set(LLVM_MOS_PLATFORM rp6502).")
endfunction()

# Where the linker output loads, for `default` below. cc65 links a flat image
# at a fixed address; llvm-mos writes the address into the first two bytes,
# which the tool spells `file`.
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
# convention for where its linker output loads, which is what lets one
# project build under either compiler.
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
    # Resolve `default` now that the compiler is known
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
    # CMAKE_CURRENT_LIST_DIR inside a function is the directory of the file
    # that defined it, so this is tools/ no matter which scope calls us.
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
    # Mark that rp6502_executable has been called for this target
    set_property(TARGET ${name} PROPERTY RP6502_EXECUTABLE_CALLED TRUE)
endfunction()

# Package anything as an RP6502 asset ROM.
#
# RP6502 Asset ROM
# ^^^^^^^^^^^^^^^^
#
#  rp6502_asset(<name> address in_file)
#
# If the address is numeric, the in_file will be loaded into
# RAM ($0-FFFF) or XRAM ($10000-1FFFF) when the ROM is loaded.
# Non-numeric addresses become filenames that can be opened
# with "ROM:filename" from a micro filesystem in the ROM.
#
function(rp6502_asset name addr in_file)
    get_target_property(executable_called ${name} RP6502_EXECUTABLE_CALLED)
    if (executable_called)
        message(FATAL_ERROR
            "rp6502_asset(${name} ...) must be registered BEFORE calling rp6502_executable()."
        )
    endif()
    get_filename_component(src_file "${in_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    file(RELATIVE_PATH rel_path "${CMAKE_SOURCE_DIR}" "${src_file}")
    if (rel_path MATCHES "^\\.\\.")
        get_filename_component(rel_path "${src_file}" NAME)
    endif()
    set(out_file "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.rp6502/${rel_path}")
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
