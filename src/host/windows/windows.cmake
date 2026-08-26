# The Win32 host seam, for the hosts whose OS is one.
#
# The desktop emulator and the libretro core share every file here. Unlike
# host/posix there is no transport to choose: overlapped I/O is the kernel's
# own, with no helper threads to outlive an unloaded library, and the
# overlapped flag belongs to fs_open — so the transport could not leave
# fs.c. See its header.
#
# What is not here is what differs between hosts rather than between
# operating systems: the frame-pacer sleep, the console attach, and the
# argv encoding. Each host answers those in its own host.c.
#
# Included after emu.cmake: it adds to emu_core.

target_sources(emu_core PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fs_dir.c
    ${CMAKE_CURRENT_LIST_DIR}/fs.c
    ${CMAKE_CURRENT_LIST_DIR}/host.c
    ${CMAKE_CURRENT_LIST_DIR}/win.c)

# CancelIoEx and SetFileInformationByHandle are Vista. PRIVATE, so a floor
# set for these files cannot hide newer APIs from the window and pad code
# that compiles into the emulator beside them.
target_compile_definitions(emu_core PRIVATE _WIN32_WINNT=0x0601)
