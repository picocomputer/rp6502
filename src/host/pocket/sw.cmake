include(${RP6502_SRC}/host/fpga/sw.cmake)
set(_p ${CMAKE_CURRENT_LIST_DIR}/sw)
rp6502_fpga_sw(DIR ${CMAKE_CURRENT_LIST_DIR}
    SOURCES ${_p}/main.c ${_p}/apf.c ${_p}/sleep.c ${_p}/hid.c ${_p}/dir.c
        ${_p}/fs.c ${_p}/proc.c
    DEPENDS ${RP6502_GEN}/pocket_version.h)
