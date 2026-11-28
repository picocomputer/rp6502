set(MISTER_TB ${CMAKE_CURRENT_LIST_DIR})

rp6502_add_module_test(mvideo SPLIT
    TOP tb_mvideo
    RTL ${RP6502_SRC}/core/vga/scan/timing_pkg.sv
        ${RP6502_SRC}/core/vga/canvas_pkg.sv
        ${RP6502_SRC}/core/vga/scan/timing.sv
        ${RP6502_MISTER_CORE}/mister_video.sv
        ${MISTER_TB}/mister_bars.sv ${MISTER_TB}/tb_mvideo.sv
    ARGS ${MISTER_TB}/mvideo.vlt
    TIMEOUT 300)

# The tree's SDC files name only what the MiSTer RTL has. wiring.sdc is
# checked too, though files.qip leaves it out until the machine joins.
rp6502_mister_rtl(MISTER_RTL)
set(MISTER_GATE_RTL --rtl ${RP6502_MISTER_TREE}/RP6502.sv)
foreach(src ${MISTER_RTL})
    list(APPEND MISTER_GATE_RTL --rtl ${src})
endforeach()
add_test(NAME sdc_filters_mister
    COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/fpga/gen/sdc_gate.py
        --sdc ${RP6502_MISTER_TREE}/RP6502.sdc --sdc ${RP6502_RTL_SDC}
        ${MISTER_GATE_RTL})
set_tests_properties(sdc_filters_mister PROPERTIES LABELS "gate;host.mister")

add_test(NAME mister_tree
    COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/mister/gen/tree_gate.py
        --tree ${RP6502_MISTER_TREE} --pin ${RP6502_SRC}/host/mister/sys.pin)
set_tests_properties(mister_tree PROPERTIES LABELS "gate;host.mister")

rp6502_mister_sys(MISTER_SYS WANTS "the sys_sdc_names gate")
if(MISTER_SYS)
    add_test(NAME sys_sdc_names
        COMMAND ${RP6502_PYTHON} ${RP6502_SRC}/host/mister/gen/sys_sdc_gate.py
            --sdc ${MISTER_SYS}/sys_top.sdc ${MISTER_GATE_RTL})
    set_tests_properties(sys_sdc_names PROPERTIES LABELS "gate;host.mister")
endif()
