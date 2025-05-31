#include "ser_cdc.h"
#include "tusb.h"
#include "modem.h"

static bool signals[8];

void ser_set(unsigned int signal, bool val)
{
    signals[signal] = val;
}

bool ser_get(unsigned int signal)
{
    uint8_t line_state = 3;
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

bool ser_is_readable(ser_inst_t ser)
{
    (void)ser;
    // return tud_cdc_n_available((unsigned int)ser) != 0;
    return false;
}

bool ser_is_writeable(ser_inst_t ser)
{
    (void)ser;
    // return tud_cdc_n_write_available((unsigned int)ser) != 0;
    return false;
}

char ser_getc(ser_inst_t ser)
{
    (void)ser;
    // return (char)tud_cdc_n_read_char((unsigned int)ser);
    return -1;
}

void ser_putc(ser_inst_t ser, char c)
{
    (void)c;
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
    (void)c;
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
    (void)ser;

    // tud_cdc_n_write_flush((unsigned int)ser);
    // tud_task();
}

void ser_puts(ser_inst_t ser, const char *s)
{
    (void)ser;
    (void)s;
    // uint32_t written = 0;
    // uint32_t length = strlen(s);
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
