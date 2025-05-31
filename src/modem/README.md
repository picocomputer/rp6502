The "Picocomputer WiFi Modem" may contain code from the following repositories.

* https://github.com/ksalin/esp8266_modem
* https://github.com/mecparts/RetroWiFiModem
* https://github.com/mecparts/PicoWiFiModem
* https://github.com/sodiumlb/PicoWiFiModemUSB

I found the following copyright notices and GPL-3.0 licensing for everything.

* Original Source Copyright (C) 2016 Jussi Salin <salinjus@gmail.com>
* Additions (C) 2018 Daniel Jameson, Stardot Contributors
* Additions (C) 2018 Paul Rickards <rickards@gmail.com>
* Additions 2020-2022 Wayne Hortensius

At first glance, this reference code looks non-blocking. Unfortunately,
there are actuall many blocking operations. The original project was
structured for Adruino and ultimately needed to be completely torn
apart and reassembled.
