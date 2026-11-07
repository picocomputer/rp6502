/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_CLI_STATE_H_
#define _HOST_SOKOL_CLI_STATE_H_

/* Savestates as files on this host's disk, for the script's state verbs. What
 * is here rather than in core is what core cannot reach: the file and the
 * in-flight file transfer, so that the caller does not have to sequence those
 * itself.
 */

#include <stdbool.h>

/* True on success. On failure *why is a static reason string, never NULL. */
bool state_save_file(const char *path, const char **why);
bool state_load_file(const char *path, const char **why);

#endif /* _HOST_SOKOL_CLI_STATE_H_ */
