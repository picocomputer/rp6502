/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The desktop's standard streams: the program's stdout mirrored to the
 * host's, and under --headless the host's stdin fed to the program's.
 */

#ifndef _HOST_SOKOL_CLI_STREAMS_H_
#define _HOST_SOKOL_CLI_STREAMS_H_

/* Mirror the program's stdout to the host's. Not under --script or --dap,
 * which own host stdout, nor --crc, whose value it is. */
void streams_mirror_stdout(void);

/* Once a frame under --headless: when the program is waiting on a cooked
 * read with nothing on its way, a line of host stdin, or end of file. */
void streams_feed_stdin(void);

#endif /* _HOST_SOKOL_CLI_STREAMS_H_ */
