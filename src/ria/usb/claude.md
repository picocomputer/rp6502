Review:

src/ria/usb/fail1.cap

src/ria/usb/hcd_rp2040.c
src/ria/usb/rp2040_usb.c
src/ria/usb/msc_host.c
src/ria/usb/msc.c
/home/rumble/rp6502/src/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c

Compare to published specs and fix anything that doesn't comply.
Pay careful attention to the SIE state.
Pay careful attention to the stall and abort paths.

_exit@0x2002472c (\home\rumble\.pico-sdk\sdk\2.2.0\src\rp2_common\pico_clib_interface\newlib_interface.c:45)
panic@0x20014882 (\home\rumble\.pico-sdk\sdk\2.2.0\src\rp2_common\pico_platform_panic\panic.c:82)
hcd_rp2040_irq@0x2001232c (\home\rumble\rp6502\src\ria\usb\hcd_rp2040.c:243)
irq_handler_chain_slots@0x20043f2c (Unknown Source:0)

This crash is what we're trying to fix.

DO NOT FUCKING COMPARE TO UPSTREAM. UPSTREAM IS FUCKING BROKEN WORSE THAN US.
DO NOT FUCKING COMPARE TO UPSTREAM. UPSTREAM IS FUCKING BROKEN WORSE THAN US.
DO NOT FUCKING COMPARE TO UPSTREAM. UPSTREAM IS FUCKING BROKEN WORSE THAN US.
DO NOT FUCKING COMPARE TO UPSTREAM. UPSTREAM IS FUCKING BROKEN WORSE THAN US.
DO NOT FUCKING COMPARE TO UPSTREAM. UPSTREAM IS FUCKING BROKEN WORSE THAN US.
DO NOT FUCKING COMPARE TO UPSTREAM. UPSTREAM IS FUCKING BROKEN WORSE THAN US.
