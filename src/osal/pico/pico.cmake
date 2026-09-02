# The RP2350 seam, for the machine whose OS is its own firmware.
#
# rp6502_osal_pico(<target>)
#
# There is no process here and no filesystem underneath: fs.c is the file
# driver over lfs.c, littlefs on the RIA's flash, and dir.c walks that. The
# entropy source is the chip's, so unlike the desktop seams there is no os.c
# left for a machine to name.

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
