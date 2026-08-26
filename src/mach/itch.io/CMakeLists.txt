# The emulator in a browser, and the itch.io bundle it assembles.
#
# The toolchain names itself, so `cmake -S src/mach/itch.io -B build/itch.io`
# is the whole invocation: host/emsdk/emsdk_toolchain.cmake fetches and
# installs Emscripten the first time. CMAKE_TOOLCHAIN_FILE is read by project() below
# and a -D on the command line still wins.

cmake_minimum_required(VERSION 3.21)

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${RP6502_SRC}/host/emsdk/emsdk_toolchain.cmake)
endif()

project(rp6502-emu-web C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

# First: it takes RP6502_CI out of the cache, so nothing may read it earlier.
include(${RP6502_ROOT}/version.cmake)

set(RP6502_HOST ${RP6502_SRC}/host/emsdk)
include(${RP6502_SRC}/core/emu.cmake)
# The browser has no <aio.h>, so this host brings its own fs seam and takes
# only the two files a POSIX host shares.
target_sources(emu_core PRIVATE
    ${RP6502_SRC}/host/posix/fs_dir.c
    ${RP6502_SRC}/host/posix/host.c
    ${RP6502_HOST}/fs.c
    ${RP6502_HOST}/host.c)

set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

include(${RP6502_SRC}/host/sokol/sokol.cmake)

add_executable(rp6502-emu
    ${RP6502_EMU_APP}
    ${RP6502_SRC}/host/sokol/main.c
    ${RP6502_HOST}/exports.c
    ${RP6502_EMU_WINDOW}
    ${RP6502_HOST}/window.c)
target_link_libraries(rp6502-emu PRIVATE emu_core)
target_compile_definitions(rp6502-emu PRIVATE SOKOL_NO_ENTRY SOKOL_GLES3)

# Browsers that back growable wasm memory with a resizable ArrayBuffer
# may still reject resizable views at TextDecoder/WebGL; keep the
# classic heap so every API handoff works everywhere.
target_link_options(rp6502-emu PRIVATE
    "-sUSE_WEBGL2=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sGROWABLE_ARRAYBUFFERS=0"
    "-sSTACK_SIZE=512KB"
    "-sFORCE_FILESYSTEM=1"
    "-lidbfs.js"
    "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,addRunDependency,removeRunDependency")

rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/sys/version.c)

# One web output: build/itch.io/bundle is the shippable itch.io sample, and
# the link writes rp6502.js/.wasm straight into it so the engine exists once.
# The tester at src/mach/itch.io/index.html is served from the source tree and
# reads the same bundle; nothing is copied for it.
set_target_properties(rp6502-emu PROPERTIES
    OUTPUT_NAME rp6502
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bundle")
add_custom_target(web_bundle ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/dist" "${CMAKE_BINARY_DIR}/bundle"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${RP6502_ROOT}/tests/roms/adventure.rp6502"
        "${CMAKE_BINARY_DIR}/bundle"
    COMMENT "Assembling web bundle (build/itch.io/bundle) + adventure.rp6502"
    VERBATIM)
add_dependencies(web_bundle rp6502-emu)

# The tester's ROM menu. Written here rather than shipped in the page so a
# new test ROM appears without editing it, and so its absence is how the
# page knows the web target has never been built. CONFIGURE_DEPENDS makes
# the build re-run cmake when the corpus changes.
file(GLOB WEB_ROMS CONFIGURE_DEPENDS "${RP6502_ROOT}/tests/roms/*.rp6502")
set(WEB_ROMS_JS "window.ROMS = [\n")
foreach(_rom IN LISTS WEB_ROMS)
    get_filename_component(_name "${_rom}" NAME)
    string(APPEND WEB_ROMS_JS "  \"${_name}\",\n")
endforeach()
string(APPEND WEB_ROMS_JS "];\n")
# The page has no other way to ask: the wasm answers --credits, but only
# once it has been fetched and started.
string(APPEND WEB_ROMS_JS "window.VERSION = \"${RP6502_VERSION_STAMP}\";\n")
file(WRITE "${CMAKE_BINARY_DIR}/roms.js" "${WEB_ROMS_JS}")
