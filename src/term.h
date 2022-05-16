/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TERM_H_
#define _TERM_H_

struct scanvideo_scanline_buffer;

void term_init();
void term_task();
void term_render(struct scanvideo_scanline_buffer *dest);

#endif /* _TERM_H_ */
