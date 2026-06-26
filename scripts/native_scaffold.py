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
        "sources":  [
            "platform/host/bb_core/bb_mem.c",
            "platform/host/bb_core/bb_claim.c",
        ],
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
        "includes": ["components/bb_diag/include", "components/bb_diag"],
        "sources": [
            "components/bb_diag/bb_diag_reset_decision.c",
            "components/bb_diag/bb_diag_scrub.c",
            "components/bb_diag/bb_diag_event_common.c",
            "platform/host/bb_diag/bb_diag_panic.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_nv": {
        "includes": ["components/bb_nv/include", "platform/espidf/bb_nv"],
        "sources":  [
            "platform/espidf/bb_nv/bb_nv.c",
            "platform/espidf/bb_nv/bb_nv_routes.c",
            "platform/espidf/bb_nv/bb_nv_delete_routes.c",
            "components/bb_nv/bb_nv_creds_mirror.c",
            "components/bb_nv/bb_nv_wifi_pending.c",
        ],
        "depends":  ["bb_core", "bb_registry", "bb_http", "bb_json"],
    },
    "bb_json": {
        "includes": ["components/bb_json/include", "platform/host/bb_json"],
        "sources":  ["platform/espidf/bb_json/bb_json_cjson.c"],
        "depends":  ["bb_core"],
    },
    "bb_http": {
        "includes": ["components/bb_http/include", "platform/host/bb_http/include"],
        "sources": [
            "components/bb_http/src/http_utils.c",
            "components/bb_http/src/bb_http_status.c",
            "components/bb_http/src/bb_http_query.c",
            "components/bb_http/src/bb_http_body.c",
            "components/bb_http/src/route_registry.c",
            "components/bb_http/src/bb_http_json_obj.c",
            "components/bb_http/src/bb_http_api_dispatch.c",
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
            "platform/host/bb_wifi/bb_wifi_emit.c",
            "components/bb_wifi/wifi_reconn_policy.c",
        ],
        "depends":  ["bb_core", "bb_log", "bb_nv", "bb_json"],
    },
    "bb_ntp": {
        "includes": ["components/bb_ntp/include"],
        "sources":  ["platform/host/bb_ntp/bb_ntp_host.c"],
        "depends":  ["bb_core", "bb_log", "bb_nv"],
    },
    "bb_system": {
        "includes": ["components/bb_system/include", "platform/host/bb_system"],
        "sources":  ["platform/host/bb_system/bb_system_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_wdt": {
        "includes": ["components/bb_wdt/include", "platform/host/bb_wdt"],
        "sources":  [
            "platform/host/bb_wdt/bb_wdt.c",
            "components/bb_wdt/bb_wdt_park_wait.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_tls": {
        "includes": ["components/bb_tls/include"],
        "sources":  ["platform/host/bb_tls/bb_tls.c"],
        "depends":  ["bb_core"],
    },
    "bb_ota_pull": {
        "includes": ["components/bb_ota_pull/include", "platform/host/bb_ota_pull"],
        "sources":  ["platform/espidf/bb_ota_pull/bb_ota_pull.c"],
        "depends":  ["bb_core", "bb_http", "bb_nv", "bb_log", "bb_json", "bb_release_manifest", "bb_http_client", "bb_ota_hooks", "bb_tls"],
    },
    "bb_ota_push": {
        "includes": ["components/bb_ota_push/include"],
        "sources":  ["platform/espidf/bb_ota_push/bb_ota_push.c"],
        "depends":  ["bb_core", "bb_http", "bb_log", "bb_ota_hooks"],
    },
    "bb_ota_boot": {
        "includes": ["components/bb_ota_boot/include"],
        "sources":  ["platform/espidf/bb_ota_boot/bb_ota_boot.c"],
        "depends":  ["bb_core", "bb_log", "bb_nv", "bb_ota_hooks"],
    },
    "bb_ota_hooks": {
        "includes": ["components/bb_ota_hooks/include"],
        "sources":  ["platform/espidf/bb_ota_hooks/bb_ota_hooks.c",
                     "platform/host/bb_ota_hooks/bb_ota_led.c"],
        "depends":  ["bb_core", "bb_log"],
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
            "components/bb_mdns/bb_mdns_util.c",
        ],
        "depends":  ["bb_core", "bb_nv"],
        # components/bb_mdns/ already in includes above, which also resolves bb_mdns_test.h
    },
    "bb_openapi": {
        "includes": ["components/bb_openapi/include"],
        "sources":  [
            "components/bb_openapi/src/bb_openapi_emit.c",
            "components/bb_openapi/src/bb_openapi_validate.c",
        ],
        "depends":  ["bb_core", "bb_http", "bb_json"],
    },
    "bb_manifest": {
        "includes": ["components/bb_manifest/include"],
        "sources":  ["components/bb_manifest/src/bb_manifest_emit.c"],
        "depends":  ["bb_core", "bb_http", "bb_json"],
    },
    "bb_websocket": {
        "includes": ["components/bb_websocket/include",
                     "platform/host/bb_websocket/include"],
        "sources":  ["platform/host/bb_websocket/bb_websocket_host.c"],
        "depends":  ["bb_core", "bb_http", "bb_log"],
    },
    "bb_display": {
        "includes": ["components/bb_display/include"],
        "sources":  [
            "components/bb_display/src/bb_display.c",
            "components/bb_display/src/bb_display_font_5x8.c",
            "components/bb_display/src/bb_display_font_6x12.c",
            "components/bb_display/src/bb_display_font_8x16.c",
        ],
        "depends":  ["bb_core", "bb_log"],
    },
    "bb_display_info": {
        "includes": ["components/bb_display_info/include", "components/bb_display_info"],
        "sources":  [
            "platform/host/bb_display_info/bb_display_info.c",
            "components/bb_display_info/bb_display_info_event_common.c",
        ],
        "depends":  ["bb_display", "bb_info", "bb_nv", "bb_json", "bb_core"],
    },
    "bb_registry": {
        "includes": ["components/bb_registry/include"],
        "sources":  ["platform/host/bb_registry/bb_registry.c"],
        "depends":  ["bb_core"],
    },
    "bb_timer": {
        "includes": ["components/bb_timer/include", "platform/host/bb_timer"],
        "sources":  ["platform/host/bb_timer/bb_timer_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_board": {
        "includes": ["components/bb_board/include"],
        "sources":  ["platform/host/bb_board/bb_board_host.c"],
        "depends":  ["bb_core", "bb_nv"],
    },
    "bb_info": {
        "includes": ["components/bb_info/include", "components/bb_info/src"],
        "sources":  [
            "platform/host/bb_info/bb_info_host.c",
            "components/bb_info/src/bb_info_build.c",
        ],
        "depends":  ["bb_core", "bb_http", "bb_json", "bb_board", "bb_wifi", "bb_section",
                     "bb_system", "bb_cache"],
    },
    "bb_health": {
        "includes": ["components/bb_health/include", "components/bb_health"],
        "sources":  [
            "platform/host/bb_health/bb_health_host.c",
            "platform/host/bb_health/bb_health_stack_host.c",
            "platform/host/bb_health/bb_health_emit.c",
            "components/bb_health/bb_health_stack_common.c",
        ],
        "depends":  ["bb_core", "bb_http", "bb_json", "bb_ota_validator", "bb_wifi", "bb_section"],
    },
    "bb_power": {
        "includes": ["components/bb_power/include"],
        "sources":  ["platform/host/bb_power/bb_power.c"],
        "depends":  ["bb_core", "bb_json"],
    },
    "bb_power_tps546": {
        "includes": ["components/bb_power_tps546/include"],
        "sources":  ["platform/host/bb_power_tps546/bb_power_tps546_program.c"],
        "depends":  ["bb_power", "bb_core"],
    },
    "bb_power_health": {
        "includes": ["components/bb_power_health/include"],
        "sources":  ["components/bb_power_health/src/bb_vcore_wd.c"],
        "depends":  [],
    },
    "bb_power_routes": {
        "includes": ["components/bb_power_routes/include"],
        "sources":  ["platform/host/bb_power_routes/bb_power_routes_host.c"],
        "depends":  ["bb_power", "bb_power_tps546", "bb_core", "bb_http", "bb_json", "bb_info"],
    },
    "bb_fan": {
        "includes": ["components/bb_fan/include", "components/bb_fan/src"],
        "sources":  [
            "platform/host/bb_fan/bb_fan.c",
            "components/bb_fan/src/bb_fan_pid.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_fan_routes": {
        "includes": ["components/bb_fan_routes/include",
                     "components/bb_fan_emc2101/include"],
        "sources":  ["platform/host/bb_fan_routes/bb_fan_routes_host.c"],
        "depends":  ["bb_fan", "bb_core", "bb_http", "bb_json", "bb_info"],
    },
    "bb_thermal": {
        "includes": ["components/bb_thermal/include",
                     "platform/host/bb_temp"],
        "sources":  ["platform/host/bb_thermal/bb_thermal_host.c"],
        "depends":  ["bb_temp", "bb_power", "bb_fan", "bb_info", "bb_core", "bb_http", "bb_json"],
    },
    "bb_led": {
        "includes": ["components/bb_led/include"],
        "sources":  [
            "platform/host/bb_led/bb_led.c",
            "platform/host/bb_led/bb_led_gamma.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_led_info": {
        "includes": ["components/bb_led_info/include"],
        "sources":  ["platform/host/bb_led_info/bb_led_info.c"],
        "depends":  ["bb_led", "bb_info", "bb_json", "bb_core"],
    },
    "bb_ntp_info": {
        "includes": ["components/bb_ntp_info/include"],
        "sources":  ["platform/host/bb_ntp_info/bb_ntp_info.c"],
        "depends":  ["bb_ntp", "bb_info", "bb_json", "bb_core"],
    },
    "bb_temp": {
        "includes": ["components/bb_temp/include", "platform/host/bb_temp"],
        "sources":  ["platform/host/bb_temp/bb_temp.c"],
        "depends":  ["bb_health", "bb_json", "bb_core"],
    },
    "bb_mqtt_info": {
        "includes": ["components/bb_mqtt_info/include"],
        "sources":  ["platform/host/bb_mqtt_info/bb_mqtt_info.c"],
        "depends":  ["bb_mqtt", "bb_health", "bb_json", "bb_core"],
    },
    "bb_net_health": {
        "includes": ["components/bb_net_health/include"],
        "sources":  ["components/bb_net_health/src/bb_net_health.c"],
        "depends":  ["bb_core", "bb_mqtt", "bb_health", "bb_json"],
    },
    "bb_sse_writer": {
        "includes": ["components/bb_sse_writer/include",
                     "components/bb_sse_writer/src"],
        "sources":  ["components/bb_sse_writer/src/bb_sse_idle.c",
                     "platform/host/bb_sse_writer/bb_sse_writer_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_led_gpio": {
        "includes": ["components/bb_led_gpio/include", "platform/host/bb_led_gpio"],
        "sources":  ["platform/host/bb_led_gpio/bb_led_gpio.c"],
        "depends":  ["bb_core", "bb_led"],
    },
    "bb_led_pwm": {
        "includes": ["components/bb_led_pwm/include", "platform/host/bb_led_pwm"],
        "sources":  ["platform/host/bb_led_pwm/bb_led_pwm.c"],
        "depends":  ["bb_core", "bb_led"],
    },
    "bb_led_rgb_pwm": {
        "includes": ["components/bb_led_rgb_pwm/include", "platform/host/bb_led_rgb_pwm"],
        "sources":  ["platform/host/bb_led_rgb_pwm/bb_led_rgb_pwm.c"],
        "depends":  ["bb_core", "bb_led"],
    },
    "bb_partition": {
        "includes": ["components/bb_partition/include", "platform/host/bb_partition"],
        "sources":  ["platform/host/bb_partition/bb_partition.c"],
        "depends":  ["bb_core"],
    },
    "bb_i2c": {
        "includes": ["components/bb_i2c/include"],
        "sources":  ["platform/host/bb_i2c/bb_i2c.c"],
        "depends":  ["bb_core", "bb_log"],
    },
    "bb_heap_arena": {
        "includes": ["components/bb_heap_arena/include"],
        "sources":  ["platform/host/bb_heap_arena/bb_heap_arena.c"],
        "depends":  ["bb_core"],
    },
    "bb_tls_creds": {
        "includes": ["components/bb_tls_creds/include"],
        "sources":  ["platform/host/bb_tls_creds/bb_tls_creds.c"],
        "depends":  ["bb_core", "bb_nv", "bb_log"],
    },
    "bb_tls_info": {
        "includes": ["components/bb_tls_info/include"],
        "sources":  ["platform/host/bb_tls_info/bb_tls_info.c"],
        "depends":  ["bb_info", "bb_core"],
    },
    "bb_mqtt": {
        "includes": ["components/bb_mqtt/include"],
        "sources":  ["platform/host/bb_mqtt/bb_mqtt.c"],
        "depends":  ["bb_core", "bb_nv", "bb_tls_creds"],
    },
    "bb_sink_http": {
        "includes": ["components/bb_sink_http/include"],
        "sources":  ["platform/host/bb_sink_http/bb_sink_http.c"],
        "depends":  ["bb_core", "bb_pub", "bb_http_client", "bb_tls_creds", "bb_nv", "bb_log"],
    },
    "bb_ota_led": {
        "includes": ["components/bb_ota_hooks/include", "platform/host/bb_ota_hooks"],
        "sources":  ["platform/host/bb_ota_hooks/bb_ota_led.c"],
        "depends":  ["bb_core"],
    },
    "bb_led_apa102": {
        "includes": ["components/bb_led_apa102/include", "platform/host/bb_led_apa102"],
        "sources":  ["platform/host/bb_led_apa102/bb_led_apa102.c"],
        "depends":  ["bb_core", "bb_led"],
    },
    "bb_led_anim": {
        "includes": ["components/bb_led_anim/include", "platform/host/bb_led_anim"],
        "sources":  ["platform/host/bb_led_anim/bb_led_anim.c"],
        "depends":  ["bb_core", "bb_led", "bb_log", "bb_timer"],
    },
    "bb_button": {
        "includes": ["components/bb_button/include"],
        "sources":  ["platform/host/bb_button/bb_button.c"],
        "depends":  ["bb_core"],
    },
    "bb_button_gpio": {
        "includes": ["components/bb_button_gpio/include", "platform/host/bb_button_gpio"],
        "sources":  ["platform/host/bb_button_gpio/bb_button_gpio.c"],
        "depends":  ["bb_core", "bb_button"],
    },
    "bb_button_events": {
        "includes": ["components/bb_button_events/include", "platform/host/bb_button_events"],
        "sources":  ["platform/host/bb_button_events/bb_button_events.c"],
        "depends":  ["bb_core", "bb_button", "bb_log", "bb_timer"],
    },
    "bb_event": {
        "includes": ["components/bb_event/include", "components/bb_event"],
        "sources": [
            "components/bb_event/bb_event_common.c",
            "platform/host/bb_event/bb_event_host.c",
        ],
        "depends":  ["bb_core", "bb_log"],
    },
    "bb_ring": {
        "includes": ["components/bb_ring/include"],
        "sources":  ["platform/host/bb_ring/bb_ring.c"],
        "depends":  ["bb_core"],
    },
    "bb_event_ring": {
        "includes": ["components/bb_event_ring/include"],
        "sources":  ["components/bb_event_ring/bb_event_ring.c"],
        "depends":  ["bb_core", "bb_event"],
    },
    "bb_http_client": {
        "includes": ["components/bb_http_client/include",
                     "platform/host/bb_http_client"],
        "sources":  ["platform/host/bb_http_client/bb_http_client_host.c"],
        "depends":  ["bb_core"],
    },
    "bb_event_routes": {
        "includes": ["components/bb_event_routes/include", "components/bb_event_routes/src"],
        "sources":  [
            "components/bb_event_routes/src/bb_event_routes_common.c",
            "platform/host/bb_event_routes/bb_event_routes_host.c",
        ],
        "depends":  ["bb_core", "bb_event", "bb_event_ring", "bb_log"],
    },
    "bb_cache": {
        "includes": ["components/bb_cache/include"],
        "sources":  ["platform/host/bb_cache/bb_cache_host.c"],
        "depends":  ["bb_core", "bb_json", "bb_event", "bb_log"],
    },
    "bb_update_check": {
        "includes": ["components/bb_update_check/include",
                     "components/bb_update_check/src"],
        "sources":  ["components/bb_update_check/src/bb_update_check_common.c"],
        "depends":  ["bb_core", "bb_http", "bb_json", "bb_nv",
                     "bb_release_manifest", "bb_http_client",
                     "bb_event", "bb_log", "bb_mdns", "bb_system"],
    },
    "bb_release_manifest": {
        "includes": ["components/bb_release_manifest/include"],
        "sources":  [
            "components/bb_release_manifest/src/bb_release_manifest_github.c",
            "components/bb_release_manifest/src/bb_release_manifest_github_stream.c",
        ],
        "depends":  ["bb_core"],
    },
    "bb_pub": {
        "includes": ["components/bb_pub/include"],
        "sources":  ["platform/host/bb_pub/bb_pub.c"],
        "depends":  ["bb_core", "bb_json", "bb_nv", "bb_log"],
    },
    "bb_sink_mqtt": {
        "includes": ["components/bb_sink_mqtt/include"],
        "sources":  ["platform/host/bb_sink_mqtt/bb_sink_mqtt.c"],
        "depends":  ["bb_core", "bb_pub", "bb_mqtt"],
    },
    "bb_sink_event": {
        "includes": ["components/bb_sink_event/include"],
        "sources":  ["platform/host/bb_sink_event/bb_sink_event.c"],
        "depends":  ["bb_core", "bb_pub", "bb_event", "bb_event_routes", "bb_json", "bb_log"],
    },
    "bb_pub_fan": {
        "includes": ["components/bb_pub_fan/include"],
        "sources":  ["platform/host/bb_pub_fan/bb_pub_fan.c"],
        "depends":  ["bb_core", "bb_pub", "bb_fan"],
    },
    "bb_pub_power": {
        "includes": ["components/bb_pub_power/include"],
        "sources":  ["platform/host/bb_pub_power/bb_pub_power.c"],
        "depends":  ["bb_core", "bb_pub", "bb_power"],
    },
    "bb_pub_thermal": {
        "includes": ["components/bb_pub_thermal/include"],
        "sources":  ["platform/host/bb_pub_thermal/bb_pub_thermal.c"],
        "depends":  ["bb_core", "bb_pub", "bb_thermal"],
    },
    "bb_pub_info": {
        "includes": ["components/bb_pub_info/include"],
        "sources":  ["platform/host/bb_pub_info/bb_pub_info.c"],
        "depends":  ["bb_core", "bb_pub", "bb_board", "bb_system", "bb_diag"],
    },
    "bb_pub_wifi": {
        "includes": ["components/bb_pub_wifi/include"],
        "sources":  ["platform/host/bb_pub_wifi/bb_pub_wifi.c"],
        "depends":  ["bb_core", "bb_pub", "bb_wifi"],
    },
    "bb_pub_health": {
        "includes": ["components/bb_pub_health/include"],
        "sources":  ["platform/host/bb_pub_health/bb_pub_health.c"],
        "depends":  ["bb_core", "bb_pub", "bb_mqtt", "bb_ota_validator", "bb_wifi"],
    },
    "bb_pub_rtos": {
        "includes": ["components/bb_pub_rtos/include"],
        "sources":  ["platform/host/bb_pub_rtos/bb_pub_rtos.c"],
        "depends":  ["bb_core", "bb_pub"],
    },
    "bb_section": {
        "includes": ["components/bb_section/include"],
        "sources":  ["platform/host/bb_section/bb_section.c"],
        "depends":  ["bb_core", "bb_json", "bb_log"],
    },
    "bb_sensors": {
        "includes": ["components/bb_sensors/include", "components/bb_sensors"],
        "sources":  ["platform/host/bb_sensors/bb_sensors_host.c"],
        "depends":  ["bb_core", "bb_json", "bb_section", "bb_fan_routes", "bb_power_routes", "bb_thermal", "bb_fan"],
    },
    "bb_telemetry": {
        "includes": ["components/bb_telemetry/include"],
        "sources":  [
            "platform/host/bb_telemetry/bb_telemetry.c",
            "platform/espidf/bb_telemetry/bb_telemetry_routes.c",
        ],
        "depends":  ["bb_core", "bb_json", "bb_log", "bb_section",
                     "bb_http", "bb_pub", "bb_nv"],
    },
    "bb_mqtt_telemetry": {
        "includes": ["components/bb_mqtt_telemetry/include"],
        "sources":  ["platform/host/bb_mqtt_telemetry/bb_mqtt_telemetry_host.c"],
        "depends":  ["bb_mqtt", "bb_core", "bb_nv", "bb_json", "bb_telemetry"],
    },
    "bb_sink_http_telemetry": {
        "includes": ["components/bb_sink_http_telemetry/include"],
        "sources":  ["platform/host/bb_sink_http_telemetry/bb_sink_http_telemetry_host.c"],
        "depends":  ["bb_sink_http", "bb_core", "bb_nv", "bb_json", "bb_telemetry"],
    },
    "bb_pub_telemetry": {
        "includes": ["components/bb_pub_telemetry/include"],
        "sources":  ["platform/host/bb_pub_telemetry/bb_pub_telemetry_host.c"],
        "depends":  ["bb_pub", "bb_core", "bb_json", "bb_telemetry"],
    },
    "bb_sink_ws": {
        "includes": ["components/bb_sink_ws/include"],
        "sources":  ["platform/host/bb_sink_ws/bb_sink_ws.c"],
        "depends":  ["bb_core", "bb_pub", "bb_websocket", "bb_log"],
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
