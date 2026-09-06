/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The desktop's standard streams: which of the program's the host's carry,
 * and in what encoding. The machine's console is cli/console.h and the
 * machine's own lines are cli/log.c; neither is a program's stream.
 */

#ifndef _HOST_SOKOL_CLI_STREAMS_H_
#define _HOST_SOKOL_CLI_STREAMS_H_

#include <stdbool.h>

/* Mirror the program's stdout to the host's. Not under --script or --dap,
 * which own host stdout, nor --crc, whose value it is. */
void streams_mirror_stdout(void);

#endif /* _HOST_SOKOL_CLI_STREAMS_H_ */
