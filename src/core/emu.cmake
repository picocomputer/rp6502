include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/chips SENTINEL chips/w65c02.h
    WANTS "the emulated 6502 and 6522")

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
set(RP6502_EMU_IPO FALSE)
if(ipo_ok AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RP6502_EMU_IPO TRUE)
endif()

include(${RP6502_SRC}/core/gen.cmake)
rp6502_gen_oemcp(oemcp)
rp6502_gen_rsmp_coef(rsmp_coef)

add_library(emu_core STATIC
    ${RP6502_SRC}/core/hid/hid_null.c
    ${RP6502_SRC}/core/sys/proc.c
    ${RP6502_SRC}/core/api/tim.c
    ${RP6502_SRC}/core/aud/mix.c
    ${RP6502_SRC}/core/aud/rsmp.c
    ${RP6502_SRC}/core/dap/dbg.c
    ${RP6502_SRC}/core/hid/hid.c
    ${RP6502_SRC}/core/hid/keyboard.c
    ${RP6502_SRC}/core/hid/mouse.c
    ${RP6502_SRC}/core/hid/gamepad.c
    ${RP6502_SRC}/core/hid/tablet.c
    ${RP6502_SRC}/core/hid/vtkeys.c
    ${RP6502_SRC}/core/rom/alias.c
    ${RP6502_SRC}/core/rom/rom.c
    ${RP6502_SRC}/core/rom/asset.c
    ${RP6502_SRC}/core/rom/pump.c
    ${RP6502_SRC}/core/sys/random.c
    ${RP6502_SRC}/core/sys/timer.c
    ${RP6502_SRC}/core/sys/config.c
    ${RP6502_SRC}/core/com/com.c
    ${RP6502_SRC}/core/com/pick.c
    ${RP6502_SRC}/core/com/tty.c
    ${RP6502_SRC}/core/wdc/bus.c
    ${RP6502_SRC}/core/wdc/phi2.c
    ${RP6502_SRC}/core/wdc/resb.c
    ${RP6502_SRC}/core/wdc/sram.c
    ${RP6502_SRC}/core/sys/xram.c
    ${RP6502_SRC}/core/ria/regs.c
    ${RP6502_SRC}/core/sys/pix.c
    ${RP6502_SRC}/core/api/xreg0.c
    ${RP6502_SRC}/core/api/xreg1.c
    ${RP6502_SRC}/core/ria/ria.c
    ${RP6502_SRC}/core/vga/vga.c
    ${RP6502_SRC}/core/wdc/via.c
    ${RP6502_SRC}/core/wdc/cpu.c
    ${RP6502_SRC}/core/sys/sys.c
    ${RP6502_SRC}/core/sys/sst.c
    ${RP6502_SRC}/core/api/api.c
    ${RP6502_SRC}/core/api/proc.c
    ${RP6502_SRC}/core/api/arg.c
    ${RP6502_SRC}/core/api/attr.c
    ${RP6502_SRC}/core/api/clk.c
    ${RP6502_SRC}/core/api/dir.c
    ${RP6502_SRC}/core/api/ops.c
    ${RP6502_SRC}/core/api/save.c
    ${RP6502_SRC}/core/str/path.c
    ${RP6502_SRC}/core/str/oem.c
    ${RP6502_SRC}/core/str/unicode.c
    ${OEMCP_C}
    ${RP6502_SRC}/core/api/std.c
    ${RP6502_SRC}/core/aud/sine.c
    ${RP6502_SRC}/core/aud/bel.c
    ${RP6502_SRC}/core/aud/bel_presets.c
    ${RP6502_SRC}/core/aud/opl.c
    ${RP6502_SRC}/core/aud/pcm.c
    ${RP6502_SRC}/core/aud/psg.c
    ${RP6502_SRC}/core/str/rln.c
    ${RP6502_SRC}/core/str/str.c
    ${RP6502_SRC}/core/vga/prog.c
    ${RP6502_SRC}/core/vga/canvas.c
    ${RP6502_SRC}/core/vga/mode/mode.c
    ${RP6502_SRC}/core/vga/mode/mode0.c
    ${RP6502_SRC}/core/vga/mode/mode1.c
    ${RP6502_SRC}/core/vga/mode/mode2.c
    ${RP6502_SRC}/core/vga/mode/mode3.c
    ${RP6502_SRC}/core/vga/mode/mode4.c
    ${RP6502_SRC}/core/vga/mode/mode5.c
    ${RP6502_SRC}/core/term/color.c
    ${RP6502_SRC}/core/term/font.c
    ${RP6502_SRC}/core/term/term.c
    ${RP6502_VENDOR}/emu8950/emu8950.c
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        ${RP6502_SRC}/core/vga/mode/mode1.c
        ${RP6502_SRC}/core/vga/mode/mode2.c
        ${RP6502_SRC}/core/vga/mode/mode3.c
        ${RP6502_SRC}/core/vga/mode/mode4.c
        ${RP6502_SRC}/core/vga/mode/mode5.c
        PROPERTIES COMPILE_OPTIONS "-Wno-pointer-to-int-cast"
    )
    # sram (core/wdc/sram.c) is a 64 KB tentative definition; -fno-common keeps it in
    # .bss (the modern GCC/Clang default) so macOS ld doesn't warn about reducing its
    # oversized __common alignment.
    target_compile_options(emu_core PRIVATE -fno-common)
    target_compile_options(emu_core PUBLIC
        $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(emu_core PRIVATE -Wno-c23-extensions)
    endif()
elseif(MSVC)
    set_source_files_properties(
        ${RP6502_SRC}/core/vga/mode/mode1.c
        ${RP6502_SRC}/core/vga/mode/mode2.c
        ${RP6502_SRC}/core/vga/mode/mode3.c
        ${RP6502_SRC}/core/vga/mode/mode4.c
        ${RP6502_SRC}/core/vga/mode/mode5.c
        PROPERTIES COMPILE_OPTIONS "/wd4311;/wd4312"
    )
endif()

set_source_files_properties(
    ${RP6502_VENDOR}/emu8950/emu8950.c
    PROPERTIES COMPILE_DEFINITIONS "USE_EMU8950_OPL=1"
)
if(MSVC)
    set_source_files_properties(
        ${RP6502_VENDOR}/emu8950/emu8950.c
        PROPERTIES COMPILE_OPTIONS "/wd4005"
    )
endif()

add_dependencies(emu_core rsmp_coef)
target_include_directories(emu_core PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${RP6502_SRC}
    ${RP6502_VENDOR}
)
target_compile_definitions(emu_core PUBLIC
    ROM_ALIAS_MAX=16 # --install is documented as repeatable to sixteen.
    _GNU_SOURCE
    RP6502_EXFAT=0
    RP6502_LOCALE=EN
    PICO_PROGRAM_NAME="RP6502-EMU")
include(${RP6502_SRC}/core/log.cmake)
rp6502_log_definitions(emu_core PUBLIC)
if(NOT MSVC)
    target_link_libraries(emu_core PUBLIC m)
endif()

set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
