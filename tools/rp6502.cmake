# The RP6502 project tools: rp6502_executable(), rp6502_asset(),
# rp6502_byproducts(), and the fetch that keeps this directory current.
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
