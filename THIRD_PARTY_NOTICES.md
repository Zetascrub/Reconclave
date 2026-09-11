# Third-party dependencies and artwork

Dependency code retains its upstream licence and notices. Reconclave's code
licence does not replace them. Source dependencies are fetched by the build;
this file is an inventory, not a replacement for their complete licence texts.

| Component | Source / licence information |
|---|---|
| RadioLib | https://github.com/jgromes/RadioLib — MIT |
| ArduinoJson | https://github.com/bblanchon/ArduinoJson — MIT |
| NimBLE-Arduino | https://github.com/h2zero/NimBLE-Arduino — Apache-2.0; inspect bundled components |
| M5Unit-NFC, M5UnitUnified, M5HAL, M5Utility | M5Stack upstream repositories — MIT declarations |
| M5Cardputer, M5Unified, M5GFX | M5Stack upstream repositories; preserve upstream and bundled notices |
| ESP-IDF / Arduino-ESP32 / toolchains | Espressif distribution and bundled component licences |
| Python zeroconf and cryptography | Installed package metadata and bundled dependency licences |
| mjansson/mdns (`devices/k230/third_party/mdns.h`) | https://github.com/mjansson/mdns — Public Domain |
| LVGL, libdrm (K230 UI cross-compile headers, `.toolchains/device-sysroot/` — not committed, see `devices/k230/fetch-device-sysroot.sh`) | https://github.com/lvgl/lvgl — MIT; https://gitlab.freedesktop.org/mesa/drm — MIT. Runtime `.so` files are pulled from the device's own stock image (Canaan/kendryte Buildroot build), not built by this project. |
| React, Vite, TypeScript and frontend dependencies | Exact resolved versions in tools/desktop-node/web/package-lock.json; upstream package notices |

Before distributing compiled firmware, produce a complete inventory and notices
from that build's resolved dependencies, including transitive libraries and
fonts. The current public release process distributes project source only.

Mascot artwork and generated image headers live under
`devices/cardputer-adv/assets/`, `devices/cardputer-adv/src/assets/`, and
`devices/k230/assets/`.
The owner has reserved rights to these assets under [ARTWORK_LICENSE.md](ARTWORK_LICENSE.md);
they are excluded from the MIT code licence. This inventory does not establish
provenance or grant rights to third-party material.
