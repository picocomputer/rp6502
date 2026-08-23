# Drives and filesystems: the host-backed MSC0:, the ROM overlay, FatFs.

# --- Host-backed MSC0: + read-only ROM: + directory enumeration ---
rp6502_add_test(fs LIBS emu_core)

# --- Drive fs.c paths: host-backed MSC0:, ROM overlay, async POSIX-AIO ---
rp6502_add_test(drive LIBS emu_core FIXTURE adventure.rp6502)

# --- Real FatFs on the emulator RAM disk (shared core/api/fat.c + host/fat.c diskio) ---
rp6502_add_test(fatfs LIBS emu_core)

# --- Directory enumeration integration (dir.rp6502: opendir/readdir/stat/getfree) ---
rp6502_add_test(dir LIBS emu_core FIXTURE dir.rp6502 TIMEOUT 60)
