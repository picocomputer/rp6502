# The POSIX host seam, for the hosts whose OS is one.
#
# Linux and macOS share all four of these files and differ only in the entropy
# source and the frame-pacer sleep, which is what each one's own host.c is.
# The web and Android hosts take dir.c and host.c from here directly and bring
# their own fs.c — neither has <aio.h> — so they do not include this.
#
# Included after emu.cmake: it adds to emu_core.

target_sources(emu_core PRIVATE
    ${RP6502_SRC}/core/posix/dir.c
    ${RP6502_SRC}/core/posix/fs.c
    ${RP6502_SRC}/core/posix/host.c)

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
