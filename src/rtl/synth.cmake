# The machine through Quartus, for area and timing. No host in it, so no host
# in the name: this is rtl/ alone with every port a virtual pin, and it exists
# to be measured rather than programmed. src/host/ owns anything that reaches
# a pad.
#
# The source list is the verilated one, so the thing measured is the thing
# tested, and the generated packages come from this build rather than a copy
# that can drift.

if(QUARTUS_MAP AND QUARTUS_FIT AND QUARTUS_STA)
    set(SYNTH_DIR ${CMAKE_BINARY_DIR}/synth)
    set(SYNTH_QSF ${SYNTH_DIR}/rp6502.qsf)
    set(SYNTH_LINES
        "set_global_assignment -name FAMILY \"Cyclone V\""
        "set_global_assignment -name DEVICE 5CEBA4F23C8"
        "set_global_assignment -name TOP_LEVEL_ENTITY rp6502"
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files"
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS ALL"
        "set_global_assignment -name SDC_FILE ${RP6502_SDC}"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        # A shift register the fitter recognises becomes an M10K. Off
        # here as it is on the Pocket, so this target measures the
        # machine's own logic rather than a trade the fitter made.
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF"
        # The Pocket brings the 6502's RAM in on the cart bus —
        # EXT_RAM(1) at pocket_core.sv — and the BRAM fallback it
        # replaces is sixty-odd blocks this device cannot also spend.
        # Measure the machine the product builds.
        "set_parameter -name EXT_RAM 1"
        # The machine's own ports outnumber the package's pins — the host
        # window alone is a hundred of them — and this target exists to
        # measure area, not to be bound to pads.
        "set_instance_assignment -name VIRTUAL_PIN ON -to *"
        # -to * means Quartus reports every node it declined to make a
        # virtual pin, which hits its ten-thousand message cap and buries
        # everything that means something.
        "set_global_assignment -name MESSAGE_DISABLE 15720")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        endif()
    endforeach()
    string(REPLACE ";" "\n" SYNTH_QSF_TEXT "${SYNTH_LINES}")
    file(MAKE_DIRECTORY ${SYNTH_DIR})
    file(WRITE ${SYNTH_QSF} "${SYNTH_QSF_TEXT}\n")
    file(WRITE ${SYNTH_DIR}/rp6502.qpf
        "PROJECT_REVISION = \"rp6502\"\n")
    add_custom_target(synth
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        WORKING_DIRECTORY ${SYNTH_DIR}
        DEPENDS w65c02_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Synthesizing the machine for the Pocket's Cyclone V"
        VERBATIM)
endif()
