# Picoprobe

This has a few changes:

main.c is replaced entirely with rp6502 variant.

probe.c hardcoded pio0 changed to pio1 because pico-extras VGA
decided to hardcode pio0 in a couple places.

cdc_uart.c tees rx to putchar_raw() for display on term.

picoprobe_config.h changes GPIO pins.
#define PROBE_PIN_OFFSET 22
#define PROBE_PIN_SWCLK PROBE_PIN_OFFSET + 0 // 22
#define PROBE_PIN_SWDIO PROBE_PIN_OFFSET + 6 // 28
