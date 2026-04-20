# Arduino Uno + CC3000 Example

This example demonstrates breadboard's `bb_log_*` and `bb_nv_*` APIs on an Arduino Uno with Adafruit CC3000 WiFi shield.

## Hardware Setup

**Uno Pinout** (CC3000 shield v1.1):
- CS: pin 10
- IRQ: pin 3
- VBAT: pin 5

Consult the [Adafruit CC3000 library docs](https://github.com/adafruit/Adafruit_CC3000_Library) for v1.0 shield pinouts if using an older shield.

## Secrets

Copy the example secrets file and fill in your WiFi credentials:

```bash
cp include/secrets.h.example include/secrets.h
# Edit include/secrets.h and set WIFI_SSID and WIFI_PASS
```

## Build and Flash

```bash
pio run -e uno -t upload
```

## Monitor

View serial output:

```bash
pio device monitor
```

Expected output:
```
I (bb-uno) boot
I (bb-uno) boot=1
I (bb-uno) cc3000 begin...
I (bb-uno) cc3000 begin ok
I (bb-uno) fw 1.24
I (bb-uno) wifi assoc failed
I (bb-uno) online
I (bb-uno) listening on :80
```

The boot count increments across resets, demonstrating EEPROM round-trip through `bb_nv_get_u32` / `bb_nv_set_u32`.

## Testing `/ping`

Once the Uno is online, test the HTTP endpoint:

```bash
curl http://<dhcp-ip>/ping
# Expected response: pong
```

## Notes

### Firmware Update (if needed)

CC3000 firmware 1.24 may exhibit flaky TCP accept behavior due to a known `SOCKOPT_ACCEPT_NONBLOCK` issue. If `/ping` requests hang or drop frequently, consider updating the CC3000 firmware using Adafruit's [firmware patcher](https://github.com/adafruit/Adafruit_CC3000_Library/blob/master/FIRMWARE.md).

### Local toolchain override (Apple Silicon)

PlatformIO bundles an x86_64 `avr-gcc` which fails on arm64 Macs with "Bad CPU type in executable". To use a native Homebrew toolchain:

1. Install avr-gcc and avrdude:
   ```bash
   brew tap osx-cross/avr
   brew install avr-gcc@9 avrdude
   ```

2. Create `~/.local/pio-avr` and `~/.local/pio-avrdude` symlink farms. For reference, the structures are:
   ```
   ~/.local/pio-avr/
   ├── avr -> /opt/homebrew/opt/avr-gcc@9/avr
   ├── bin -> /opt/homebrew/opt/avr-gcc@9/bin
   └── package.json (minimal metadata)
   
   ~/.local/pio-avrdude/
   ├── bin -> /opt/homebrew/opt/avrdude/bin
   └── package.json (minimal metadata)
   ```

3. Copy the example override file and edit with your paths:
   ```bash
   cp platformio_local.ini.example platformio_local.ini
   # Edit platformio_local.ini: replace YOUR_USERNAME with your macOS login
   ```

The `platformio_local.ini` file is gitignored, so your personal paths never commit to the repo. On CI (Ubuntu x86_64), the bundled toolchain works natively.

## Flash / SRAM Budget

The Uno has 32 KB flash and 2 KB SRAM. Current binary sizes:
- Text + data: ~6–8 KB (varies with CC3000 lib linkage)
- Runtime heap: <1 KB (CC3000 ~1.2 KB, app ~100 B)

See `pio run -e uno` output for the final `avr-size` report.
