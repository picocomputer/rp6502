# Picoprobe

This has a few changes:

probe.c hardcoded pio0 changed to pio1 because pico-extras VGA
decided to hardcode pio0 in a couple places.

cdc_uart.c tees rx to putchar_raw() for display on term.

cdc_uart.c remove tud_cdc_line_coding_cb()

cdc_uart.c support tud_cdc_send_break_cb()

picoprobe_config.h changes GPIO pins.
#define PROBE_PIN_OFFSET 22
#define PROBE_PIN_SWCLK PROBE_PIN_OFFSET + 0 // 22
#define PROBE_PIN_SWDIO PROBE_PIN_OFFSET + 6 // 28
