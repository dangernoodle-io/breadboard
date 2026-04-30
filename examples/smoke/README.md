# Breadboard smoke example

A single portable smoke app that exercises `bb_log` + `bb_nv` + `bb_wifi` + `bb_http` across multiple boards. The same `src/smoke_app.c` compiles unchanged on every env; the entry shim and `bb_wifi` backend differ.

## Envs

| env | board | bb_wifi backend | hardware |
|---|---|---|---|
| `r4_wifis3` | Arduino UNO R4 WiFi | WiFiS3 (on-board ESP32-S3) | verified |

ESP-IDF envs (`esp32`, `esp32p4`, `esp32c3`, `tdongle`) land in a follow-up PR.

The CC3000 backend lives in its own example at `examples/arduino-uno-cc3000/` (AVR Uno only) — Adafruit_CC3000 doesn't support the renesas-ra SPI API, so a CC3000 env can't be folded into smoke until the upstream library is forked or replaced.

## Setup

```bash
cp include/secrets.h.example include/secrets.h
# Edit include/secrets.h and set WIFI_SSID / WIFI_PASS
```

## Build

```bash
pio run -e r4_wifis3
```

## Flash + monitor (R4 WiFi)

```bash
pio run -e r4_wifis3 -t upload
pio device monitor -e r4_wifis3
```

## Test

Once associated, `curl http://<ip>/ping` returns `pong`.

## Apple Silicon notes

The PlatformIO bundled `arm-none-eabi-gcc` and `bossac` for the Renesas RA platform are x86_64 binaries. On arm64 Macs without Rosetta, install native equivalents and override via `platformio_local.ini`:

1. Install Arm GNU Toolchain (native arm64 build, not the homebrew `gcc-arm-embedded` cask which currently bundles GCC 15 — incompatible with the Arduino renesas core libstdc++):
   ```bash
   curl -L -o /tmp/arm-gnu.tar.xz \
     https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-darwin-arm64-arm-none-eabi.tar.xz
   mkdir -p ~/.local/arm-gnu-13.3
   tar -xJf /tmp/arm-gnu.tar.xz -C ~/.local/arm-gnu-13.3 --strip-components=1
   ```
2. Install Arduino's BOSSA fork from source (vanilla homebrew `bossa` doesn't recognize the R4):
   ```bash
   git clone --depth 1 https://github.com/arduino/BOSSA.git ~/.local/src/BOSSA
   cd ~/.local/src/BOSSA
   PATH=~/.local/arm-gnu-13.3/bin:$PATH ARMAS=arm-none-eabi-as \
     make VERSION=1.9.1-arduino5 \
       COMMON_CXXFLAGS='-arch arm64 -mmacosx-version-min=10.9 -DVERSION="\"1.9.1-arduino5\""' \
       COMMON_LDFLAGS='-arch arm64 -mmacosx-version-min=10.9' \
       bin/bossac
   ```
3. Build symlink farms and `platformio_local.ini`:
   ```bash
   mkdir -p ~/.local/pio-arm-none-eabi ~/.local/pio-bossac
   ln -sfn ~/.local/arm-gnu-13.3/{bin,lib,share,arm-none-eabi} ~/.local/pio-arm-none-eabi/
   ln -sfn ~/.local/src/BOSSA/bin/bossac ~/.local/pio-bossac/bossac
   echo '{"name":"toolchain-gccarmnoneeabi","version":"13.3.1","system":["darwin_arm64"]}' \
     > ~/.local/pio-arm-none-eabi/package.json
   echo '{"name":"tool-bossac","version":"1.9.1","system":["darwin_arm64"]}' \
     > ~/.local/pio-bossac/package.json
   cp platformio_local.ini.example platformio_local.ini
   # Edit YOUR_USERNAME -> your macOS login
   ```

CI (Linux) uses the bundled toolchain — no override needed.

## Renesas WiFiSSLClient patch

`scripts/patch_renesas_wifissl.py` is a pre-build hook that fixes an upstream Arduino renesas-core ambiguity (`byte` vs `std::byte`) that breaks builds on libstdc++ ≥ 11. Idempotent. Tracking arduino/ArduinoCore-renesas for a fix.
