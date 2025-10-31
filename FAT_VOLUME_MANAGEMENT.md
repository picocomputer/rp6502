# FAT Volume Management Implementation

This document describes the multi-partition and volume management features added to support issue #13.

## Changes Made

### 1. FatFs Configuration (`src/fatfs/ffconf.h`)

- **`FF_USE_MKFS = 1`** — Enables `f_mkfs()` for creating new filesystems
- **`FF_MULTI_PARTITION = 1`** — Enables multiple partitions per physical drive and `f_fdisk()`

### 2. Volume-to-Partition Mapping (`src/ria/usb/msc.c`)

Added `VolToPart[]` table mapping logical volumes to physical drives:

```c
const PARTITION VolToPart[FF_VOLUMES] = {
    {0, 0}, {1, 0}, {2, 0}, {3, 0},
    {4, 0}, {5, 0}, {6, 0}, {7, 0}
};
```

**Format:** `{physical_drive, partition}`

- `partition = 0` — Auto-detect first valid FAT partition
- `partition = 1-4` — Specific MBR partition number

**Current mapping:** Each logical volume (USB0-USB7) maps to its corresponding physical drive (0-7) with auto-detection.

### 3. New API Functions (`src/ria/api/dir.c`)

#### `dir_api_mkfs()` — Format a volume (API 0x2F)

Creates a FAT32 filesystem on the specified logical volume.

**6502 Interface:**

```
.A = 0x2F
xstack: path string (e.g., "USB0:")
```

**Returns:**

- `AX = 0` on success
- `AX = -1` on failure (errno set via FatFs FRESULT)

**Parameters:** Uses default FAT32 formatting parameters (auto cluster size, 2 FATs, etc.)

#### `dir_api_fdisk()` — Create MBR partition table (API 0x30)

Creates an MBR partition table on a physical drive with up to 4 partitions.

**6502 Interface:**

```
.A = 0x2F
xstack (bottom to top):
  - physical drive number (byte)
  - partition 1 size in sectors (32-bit LBA)
  - partition 2 size in sectors (32-bit LBA)  
  - partition 3 size in sectors (32-bit LBA)
  - partition 4 size in sectors (32-bit LBA)
```

**Returns:**

- `AX = 0` on success
- `AX = -1` on failure (errno set)

**Notes:**

- Set partition size to 0 to skip that partition
- Partitions are created sequentially starting from LBA 2048
- After `f_fdisk()`, use `f_mkfs()` to format each partition

### 4. API Registration (`src/ria/main.c`)

Registered new API operations:

- `0x2F` → `dir_api_mkfs()`
- `0x30` → `dir_api_fdisk()`

## Usage Examples

### Example 1: Format a USB drive

```c
// Format USB0: as FAT32
asm("lda #$2F");  // mkfs API
char path[] = "USB0:";
// push path to xstack...
// call API
```

### Example 2: Create multi-partition drive

```assembly
; Partition USB drive 0 into two 1GB partitions
lda #$30        ; fdisk API
lda #0          ; physical drive 0
; push to xstack
; partition 1: 2097152 sectors (1GB)
lda #$00
lda #$00
lda #$20
lda #$00
; partition 2: 2097152 sectors (1GB)
lda #$00
lda #$00
lda #$20
lda #$00
; partition 3 & 4: unused
lda #$00
...
; call API
```

After partitioning, format each partition:

```c
f_mkfs("0:", ...);  // Format partition 1 on drive 0
f_mkfs("1:", ...);  // Format partition 2 on drive 0
```

## Volume Mapping Customization

To map specific partitions to logical volumes, edit `VolToPart[]` in `src/ria/usb/msc.c`:

```c
// Example: USB0 → drive 0 partition 1, USB1 → drive 0 partition 2
const PARTITION VolToPart[FF_VOLUMES] = {
    {0, 1},  // USB0: → physical drive 0, partition 1
    {0, 2},  // USB1: → physical drive 0, partition 2
    {1, 0},  // USB2: → physical drive 1, auto-detect
    {2, 0},  // USB3: → physical drive 2, auto-detect
    {3, 0}, {4, 0}, {5, 0}, {6, 0}
};
```

## Limitations & Notes

1. **Simplified mkfs interface** — Uses default FAT32 parameters. For advanced options (cluster size, FAT type selection), call FatFs `f_mkfs()` directly with a custom `MKFS_PARM` struct.

2. **MBR only** — No GPT support (requires `FF_LBA64` and 64-bit sector addressing).

3. **No exFAT** — Current build has `RP6502_EXFAT = 0`. To enable exFAT:
   - Set `RP6502_EXFAT = 1` in CMakeLists.txt
   - Rebuild with exFAT support

4. **Work buffer** — `f_mkfs()` and `f_fdisk()` use a static 512-byte work buffer. Only one format/partition operation can run at a time.

5. **Physical drive mapping** — Physical drive numbers correspond to USB device addresses (0-7). Ensure the USB device is mounted before calling these functions.

## Testing

### Manual Test: Format a USB drive

1. Insert USB drive
2. Check mounted volumes: `mon` → `status`
3. Format: Call `f_mkfs("USB0:", ...)` via 6502 API
4. Verify: Directory listing should work on newly formatted volume

### Manual Test: Multi-partition setup

1. Insert USB drive on USB0
2. Partition: Call `f_fdisk(0, [sizes], ...)`
3. Update `VolToPart[]` to map USB0→(0,1), USB1→(0,2)
4. Rebuild and reflash firmware
5. Format each: `f_mkfs("0:", ...)` and `f_mkfs("1:", ...)`
6. Mount and verify both partitions accessible

## Future Enhancements

- **Removable media detection** — Auto-remount when devices are hot-swapped
- **Partition browsing API** — Query partition table without formatting
- **GPT support** — For drives >2TB
- **Format progress callback** — For user feedback during long formats
- **Volume label in mount** — Auto-detect and use volume labels as mount points

## References

- FatFs Module: <http://elm-chan.org/fsw/ff/00index_e.html>
- `f_mkfs()`: <http://elm-chan.org/fsw/ff/doc/mkfs.html>
- `f_fdisk()`: <http://elm-chan.org/fsw/ff/doc/fdisk.html>
- Issue #13: <https://github.com/picocomputer/rp6502/issues/13>
