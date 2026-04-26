#!/usr/bin/env python3
"""
breadboard native-test host scaffold for PlatformIO.

PlatformIO extra_script that wires breadboard bb_* component includes and source
files (both host and espidf) into the consumer's [env:native] based on a
custom_bb_components declaration.

Consumers declare components via:
  custom_bb_components = bb_log bb_nv bb_json

Script appends:
  - -I<breadboard_root>/components/<name>/include to build_flags
  - -I<breadboard_root>/components/<name>/src (if it exists) to build_flags
  - absolute paths to platform/host/ and platform/espidf/ source files to build_src_filter

If bb_json is included, the consumer MUST add to platformio.ini:
  lib_deps = https://github.com/DaveGamble/cJSON.git#v1.7.18

Paths are resolved relative to BREADBOARD_ROOT (the directory containing this
script/../..), ensuring absolute path safety whether the consumer symlinked
breadboard, cloned it, or installed a tagged release.
"""

import inspect
import os

Import("env")

# Anchor breadboard root from the script's own location.
# Script lives at <breadboard_root>/scripts/native_scaffold.py — going up two
# levels gets us to <breadboard_root>, regardless of where PIO is running from.
# Works identically whether breadboard is the project itself or a consumer's
# .breadboard symlink/clone, and isn't fooled by a consumer that happens to
# have its own components/ directory at the same level.
#
# `__file__` isn't set when SCons exec()'s the script — inspect the current
# frame's code-object filename (which SCons sets via compile(..., scriptname))
# to recover the absolute path.
_SCRIPT_PATH = os.path.abspath(inspect.currentframe().f_code.co_filename)
BREADBOARD_ROOT = os.path.dirname(os.path.dirname(_SCRIPT_PATH))

# Component mapping: name -> (includes, sources)
# includes: list of relative paths under BREADBOARD_ROOT
# sources: list of relative paths under BREADBOARD_ROOT
COMPONENT_MAP = {
    "bb_log": (
        ["components/bb_log/include", "components/bb_log/src"],
        [
            "platform/espidf/bb_log/bb_log.c",
            "platform/host/bb_log/bb_log_level.c",
            "components/bb_log/src/bb_log_level.c",
        ],
    ),
    "bb_nv": (
        ["components/bb_nv/include"],
        ["platform/espidf/bb_nv/bb_nv.c"],
    ),
    "bb_json": (
        ["components/bb_json/include"],
        ["platform/espidf/bb_json/bb_json_cjson.c"],
    ),
    "bb_http": (
        ["components/bb_http/include"],
        ["components/bb_http/src/http_utils.c"],
    ),
    "bb_prov": (
        ["components/bb_prov/include"],
        ["components/bb_prov/src/bb_prov_parse.c"],
    ),
    "bb_wifi": (
        ["components/bb_wifi/include"],
        ["platform/host/bb_wifi/bb_wifi_host.c"],
    ),
    "bb_ntp": (
        ["components/bb_ntp/include"],
        ["platform/host/bb_ntp/bb_ntp_host.c"],
    ),
    "bb_system": (
        ["components/bb_system/include"],
        ["platform/host/bb_system/bb_system_host.c"],
    ),
    "bb_ota_pull": (
        ["components/bb_ota_pull/include"],
        ["platform/espidf/bb_ota_pull/bb_ota_pull.c"],
    ),
    "bb_ota_push": (
        ["components/bb_ota_push/include"],
        ["platform/espidf/bb_ota_push/bb_ota_push.c"],
    ),
    "bb_ota_validator": (
        ["components/bb_ota_validator/include"],
        ["platform/host/bb_ota_validator/bb_ota_validator_host.c"],
    ),
    "bb_mdns": (
        ["components/bb_mdns/include"],
        ["platform/host/bb_mdns/bb_mdns_host.c"],
    ),
}

cJSON_LIB_DEP = "https://github.com/DaveGamble/cJSON.git#v1.7.18"

# Read the custom_bb_components option (space-separated component names)
components_str = env.GetProjectOption("custom_bb_components", "")

if components_str.strip():
    component_names = components_str.split()

    # Validate and process each component
    for name in component_names:
        if name not in COMPONENT_MAP:
            print(
                f"bb_native_scaffold: unknown component '{name}'; known: {sorted(COMPONENT_MAP.keys())}"
            )
            env.Exit(1)

        includes, sources = COMPONENT_MAP[name]

        # Wire include paths
        for inc_path in includes:
            abs_inc = os.path.join(BREADBOARD_ROOT, inc_path)
            flag = f"-I{abs_inc}"
            # Skip if already in BUILD_FLAGS
            if flag not in env.get("BUILD_FLAGS", []):
                env.Append(BUILD_FLAGS=[flag])

        # Wire source files to build_src_filter
        for src_path in sources:
            abs_src = os.path.join(BREADBOARD_ROOT, src_path)
            src_filter_entry = f"+<{abs_src}>"
            # Check if already in SRC_FILTER (PlatformIO's build_src_filter key)
            current_filter = env.get("SRC_FILTER", "")
            if src_filter_entry not in current_filter:
                env.Append(SRC_FILTER=[src_filter_entry])

        # Print one line per component for build visibility
        print(f"bb_native_scaffold: {name} -> {len(sources)} sources, {len(includes)} includes")
