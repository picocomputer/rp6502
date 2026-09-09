# rp6502_osal_pico(<target>)
#
# fs.c is the file driver over FatFs and over lfs.c, which is littlefs on the
# RIA's flash.

include_guard(GLOBAL)

set(RP6502_OSAL_PICO ${CMAKE_CURRENT_LIST_DIR})

function(rp6502_osal_pico target)
    target_sources(${target} PRIVATE
        ${RP6502_OSAL_PICO}/dir.c
        ${RP6502_OSAL_PICO}/errmap.c
        ${RP6502_OSAL_PICO}/fs.c
        ${RP6502_OSAL_PICO}/lfs.c
        ${RP6502_OSAL_PICO}/os.c)
endfunction()
