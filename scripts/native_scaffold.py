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

Transitive deps are auto-resolved: the COMPONENT_MAP entry for each component
declares its required peers via a `depends` field, and the resolver walks the
graph so consumers only list direct dependencies. e.g. listing `bb_nv` pulls
in `bb_core` (for bb_err_t) and `bb_registry` (referenced by bb_nv.c) without
the consumer having to enumerate them. Closes B1-77.

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

# Component mapping: name -> {includes, sources, depends}
# - includes: list of relative paths under BREADBOARD_ROOT
# - sources:  list of relative paths under BREADBOARD_ROOT
# - depends:  list of other bb_* component names this one needs at compile
#             time (mirrors the public REQUIRES from each component's
#             CMakeLists.txt; private REQUIRES that don't surface in headers
#             can be omitted).
COMPONENT_MAP = {
    "bb_core": {
        "includes": ["components/bb_core/include"],
        "sources":  [],
        "depends":  [],
    },
    "bb_log": {
        "includes": ["components/bb_log/include", "components/bb_log/src"],
        "sources": [
            "platform/espidf/bb_log/bb_log.c",
            "platform/host/bb_log/bb_log_level.c",
            "components/bb_log/src/bb_log_level.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_diag": {
        "includes": ["components/bb_diag/include"],
        "sources": [
            "platform/host/bb_diag/bb_diag_panic.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_nv": {
        "includes": ["components/bb_nv/include"],
        "sources":  ["platform/espidf/bb_nv/bb_nv.c"],
        "depends":  ["bb_core", "bb_registry"],
    },
    "bb_json": {
        "includes": ["components/bb_json/include", "platform/host/bb_json"],
        "sources":  ["platform/espidf/bb_json/bb_json_cjson.c"],
        "depends":  ["bb_core"],
    },
    "bb_http": {
        "includes": ["components/bb_http/include"],
        "sources": [
            "components/bb_http/src/http_utils.c",
            "components/bb_http/src/route_registry.c",
            "platform/host/bb_http/bb_http_host.c",
        ],
        "depends":  ["bb_core", "bb_log"],
    },
    "bb_prov": {
        "includes": ["components/bb_prov/include"],
        "sources":  ["components/bb_prov/src/bb_prov_parse.c"],
        "depends":  ["bb_core", "bb_http", "bb_nv"],
    },
    "bb_wifi": {
        "includes": ["components/bb_wifi/include", "components/bb_wifi"],
        "sources": [
            "platform/host/bb_wifi/bb_wifi_host.c",
            "components/bb_wifi/wifi_reconn_policy.c",
        ],
        "depends":  ["bb_core", "bb_log", "bb_nv"],
    },
    "bb_ntp": {
        "includes": ["components/bb_ntp/include"],
        "sources":  ["platform/host/bb_ntp/bb_ntp_host.c"],
        "depends":  ["bb_core", "bb_log", "bb_nv"],
    },
    "bb_system": {
        "includes": ["components/bb_system/include"],
        "sources":  ["platform/host/bb_system/bb_system_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_ota_pull": {
        "includes": ["components/bb_ota_pull/include", "platform/host/bb_ota_pull"],
        "sources":  ["platform/espidf/bb_ota_pull/bb_ota_pull.c"],
        "depends":  ["bb_core", "bb_http", "bb_nv", "bb_log", "bb_json"],
    },
    "bb_ota_push": {
        "includes": ["components/bb_ota_push/include"],
        "sources":  ["platform/espidf/bb_ota_push/bb_ota_push.c"],
        "depends":  ["bb_core", "bb_http", "bb_log"],
    },
    "bb_ota_validator": {
        "includes": ["components/bb_ota_validator/include"],
        "sources":  ["platform/host/bb_ota_validator/bb_ota_validator_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_mdns": {
        "includes": ["components/bb_mdns/include", "platform/host/bb_mdns", "components/bb_mdns"],
        "sources": [
            "platform/host/bb_mdns/bb_mdns_host.c",
            "components/bb_mdns/bb_mdns_lifecycle.c",
        ],
        "depends":  ["bb_core", "bb_nv"],
    },
    "bb_openapi": {
        "includes": ["components/bb_openapi/include"],
        "sources":  ["components/bb_openapi/src/bb_openapi_emit.c"],
        "depends":  ["bb_core", "bb_http", "bb_json"],
    },
    "bb_manifest": {
        "includes": ["components/bb_manifest/include"],
        "sources":  ["components/bb_manifest/src/bb_manifest_emit.c"],
        "depends":  ["bb_core", "bb_http", "bb_json"],
    },
    "bb_registry": {
        "includes": ["components/bb_registry/include"],
        "sources":  ["platform/host/bb_registry/bb_registry.c"],
        "depends":  ["bb_core"],
    },
    "bb_timer": {
        "includes": ["components/bb_timer/include"],
        "sources":  ["platform/host/bb_timer/bb_timer_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_board": {
        "includes": ["components/bb_board/include"],
        "sources":  ["platform/host/bb_board/bb_board_host.c"],
        "depends":  ["bb_core", "bb_nv"],
    },
    "bb_info": {
        "includes": ["components/bb_info/include"],
        "sources":  ["platform/host/bb_info/bb_info_host.c"],
        "depends":  ["bb_core", "bb_http", "bb_json", "bb_board", "bb_wifi"],
    },
}


cJSON_LIB_DEP = "https://github.com/DaveGamble/cJSON.git#v1.7.18"


def resolve_components(requested):
    """Walk the depends graph from each requested component, returning the
    full transitive closure. Order is stable: dependencies come before
    dependents so include-path / source-list ordering matches REQUIRES
    semantics. Cycles are tolerated via a visited set.
    """
    resolved = []
    seen = set()

    def visit(name):
        if name in seen:
            return
        if name not in COMPONENT_MAP:
            print(
                f"bb_native_scaffold: unknown component '{name}'; "
                f"known: {sorted(COMPONENT_MAP.keys())}"
            )
            env.Exit(1)
            return
        seen.add(name)
        for dep in COMPONENT_MAP[name].get("depends", []):
            visit(dep)
        resolved.append(name)

    for name in requested:
        visit(name)
    return resolved


# Read the custom_bb_components option (space-separated component names)
components_str = env.GetProjectOption("custom_bb_components", "")

if components_str.strip():
    requested = components_str.split()
    component_names = resolve_components(requested)

    # Process each component (already in dependency order)
    for name in component_names:
        entry = COMPONENT_MAP[name]
        includes = entry["includes"]
        sources = entry["sources"]

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
