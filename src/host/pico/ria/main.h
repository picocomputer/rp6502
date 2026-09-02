/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MAIN_H_
#define _RIA_MAIN_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/driver.h"
#include "core/sys/sys.h"

/* This manages the main loop for the operating system.
 * Device drivers (everything is a device driver) are notified of various
 * events like init, task, run, stop, break, and reclock. The walks come from
 * this machine's drivers.h; the two task columns are pumped separately,
 * because only one of them is safe to call during blocking file IO.
 * API and XREG calls are dispatched from here too. Everything follows
 * this pattern so it's worth reading main.c in its entirety.
 */

#endif /* _RIA_MAIN_H_ */
