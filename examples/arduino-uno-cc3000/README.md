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

### macOS ARM64 (M1/M2)

If the PlatformIO Homebrew toolchain (x86_64) doesn't run under Rosetta on your Mac, set up a local symlink-farm toolchain:

1. Install the cross-compiler:
   ```bash
   brew tap osx-cross/avr
   brew install avr-gcc avrdude
   ```

2. Create symlinks in `~/.local/pio-avr` and `~/.local/pio-avrdude` pointing to the Homebrew-installed toolchain paths.

3. Create `platformio_local.ini` in this directory:
   ```ini
   [env:uno]
   platform_packages =
       toolchain-atmelavr@symlink:///Users/yourname/.local/pio-avr
       tool-avrdude@symlink:///Users/yourname/.local/pio-avrdude
   ```

(This file is gitignored to avoid committing personal paths.)

## Flash / SRAM Budget

The Uno has 32 KB flash and 2 KB SRAM. Current binary sizes:
- Text + data: ~6–8 KB (varies with CC3000 lib linkage)
- Runtime heap: <1 KB (CC3000 ~1.2 KB, app ~100 B)

See `pio run -e uno` output for the final `avr-size` report.
