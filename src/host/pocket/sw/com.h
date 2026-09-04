/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_COM_H_
#define _HOST_POCKET_SW_COM_H_

#include "core/sys/com.h"
#include "core/com/com.h"

/* This machine's once-a-frame console service. The rest of the console is the
 * shared one: what any machine may ask of a console is core/sys/com.h, and what
 * this machine's is made of -- the rings, the bell, the single sink -- is
 * core/com/com.h. */
void com_task(void);

#endif /* _HOST_POCKET_SW_COM_H_ */
