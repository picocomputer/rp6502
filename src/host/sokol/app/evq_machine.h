/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's end of the event queue (evq.h): each message from the window
 * handed to the machine, its user's events to core/sys/event.h, and what the
 * machine tells the window back. All of it runs on the machine's thread.
 */

#ifndef _HOST_SOKOL_APP_EVQ_MACHINE_H_
#define _HOST_SOKOL_APP_EVQ_MACHINE_H_

#include "host/sokol/app/evq.h"
#include <stdbool.h>
#include <stdint.h>

/* Whether a window is up, which makes the clipboard the host's. Without one,
 * a script or a headless run, the clipboard is one the emulator keeps for
 * itself. Set before the machine's thread starts and after it stops. */
void machine_set_window(bool on);

/* One message from the window, any but a display tick (run.c). */
void machine_take(const evq_msg_t *m);

/* What the window needs of the machine, told whenever it changes, with
 * whether the machine's frames follow the display and the display's period
 * as the machine measures it (event_status). */
void machine_status(bool follows, int64_t period_ns);

/* Whether the drop-a-ROM card is up, as the window last said. */
bool machine_menu_up(void);

/* Whether a window is up, for the host's answers (clip.c). */
bool machine_has_window(void);

/* Boot a .rp6502, true on success. The path is the host path from the OS,
 * converted for the boot by machine_rom_path. A file that is no ROM leaves
 * the running program running. It returns false without booting while a DAP
 * client controls the machine. A failed load leaves the machine stopped,
 * because rom_load streams records into live RAM before it can fail, as on
 * hardware where a failed LOAD leaves the CPU stopped in the monitor. */
bool machine_boot_rom(const char *path);

/* The name to boot a ROM file by, from its host path in UTF-8, allocated for
 * the caller to free, or NULL. A path that has a drive path in the code page
 * becomes that drive path. A program could not open any other path, so that
 * file is installed on the null drive and the result is its ":name". */
char *machine_rom_path(const char *host);

/* Back to the start, for tests. */
void machine_reset(void);

#endif /* _HOST_SOKOL_APP_EVQ_MACHINE_H_ */
