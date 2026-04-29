# Arduino Uno R4 WiFi Example

Smoke test for breadboard's `bb_log_*`, `bb_nv_*`, `bb_wifi_*`, and `bb_http_*` APIs on the Arduino Uno R4 WiFi using its built-in radio (`WiFiS3`).

This is the same shape as the [`arduino-uno-cc3000`](../arduino-uno-cc3000/) example — the only difference is the build flag `-DBB_WIFI_BACKEND_R4` selecting the WiFiS3 backend.

## Hardware

- Arduino Uno R4 WiFi (board id `uno_r4_wifi`).
- USB-C cable.

The R4 WiFi has the radio built-in; no shield required.

## Secrets

```bash
cp include/secrets.h.example include/secrets.h
# Edit include/secrets.h and set WIFI_SSID and WIFI_PASS
```

## Build and Flash

```bash
pio run -e uno_r4_wifi -t upload
```

## Monitor

```bash
pio device monitor
```

Expected output:
```
I (bb-uno-r4) boot
I (bb-uno-r4) boot=1
I (bb_wifi) wifi connected
I (bb_wifi) ip <your-dhcp-ip>
```

The boot count increments across resets, demonstrating EEPROM round-trip through `bb_nv_get_u32` / `bb_nv_set_u32`. (R4 WiFi emulates EEPROM in flash.)

## Testing `/ping`

```bash
curl http://<dhcp-ip>/ping
# Expected: pong
```

## Backend swap

To run the same `src/main.cpp` against a CC3000 shield on a classic Uno, see the [`arduino-uno-cc3000`](../arduino-uno-cc3000/) example. The app code is identical; the build flag and `lib_deps` are the only differences.
