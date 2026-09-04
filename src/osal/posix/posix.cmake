# The POSIX seam, for the machines whose OS is one.
#
# rp6502_osal_posix(<target> TRANSPORT aio|sync)
#
# fs.c is the file driver minus its read/write/close, which are a file of their
# own because there is more than one right answer: fs_aio.c for a machine that
# owns its process, fs_sync.c for one that is a guest in someone else's — the
# libretro core, the browser and Android, none of which has a usable <aio.h>.
#
# What is not here is the entropy source: that differs between machines rather
# than between operating systems, so each names its own osal/<os>/os.c.

include_guard(GLOBAL)

set(RP6502_OSAL_POSIX ${CMAKE_CURRENT_LIST_DIR})

function(rp6502_osal_posix target)
    cmake_parse_arguments(P "" "TRANSPORT" "" ${ARGN})
    if(NOT P_TRANSPORT MATCHES "^(aio|sync)$")
        message(FATAL_ERROR "rp6502_osal_posix(${target}): TRANSPORT is aio or sync")
    endif()
    target_sources(${target} PRIVATE
        ${RP6502_OSAL_POSIX}/dir.c
        ${RP6502_OSAL_POSIX}/errmap.c
        ${RP6502_OSAL_POSIX}/fs.c
        ${RP6502_OSAL_POSIX}/fs_${P_TRANSPORT}.c
        ${RP6502_OSAL_POSIX}/os.c)
    if(NOT P_TRANSPORT STREQUAL "aio")
        return()
    endif()
    # POSIX AIO. On macOS aio_read is in libc and there is no librt to find; the
    # check is the same either way and answers for the platform it runs on.
    include(CheckSymbolExists)
    find_library(RT_LIBRARY rt)
    if(RT_LIBRARY)
        target_link_libraries(${target} PUBLIC ${RT_LIBRARY})
        set(CMAKE_REQUIRED_LIBRARIES ${RT_LIBRARY})
    endif()
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(aio_read "aio.h" EMU_POSIX_AIO)
    if(NOT EMU_POSIX_AIO)
        message(FATAL_ERROR "POSIX AIO (aio_read/<aio.h>) is required on this platform")
    endif()
endfunction()
