# Drives and filesystems: the host-backed MSC0:, the ROM overlay, FatFs.

# --- Host-backed MSC0: + read-only ROM: + directory enumeration ---
rp6502_add_test(fs SOURCES test_fs.c ${RP6502_BENCH}/tb_hostos.c LIBS emu_core)

# --- Drive fs.c paths: host-backed MSC0:, ROM overlay, async POSIX-AIO ---
rp6502_add_test(drive SOURCES test_drive.c ${RP6502_BENCH}/tb_hostos.c
    LIBS emu_core FIXTURE adventure.rp6502)

# --- Real FatFs on the emulator RAM disk (shared core/api/fat.c + host/fat.c diskio) ---
rp6502_add_test(fatfs LIBS emu_core)

# --- Directory enumeration integration (dir.rp6502: opendir/readdir/stat/getfree) ---
rp6502_add_test(dir SOURCES test_dir.c ${RP6502_BENCH}/tb_hostos.c
    LIBS emu_core FIXTURE dir.rp6502 TIMEOUT 60)

# --- The conformance ROM on this machine too ---
# The same forty-eight checks the Pocket's bench runs (tests/host/pocket,
# pfile.the_whole_drive_conforms), against a host-backed drive. Four of them
# describe the platform rather than the filesystem and are expected to differ;
# the generator says which, so this asserts that they are the only ones.
#
# The script is not written down: the file that assembles the program drives
# it too, over the pipe scr.c answers on, so the expectation and the program
# it is about cannot drift apart.
if(TARGET rp6502-emu)
    rp6502_add_script_test(fstest DRIVER ${RP6502_TESTS_DIR}/gen/fstest_rom_gen.py
        ROM ${FSTEST_ROM} DEPENDS fstest_rom TIMEOUT 120)

    # The round trip, and the streaming read the Pocket interrupts with a
    # sleep. Here nothing interrupts them, which is what makes them the
    # control: the same programs, a machine that never slept.
    rp6502_add_script_test(file DRIVER ${RP6502_TESTS_DIR}/gen/file_rom_gen.py
        ROM ${FILE_ROM} DEPENDS file_rom TIMEOUT 120)
    rp6502_add_script_test(stream DRIVER ${RP6502_TESTS_DIR}/gen/stream_rom_gen.py
        ROM ${STREAM_ROM} DEPENDS stream_rom TIMEOUT 120)
endif()
