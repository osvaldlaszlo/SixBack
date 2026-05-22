# Third-Party Components

SixBack links against the following third-party libraries.  Each retains its
own license; nothing on this page is altered by SixBack's own
[LICENSE](LICENSE) (PolyForm Noncommercial 1.0.0).

| Component | License | Project URL |
|---|---|---|
| Arduino-ESP32 framework (board + cores + WiFi / lwIP / Update / Preferences / LittleFS / mDNS / FS / Network / HTTPClient) | LGPL-2.1-or-later | https://github.com/espressif/arduino-esp32 |
| ESP32Async / ESPAsyncWebServer | LGPL-3.0-or-later | https://github.com/ESP32Async/ESPAsyncWebServer |
| ESP32Async / AsyncTCP | LGPL-3.0-or-later | https://github.com/ESP32Async/AsyncTCP |
| bblanchon / ArduinoJson | MIT | https://github.com/bblanchon/ArduinoJson |
| tostmann / improv-wifi-busware (fork of Improv-WiFi-Library) | Apache-2.0 | https://github.com/tostmann/improv-wifi-busware |
| ESP-IDF (lwIP, mbedTLS, FreeRTOS, NVS, esp_wifi, ...) | Apache-2.0 / BSD-3-Clause / public-domain (varies per file) | https://github.com/espressif/esp-idf |

## LGPL — what it means for the SixBack firmware binary

The LGPL-licensed components above (Arduino-ESP32, AsyncWebServer, AsyncTCP)
are statically linked into the SixBack firmware image.  Under LGPL-2.1
section 6 / LGPL-3.0 section 4(d), recipients of the firmware binary have
the right to relink the binary with a modified version of the LGPL'd
libraries.

To satisfy this:

1.  The source of each LGPL'd library is publicly available at the project
    URL listed above.  Recipients can clone, modify, and rebuild from
    there.
2.  The exact library versions SixBack v0.7.x links against are pinned in
    [`platformio.ini`](platformio.ini) (`lib_deps`).  PlatformIO will fetch
    the same versions during a relink.
3.  SixBack's own build instructions are in [`README.md`](README.md) and
    [`scripts/build_release.sh`](scripts/build_release.sh) — anyone with
    a working PlatformIO toolchain can rebuild a binary identical to the
    one shipped on install.busware.de (excluding only the auto-incremented
    `build_number.txt` counter).

The LGPL relink obligation applies regardless of how SixBack itself is
licensed.  If you redistribute the binary, you inherit this obligation
together with the obligations from SixBack's own PolyForm Noncommercial
license.

## License compatibility note

PolyForm Noncommercial 1.0.0 is **not** an OSI-approved open-source license.
This means SixBack as a whole cannot be re-distributed under a more
permissive open-source umbrella.  The LGPL'd dependencies remain available
to anyone under their original LGPL terms — re-using them in a fully
open-source project is unaffected by SixBack's licensing choice.

## Trademarks

Bose® and SoundTouch® are registered trademarks of Bose Corporation.
SixBack is not affiliated with, endorsed by, or sponsored by Bose
Corporation.  The trademarks appear here only nominatively, to describe
which hardware SixBack interoperates with.
