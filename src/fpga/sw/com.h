/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_COM_H_
#define _FPGA_SW_COM_H_

#include "ria/sys/com.h"

/* Bytes moved through the manifold since boot, every direction summed.
 * The simulation's main loop watches it to know when the machine went
 * quiet; real hardware never exits and never asks. */
uint32_t com_moved(void);

#endif /* _FPGA_SW_COM_H_ */
