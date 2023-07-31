/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_H_
#define _RIA_H_

void ria_init(void);
void ria_reclock(void);
void ria_backchan_req(void);
void ria_backchan_ack(void);
void ria_vsync(void);

#endif /* _RIA_H_ */
