#include "tusb_config.h"
#include "tusb.h"
#include "usb_cdc.h"
#include "pico/stdio/driver.h"
#include "pico/cyw43_arch.h"

extern volatile bool dtrWentInactive;
static bool cdc_led;

void cdc_stdio_out_chars(const char *buf, int length);
void cdc_stdio_out_flush(void);
static int cdc_stdio_in_chars(char *buf, int length);

static stdio_driver_t cdc_stdio_app = {
    .out_chars = cdc_stdio_out_chars,
    .out_flush = cdc_stdio_out_flush,
    .in_chars = cdc_stdio_in_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
};

void cdc_stdio_out_chars(const char *buf, int length)
{
    // for(uint8_t i=0; i < CFG_TUH_CDC; i++){
    uint32_t written = 0;
    do
    {
        written += tud_cdc_n_write(0, (char *)(buf + written), length - written);
        if (written < length)
            tud_task();
    } while (written < length);
    //}
}

void cdc_stdio_out_flush(void)
{
    // for(uint8_t i=0; i < CFG_TUH_CDC; i++){
    tud_cdc_n_write_flush(0);
    //}
}

static int cdc_stdio_in_chars(char *buf, int length)
{
    int ret = 0;
    // for(uint8_t i=0; i < CFG_TUH_CDC; i++){
    if (tud_cdc_n_available(0))
    {
        ret = tud_cdc_n_read(0, buf, length);
        cdc_led = false;
        //        break;
    }
    //}
    return ret;
}

void cdc_init(void)
{
    stdio_set_driver_enabled(&cdc_stdio_app, true);
}

void cdc_task(void)
{
    cdc_stdio_out_flush();
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cdc_led);
}

/*
// Invoked when received new data
TU_ATTR_WEAK void tud_cdc_rx_cb(uint8_t itf);

// Invoked when received `wanted_char`
TU_ATTR_WEAK void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted_char);

// Invoked when a TX is complete and therefore space becomes available in TX buffer
TU_ATTR_WEAK void tud_cdc_tx_complete_cb(uint8_t itf);

// Invoked when line coding is change via SET_LINE_CODING
TU_ATTR_WEAK void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding);

// Invoked when received send break
TU_ATTR_WEAK void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms);
*/

// Invoked when line state DTR & RTS are changed via SET_CONTROL_LINE_STATE
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    static bool oneshot = true;
    /*if(dtr)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN,true);
        //tud_cdc_n_write(0,"*",1);
    else
        if(oneshot){
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN,false);
            oneshot = false;
        }
        //tud_cdc_n_write(0,"#",1);
    */
    dtrWentInactive = !dtr;
}
