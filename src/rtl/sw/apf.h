/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_APF_H_
#define _FPGA_SW_APF_H_

/* APF's controller slots as HID devices, this platform's usb.c.
 */

void apf_init(void);
void apf_task(void);

/* Re-send every slot on the next pass. For a program mapping a driver
 * into XRAM: the mapping blanks the record and these registers are
 * levels, so a control standing still would leave the blank there. */
void apf_refresh(void);

#endif /* _FPGA_SW_APF_H_ */
