include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/gen.cmake)

set(RP6502_ASSETS ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${RP6502_ASSETS})

# verilate() runs Verilator over its sources when CMake configures, so a
# missing output is generated at configure time as well as by the build rule.
# The generator runs at configure time only when an output is missing,
# because each run gives its outputs new timestamps and the Pocket fit
# depends on the generated packages, so the fit would run again after every
# configure.
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

include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/chips SENTINEL codegen/w65c02_gen.py
    WANTS "the w65c02 decode table generator")
rp6502_submodule(vendor/opl2_fpga
    SENTINEL fpga/modules/operator/src/opl2_log_sine_lut.sv
    WANTS "the OPL2 core and its lookup tables")

set(W65C02_GEN ${RP6502_SRC}/core/gen/w65c02_rom_gen.py)
set(W65C02_ROM ${RP6502_ASSETS}/w65c02_rom_pkg.sv)
rp6502_core_asset(w65c02_rom GEN ${W65C02_GEN}
    ARGS --emit ${W65C02_ROM}
    OUTPUTS ${W65C02_ROM}
    COMMENT "Generating the w65c02 decode table")

set(VID_FONT_BIN ${RP6502_ASSETS}/fonts.bin)
set(VID_FONT_ASSET_H ${RP6502_ASSETS}/vid_font_asset.h)
rp6502_core_asset(vid_font_rom GEN ${RP6502_SRC}/core/gen/vid_font_gen.py
    ARGS --emit-bin ${VID_FONT_BIN} --emit-asset-h ${VID_FONT_ASSET_H}
    OUTPUTS ${VID_FONT_BIN} ${VID_FONT_ASSET_H}
    DEPENDS ${RP6502_SRC}/core/term/font.c
    COMMENT "Generating the font asset")

set(VID_PALETTE_PKG ${RP6502_ASSETS}/vid_palette_pkg.sv)
rp6502_core_asset(vid_palette_rom GEN ${RP6502_SRC}/core/gen/vid_palette_gen.py
    ARGS --emit-sv ${VID_PALETTE_PKG}
    OUTPUTS ${VID_PALETTE_PKG}
    DEPENDS ${RP6502_SRC}/core/term/color.c
    COMMENT "Generating the vid palette ROM")

set(AUD_SINE_PKG ${RP6502_ASSETS}/aud_sine_pkg.sv)
rp6502_core_asset(aud_sine_rom GEN ${RP6502_SRC}/core/gen/aud_sine_gen.py
    ARGS --emit-sv ${AUD_SINE_PKG}
    OUTPUTS ${AUD_SINE_PKG}
    COMMENT "Generating the aud sine ROM")

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

set(RSMP_COEF_PKG ${RP6502_ASSETS}/rsmp_coef_pkg.sv)
rp6502_core_asset(rsmp_coef_pkg GEN ${RP6502_SRC}/core/gen/rsmp_coef_gen.py
    ARGS --emit-sv ${RSMP_COEF_PKG}
    OUTPUTS ${RSMP_COEF_PKG}
    COMMENT "Generating the resampler coefficient package")

set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_BIN ${RP6502_ASSETS}/oemcp.bin)
rp6502_core_asset(oemcp_bin GEN ${RP6502_SRC}/core/gen/oem_table_gen.py
    ARGS --ffunicode ${OEMCP_SRC} --emit-bin ${OEMCP_BIN}
    OUTPUTS ${OEMCP_BIN}
    DEPENDS ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables")

set(KBDLAY_BIN ${RP6502_ASSETS}/keyboard.bin)
rp6502_core_asset(kbdlay_bin GEN ${RP6502_SRC}/core/gen/keyboard_layout_gen.py
    ARGS --manifest ${KBDLAY_MANIFEST} --emit-bin ${KBDLAY_BIN}
    OUTPUTS ${KBDLAY_BIN}
    DEPENDS ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
    COMMENT "Generating the keyboard layouts")
