# Reference for Picocomputer WiFi Modem

This is some code I found that I thought might work in the Picocomputer
RP67502 RIA. It turned out to have too many blocking operations.
It has been reorganized from Arduino into C.
This code does not work. However, it does compile.

DO NOT USE. Check out one of the projects below instead.
You have been warned. This is here because it's useful as
a checklist while writing a new and fully non-blocking
modem for the Picocomputer. Super useful to avoid forgetting
arcane details like disabling Nagle's algorithm.


## Licensing and Credits

This project is based on code and ideas from the following repositories:

- https://github.com/ksalin/esp8266_modem
- https://github.com/mecparts/RetroWiFiModem
- https://github.com/mecparts/PicoWiFiModem
- https://github.com/sodiumlb/PicoWiFiModemUSB
- https://github.com/bozimmerman/Zimodem

The code includes contributions under the GPL-3.0 license from:

- Original Source: (C) 2016 Jussi Salin <salinjus@gmail.com>
- Additions: (C) 2018 Daniel Jameson, Stardot Contributors
- Additions: (C) 2018 Paul Rickards <rickards@gmail.com>
- Additions: 2020-2022 Wayne Hortensius
