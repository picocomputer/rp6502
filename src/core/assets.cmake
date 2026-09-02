# The generated assets: decode tables, font and palette images, sine and audio
# lookup ROMs, code pages. All of them come out of src/core/gen, none of them is
# committed, and more than one concern needs the same ones -- the verilated
# machine compiles the palette and sine packages in, the soft CPU links the font
# table, and the Pocket's dist tree ships the image. So they are built once here
# rather than by whoever asked first.
#
# What is here is what the machine is built from or staged into. A .rp6502 is
# neither: it is a program the machine runs, and the same one runs on every
# machine, so those are made in tests/rp6502_tests.cmake where every tree can
# reach them.
#
# Only the fabric needs these. An emulator build never includes this file.

include_guard(GLOBAL)

# The manifest and the defs the keyboard image is built from are gen.cmake's.
include(${CMAKE_CURRENT_LIST_DIR}/gen.cmake)

set(RP6502_ASSETS ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${RP6502_ASSETS})

# rp6502_core_asset(<target> GEN <script> OUTPUTS <file>... [ARGS <arg>...]
#                   [DEPENDS <file>...] COMMENT <text>)
#
# Named for what it makes: something the core is built from or staged into.
# rp6502_asset is the SDK's, in tools/rp6502.cmake, and takes an address and a
# file — a downstream project calls that one, and the two would share a name.
#
# Generates at configure time AND at build time, which looks redundant and is
# not: verilate() reads its sources when cmake configures, so a package that
# only appears at build time is missing when the model is elaborated. The
# build-time rule is what keeps it fresh when the generator changes.
#
# Existence is the whole of what configure needs, so it generates only when
# something is missing. Running unconditionally moves every asset's timestamp
# on every configure, and the IDE configures on every CMakeLists edit — which
# reads downstream as a changed design and costs a ten minute refit.
function(rp6502_core_asset target)
    cmake_parse_arguments(A "" "GEN;COMMENT" "OUTPUTS;ARGS;DEPENDS" ${ARGN})
    set(_absent FALSE)
    foreach(_out IN LISTS A_OUTPUTS)
        if(NOT EXISTS ${_out})
            set(_absent TRUE)
        endif()
    endforeach()
    if(_absent)
        execute_process(COMMAND python3 ${A_GEN} ${A_ARGS} RESULT_VARIABLE _rc)
        if(_rc)
            get_filename_component(_name ${A_GEN} NAME)
            message(FATAL_ERROR "${_name} failed")
        endif()
    endif()
    add_custom_command(OUTPUT ${A_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${A_GEN} ${A_ARGS}
        DEPENDS ${A_GEN} ${A_DEPENDS}
        COMMENT ${A_COMMENT}
        VERBATIM)
    add_custom_target(${target} ALL DEPENDS ${A_OUTPUTS})
endfunction()

# Asked for here and not inside rp6502_core_asset, which only generates when
# an output is missing: a warm tree whose submodule went away would otherwise
# configure clean and fail at the build rule instead.
include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/chips SENTINEL codegen/w65c02_gen.py
    WANTS "the w65c02 decode table generator")
rp6502_submodule(vendor/opl2_fpga
    SENTINEL fpga/modules/operator/src/opl2_log_sine_lut.sv
    WANTS "the OPL2 core and its lookup tables")

# --- The generator agrees with the C it generates from ---
set(W65C02_GEN ${RP6502_SRC}/core/gen/w65c02_rom_gen.py)
set(W65C02_ROM ${RP6502_ASSETS}/w65c02_rom_pkg.sv)
rp6502_core_asset(w65c02_rom GEN ${W65C02_GEN}
    ARGS --emit ${W65C02_ROM}
    OUTPUTS ${W65C02_ROM}
    COMMENT "Generating the w65c02 decode table")

# The font asset comes from core/term/font.c: the image the firmware
# copies into the store, and an offsets header for it.
set(VID_FONT_BIN ${RP6502_ASSETS}/fonts.bin)
set(VID_FONT_ASSET_H ${RP6502_ASSETS}/vid_font_asset.h)
rp6502_core_asset(vid_font_rom GEN ${RP6502_SRC}/core/gen/vid_font_gen.py
    ARGS --emit-bin ${VID_FONT_BIN} --emit-asset-h ${VID_FONT_ASSET_H}
    OUTPUTS ${VID_FONT_BIN} ${VID_FONT_ASSET_H}
    DEPENDS ${RP6502_SRC}/core/term/font.c
    COMMENT "Generating the font asset")

# The builtin palettes ride the same way, from core/term/color.c.
set(VID_PALETTE_PKG ${RP6502_ASSETS}/vid_palette_pkg.sv)
rp6502_core_asset(vid_palette_rom GEN ${RP6502_SRC}/core/gen/vid_palette_gen.py
    ARGS --emit-sv ${VID_PALETTE_PKG}
    OUTPUTS ${VID_PALETTE_PKG}
    DEPENDS ${RP6502_SRC}/core/term/color.c
    COMMENT "Generating the vid palette ROM")

# The PSG's sine table, from aud_init's runtime formula.
set(AUD_SINE_PKG ${RP6502_ASSETS}/aud_sine_pkg.sv)
rp6502_core_asset(aud_sine_rom GEN ${RP6502_SRC}/core/gen/aud_sine_gen.py
    ARGS --emit-sv ${AUD_SINE_PKG}
    OUTPUTS ${AUD_SINE_PKG}
    COMMENT "Generating the aud sine ROM")

# The OPL2's two operator tables as one 512x12 package. The vendor's
# generated case arms are the source — parsed and re-emitted, so the
# fabric's words are the submodule's by construction — and the formulas
# from its headers are recomputed as a tripwire against a submodule
# update changing either table silently.
set(OPL2_LUT_SRC ${RP6502_VENDOR}/opl2_fpga/fpga/modules/operator/src)
set(OPL2_LUT_PKG ${RP6502_ASSETS}/opl2_lut_pkg.sv)
rp6502_core_asset(opl2_lut_rom GEN ${RP6502_SRC}/core/gen/opl2_lut_gen.py
    ARGS --log-sine ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        --exp ${OPL2_LUT_SRC}/opl2_exp_lut.sv
        --emit-sv ${OPL2_LUT_PKG}
    OUTPUTS ${OPL2_LUT_PKG}
    DEPENDS ${OPL2_LUT_SRC}/opl2_log_sine_lut.sv
        ${OPL2_LUT_SRC}/opl2_exp_lut.sv
    COMMENT "Generating the merged OPL2 LUT ROM")


# The resampler's coefficients, as the package the RTL reads. The same
# script writes the C table emu.cmake compiles, so there is one design behind both
# and the lockstep is comparing implementations rather than designs.
set(RSMP_COEF_PKG ${RP6502_ASSETS}/rsmp_coef_pkg.sv)
rp6502_core_asset(rsmp_coef_pkg GEN ${RP6502_SRC}/core/gen/rsmp_coef_gen.py
    ARGS --emit-sv ${RSMP_COEF_PKG}
    OUTPUTS ${RSMP_COEF_PKG}
    COMMENT "Generating the resampler coefficient package")

# The OEM code page tables. This machine cannot link them in, so it gets
# the binary and loads it into the staging store beside the fonts.
set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_BIN ${RP6502_ASSETS}/oemcp.bin)
rp6502_core_asset(oemcp_bin GEN ${RP6502_SRC}/core/gen/oem_table_gen.py
    ARGS --ffunicode ${OEMCP_SRC} --emit-bin ${OEMCP_BIN}
    OUTPUTS ${OEMCP_BIN}
    DEPENDS ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables")

# The keyboard layouts, for the same reason: twenty kilobytes of table
# as a compiler lays it out, eight as the generator does, and no room
# for either in a 96 KB tightly coupled memory.
set(KBDLAY_BIN ${RP6502_ASSETS}/keyboard.bin)
rp6502_core_asset(kbdlay_bin GEN ${RP6502_SRC}/core/gen/keyboard_layout_gen.py
    ARGS --manifest ${KBDLAY_MANIFEST} --emit-bin ${KBDLAY_BIN}
    OUTPUTS ${KBDLAY_BIN}
    DEPENDS ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
    COMMENT "Generating the keyboard layouts")
