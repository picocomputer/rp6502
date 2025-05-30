#include <stdbool.h>
#include <stdint.h>
#include "ser_hal.h"
#include "tusb.h"
#include "modem.h"

static bool signals[8];

void ser_set(unsigned int signal, bool val)
{
    signals[signal] = val;
}

bool ser_get(unsigned int signal)
{
    uint8_t line_state;
    if (signal == DTR || signal == RTS)
    {
        // line_state = tud_cdc_n_get_line_state(0);
        if (signal == DTR)
            return !!(line_state & 0x01);
        else // RTS
            return !!(line_state & 0x02);
    }
    else
    {
        return signals[signal];
    }
}

unsigned int ser_set_baudrate(ser_inst_t ser, unsigned int baudrate)
{
    // Setting is done from the host
    // cdc_line_coding_t line_coding;
    // tud_cdc_n_get_line_coding((uint8_t)ser, &line_coding);
    // return line_coding.bit_rate;
}

void ser_set_format(ser_inst_t ser, unsigned int dataBits, unsigned int stopBits, ser_parity_t parity)
{
    // Setting is done from the host
}

void ser_set_translate_crlf(ser_inst_t ser, bool translate)
{
    // uart_set_translate_crlf(uarts[ser],translate);
}

bool ser_is_readable(ser_inst_t ser)
{
    // return tud_cdc_n_available((unsigned int)ser) != 0;
}

bool ser_is_writeable(ser_inst_t ser)
{
    // return tud_cdc_n_write_available((unsigned int)ser) != 0;
}

char ser_getc(ser_inst_t ser)
{
    // return (char)tud_cdc_n_read_char((unsigned int)ser);
}

void ser_putc(ser_inst_t ser, char c)
{
    while (!ser_is_writeable(ser))
    {
        // tud_cdc_n_write_flush((unsigned int)ser);
        // tud_task();
    }
    // tud_cdc_n_write_char((unsigned int)ser, c);
    // tud_cdc_n_write_flush((unsigned int)ser);
}

void ser_putc_raw(ser_inst_t ser, char c)
{
    while (!ser_is_writeable(ser))
    {
        // tud_cdc_n_write_flush((unsigned int)ser);
        // tud_task();
    }
    // tud_cdc_n_write_char((unsigned int)ser, c);
    // tud_cdc_n_write_flush((unsigned int)ser);
}

void ser_tx_wait_blocking(ser_inst_t ser)
{
    // tud_cdc_n_write_flush((unsigned int)ser);
    // tud_task();
}

void ser_puts(ser_inst_t ser, const char *s)
{
    uint32_t written = 0;
    uint32_t length = strlen(s);
    // do
    // {
    //     written += tud_cdc_n_write((unsigned int)ser, (char *)(s + written), length - written);
    //     tud_cdc_n_write_flush((unsigned int)ser);
    //     if (written < length)
    //         tud_task();
    // } while (written < length);
}

void ser_set_break(ser_inst_t ser, bool en)
{
    if (en)
    {
        while (!ser_is_writeable(ser))
        {
            // tud_cdc_n_write_flush((unsigned int)ser);
            // tud_task();
        }
        // tud_cdc_n_write_char((unsigned int)ser, (char)243); // Send BRK. Assumes Telnet. ok?
        // tud_cdc_n_write_flush((unsigned int)ser);
    }
}

void ser_set_hw_flow(ser_inst_t ser, bool cts, bool rts)
{
    // uart_set_hw_flow(uarts[ser], cts, rts);
}
