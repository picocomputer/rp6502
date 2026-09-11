# Inside a function body CMAKE_CURRENT_LIST_DIR names the file that called the
# function, so the directory this file lives in is captured here for
# rp6502_emu_debugger below.
set(RP6502_SOKOL ${CMAKE_CURRENT_LIST_DIR})

rp6502_submodule(vendor/sokol SENTINEL sokol_app.h
    WANTS "the emulator's window, input and audio")

set(RP6502_EMU_APP
    ${CMAKE_CURRENT_LIST_DIR}/cli/cli.c
    ${CMAKE_CURRENT_LIST_DIR}/app/input.c
    ${CMAKE_CURRENT_LIST_DIR}/cli/png.c
    ${CMAKE_CURRENT_LIST_DIR}/cli/script.c
    ${CMAKE_CURRENT_LIST_DIR}/cli/state.c
    ${RP6502_SRC}/core/sys/crc32.c
    ${RP6502_SRC}/core/sys/version.c)

set(RP6502_EMU_WINDOW
    ${CMAKE_CURRENT_LIST_DIR}/app/sokol.c
    ${CMAKE_CURRENT_LIST_DIR}/app/icon.c
    ${CMAKE_CURRENT_LIST_DIR}/app/app.c
    ${CMAKE_CURRENT_LIST_DIR}/app/prompt.c
    ${CMAKE_CURRENT_LIST_DIR}/app/gfx.c)

set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/app/sokol.c
    PROPERTIES COMPILE_DEFINITIONS SOKOL_IMPL)

function(rp6502_emu_debugger tgt)
    rp6502_submodule(vendor/imgui SENTINEL imgui.cpp
        WANTS "the debugger's interface")
    rp6502_submodule(vendor/cppdap SENTINEL CMakeLists.txt
        WANTS "the debug adapter")
    # cppdap keeps its JSON library as a submodule of its own and fails to
    # configure without it, so that submodule is asked for by name here.
    rp6502_submodule(third_party/json SUPER ${RP6502_VENDOR}/cppdap
        SENTINEL include/nlohmann/json.hpp
        WANTS "cppdap's JSON backend")

    set(CPPDAP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CPPDAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CPPDAP_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RP6502_VENDOR}/cppdap cppdap)

    # -Warray-bounds mis-reads cppdap's dap::any scalar storage
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
    # CHIPS_UI_IMPL and SOKOL_IMGUI_IMPL each emit an implementation, so each
    # belongs to exactly one translation unit.
    set_source_files_properties(${RP6502_SOKOL}/dbg/imgui_impl.cc
        PROPERTIES COMPILE_DEFINITIONS "SOKOL_IMGUI_IMPL")
    set_source_files_properties(${RP6502_SOKOL}/dbg/dbgui.cc
        PROPERTIES COMPILE_DEFINITIONS "CHIPS_UI_IMPL")
endfunction()
