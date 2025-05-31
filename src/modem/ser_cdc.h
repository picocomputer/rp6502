
/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Sodiumlightbaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifndef _MODEM_SER_CDC_H_
#define _MODEM_SER_CDC_H_

#include <stdbool.h>

typedef enum
{
    ser0,
    ser1
} ser_inst_t;

void ser_set(unsigned int signal, bool val);
bool ser_get(unsigned int signal);
bool ser_is_readable(ser_inst_t ser);
bool ser_is_writeable(ser_inst_t ser);
char ser_getc(ser_inst_t ser);
void ser_putc(ser_inst_t ser, char c);
void ser_putc_raw(ser_inst_t ser, char c);
void ser_tx_wait_blocking(ser_inst_t ser);
void ser_puts(ser_inst_t ser, const char *s);
void ser_set_break(ser_inst_t ser, bool en);

#endif /* _MODEM_SER_CDC_H_ */
