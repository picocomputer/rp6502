
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

#ifndef _SER_HAL_H_
#define _SER_HAL_H_

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        ser0,
        ser1
    } ser_inst_t;
    typedef enum
    {
        SER_PARITY_NONE,
        SER_PARITY_EVEN,
        SER_PARITY_ODD
    } ser_parity_t;

    void ser_set(unsigned int signal, bool val);
    bool ser_get(unsigned int signal);
    unsigned int ser_set_baudrate(ser_inst_t ser, unsigned int baudrate);
    void ser_set_format(ser_inst_t ser, unsigned int dataBits, unsigned int stopBits, ser_parity_t parity);
    void ser_set_translate_crlf(ser_inst_t ser, bool translate);
    bool ser_is_readable(ser_inst_t ser);
    bool ser_is_writeable(ser_inst_t ser);
    char ser_getc(ser_inst_t ser);
    void ser_putc(ser_inst_t ser, char c);
    void ser_putc_raw(ser_inst_t ser, char c);
    void ser_tx_wait_blocking(ser_inst_t ser);
    void ser_puts(ser_inst_t ser, const char *s);
    void ser_set_break(ser_inst_t ser, bool en);
    void ser_set_hw_flow(ser_inst_t ser, bool cts, bool rts);

#ifdef __cplusplus
}
#endif

#endif /* _SER_HAL_H_ */
