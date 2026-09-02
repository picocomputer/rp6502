# emu_core: the machine as a library, for every host that runs it in software.
#
# What is here is what does not depend on which OS this is — the core's own
# sources, the tables generated from them, and the facts that belong to a
# compiler rather than a platform.
#
# A root names two things this file does not: the directory under src/osal that
# answers osal/os.h, through that seam's own cmake or a line of its own; and its
# own machine directory on emu_core's include path, for the drivers.h
# core/sys/sys.c includes by bare name.
#
# RP6502_EMU_IPO  whether this build asked for link-time optimization, so a root
#                 can hand it to everything it makes after emu_core. This file
#                 has already given it to emu_core and to the caller's scope.

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
    ${RP6502_SRC}/core/sys/exec.c
    ${RP6502_SRC}/core/api/tim.c
    ${RP6502_SRC}/core/aud/aud_mix.c
    ${RP6502_SRC}/core/aud/rsmp.c
    ${RP6502_SRC}/core/dap/dbg.c
    ${RP6502_SRC}/core/hid/hid.c
    ${RP6502_SRC}/core/hid/keyboard.c
    ${RP6502_SRC}/core/hid/mouse.c
    ${RP6502_SRC}/core/hid/gamepad.c
    ${RP6502_SRC}/core/hid/tablet.c
    # Two HID layers: the device layer every machine has is above, and these
    # are what a software machine answers where another has fabric or a
    # firmware -- the host already produced the characters, so there is no
    # layout engine here at all.
    ${RP6502_SRC}/core/hid/vtkeys.c
    ${RP6502_SRC}/core/rom/alias.c
    ${RP6502_SRC}/core/rom/rom.c
    ${RP6502_SRC}/core/rom/asset.c
    ${RP6502_SRC}/core/rom/pump.c
    ${RP6502_SRC}/core/sys/random.c
    ${RP6502_SRC}/core/sys/timer.c
    ${RP6502_SRC}/core/sys/config.c
    ${RP6502_SRC}/core/com/com.c
    ${RP6502_SRC}/core/com/tty.c
    ${RP6502_SRC}/core/wdc/bus.c
    ${RP6502_SRC}/core/wdc/phi2_div.c
    ${RP6502_SRC}/core/wdc/resb.c
    ${RP6502_SRC}/core/mem/mem.c
    ${RP6502_SRC}/core/sys/pix.c
    ${RP6502_SRC}/core/api/xreg0.c
    ${RP6502_SRC}/core/api/xreg1.c
    ${RP6502_SRC}/core/ria/ria.c
    ${RP6502_SRC}/core/vga/vga.c
    ${RP6502_SRC}/core/wdc/via.c
    ${RP6502_SRC}/core/wdc/cpu.c
    ${RP6502_SRC}/core/sys/sys.c
    ${RP6502_SRC}/core/api/api.c
    ${RP6502_SRC}/core/api/proc.c
    ${RP6502_SRC}/core/api/arg.c
    ${RP6502_SRC}/core/api/attr.c
    ${RP6502_SRC}/core/api/clk.c
    ${RP6502_SRC}/core/api/dir.c
    ${RP6502_SRC}/core/api/ops.c
    ${RP6502_SRC}/core/str/path.c
    ${RP6502_SRC}/core/str/oem.c
    ${RP6502_SRC}/core/str/unicode.c
    ${OEMCP_C}
    ${RP6502_SRC}/core/api/std.c
    ${RP6502_SRC}/core/aud/aud.c
    ${RP6502_SRC}/core/aud/bel.c
    ${RP6502_SRC}/core/aud/bel_presets.c
    ${RP6502_SRC}/core/aud/opl.c
    ${RP6502_SRC}/core/aud/psg.c
    ${RP6502_SRC}/core/str/rln.c
    ${RP6502_SRC}/core/str/str.c
    ${RP6502_SRC}/core/vga/prog.c
    ${RP6502_SRC}/core/vga/canvas.c
    ${RP6502_SRC}/core/vga/mode.c
    ${RP6502_SRC}/core/vga/mode0.c
    ${RP6502_SRC}/core/vga/mode1.c
    ${RP6502_SRC}/core/vga/mode2.c
    ${RP6502_SRC}/core/vga/mode3.c
    ${RP6502_SRC}/core/vga/mode4.c
    ${RP6502_SRC}/core/vga/mode5.c
    ${RP6502_SRC}/core/term/color.c
    ${RP6502_SRC}/core/term/font.c
    ${RP6502_SRC}/core/term/term.c
    ${RP6502_VENDOR}/emu8950/emu8950.c
)

# The vendored firmware targets the 32-bit RP2350, where pointers are 32-bit, so the
# VGA renderers cast pointers to 32-bit ints. Silence that per-compiler on those files.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        ${RP6502_SRC}/core/vga/mode1.c
        ${RP6502_SRC}/core/vga/mode2.c
        ${RP6502_SRC}/core/vga/mode3.c
        ${RP6502_SRC}/core/vga/mode4.c
        ${RP6502_SRC}/core/vga/mode5.c
        PROPERTIES COMPILE_OPTIONS "-Wno-pointer-to-int-cast"
    )
    # ram/xram (core/mem/mem.c) are 64 KB tentative definitions; -fno-common keeps them in
    # .bss (the modern GCC/Clang default) so macOS ld doesn't warn about reducing their
    # oversized __common alignment.
    target_compile_options(emu_core PRIVATE -fno-common)
    # Clang rejects these outright, so GCC has to as well or the difference
    # only shows up on a runner. PUBLIC: the app and the tests compile our C too.
    target_compile_options(emu_core PUBLIC
        $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
    # Clang flags the shared firmware's one-arg static_assert(...) as a C23 extension.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(emu_core PRIVATE -Wno-c23-extensions)
    endif()
elseif(MSVC)
    set_source_files_properties(
        ${RP6502_SRC}/core/vga/mode1.c
        ${RP6502_SRC}/core/vga/mode2.c
        ${RP6502_SRC}/core/vga/mode3.c
        ${RP6502_SRC}/core/vga/mode4.c
        ${RP6502_SRC}/core/vga/mode5.c
        PROPERTIES COMPILE_OPTIONS "/wd4311;/wd4312"
    )
endif()

# emu8950.c gates its whole body on USE_EMU8950_OPL
set_source_files_properties(
    ${RP6502_VENDOR}/emu8950/emu8950.c
    PROPERTIES COMPILE_DEFINITIONS "USE_EMU8950_OPL=1"
)
# Vendored emu8950 redefines min/max that windows.h already provides.
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
    ROM_ALIAS_MAX=16 # --rom's null drive; the docs promise sixteen
    _GNU_SOURCE
    RP6502_EXFAT=0
    RP6502_LOCALE=EN
    PICO_PROGRAM_NAME="RP6502-EMU")
# MSVC has no separate libm; what it does need instead is in
# src/osal/windows/windows.cmake, which is that seam's.
if(NOT MSVC)
    target_link_libraries(emu_core PUBLIC m)
endif()

# The link-time optimization this build asked for, given to emu_core and to the
# scope that included this file -- every root wants both and all of them are
# building the same library.
set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
