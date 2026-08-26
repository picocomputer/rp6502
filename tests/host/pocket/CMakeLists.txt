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
        ${RP6502_SRC}/core/hid/vt.c
        ${RP6502_SRC}/core/api/uni.c
        ${KBDLAY_C} ${OEMCP_C}
        apf_host.c
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC}
        ${KBDLAY_DIR} ${OEMCP_DIR})
add_dependencies(test_apf kbdlay oemcp)

set(POCKET_TB ${CMAKE_CURRENT_LIST_DIR})
set(POCKET_STACK
    ${RP6502_HOST_POCKET}/pocket_fifo.sv
    ${RP6502_HOST_POCKET}/pocket_video.sv
    ${RP6502_HOST_POCKET}/pocket_i2s.sv
    ${RP6502_HOST_POCKET}/pocket_sdram.sv
    ${RP6502_HOST_POCKET}/pocket_sram.sv
    ${RP6502_HOST_POCKET}/pocket_bridge.sv
    ${RP6502_HOST_POCKET}/pocket_file.sv
    ${RP6502_HOST_POCKET}/pocket_sst.sv
    ${RP6502_HOST_POCKET}/pocket_core.sv
    ${POCKET_TB}/sdram_model.sv
    ${POCKET_TB}/asram_model.sv
    ${POCKET_TB}/tb_pocket.sv)

# --- The platform FIFO under hostile clocking ---
rp6502_add_module_test(fifo
    TOP pocket_fifo
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
    TIMEOUT 120)

# --- The savestate bridge: the handshake and the outbound stream ---
rp6502_add_module_test(psst
    TOP pocket_sst
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_sst.sv
    TIMEOUT 120)

# --- The debug log, playing both the console and the host ---
rp6502_add_module_test(pdbglog
    TOP pocket_dbglog
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_dbglog.sv
    TIMEOUT 120)

# --- The scaler-side raster from a replayed beam ---
rp6502_add_module_test(pvideo
    TOP pocket_video
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_video.sv
    TIMEOUT 120)

# --- The codec-side I2S from replayed samples ---
rp6502_add_module_test(pi2s
    TOP pocket_i2s
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_i2s.sv
    TIMEOUT 120)

# --- The staging SDRAM controller against a behavioral chip ---
rp6502_add_module_test(psdram
    TOP tb_psdram
    RTL ${RP6502_HOST_POCKET}/pocket_sdram.sv
        ${POCKET_TB}/sdram_model.sv ${POCKET_TB}/tb_psdram.sv
    TIMEOUT 300)

# --- The bridge chain, played like the host ---
rp6502_add_module_test(pbridge
    TOP tb_pbridge
    RTL ${RP6502_HOST_POCKET}/pocket_fifo.sv
        ${RP6502_HOST_POCKET}/pocket_sdram.sv
        ${RP6502_HOST_POCKET}/pocket_bridge.sv
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
        DEPENDS file_rom fstest_rom
        TIMEOUT 1200)
endif()
