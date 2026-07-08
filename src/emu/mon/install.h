/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_MON_INSTALL_H_
#define _EMU_MON_INSTALL_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

bool install_rom(const char *hostpath);
/* Map a boot/exec ROM path (":name" / drive path / bare) to the host file the
 * loader opens. */
bool install_resolve(const char *path, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MON_INSTALL_H_ */
