# The application around emu_core: the window, the command line, the debugger.
#
# Two lists, in the order the link wants them: the program, then the window.
# What goes between is the host's — main() for everyone who enters through
# one, and the browser's exported entry points — so a root writes
#
#     ${RP6502_EMU_APP} main.c ${RP6502_EMU_WINDOW} entry.c ...
#
# and Android, which enters through its own sokol_main, simply omits main.c.
#
# rp6502_emu_debugger(<target>) adds the imgui and DAP halves. Web and Android
# ship without it, so it is a call rather than something this include does.

# This directory, captured for the debugger function below: inside a function
# body CMAKE_CURRENT_LIST_DIR is the caller's file, not this one.
set(RP6502_SOKOL ${CMAKE_CURRENT_LIST_DIR})

rp6502_submodule(vendor/sokol SENTINEL sokol_app.h
    WANTS "the emulator's window, input and audio")

set(RP6502_EMU_APP
    ${CMAKE_CURRENT_LIST_DIR}/cli/cli.c
    ${CMAKE_CURRENT_LIST_DIR}/app/input.c
    ${CMAKE_CURRENT_LIST_DIR}/cli/png.c
    ${CMAKE_CURRENT_LIST_DIR}/cli/script.c
    ${RP6502_SRC}/core/sys/crc32.c
    ${RP6502_SRC}/core/sys/version.c)

# The shared render core. A platform's own entry.c stands on it.
set(RP6502_EMU_WINDOW
    ${CMAKE_CURRENT_LIST_DIR}/app/sokol.c
    ${CMAKE_CURRENT_LIST_DIR}/app/icon.c
    ${CMAKE_CURRENT_LIST_DIR}/app/app.c
    ${CMAKE_CURRENT_LIST_DIR}/app/prompt.c
    ${CMAKE_CURRENT_LIST_DIR}/app/gfx.c)

set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/app/sokol.c
    PROPERTIES COMPILE_DEFINITIONS SOKOL_IMPL)

# The on-screen debugger and the DAP server. Two submodules and a third under
# one of them, all asked for here so a host that wants none of it pays for
# none of it.
function(rp6502_emu_debugger tgt)
    rp6502_submodule(vendor/imgui SENTINEL imgui.cpp
        WANTS "the debugger's interface")
    # cppdap carries its own JSON library as a submodule, and fails hard
    # without it rather than degrading, so it is asked for by name.
    rp6502_submodule(vendor/cppdap SENTINEL CMakeLists.txt
        WANTS "the debug adapter")
    rp6502_submodule(third_party/json SUPER ${RP6502_VENDOR}/cppdap
        SENTINEL include/nlohmann/json.hpp
        WANTS "cppdap's JSON backend")

    # cppdap (the DAP server library): no examples/tests/fuzzer, vendored
    # nlohmann/json. Exposes the `cppdap` target with its include dir.
    set(CPPDAP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CPPDAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CPPDAP_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RP6502_VENDOR}/cppdap cppdap)

    # GCC's -Warray-bounds mis-reads cppdap's dap::any scalar storage
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(cppdap PRIVATE -Wno-array-bounds)
    endif()

    target_sources(${tgt} PRIVATE
        ${RP6502_SRC}/core/dap/cc65dbg.c
        ${RP6502_SRC}/core/dap/dap.cpp
        ${RP6502_SOKOL}/dbg/dbgui_layout.cc
        ${RP6502_SOKOL}/dbg/dbgui.cc
        ${RP6502_SRC}/core/dap/dwarf_cursor.c
        ${RP6502_SRC}/core/dap/dwarf_elf.c
        ${RP6502_SRC}/core/dap/dwarf_frame.c
        ${RP6502_SRC}/core/dap/dwarf_info.c
        ${RP6502_SRC}/core/dap/dwarf_line.c
        ${RP6502_SOKOL}/dbg/imgui_impl.cc
        ${RP6502_VENDOR}/imgui/imgui_draw.cpp
        ${RP6502_VENDOR}/imgui/imgui_tables.cpp
        ${RP6502_VENDOR}/imgui/imgui_widgets.cpp
        ${RP6502_VENDOR}/imgui/imgui.cpp)
    target_include_directories(${tgt} PRIVATE ${RP6502_VENDOR}/imgui)
    target_compile_definitions(${tgt} PRIVATE EMU_WITH_DEBUGGER UI_DBG_USE_W65C02 UI_DASM_USE_W65C02)
    target_link_libraries(${tgt} PRIVATE cppdap)
    # CHIPS_UI_IMPL / SOKOL_IMGUI_IMPL each belong to exactly one TU.
    set_source_files_properties(${RP6502_SOKOL}/dbg/imgui_impl.cc
        PROPERTIES COMPILE_DEFINITIONS "SOKOL_IMGUI_IMPL")
    set_source_files_properties(${RP6502_SOKOL}/dbg/dbgui.cc
        PROPERTIES COMPILE_DEFINITIONS "CHIPS_UI_IMPL")
endfunction()
