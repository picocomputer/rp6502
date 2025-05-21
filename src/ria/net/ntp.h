/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _NTP_H_
#define _NTP_H_

void ntp_init(void);
void ntp_task(void);
void ntp_print_status(void);

#endif /* _NTP_H_ */
