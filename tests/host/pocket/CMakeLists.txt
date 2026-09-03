# The Analogue Pocket's own layer: the host bridge, its SDRAM staging, the
# scaler crossing, the codec, and what its dock says it holds.
#
# These are here rather than under a subsystem because they genuinely are
# about the host. A bridge FIFO crossing 74.25 MHz into 50.4 MHz is not a
# fact about the RP6502; it is a fact about this board.
#
# One of them needs no simulator — the dock's HID mapping is C — but it is a
# claim about this board either way, so it sits with the rest and runs
# wherever the board is built.

# --- What the Pocket says its dock holds, against the drivers it tells ---
# The dock's registers are handed to core/hid as HID reports, and the driver
# structs apf.c writes out are what says where every bit went. Wrong ones
# build clean and boot clean, so the registers go in here and the XRAM
# records come back out. The real drivers: it is the firmware's mapping
# being asked about.
rp6502_add_test(apf
    SOURCES test_apf.c
        ${RP6502_SRC}/core/hid/hid.c
        ${RP6502_SRC}/core/hid/keyboard.c
        ${RP6502_SRC}/core/hid/layout.c
        ${RP6502_SRC}/core/hid/keymap.c
        ${RP6502_SRC}/core/hid/mouse.c
        ${RP6502_SRC}/core/hid/gamepad.c
        ${RP6502_SRC}/core/hid/tablet.c
        ${RP6502_SRC}/core/str/unicode.c
        ${RP6502_SRC}/core/str/str.c
        ${RP6502_SRC}/core/sys/config.c
        ${RP6502_SRC}/core/sys/timer.c
        ${KBDLAY_C} ${OEMCP_C}
        apf_host.c
    # The pocket's own roster: keymap and str keep their settings there, so
    # the generated accessors come from the machine this bench is standing in for.
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC}
        ${KBDLAY_DIR} ${OEMCP_DIR}
    DEFS PICO_PROGRAM_NAME="RP6502-FPGA" RP6502_LOCALE=EN RP6502_EXFAT=0)
add_dependencies(test_apf kbdlay oemcp)

set(POCKET_TB ${CMAKE_CURRENT_LIST_DIR})
set(POCKET_STACK
    ${RP6502_POCKET_CORE}/pocket_fifo.sv
    ${RP6502_POCKET_CORE}/pocket_video.sv
    ${RP6502_POCKET_CORE}/pocket_i2s.sv
    ${RP6502_POCKET_CORE}/pocket_sdram.sv
    ${RP6502_POCKET_CORE}/pocket_sram.sv
    ${RP6502_POCKET_CORE}/pocket_bridge.sv
    ${RP6502_POCKET_CORE}/pocket_file.sv
    ${RP6502_POCKET_CORE}/pocket_sst.sv
    ${RP6502_POCKET_CORE}/pocket_core.sv
    ${POCKET_TB}/sdram_model.sv
    ${POCKET_TB}/asram_model.sv
    ${POCKET_TB}/tb_pocket.sv)

# --- The platform FIFO under hostile clocking ---
rp6502_add_module_test(fifo
    TOP pocket_fifo
    RTL ${RP6502_POCKET_CORE}/pocket_fifo.sv
    TIMEOUT 120)

# --- The savestate bridge: the handshake and the outbound stream ---
rp6502_add_module_test(psst
    TOP pocket_sst
    RTL ${RP6502_POCKET_CORE}/pocket_fifo.sv
        ${RP6502_POCKET_CORE}/pocket_sst.sv
    TIMEOUT 120)

# --- The debug log, playing both the console and the host ---
rp6502_add_module_test(pdbglog
    TOP pocket_dbglog
    RTL ${RP6502_POCKET_CORE}/pocket_fifo.sv
        ${RP6502_POCKET_CORE}/pocket_dbglog.sv
    TIMEOUT 120)

# --- The scaler-side raster from a replayed beam ---
rp6502_add_module_test(pvideo
    TOP pocket_video
    RTL ${RP6502_POCKET_CORE}/pocket_fifo.sv
        ${RP6502_POCKET_CORE}/pocket_video.sv
    TIMEOUT 120)

# --- The codec-side I2S from replayed samples ---
rp6502_add_module_test(pi2s
    TOP pocket_i2s
    RTL ${RP6502_POCKET_CORE}/pocket_fifo.sv
        ${RP6502_POCKET_CORE}/pocket_i2s.sv
    TIMEOUT 120)

# --- The staging SDRAM controller against a behavioral chip ---
rp6502_add_module_test(psdram
    TOP tb_psdram
    RTL ${RP6502_POCKET_CORE}/pocket_sdram.sv
        ${POCKET_TB}/sdram_model.sv ${POCKET_TB}/tb_psdram.sv
    TIMEOUT 300)

# --- The bridge chain, played like the host ---
rp6502_add_module_test(pbridge
    TOP tb_pbridge
    RTL ${RP6502_POCKET_CORE}/pocket_fifo.sv
        ${RP6502_POCKET_CORE}/pocket_sdram.sv
        ${RP6502_POCKET_CORE}/pocket_bridge.sv
        ${POCKET_TB}/sdram_model.sv ${POCKET_TB}/tb_pbridge.sv
    TIMEOUT 300)

if(RP6502_SOFT_CPU)
    # --- The whole Pocket stack, frame-exact ---
    rp6502_add_machine_test(pocket FIRMWARE SPLIT
        TOP tb_pocket
        RTL ${POCKET_STACK}
        DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
        TIMEOUT 1200)

    # --- Sleep, with the reconfigure in it ---
    rp6502_add_machine_test(psleep FIRMWARE SPLIT
        TOP tb_pocket
        RTL ${POCKET_STACK}
        DEFS FILE_ROM="${FILE_ROM}" STREAM_ROM="${STREAM_ROM}"
            ROMS_DIR="${RP6502_TEST_CORPUS}"
        DEPENDS file_rom stream_rom
        TIMEOUT 1200)

    # --- The file bridge, with the bench playing the host ---
    rp6502_add_machine_test(pfile FIRMWARE SPLIT
        TOP tb_pocket
        RTL ${POCKET_STACK}
        DEFS FILE_ROM="${FILE_ROM}" FSTEST_ROM="${FSTEST_ROM}"
            ARGV_ROM="${ARGV_ROM}" SLEEPFILE_ROM="${SLEEPFILE_ROM}"
        DEPENDS file_rom fstest_rom argv_rom sleepfile_rom
        TIMEOUT 1200)
endif()

# --- What the board's own JSON says, against what the machine was built from ---
# The core's menu picks a keyboard layout by its position in the manifest and
# the data slot declares the image's exact size; and where the host writes each
# staging slot is the same map the firmware reads back. Both are two files that
# have to agree, and a disagreement is silent.
set(POCKET_CORE_JSON ${RP6502_SRC}/host/pocket/dist/Cores/Rumbledethumps.RP6502)
add_test(NAME kbdlay_json
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/core/gen/keyboard_layout_gen.py --manifest ${KBDLAY_MANIFEST}
        --check-interact ${POCKET_CORE_JSON}/interact.json
        --check-data ${POCKET_CORE_JSON}/data.json)
set_tests_properties(kbdlay_json PROPERTIES LABELS "gate;host.pocket")
add_test(NAME stage_map
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/host/pocket/gen/stage_map_gate.py
        --data ${POCKET_CORE_JSON}/data.json
        --mmio ${RP6502_SRC}/host/pocket/sw/mmio.h
        --bench ${RP6502_ROOT}/tests/bench/tb_stage.h
        --engine ${RP6502_POCKET_CORE}/sst_engine.sv
        --tcm ${RP6502_SRC}/core/riscv/tcm_pkg.sv
        --sst ${RP6502_POCKET_CORE}/pocket_sst.sv
        --top ${RP6502_SRC}/host/pocket/core/core_top.sv)
set_tests_properties(stage_map PROPERTIES LABELS "gate;host.pocket")

# Quartus does not fail on a constraint that matches nothing -- it prints
# "Ignored" and fits without it, so a renamed module drops whatever the
# constraint was protecting and the report still says timing closed.
# rp6502.sdc's own comment records this having happened. The bitstream job
# cannot catch it; this can, and needs no Quartus.
#
# It does need every tree the two files name, and one of them is Analogue's:
# pocket.sdc constrains a signal inside their synch_3, so without their
# template the gate cannot tell a dead filter from an absent one. That
# submodule is fetched by the bitstream root and not by this one.
rp6502_submodule(vendor/openfpga SENTINEL src/fpga/ap_core.qsf
    WANTS "the timing constraint gate" OPTIONAL RESULT RP6502_HAVE_APF_RTL)
if(RP6502_HAVE_APF_RTL)
    set(SDC_GATE_RTL)
    foreach(src ${RP6502_MACHINE_SOURCES})
        list(APPEND SDC_GATE_RTL --rtl ${src})
    endforeach()
    list(APPEND SDC_GATE_RTL
        --rtl-dir ${RP6502_POCKET_CORE}
        --rtl-dir ${RP6502_VENDOR}/hazard3/hdl
        --rtl-dir ${RP6502_VENDOR}/opl2_fpga/fpga
        --rtl-dir ${RP6502_VENDOR}/openfpga/src/fpga)
    add_test(NAME sdc_filters
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${RP6502_SRC}/host/pocket/gen/sdc_gate.py
            --sdc ${RP6502_SRC}/host/pocket/quartus/wiring.sdc
            --sdc ${RP6502_SRC}/host/pocket/quartus/pocket.sdc
            ${SDC_GATE_RTL})
    set_tests_properties(sdc_filters PROPERTIES LABELS "gate;host.pocket")
else()
    message(WARNING "openfpga template absent -- timing constraint gate skipped.")
endif()
