/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_APF_H_
#define _HOST_POCKET_SW_APF_H_

/* APF's controller slots as HID devices, this platform's usb.c.
 */

void apf_init(void);
void apf_task(void);

/* Re-send every slot on the next pass. For a program mapping a driver
 * into XRAM: the mapping blanks the record and these registers are
 * levels, so a control standing still would leave the blank there. */
void apf_refresh(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define APF_DRIVER DRIVER(apf_init, apf_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_APF_H_ */
