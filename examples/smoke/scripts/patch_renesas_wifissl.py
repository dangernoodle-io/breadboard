"""
Pre-build patch for the Arduino Renesas core's WiFiSSLClient.

Upstream uses `byte` (a typedef of uint8_t in Arduino's Common.h) in the
`setEccSlot` signature. On libstdc++ >= 11, `<cstddef>` exports `std::byte`
to the global namespace, which renders the unqualified `byte` ambiguous
between the .h declaration and the .cpp definition — the build fails with
"no declaration matches".

Substituting `byte` -> `uint8_t` is semantically a no-op (same type) and
disambiguates against std::byte. The patch is idempotent.

Tracking: arduino/ArduinoCore-renesas (no fix PR as of 2026-04).
Remove this hook once upstream lands a fix.
"""
import os

Import("env")  # provided by PlatformIO

FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-arduinorenesas-uno")
TARGETS = [
    os.path.join(FRAMEWORK_DIR, "libraries", "WiFiS3", "src", "WiFiSSLClient.h"),
    os.path.join(FRAMEWORK_DIR, "libraries", "WiFiS3", "src", "WiFiSSLClient.cpp"),
]
NEEDLE = "const byte cert[]"
REPL = "const uint8_t cert[]"

for path in TARGETS:
    if not os.path.exists(path):
        continue
    with open(path, "r") as f:
        text = f.read()
    if NEEDLE not in text:
        continue
    with open(path, "w") as f:
        f.write(text.replace(NEEDLE, REPL))
    print("[patch_renesas_wifissl] patched %s" % path)
