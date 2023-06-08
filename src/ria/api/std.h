/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _STD_H_
#define _STD_H_

// Kernel events
void std_stop(void);

/*
 * The API implementation for stdio support.
 */

void std_api_open(void);
void std_api_close(void);
void std_api_read_(void);
void std_api_readx(void);
void std_api_write_(void);
void std_api_writex(void);
void std_api_lseek(void);

#endif /* _STD_H_ */
