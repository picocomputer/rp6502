/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Installed ROMs on the firmware's null drive ":" (install.c). `--rom <file>`
 * installs a .rp6502 by host basename, reached as ":basename" (matched
 * case-insensitively). Like the firmware, only the boot/exec loader resolves
 * them — not a 6502 open(); read-only, surviving exec.
 */

#ifndef _EMU_INSTALL_H_
#define _EMU_INSTALL_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

bool fs_install_rom(const char *hostpath);
/* Map a boot/exec ROM path (":name" / drive path / bare) to the host file the
 * loader opens. */
bool fs_resolve_rom(const char *path, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_INSTALL_H_ */
