# rp6502_osal_posix(<target> TRANSPORT aio|sync)
#
# fs.c is the file driver without its read, write, close and settle, which are
# a file of their own because there is more than one right answer: fs_aio.c for
# a machine that owns its process, fs_sync.c for one running inside someone
# else's. The libretro core takes sync because a frontend unloads it and
# glibc's AIO helper threads would be left holding a buffer inside a library
# that is going away; the browser and Android take it because they have no
# POSIX AIO at all.

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
    # On macOS aio_read is in libc and there is no librt to find, so the check
    # below answers for whichever platform it runs on.
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
