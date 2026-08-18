# License

This repository, **iface-ble** (RNS over Bluetooth Low Energy for reticulous,
speaking the ble-reticulum v2.2 protocol), is released under the
**Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None. The wire protocol is torlando-tech/ble-reticulum's `BLE_PROTOCOL_v2.2`,
implemented independently here; no upstream code is reused.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml`:

| Component / package | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| Apache NimBLE (bundled with ESP-IDF, `components/bt`) | apache/mynewt-nimble | Apache-2.0 |
