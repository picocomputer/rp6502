# emu_core: the machine as a library, for every host that runs it.
#
# What is here is what does not depend on which host this is — the shared
# sources, the tables generated from them, and the facts that belong to a
# compiler rather than a platform. A root sets RP6502_HOST to its own
# directory and includes this; that directory leads the include path, so
# `#include "host.h"` from any file — core, emu or a test — is that host's.
#
# The host's own files are the root's to name: src/host/posix/posix.cmake for
# the two that share a POSIX seam, and a line apiece for the rest.
#
# RP6502_EMU_IPO  whether this build asked for link-time optimization, for a
#                 root to hand to emu_core and to everything it makes after.

include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/chips SENTINEL chips/w65c02.h
    WANTS "the emulated 6502 and 6522")

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
set(RP6502_EMU_IPO FALSE)
if(ipo_ok AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(RP6502_EMU_IPO TRUE)
endif()

# The OEM code page tables, lifted out of vendor/fatfs/ffunicode.c by a
# generator so the logic in core/api/uni.c can be read without a
# preprocessor. See src/core/gen/oem_table_gen.py.
set(OEMCP_GEN ${RP6502_SRC}/core/gen/oem_table_gen.py)
set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_C ${CMAKE_CURRENT_BINARY_DIR}/oemcp.c)
set(OEMCP_H ${CMAKE_CURRENT_BINARY_DIR}/oemcp.h)
add_custom_command(OUTPUT ${OEMCP_C} ${OEMCP_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OEMCP_GEN}
        --ffunicode ${OEMCP_SRC} --emit-c ${OEMCP_C} --emit-h ${OEMCP_H}
    DEPENDS ${OEMCP_GEN} ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables"
    VERBATIM)
add_custom_target(oemcp DEPENDS ${OEMCP_C} ${OEMCP_H})
set(OEMCP_DIR ${CMAKE_CURRENT_BINARY_DIR})

# The OPL resampler's polyphase coefficients, on the same terms: three
# thousand numbers nobody can check by eye, so they are built rather than
# committed. Standard library only — a windowed sinc needs no solver — so
# this costs the build nothing but python3, which it already needs.
set(RSMP_GEN ${RP6502_SRC}/core/gen/rsmp_coef_gen.py)
set(RSMP_COEF_H ${CMAKE_CURRENT_BINARY_DIR}/rsmp_coef.h)
set(RSMP_COEF_DIR ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(OUTPUT ${RSMP_COEF_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${RSMP_GEN} --emit-h ${RSMP_COEF_H}
    DEPENDS ${RSMP_GEN}
    COMMENT "Generating the resampler coefficients"
    VERBATIM)
add_custom_target(rsmp_coef DEPENDS ${RSMP_COEF_H})

add_library(emu_core STATIC
    ${RP6502_SRC}/core/sys/main.c
    ${RP6502_SRC}/core/sys/proc.c
    ${RP6502_SRC}/core/sys/tim.c
    ${RP6502_SRC}/core/aud/aud_mix.c
    ${RP6502_SRC}/core/aud/rsmp.c
    ${RP6502_SRC}/core/dap/dbg.c
    ${RP6502_SRC}/core/hid/vt.c
    ${RP6502_SRC}/core/hid/hid.c
    ${RP6502_SRC}/core/hid/keyboard.c
    ${RP6502_SRC}/core/hid/mouse.c
    ${RP6502_SRC}/core/hid/gamepad.c
    ${RP6502_SRC}/core/hid/tablet.c
    # Two HID layers: the device layer every machine has is above, and these
    # are what a software machine answers where another has fabric or a
    # firmware -- the host already produced the characters, so there is no
    # layout engine here at all.
    ${RP6502_SRC}/core/sys/hid.c
    ${RP6502_SRC}/core/sys/keyboard.c
    ${RP6502_SRC}/core/sys/log.c
    ${RP6502_SRC}/core/sys/msc.c
    ${RP6502_SRC}/core/sys/rom.c
    ${RP6502_SRC}/core/sys/rom_rec.c
    ${RP6502_SRC}/core/sys/rom_win.c
    ${RP6502_SRC}/core/rand.c
    ${RP6502_SRC}/core/sys/rand.c
    ${RP6502_SRC}/core/sys/cfg.c
    ${RP6502_SRC}/core/com/com.c
    ${RP6502_SRC}/core/sys/tty.c
    ${RP6502_SRC}/core/wdc/cpu.c
    ${RP6502_SRC}/core/mem/mem.c
    ${RP6502_SRC}/core/sys/pix.c
    ${RP6502_SRC}/core/sys/main_xreg_0.c
    ${RP6502_SRC}/core/sys/main_xreg_1.c
    ${RP6502_SRC}/core/ria/ria.c
    ${RP6502_SRC}/core/sys/sys.c
    ${RP6502_SRC}/core/vga/vga.c
    ${RP6502_SRC}/core/wdc/via.c
    ${RP6502_SRC}/core/wdc/w65c02.c
    ${RP6502_SRC}/core/main.c
    ${RP6502_SRC}/core/api/api.c
    ${RP6502_SRC}/core/api/proc.c
    ${RP6502_SRC}/core/api/arg.c
    ${RP6502_SRC}/core/api/attr.c
    ${RP6502_SRC}/core/api/clk.c
    ${RP6502_SRC}/core/api/dir.c
    ${RP6502_SRC}/core/api/ops.c
    ${RP6502_SRC}/core/api/oem.c
    ${RP6502_SRC}/core/api/uni.c
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
    ${RP6502_HOST}
    ${RP6502_SRC}
    ${RP6502_VENDOR}
)
target_compile_definitions(emu_core PUBLIC
    _GNU_SOURCE
    RP6502_EXFAT=0
    RP6502_LOCALE=EN
    PICO_PROGRAM_NAME="RP6502-EMU")
# MSVC has no separate libm
if(NOT MSVC)
    target_link_libraries(emu_core PUBLIC m)
else()
    # Shims MSVC alone needs, including a <strings.h> it has no system header
    # for. Their own directory: this goes on every consumer's include path, and
    # the host's own headers are not ours to publish.
    target_include_directories(emu_core PUBLIC ${RP6502_SRC}/host/windows/msvc)
    target_compile_options(emu_core PUBLIC /utf-8 /experimental:c11atomics /FIcompat.h)
    # Shared firmware idioms MSVC dislikes but GCC/Clang accept: #pragma GCC (C4068) and
    # `return void_expr;` from a void function (C4098). GCC gates any real value-return.
    target_compile_options(emu_core PRIVATE /wd4068 /wd4098)
    target_compile_definitions(emu_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
