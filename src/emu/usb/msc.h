/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator RAM block device behind the FatFs volume (see usb/msc.c). The
 * diskio entry points are called by FatFs; this header exposes only the reset.
 */

#ifndef _EMU_USB_MSC_H_
#define _EMU_USB_MSC_H_

#ifdef __cplusplus
extern "C"
{
#endif

void emu_ramdisk_reset(void); /* wipe the RAM disk to an unformatted state */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_USB_MSC_H_ */
