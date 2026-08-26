# The POSIX host seam, for the hosts whose OS is one.
#
# Linux and macOS share all of these files and differ only in the entropy
# source and the frame-pacer sleep, which is what each one's own host.c is.
# The web and Android hosts take fs_dir.c and host.c from here directly and bring
# their own fs.c — neither has <aio.h> — so they do not include this.
#
# fs.c is the seam minus its byte transport, and the transport is a file of
# its own because there is more than one right answer: fs_aio.c for a host
# that owns its process, fs_sync.c for one that is a guest in someone
# else's. This include takes the asynchronous one; a root that wants the
# other names these three files itself.
#
# Included after emu.cmake: it adds to emu_core.

target_sources(emu_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fs_dir.c
    ${CMAKE_CURRENT_LIST_DIR}/fs.c
    ${CMAKE_CURRENT_LIST_DIR}/fs_aio.c
    ${CMAKE_CURRENT_LIST_DIR}/host.c)

# POSIX AIO. On macOS aio_read is in libc and there is no librt to find; the
# check is the same either way and answers for the platform it runs on.
include(CheckSymbolExists)
find_library(RT_LIBRARY rt)
if(RT_LIBRARY)
    target_link_libraries(emu_core PUBLIC ${RT_LIBRARY})
    set(CMAKE_REQUIRED_LIBRARIES ${RT_LIBRARY})
endif()
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(aio_read "aio.h" EMU_POSIX_AIO)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT EMU_POSIX_AIO)
    message(FATAL_ERROR "POSIX AIO (aio_read/<aio.h>) is required on this platform")
endif()
