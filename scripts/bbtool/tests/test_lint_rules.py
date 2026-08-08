"""Per-rule fixture tests ported from check_lint_test.sh."""
import argparse
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

# Make bbtool package importable
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from core import Context
from commands.lint import (
    _check_deprecated_http_send,
    _check_public_header_leak,
    _check_state_topic_post,
    _check_raw_allocator,
    _check_raw_esp_timer,
    _check_timer_cb_heavy,
    _check_platform_error_in_public_struct,
    _check_ticket_ref_in_log,
    _check_bb_prefix,
    _check_pragma_once,
    _check_no_arduino_string,
    _check_public_header_inline_platform_call,
    _check_mutating_route_needs_body_schema,
    _check_event_topic_needs_schema,
    _check_kconfig_default_mismatch,
    _check_task_creation_without_registration,
    _check_public_requires_unused,
    _check_kconfig_bridge_shadow,
    _check_raw_timestamp_divide,
    _check_emit_seam_unwired_subscriber,
    _check_prov_default_form_internal_ref,
    _check_init_marker_gated_srcs,
    _check_binds_data_mismatch,
    _check_binds_data_hidden_bind,
    _check_kconfig_inert_symbol,
    _join_preproc_continuations,
    _strip_noise,
    _parse_kconfig_int_defaults,
    run as lint_run,
)


def make_ctx(root: str) -> Context:
    return Context(root=root, config={})


class TestDeprecatedHttpSend(unittest.TestCase):
    def _make_comp(self, tmpdir: str, filename: str, content: str) -> str:
        src = os.path.join(tmpdir, "components", "bb_fake", "src")
        os.makedirs(src, exist_ok=True)
        path = os.path.join(src, filename)
        Path(path).write_text(content)
        return tmpdir

    def test_fires_on_send_json(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_comp(td, "fake.c",
                'bb_err_t foo(bb_http_request_t *r) { return bb_http_resp_send_json(r, doc); }\n')
            violations = _check_deprecated_http_send(make_ctx(td))
            self.assertTrue(violations, "expected violation on bb_http_resp_send_json(")

    def test_fires_on_send_err(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_comp(td, "fake.c",
                'void bar(void) { bb_http_resp_send_err(r, code, msg); }\n')
            violations = _check_deprecated_http_send(make_ctx(td))
            self.assertTrue(violations, "expected violation on bb_http_resp_send_err(")

    def test_no_fire_on_send_chunk(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_comp(td, "fake.c",
                'void baz(void) { bb_http_resp_send_chunk(r, buf, len); }\n')
            violations = _check_deprecated_http_send(make_ctx(td))
            self.assertFalse(violations, "send_chunk must NOT fire")

    def test_no_fire_on_sendstr(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_comp(td, "fake.c",
                'void baz(void) { bb_http_resp_sendstr(r, s); }\n')
            violations = _check_deprecated_http_send(make_ctx(td))
            self.assertFalse(violations, "sendstr must NOT fire")


class TestPublicHeaderLeak(unittest.TestCase):
    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        comp_dir = os.path.join(tmpdir, "components", comp)
        inc = os.path.join(comp_dir, "include")
        os.makedirs(inc, exist_ok=True)
        # A CMakeLists.txt marks this dir as a leaf component under
        # discovery.py's leaf rule (B1-1084 consumer migration) — without
        # it, build_index() finds zero components and the discovery-SSOT
        # rules under test never see this fixture's header at all.
        Path(os.path.join(comp_dir, "CMakeLists.txt")).write_text("")
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def test_fires_on_ungated_esp_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "esp_http_server.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "expected violation on ungated esp_ include")

    def test_no_fire_on_gated_esp_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "esp_http_server.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated include must NOT fire")

    def test_bb_display_ek79007_exempt(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_display_ek79007", "bb_display_ek79007.h",
                '#pragma once\n#include "esp_lcd.h"\n#include "lvgl.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "bb_display_ek79007 must be exempt")

    def test_fires_on_driver_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include <driver/i2c.h>\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated driver/ include must fire")

    def test_fires_on_cjson_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include <cJSON.h>\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated cJSON.h include must fire")

    def test_fires_on_lwip_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "lwip/ip4_addr.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated lwip/ include must fire")

    def test_no_fire_on_gated_lwip_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "lwip/ip4_addr.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated lwip/ include must NOT fire")

    def test_fires_on_mbedtls_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "mbedtls/ssl.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated mbedtls/ include must fire")

    def test_no_fire_on_gated_mbedtls_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "mbedtls/ssl.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated mbedtls/ include must NOT fire")

    def test_fires_on_mdns_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "mdns.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated mdns.h include must fire")

    def test_no_fire_on_gated_mdns_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "mdns.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated mdns.h include must NOT fire")

    def test_fires_on_nvs_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "nvs.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated nvs.h include must fire")

    def test_fires_on_nvs_flash_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "nvs_flash.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated nvs_flash.h include must fire")

    def test_no_fire_on_gated_nvs_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "nvs_flash.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated nvs_flash.h include must NOT fire")

    def test_fires_on_esp_http_server_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "esp_http_server.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated esp_http_server.h include must fire")

    def test_fires_on_httpd_priv_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "httpd_priv.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated httpd_* include must fire")

    def test_no_fire_on_gated_httpd_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "httpd_priv.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated httpd_* include must NOT fire")

    def test_fires_on_sdkconfig_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include "sdkconfig.h"\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated sdkconfig.h include must fire")

    def test_no_fire_on_gated_sdkconfig_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include "sdkconfig.h"\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated sdkconfig.h include must NOT fire")

    def test_fires_on_pthread_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#include <pthread.h>\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertTrue(violations, "ungated pthread.h include must fire")

    def test_no_fire_on_gated_pthread_include(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n#ifdef ESP_PLATFORM\n#include <pthread.h>\n#endif\n')
            violations = _check_public_header_leak(make_ctx(td))
            self.assertFalse(violations, "gated pthread.h include must NOT fire")


class TestStateTopicPost(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_outside_bb_cache(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n')
            violations = _check_state_topic_post(make_ctx(td))
            self.assertTrue(violations, "expected violation outside bb_cache")

    def test_no_fire_inside_espidf_bb_cache(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_cache/bb_cache.c",
                'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n')
            violations = _check_state_topic_post(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside platform/espidf/bb_cache")

    def test_no_fire_inside_host_bb_cache(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/host/bb_cache/bb_cache.c",
                'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n')
            violations = _check_state_topic_post(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside platform/host/bb_cache")

    def test_no_fire_inside_components_bb_cache(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_cache/bb_cache.c",
                'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n')
            violations = _check_state_topic_post(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside components/bb_cache")

    def test_no_fire_inside_test(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "test/test_fake.c",
                'void foo(void) { bb_event_post(ev, "net.health", data, len); }\n')
            violations = _check_state_topic_post(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside test/")

    def test_fires_on_BB_NET_HEALTH_TOPIC_macro(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) { bb_event_post(ev, BB_NET_HEALTH_TOPIC, data, len); }\n')
            violations = _check_state_topic_post(make_ctx(td))
            self.assertTrue(violations, "expected violation on BB_NET_HEALTH_TOPIC macro")


class TestRawEspTimer(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_outside_bb_timer(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_net/bb_net.c",
                'esp_timer_create_args_t args = {0};\n'
                'esp_timer_create(&args, &h);\n')
            violations = _check_raw_esp_timer(make_ctx(td))
            self.assertTrue(violations, "expected violation outside bb_timer/")

    def test_fires_in_components(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'esp_timer_handle_t h;\n'
                'esp_timer_create_args_t args = {0};\n')
            violations = _check_raw_esp_timer(make_ctx(td))
            self.assertTrue(violations, "expected violation in components/")

    def test_no_fire_inside_bb_timer(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_timer/bb_timer.c",
                'esp_timer_create_args_t args = {0};\n'
                'esp_timer_create(&args, &h);\n')
            violations = _check_raw_esp_timer(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside platform/espidf/bb_timer/")

    def test_no_fire_on_esp_timer_get_time(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'uint64_t t = (uint64_t)esp_timer_get_time();\n')
            violations = _check_raw_esp_timer(make_ctx(td))
            self.assertFalse(violations, "esp_timer_get_time must NOT fire")

    def test_no_fire_on_esp_timer_handle_t(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.h",
                'typedef struct { esp_timer_handle_t h; } my_t;\n')
            violations = _check_raw_esp_timer(make_ctx(td))
            self.assertFalse(violations, "esp_timer_handle_t must NOT fire")


class TestTimerCbHeavy(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_malloc_in_cb(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'static void my_cb(void *arg) {\n'
                '    void *p = malloc(64);\n'
                '}\n'
                'void init(void) {\n'
                '    bb_timer_periodic_create(my_cb, NULL, "t", &h);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertTrue(violations, "expected violation for malloc in callback")
            self.assertIn("my_cb", violations[0]["detail"])

    def test_fires_on_xsemaphoretake_portmaxdelay(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'static void my_cb(void *arg) {\n'
                '    xSemaphoreTake(s_mutex, portMAX_DELAY);\n'
                '    do_work();\n'
                '    xSemaphoreGive(s_mutex);\n'
                '}\n'
                'void init(void) {\n'
                '    bb_timer_oneshot_create(my_cb, NULL, "t", &h);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertTrue(violations, "expected violation for xSemaphoreTake(portMAX_DELAY)")

    def test_no_fire_when_heavy_outside_cb(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'static void my_cb(void *arg) {\n'
                '    do_light_work();\n'
                '}\n'
                'void init(void) {\n'
                '    void *p = malloc(64);\n'
                '    bb_timer_periodic_create(my_cb, NULL, "t", &h);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertFalse(violations, "malloc outside callback must NOT fire")

    def test_no_fire_for_deferred_create(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'static void my_work(void *arg) {\n'
                '    void *p = malloc(64);\n'
                '    bb_json_free(p);\n'
                '}\n'
                'void init(void) {\n'
                '    bb_timer_deferred_periodic_create(my_work, NULL, "t", &h);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertFalse(violations, "bb_timer_deferred_* must NOT fire")

    def test_no_fire_for_worker_create(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'static void my_work(void *arg) {\n'
                '    esp_wifi_scan_start(NULL, false);\n'
                '}\n'
                'void init(void) {\n'
                '    bb_timer_worker_periodic_create(my_work, NULL, "t", NULL, &h);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertFalse(violations, "bb_timer_worker_* must NOT fire")

    def test_no_fire_bb_timer_c_self_match(self):
        """bb_timer_periodic_create(void (*cb)...) definition must NOT fire (keyword filter)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_timer/bb_timer.c",
                'static void worker_task_fn(void *arg) {\n'
                '    if (xSemaphoreTake(t->worker_sem, portMAX_DELAY) == pdTRUE) {\n'
                '        t->work_fn(t->arg);\n'
                '    }\n'
                '}\n'
                'bb_err_t bb_timer_periodic_create(void (*cb)(void *arg), void *arg,\n'
                '                                  const char *name, bb_periodic_timer_t *out) {\n'
                '    xTaskCreate(worker_task_fn, "t", 4096, NULL, 5, NULL);\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertFalse(violations, "bb_timer.c self-definition must NOT fire (void keyword filter)")

    def test_cb_defined_above_registration(self):
        """Callback defined before the registration is still detected."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'static void early_cb(void *arg) {\n'
                '    esp_restart();\n'
                '}\n'
                'void init(void) {\n'
                '    bb_timer_periodic_create(early_cb, NULL, "t", &h);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertTrue(violations, "callback defined above registration must be detected")

    def test_cb_defined_below_registration(self):
        """Callback defined after the registration is still detected."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void init(void) {\n'
                '    bb_timer_periodic_create(late_cb, NULL, "t", &h);\n'
                '}\n'
                'static void late_cb(void *arg) {\n'
                '    bb_event_post(ev, "foo", data, len);\n'
                '}\n')
            violations = _check_timer_cb_heavy(make_ctx(td))
            self.assertTrue(violations, "callback defined below registration must be detected")


class TestPlatformErrorInPublicStruct(unittest.TestCase):
    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        comp_dir = os.path.join(tmpdir, "components", comp)
        inc = os.path.join(comp_dir, "include")
        os.makedirs(inc, exist_ok=True)
        # See TestPublicHeaderLeak._make_header — a CMakeLists.txt marks
        # this dir as a leaf component under discovery.py's leaf rule.
        Path(os.path.join(comp_dir, "CMakeLists.txt")).write_text("")
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def _ctx(self, tmpdir: str, config: dict = None) -> Context:
        return Context(root=tmpdir, config=config or {})

    def test_fires_on_int_tls_error_code(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    int tls_error_code; // raw mbedtls code\n'
                '} bb_fake_t;\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertTrue(violations, "tls_error_code integer field must fire")

    def test_fires_on_uint_disc_reason(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    uint8_t disc_reason;\n'
                '} bb_fake_t;\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertTrue(violations, "disc_reason uint8_t field must fire")

    def test_fires_on_comment_match(self):
        """Field name is neutral but trailing comment contains mbedtls."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    int diag_code; // raw mbedtls error\n'
                '} bb_fake_t;\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertTrue(violations, "mbedtls in trailing comment must fire")

    def test_no_fire_on_portable_enum_field(self):
        """A field typed with a portable bb_* typedef must NOT fire (not an int scalar)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    bb_mqtt_client_disc_t disc_reason;\n'
                '    bb_tls_fail_t  tls_fail;\n'
                '} bb_fake_t;\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertFalse(violations, "portable bb_* enum fields must NOT fire")

    def test_no_fire_outside_struct(self):
        """Integer field names that match but are NOT inside a struct body must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'int disc_reason;\n'
                'bb_err_t bb_fake_get(int *disc_reason);\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertFalse(violations, "top-level declarations outside struct must NOT fire")

    def test_no_fire_bb_display_ek79007_exempt(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_display_ek79007", "bb_display_ek79007.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    int tls_error_code;\n'
                '} bb_display_ek79007_t;\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertFalse(violations, "bb_display_ek79007 must be exempt")

    def test_allowlist_field_name(self):
        """A field name in the allowlist must be silenced."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    int tls_error_code; // raw mbedtls code\n'
                '} bb_fake_t;\n')
            config = {"lint": {"rules": {"platform-error-in-public-struct": {
                "allow": ["tls_error_code"]
            }}}}
            violations = _check_platform_error_in_public_struct(self._ctx(td, config))
            self.assertFalse(violations, "allowlisted field name must NOT fire")

    def test_no_fire_non_integer_type(self):
        """bool / float / pointer types must NOT fire even with suspicious names."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'typedef struct {\n'
                '    bool disc_reason;\n'
                '    float err_code;\n'
                '    void *errno_ptr;\n'
                '} bb_fake_t;\n')
            violations = _check_platform_error_in_public_struct(self._ctx(td))
            self.assertFalse(violations, "non-integer types must NOT fire")


class TestTicketRefInLog(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def _ctx(self, tmpdir: str, config: dict = None) -> Context:
        return Context(root=tmpdir, config=config or {})

    def test_fires_on_ticket_in_log_string(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) {\n'
                '    bb_log_e(TAG, "error B1-123 occurred");\n'
                '}\n')
            violations = _check_ticket_ref_in_log(self._ctx(td))
            self.assertTrue(violations, "B1-NNN in log string must fire")

    def test_fires_on_ta_prefix(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) {\n'
                '    bb_log_w(TAG, "see TA-456 for context");\n'
                '}\n')
            violations = _check_ticket_ref_in_log(self._ctx(td))
            self.assertTrue(violations, "TA-NNN in log string must fire")

    def test_no_fire_comment_only(self):
        """Ticket ID in a comment (not in a string literal) must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) {\n'
                '    // B1-123: deferred because of the issue\n'
                '    bb_log_i(TAG, "normal message");\n'
                '}\n')
            violations = _check_ticket_ref_in_log(self._ctx(td))
            self.assertFalse(violations, "comment-only ticket ref must NOT fire")

    def test_no_fire_no_log_call(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) {\n'
                '    const char *msg = "B1-999 see ticket";\n'
                '}\n')
            violations = _check_ticket_ref_in_log(self._ctx(td))
            self.assertFalse(violations, "ticket in non-log string must NOT fire")

    def test_configurable_prefix(self):
        """Custom prefix list: only JIRA- fires; B1-/TA- do not."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void foo(void) {\n'
                '    bb_log_e(TAG, "JIRA-42 broke things");\n'
                '    bb_log_w(TAG, "B1-100 unrelated");\n'
                '}\n')
            config = {"lint": {"rules": {"ticket-ref-in-log": {"prefixes": ["JIRA"]}}}}
            violations = _check_ticket_ref_in_log(self._ctx(td, config))
            self.assertEqual(len(violations), 1, "only JIRA- prefix should fire with custom config")
            self.assertIn("JIRA-42", violations[0]["detail"])

    def test_no_fire_test_directory(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/test/test_fake.c",
                'void test_foo(void) {\n'
                '    bb_log_e(TAG, "B1-123 test case");\n'
                '}\n')
            violations = _check_ticket_ref_in_log(self._ctx(td))
            self.assertFalse(violations, "test/ directory must be excluded")

    def test_fires_on_platform_file(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void foo(void) {\n'
                '    bb_log_d(TAG, "debug B1-777");\n'
                '}\n')
            violations = _check_ticket_ref_in_log(self._ctx(td))
            self.assertTrue(violations, "ticket in platform/ log string must fire")


class TestBbPrefix(unittest.TestCase):
    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        comp_dir = os.path.join(tmpdir, "components", comp)
        inc = os.path.join(comp_dir, "include")
        os.makedirs(inc, exist_ok=True)
        # See TestPublicHeaderLeak._make_header — a CMakeLists.txt marks
        # this dir as a leaf component under discovery.py's leaf rule.
        Path(os.path.join(comp_dir, "CMakeLists.txt")).write_text("")
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def _ctx(self, tmpdir: str, config: dict = None) -> Context:
        return Context(root=tmpdir, config=config or {})

    def test_fires_on_non_bb_function(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'void init_something(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "non-bb_ function declaration must fire")
            self.assertIn("init_something", violations[0]["detail"])

    def test_fires_on_non_BB_macro(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '#define CHIP_MAX_RETRIES 3\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "non-BB_ macro must fire")
            self.assertIn("CHIP_MAX_RETRIES", violations[0]["detail"])

    def test_no_fire_on_bb_function(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'bb_err_t bb_fake_init(void);\n'
                'void bb_fake_deinit(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "bb_-prefixed functions must NOT fire")

    def test_no_fire_on_BB_macro(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '#define BB_FAKE_MAX 10\n'
                '#define BB_FAKE_FLAG (1u << 0)\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "BB_-prefixed macros must NOT fire")

    def test_no_fire_on_header_guard(self):
        """Header guards matching *_H pattern must be skipped."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#ifndef BB_FAKE_H\n'
                '#define BB_FAKE_H\n'
                '#endif\n')
            violations = _check_bb_prefix(self._ctx(td))
            # BB_FAKE_H starts with BB_ so it won't fire; FAKE_H style would too be skipped by guard heuristic
            self.assertFalse(violations, "header guards must NOT fire")

    def test_no_fire_on_static_function(self):
        """static functions are implementation detail — must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'static inline int helper_fn(void) { return 0; }\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "static/inline functions must NOT fire")

    def test_allowlist_respected(self):
        """A symbol name in the allowlist must be silenced."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '#define CHIP_MAX_RETRIES 3\n')
            config = {"lint": {"rules": {"bb-prefix": {"allow": ["CHIP_MAX_RETRIES"]}}}}
            violations = _check_bb_prefix(self._ctx(td, config))
            self.assertFalse(violations, "allowlisted symbol must NOT fire")

    def test_bb_display_ek79007_exempt(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_display_ek79007", "bb_display_ek79007.h",
                '#pragma once\n'
                'void lvgl_init(void);\n'
                '#define LVGL_PANEL_W 1024\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "bb_display_ek79007 must be exempt")


class TestBbPrefixPreviouslyBlindDeclStyles(unittest.TestCase):
    """B1-1470: `_BB_PREFIX_FN_RE` shares the same three blind spots
    `fence/variant_ladder.py`'s `_FN_DECL_RE` went through three rounds of
    fixes for (see `TestPointerReturnLadderFires` there) -- ported the same
    fix here. Each positive case below is a mutation test in spirit: it
    only proves something if it fails against the OLD (pre-fix) regex/skip
    logic; see the scout report for the before/after table."""

    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        comp_dir = os.path.join(tmpdir, "components", comp)
        inc = os.path.join(comp_dir, "include")
        os.makedirs(inc, exist_ok=True)
        Path(os.path.join(comp_dir, "CMakeLists.txt")).write_text("")
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def _ctx(self, tmpdir: str, config: dict = None) -> Context:
        return Context(root=tmpdir, config=config or {})

    # --- positive cases: a non-bb_-prefixed declaration in each
    # previously-blind style must fire ---

    def test_fires_on_pointer_attached_star(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'char *bad_name(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "pointer-attached-star declaration must fire")
            self.assertIn("bad_name", violations[0]["detail"])

    def test_fires_on_const_qualifier(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'const bb_foo_t bad_name(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "const-qualified declaration must fire")
            self.assertIn("bad_name", violations[0]["detail"])

    def test_fires_on_struct_return(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'struct bb_foo bad_name(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "struct-returning declaration must fire")
            self.assertIn("bad_name", violations[0]["detail"])

    def test_fires_on_enum_return(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'enum bb_state bad_name(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "enum-returning declaration must fire")
            self.assertIn("bad_name", violations[0]["detail"])

    def test_fires_on_union_return(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'union bb_value bad_name(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertTrue(violations, "union-returning declaration must fire")
            self.assertIn("bad_name", violations[0]["detail"])

    # --- negative cases: a correctly bb_-prefixed declaration in each of
    # the same styles must NOT fire, and a true type definition/
    # forward-decl (not a function declaration at all) must still be
    # skipped even after widening the const/struct/enum/union handling ---

    def test_no_fire_on_prefixed_pointer_attached_star(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'char *bb_fake_get(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "bb_-prefixed pointer-star declaration must NOT fire")

    def test_no_fire_on_prefixed_const_qualifier(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'const bb_foo_t bb_fake_get(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "bb_-prefixed const-qualified declaration must NOT fire")

    def test_no_fire_on_prefixed_struct_return(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'struct bb_foo bb_fake_get(void);\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "bb_-prefixed struct-returning declaration must NOT fire")

    def test_no_fire_on_struct_type_definition_no_paren(self):
        """A genuine struct type definition has no `(` before the name, so
        `_BB_PREFIX_FN_RE` itself never matches -- it's rejected outright,
        not via `_is_type_definition_line`'s disambiguation (which is
        unreachable, see that function's docstring)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'struct bb_foo {\n'
                '    int x;\n'
                '};\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "a struct type definition must NOT fire")

    def test_no_fire_on_struct_forward_decl_no_paren(self):
        """No `(` before the name -- `_BB_PREFIX_FN_RE` rejects the line
        outright; a forward-declaration is never considered a function."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'struct bb_foo;\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "a struct forward-declaration must NOT fire")

    def test_no_fire_on_enum_type_definition_no_paren(self):
        """No `(` before the name -- `_BB_PREFIX_FN_RE` rejects the line
        outright; an enum type definition is never considered a function."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'enum bb_state { BB_STATE_A, BB_STATE_B };\n')
            violations = _check_bb_prefix(self._ctx(td))
            self.assertFalse(violations, "an enum type definition must NOT fire")


class TestPragmaOnce(unittest.TestCase):
    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        comp_dir = os.path.join(tmpdir, "components", comp)
        inc = os.path.join(comp_dir, "include")
        os.makedirs(inc, exist_ok=True)
        # See TestPublicHeaderLeak._make_header — a CMakeLists.txt marks
        # this dir as a leaf component under discovery.py's leaf rule.
        Path(os.path.join(comp_dir, "CMakeLists.txt")).write_text("")
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def test_fires_on_missing_pragma_once(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#ifndef BB_FAKE_H\n'
                '#define BB_FAKE_H\n'
                'void bb_fake_init(void);\n'
                '#endif\n')
            violations = _check_pragma_once(make_ctx(td))
            self.assertTrue(violations, "header with #ifndef guard but no #pragma once must fire")

    def test_no_fire_on_pragma_once(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                'void bb_fake_init(void);\n')
            violations = _check_pragma_once(make_ctx(td))
            self.assertFalse(violations, "header with #pragma once must NOT fire")

    def test_no_fire_on_feature_gate_ifdef(self):
        """#ifdef ESP_PLATFORM feature gates must NOT be mistaken for include guards."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '#ifdef ESP_PLATFORM\n'
                '#include "esp_err.h"\n'
                '#endif\n'
                'void bb_fake_init(void);\n')
            violations = _check_pragma_once(make_ctx(td))
            self.assertFalse(violations, "feature-gate #ifdef must NOT cause false-positive")

    def test_bb_display_ek79007_exempt(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_display_ek79007", "bb_display_ek79007.h",
                '#ifndef BB_DISPLAY_EK79007_H\n'
                '#define BB_DISPLAY_EK79007_H\n'
                '#endif\n')
            violations = _check_pragma_once(make_ctx(td))
            self.assertFalse(violations, "bb_display_ek79007 must be exempt")


class TestNoArduinoString(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_String_variable(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.cpp",
                '#include <Arduino.h>\n'
                'void foo(void) {\n'
                '    String x = "hello";\n'
                '}\n')
            violations = _check_no_arduino_string(make_ctx(td))
            self.assertTrue(violations, "String variable declaration must fire")

    def test_fires_on_String_ctor(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/arduino/bb_fake/bb_fake.cpp",
                'void bar(String s) {}\n')
            violations = _check_no_arduino_string(make_ctx(td))
            self.assertTrue(violations, "String parameter type must fire")

    def test_no_fire_on_c_code(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void bb_fake_init(void) {\n'
                '    const char *s = "hello";\n'
                '}\n')
            violations = _check_no_arduino_string(make_ctx(td))
            self.assertFalse(violations, "plain C code with no String must NOT fire")

    def test_no_fire_on_string_in_comment(self):
        """'String' in a comment must not fire (stripped by _strip_noise)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void bb_fake_init(void) {\n'
                '    // String copy sub-state (COPY_STRING / SKIP_STRING)\n'
                '    const char *s = "String not used here";\n'
                '}\n')
            violations = _check_no_arduino_string(make_ctx(td))
            self.assertFalse(violations, "String only in comments/strings must NOT fire")

    def test_no_fire_in_test_directory(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/test/test_fake.cpp",
                'String s = "test";\n')
            violations = _check_no_arduino_string(make_ctx(td))
            self.assertFalse(violations, "test/ directory must be excluded")


class TestStripNoise(unittest.TestCase):
    def test_blanks_string_literal(self):
        src = 'char *s = "hello world";\n'
        result = _strip_noise(src)
        self.assertNotIn("hello", result)
        self.assertEqual(result.count('\n'), src.count('\n'))

    def test_preserves_newlines_in_block_comment(self):
        src = '/* line1\nline2 */\n'
        result = _strip_noise(src)
        self.assertEqual(result.count('\n'), src.count('\n'))

    def test_blanks_line_comment(self):
        src = 'int x = 1; // esp_timer_create\nint y = 2;\n'
        result = _strip_noise(src)
        self.assertNotIn("esp_timer_create", result)


class TestPublicHeaderInlinePlatformCall(unittest.TestCase):
    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        comp_dir = os.path.join(tmpdir, "components", comp)
        inc = os.path.join(comp_dir, "include")
        os.makedirs(inc, exist_ok=True)
        # See TestPublicHeaderLeak._make_header — a CMakeLists.txt marks
        # this dir as a leaf component under discovery.py's leaf rule.
        Path(os.path.join(comp_dir, "CMakeLists.txt")).write_text("")
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def test_fires_on_inline_with_esp_call(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '#include <stdint.h>\n'
                'static inline uint32_t bb_fake_now(void) {\n'
                '    return (uint32_t)(esp_timer_get_time() / 1000u);\n'
                '}\n')
            violations = _check_public_header_inline_platform_call(make_ctx(td))
            self.assertTrue(violations,
                "expected violation: inline body calls esp_timer_get_time()")

    def test_no_fire_on_declaration_with_esp_mention(self):
        """A plain declaration (no body) must not fire even if docs mention esp_*."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '// Implemented via esp_timer_get_time() on ESP-IDF.\n'
                'uint32_t bb_fake_now(void);\n')
            violations = _check_public_header_inline_platform_call(make_ctx(td))
            self.assertFalse(violations,
                "plain declaration with esp_ in comment must NOT fire")

    def test_no_fire_on_inline_without_platform_call(self):
        """An inline function with no platform API call must not fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_header(td, "bb_fake", "bb_fake.h",
                '#pragma once\n'
                '#include <stdint.h>\n'
                'static inline uint32_t bb_fake_double(uint32_t x) {\n'
                '    return x * 2u;\n'
                '}\n')
            violations = _check_public_header_inline_platform_call(make_ctx(td))
            self.assertFalse(violations,
                "inline with no platform call must NOT fire")


class TestMutatingRouteNeedsBodySchema(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_patch_with_json_body_and_null_schema(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = NULL,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(violations, "PATCH with JSON body and NULL schema must fire")

    def test_no_fire_on_null_schema_with_patched_at_init_comment(self):
        """NULL-then-runtime-patched (CONFIG_BB_OPENAPI_RUNTIME_META convention,
        same shape as this file's response-schema fields) is trusted, same as
        a variable reference — the runtime patch can't be inspected statically."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/bb_fake.c",
                'static bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_POST,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = NULL /* patched at init */,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations,
                              "NULL schema with a 'patched' comment must NOT fire")

    def test_fires_on_null_schema_with_unrelated_comment(self):
        """A comment that doesn't mention "patched" must NOT suppress the rule --
        only the documented NULL-then-runtime-patched convention is trusted."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_POST,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = NULL /* not yet wired */,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(violations,
                             "NULL schema with an unrelated comment must still fire")

    def test_fires_on_post_with_bare_object_schema(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_POST,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = "{\\"type\\":\\"object\\"}",\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(violations, "POST with bare-object schema must fire")

    def test_fires_on_patch_missing_schema_field(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(violations, "PATCH with JSON body and no schema field must fire")

    def test_no_fire_on_patch_with_properties_schema(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = "{\\"type\\":\\"object\\",\\"properties\\":{\\"x\\":{\\"type\\":\\"string\\"}}}",\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "schema with properties must NOT fire")

    def test_no_fire_on_post_bodyless_action(self):
        """POST with no content_type and no schema = intentional bodyless action."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method    = BB_HTTP_POST,\n'
                '    .path      = "/api/reboot",\n'
                '    .responses = s_responses,\n'
                '    .handler   = reboot_handler,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "bodyless POST action must NOT fire")

    def test_no_fire_on_post_octet_stream(self):
        """POST with octet-stream body (binary upload) must not fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_POST,\n'
                '    .path                 = "/api/update/push",\n'
                '    .request_content_type = "application/octet-stream",\n'
                '    .request_schema       = NULL,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "binary upload with NULL schema must NOT fire")

    def test_no_fire_on_get_route(self):
        """GET routes are never checked (not mutating)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method    = BB_HTTP_GET,\n'
                '    .path      = "/api/fake",\n'
                '    .responses = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "GET route must NOT fire")

    def test_no_fire_on_variable_schema_reference(self):
        """A schema variable reference (not NULL) is trusted."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = k_fake_patch_schema,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "variable schema reference must NOT fire")

    def test_fires_on_variable_schema_resolving_to_mutable_char_buffer(self):
        """B1-1244: a .request_schema variable reference resolving in-file to a
        mutable `char foo[N]` buffer (always non-NULL) must fire — this is the
        exact defect shape fixed in PR #1103 via NULL-then-patch."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static char s_fake_schema_buf[256];\n'
                '\n'
                'static bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = s_fake_schema_buf,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(
                violations,
                "variable reference resolving to a mutable char buffer must fire")

    def test_no_fire_on_variable_schema_resolving_to_const_char_buffer(self):
        """A `const char foo[N]` (or `static const char foo[N]`) is not the
        B1-1244 shape — it can never silently change after init — so it stays
        trusted."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const char s_fake_schema_buf[] = "{\\"type\\":\\"object\\"}";\n'
                '\n'
                'static bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = s_fake_schema_buf,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(
                violations,
                "variable reference resolving to a const char buffer must NOT fire")

    def test_no_fire_on_variable_schema_with_comment_embedded_decl(self):
        """A mutable-buffer declaration mentioned only inside a comment must
        not shadow the real const declaration — resolution must run against
        the comment-blanked text, not raw source."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '/* historical note: this used to be\n'
                ' * static char s_fake_schema_buf[64];\n'
                ' */\n'
                'static const char s_fake_schema_buf[] = "{\\"type\\":\\"object\\"}";\n'
                '\n'
                'static bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = s_fake_schema_buf,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(
                violations,
                "comment-embedded mutable declaration must NOT shadow the"
                " real const declaration")

    def test_no_fire_on_variable_schema_shadowed_by_unrelated_local(self):
        """A same-named mutable local in an unrelated function must not
        shadow the real file-scope const declaration — every matching
        declaration must be considered, not just the first textual hit."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static void unrelated_fn(void) {\n'
                '    char s_fake_schema_buf[16];\n'
                '    (void)s_fake_schema_buf;\n'
                '}\n'
                '\n'
                'static const char s_fake_schema_buf[] = "{\\"type\\":\\"object\\"}";\n'
                '\n'
                'static bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = s_fake_schema_buf,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(
                violations,
                "unrelated same-named mutable local must NOT shadow the"
                " real const declaration")

    def test_no_fire_on_variable_schema_unresolvable_in_file(self):
        """A variable reference this rule cannot resolve in-file (declared
        elsewhere, e.g. via a shared header) must stay trusted — never flag
        what can't be proven mutable."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PATCH,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = k_fake_patch_schema,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(
                violations,
                "unresolvable variable reference must NOT fire")

    def test_fires_on_put_with_bare_object_schema(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_PUT,\n'
                '    .path                 = "/api/fake",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = "{\\"type\\":\\"object\\",\\"description\\":\\"body\\"}",\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(violations, "PUT with bare object schema must fire")

    def test_fires_on_delete_with_json_body_and_null_schema(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_DELETE,\n'
                '    .path                 = "/api/nvs",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = NULL,\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertTrue(violations, "DELETE with JSON body and NULL schema must fire")

    def test_no_fire_on_delete_with_properties_schema(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method               = BB_HTTP_DELETE,\n'
                '    .path                 = "/api/nvs",\n'
                '    .request_content_type = "application/json",\n'
                '    .request_schema       = "{\\"type\\":\\"object\\",\\"properties\\":{\\"key\\":{\\"type\\":\\"string\\"}}}",\n'
                '    .responses            = s_responses,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "DELETE with schema having properties must NOT fire")

    def test_no_fire_on_delete_bodyless_action(self):
        """DELETE with no content_type and no schema = bodyless action (e.g., delete by path)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'static const bb_route_t k_route = {\n'
                '    .method    = BB_HTTP_DELETE,\n'
                '    .path      = "/api/cache/*",\n'
                '    .responses = s_responses,\n'
                '    .handler   = delete_handler,\n'
                '};\n')
            violations = _check_mutating_route_needs_body_schema(make_ctx(td))
            self.assertFalse(violations, "bodyless DELETE action must NOT fire")


class TestEventTopicNeedsSchema(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_when_topic_attached_but_no_schema(self):
        """Attaching a topic with no schema registration must fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'bb_err_t bb_fake_init(void) {\n'
                '    bb_event_routes_attach_ex("my.topic", false);\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertTrue(violations, "attached topic with no schema must fire")
            self.assertIn('"my.topic"', violations[0]["detail"])

    def test_fires_when_macro_topic_attached_but_no_schema(self):
        """Macro-named topic with no matching schema must fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'bb_err_t bb_fake_init(void) {\n'
                '    bb_event_routes_attach_ex(MY_FAKE_TOPIC, false);\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertTrue(violations, "macro topic with no schema must fire")
            self.assertIn("MY_FAKE_TOPIC", violations[0]["detail"])

    def test_no_fire_when_topic_has_register_topic_schema(self):
        """Topic with bb_openapi_register_topic_schema must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'bb_err_t bb_fake_init(void) {\n'
                '    bb_openapi_register_topic_schema("my.topic", k_schema, "MyTopic");\n'
                '    bb_event_routes_attach_ex("my.topic", false);\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertFalse(violations, "topic with schema must NOT fire")

    def test_no_fire_cross_file_schema_in_other_file(self):
        """Schema in file2 satisfies the attach in file1 (cross-file)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_attacher/bb_attacher.c",
                'bb_err_t init(void) {\n'
                '    bb_event_routes_attach_ex("cross.topic", false);\n'
                '    return BB_OK;\n'
                '}\n')
            self._make_file(td, "platform/host/bb_schema/bb_schema.c",
                'void register(void) {\n'
                '    bb_openapi_register_topic_schema("cross.topic", k_s, "CrossTopic");\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertFalse(violations, "cross-file schema coverage must NOT fire")

    def test_no_fire_when_schema_via_register_schema_with_sse_topic(self):
        """bb_openapi_register_schema with non-NULL sse_topic must satisfy the rule."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'bb_err_t bb_fake_init(void) {\n'
                '    bb_openapi_register_schema("FakeTopic", k_schema, "my.topic2");\n'
                '    bb_event_routes_attach_ex("my.topic2", false);\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertFalse(violations, "bb_openapi_register_schema with sse_topic must satisfy the rule")

    def test_no_fire_when_register_schema_sse_topic_null(self):
        """bb_openapi_register_schema with NULL sse_topic does NOT satisfy a different topic's attach."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'bb_err_t bb_fake_init(void) {\n'
                '    bb_openapi_register_schema("WifiInfo", k_schema, NULL);\n'
                '    bb_event_routes_attach_ex("wifi.info", false);\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertTrue(violations, "REST-only schema (NULL sse_topic) must not satisfy SSE attach")

    def test_no_fire_variable_arg_in_attach_not_checked(self):
        """A variable (lowercase) arg to attach is not checked by the rule."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/host/bb_sink_event/bb_sink_event.c",
                'bb_err_t register_topic(const char *subtopic) {\n'
                '    return bb_event_routes_attach_ex(subtopic, false);\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertFalse(violations, "variable arg to attach must NOT fire (can't statically resolve)")

    def test_fires_with_attach_variant(self):
        """Plain bb_event_routes_attach (no _ex) must also be checked."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'bb_err_t bb_fake_init(void) {\n'
                '    bb_event_routes_attach("orphan.topic");\n'
                '    return BB_OK;\n'
                '}\n')
            violations = _check_event_topic_needs_schema(make_ctx(td))
            self.assertTrue(violations, "plain bb_event_routes_attach with no schema must fire")


class TestRawAllocator(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_malloc(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'void *p = malloc(64);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation for raw malloc(")

    def test_fires_on_calloc(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'void *p = calloc(1, sizeof(*p));\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation for raw calloc(")

    def test_fires_on_free(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'free(p);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation for raw free(")

    def test_fires_in_components(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'void *buf = malloc(256);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation in components/")

    def test_no_fire_in_bb_mem_c(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_core/bb_mem.c",
                'void *p = malloc(size);\n'
                'free(p);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertFalse(violations, "bb_mem.c must be exempt (facade impl)")

    def test_no_fire_in_test_dir(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/test/test_fake.c",
                'void *p = malloc(64);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertFalse(violations, "test/ directories must be exempt")

    def test_respects_allowlist(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_json/bb_json_cjson.c",
                'buf = (char *)malloc(len + 1);\n'
                'if (heap) free(buf);\n')
            config = {"lint": {"rules": {"raw-allocator": {
                "allow": ["platform/espidf/bb_json/bb_json_cjson.c"]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_raw_allocator(ctx)
            self.assertFalse(violations, "allowlisted path must not fire")

    def test_ignores_comments_and_strings(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                '// malloc(64) — old pattern, now use bb_malloc_prefer_spiram\n'
                'const char *msg = "call free(ptr) to release";\n'
                'void *p = bb_malloc_prefer_spiram(64);\n'
                'bb_mem_free(p);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertFalse(violations, "comments and string literals must not fire")

    def test_no_fire_on_bb_mem_free(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'bb_mem_free(p);\n'
                'bb_malloc_prefer_spiram(64);\n'
                'bb_calloc_prefer_spiram(1, 64);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertFalse(violations, "bb_mem_* calls must not fire (no bare word boundary)")

    def test_fires_on_realloc(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'p = realloc(p, new_size);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation for raw realloc(")

    def test_fires_on_heap_caps_malloc(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'void *p = heap_caps_malloc(64, MALLOC_CAP_DEFAULT);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation for raw heap_caps_malloc(")

    def test_fires_on_heap_caps_free(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'heap_caps_free(p);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertTrue(violations, "expected violation for raw heap_caps_free(")

    def test_no_fire_on_heap_caps_introspection(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'size_t f = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);\n'
                'size_t b = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);\n'
                'heap_caps_get_info(&info, MALLOC_CAP_8BIT);\n')
            violations = _check_raw_allocator(make_ctx(td))
            self.assertFalse(violations, "heap_caps introspection calls must NOT fire")

    def test_path_line_allowlist(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_foo/bb_foo.c",
                'void *a = malloc(8);\n'   # line 1 — allowlisted
                'void *b = malloc(16);\n'  # line 2 — not allowlisted → violation
            )
            config = {"lint": {"rules": {"raw-allocator": {
                "allow": ["platform/espidf/bb_foo/bb_foo.c:1"]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_raw_allocator(ctx)
            self.assertEqual(len(violations), 1,
                             "only line 2 must fire; line 1 is path:line allowlisted")


class TestKconfigDefaultMismatch(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_mismatched_default(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n'
                '    range 8 256\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifdef ESP_PLATFORM\n'
                '#include "sdkconfig.h"\n'
                '#ifdef CONFIG_BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN CONFIG_BB_FAKE_LEN\n'
                '#endif\n'
                '#endif\n'
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 48\n'
                '#endif\n')
            violations = _check_kconfig_default_mismatch(make_ctx(td))
            self.assertTrue(violations, "mismatched C fallback vs Kconfig base default must fire")
            self.assertIn("BB_FAKE_LEN", violations[0]["detail"])

    def test_no_fire_on_matching_default(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n'
                '    range 8 256\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 24\n'
                '#endif\n')
            violations = _check_kconfig_default_mismatch(make_ctx(td))
            self.assertFalse(violations, "matching C fallback default must NOT fire")

    def test_no_false_positive_on_gate_keyed_default(self):
        """Base (non-gated) default must be used — the SPIRAM-gated default is ignored."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 48 if SPIRAM\n'
                '    default 24\n'
                '    range 8 256\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 24\n'
                '#endif\n')
            violations = _check_kconfig_default_mismatch(make_ctx(td))
            self.assertFalse(violations, "must match against base default (24), not gated default (48)")

    def test_no_fire_on_non_int_kconfig(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_ENABLE\n'
                '    bool "Enable fake"\n'
                '    default n\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_FAKE_ENABLE\n'
                '#define BB_FAKE_ENABLE 1\n'
                '#endif\n')
            violations = _check_kconfig_default_mismatch(make_ctx(td))
            self.assertFalse(violations, "bool-typed Kconfig entries must NOT be compared")

    def test_no_fire_on_unrelated_c_default(self):
        """A #ifndef BB_X with no matching Kconfig config must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_OTHER\n'
                '    int "Other"\n'
                '    default 1\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_UNRELATED\n'
                '#define BB_UNRELATED 999\n'
                '#endif\n')
            violations = _check_kconfig_default_mismatch(make_ctx(td))
            self.assertFalse(violations, "C default with no matching Kconfig entry must NOT fire")

    def test_allowlist_by_name(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 48\n'
                '#endif\n')
            config = {"lint": {"rules": {"kconfig-default-mismatch": {
                "allow": ["BB_FAKE_LEN"]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_kconfig_default_mismatch(ctx)
            self.assertFalse(violations, "allowlisted symbol name must NOT fire")

    def test_allowlist_by_path_line(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 48\n'
                '#endif\n')
            key = "platform/espidf/bb_fake/bb_fake.c:2"
            config = {"lint": {"rules": {"kconfig-default-mismatch": {
                "allow": [key]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_kconfig_default_mismatch(ctx)
            self.assertFalse(violations, "allowlisted path:line key must NOT fire")

    def test_first_ungated_default_wins(self):
        text = (
            'config BB_X\n'
            '    int "Fake"\n'
            '    default 24\n'
            '    default 99\n'
        )
        result = _parse_kconfig_int_defaults(text)
        self.assertEqual(result["BB_X"], 24, "first ungated default must win")


class TestTaskCreationWithoutRegistration(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_unregistered_xtaskcreate(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    xTaskCreate(worker, "fake", 2048, NULL, 5, &h);\n'
                '}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            self.assertTrue(violations, "xTaskCreate with no registration in file must fire")

    def test_no_fire_when_registered_same_file(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    xTaskCreate(worker, "fake", 2048, NULL, 5, &h);\n'
                '    bb_task_registry_register("fake", 2048, h, NULL, NULL);\n'
                '}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            self.assertFalse(violations, "xTaskCreate paired with registration must NOT fire")

    def test_fires_on_pinned_to_core_variant(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    xTaskCreatePinnedToCore(worker, "fake", 2048, NULL, 5, &h, 0);\n'
                '}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            self.assertTrue(violations, "xTaskCreatePinnedToCore with no registration must fire")

    def test_no_fire_on_static_variant_with_registration(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    TaskHandle_t h = xTaskCreateStatic(worker, "fake", 512, NULL, 5,\n'
                '                                        stack, &tcb);\n'
                '    bb_task_registry_register("fake", 512, h, NULL, NULL);\n'
                '}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            self.assertFalse(violations, "xTaskCreateStatic paired with registration must NOT fire")

    def test_no_fire_on_comment_mention(self):
        """A bare mention in a comment (no real call) must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '// xTaskCreatePinnedToCore asserts on unicore targets.\n'
                'void start(void) {}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            self.assertFalse(violations, "comment-only mention must NOT fire")

    def test_no_fire_outside_scanned_dirs(self):
        """xTaskCreate outside components/ and platform/espidf/ (e.g. platform/host) is out of scope."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/host/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    xTaskCreate(worker, "fake", 2048, NULL, 5, &h);\n'
                '}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            self.assertFalse(violations, "platform/host is out of scope for this rule")

    def test_allowlist_by_path(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    xTaskCreate(worker, "fake", 2048, NULL, 5, &h);\n'
                '}\n')
            config = {"lint": {"rules": {"task-creation-without-registration": {
                "allow": ["platform/espidf/bb_fake/bb_fake.c"]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_task_creation_without_registration(ctx)
            self.assertFalse(violations, "allowlisted file path must NOT fire")

    def test_two_creates_one_register_file_scope_no_violation(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                'void start(void) {\n'
                '    xTaskCreate(worker, "fake1", 2048, NULL, 5, &h1);\n'
                '    xTaskCreate(worker2, "fake2", 2048, NULL, 5, &h2);\n'
                '    bb_task_registry_register("fake1", 2048, h1, NULL, NULL);\n'
                '}\n')
            violations = _check_task_creation_without_registration(make_ctx(td))
            # Pins the documented file-scope heuristic: 1 register satisfies N
            # creates in the same file. A future call-site-precise rule would
            # flip this to a violation — see B1-466 follow-up.
            self.assertFalse(violations, "1 register satisfies N creates in the same file")


class TestPublicRequiresUnused(unittest.TestCase):
    def _make_cmake(self, tmpdir: str, comp: str, content: str) -> str:
        d = os.path.join(tmpdir, "components", comp)
        os.makedirs(d, exist_ok=True)
        Path(os.path.join(d, "CMakeLists.txt")).write_text(content)
        return tmpdir

    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        inc = os.path.join(tmpdir, "components", comp, "include")
        os.makedirs(inc, exist_ok=True)
        Path(os.path.join(inc, filename)).write_text(content)
        return tmpdir

    def test_no_fire_when_public_header_references_dep(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_netstuff",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_netif\n)\n')
            self._make_header(td, "bb_netstuff", "bb_netstuff.h",
                '#pragma once\n#include "bb_core.h"\n#include "esp_netif.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "esp_netif referenced by a public header must NOT fire")

    def test_fires_when_dep_not_referenced(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_netstuff",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_netif\n)\n')
            self._make_header(td, "bb_netstuff", "bb_netstuff.h",
                '#pragma once\n#include "bb_core.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertTrue(violations,
                "esp_netif in REQUIRES with no public-header reference must fire")
            self.assertIn("esp_netif", violations[0]["detail"])

    def test_no_fire_on_priv_requires(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_netstuff",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core\n    PRIV_REQUIRES esp_netif\n)\n')
            self._make_header(td, "bb_netstuff", "bb_netstuff.h",
                '#pragma once\n#include "bb_core.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations, "PRIV_REQUIRES deps must never be checked")

    def test_default_allowlist_bb_display_ek79007(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_display_ek79007",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core bb_display esp_lvgl_port\n)\n')
            self._make_header(td, "bb_display_ek79007", "bb_display_ek79007.h",
                '#pragma once\n#include "bb_core.h"\n#include "bb_display.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "bb_display_ek79007 / esp_lvgl_port must be allowlisted by default")

    def test_config_allowlist_pair(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_custom",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_weird\n)\n')
            self._make_header(td, "bb_custom", "bb_custom.h",
                '#pragma once\n#include "bb_core.h"\n')
            config = {"lint": {"rules": {"public-requires-unused": {
                "allow": [["bb_custom", "esp_weird"]]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_public_requires_unused(ctx)
            self.assertFalse(violations, "config-allowlisted (component, dep) pair must NOT fire")

    def test_no_fire_on_bb_prefixed_dep_even_when_unreferenced(self):
        """bb_* tokens are out of scope entirely — never evaluated, regardless
        of whether a public header references them."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_consumer",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core bb_helper_unrelated\n)\n')
            self._make_header(td, "bb_consumer", "bb_consumer.h",
                '#pragma once\n#include "bb_core.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "bb_* REQUIRES tokens must never be flagged (internal coupling is out of scope)")

    def test_fires_on_platform_dep_not_referenced_alongside_bb_deps(self):
        """A genuine platform-dep leak still fires even when bb_* deps are
        also present in REQUIRES (and correctly ignored)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_wifi",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core bb_event esp_netif\n)\n')
            self._make_header(td, "bb_wifi", "bb_wifi.h",
                '#pragma once\n#include "bb_core.h"\n#include "bb_event.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertEqual(len(violations), 1,
                "esp_netif must fire even with bb_* deps present (which must be ignored)")
            self.assertIn("esp_netif", violations[0]["detail"])

    def test_default_allowlist_bb_fan_emc2101_esp_driver_i2c(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_fan_emc2101",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core bb_fan esp_driver_i2c\n)\n')
            self._make_header(td, "bb_fan_emc2101", "bb_fan_emc2101.h",
                '#pragma once\n#include "bb_core.h"\n#include "bb_fan.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "bb_fan_emc2101 / esp_driver_i2c must be allowlisted by default")

    def test_default_allowlist_bb_power_tps546_esp_driver_i2c(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_power_tps546",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core bb_power esp_driver_i2c\n)\n')
            self._make_header(td, "bb_power_tps546", "bb_power_tps546.h",
                '#pragma once\n#include "bb_core.h"\n#include "bb_power.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "bb_power_tps546 / esp_driver_i2c must be allowlisted by default")

    def test_non_allowlisted_component_esp_driver_i2c_still_fires(self):
        """A component NOT on the allowlist with an unreferenced esp_driver_i2c
        dep must still fire — the allowlist is component+dep scoped, not global."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_fake_i2c",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_driver_i2c\n)\n')
            self._make_header(td, "bb_fake_i2c", "bb_fake_i2c.h",
                '#pragma once\n#include "bb_core.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertTrue(violations,
                "non-allowlisted component with unreferenced esp_driver_i2c must fire")

    def test_no_fire_json_dep_referencing_cjson_header(self):
        """REQUIRES json provides cJSON.h — the component-name/header-stem
        alias map must not false-flag this as unused."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_jsonstuff",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core json\n)\n')
            self._make_header(td, "bb_jsonstuff", "bb_jsonstuff.h",
                '#pragma once\n#include "bb_core.h"\n#include "cJSON.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "json / cJSON.h must NOT be flagged via the alias map")

    def test_no_fire_espressif_mdns_dep_referencing_mdns_header(self):
        """REQUIRES espressif__mdns provides mdns.h — the leading
        espressif__ namespace must be stripped before matching."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_mdnsstuff",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core espressif__mdns\n)\n')
            self._make_header(td, "bb_mdnsstuff", "bb_mdnsstuff.h",
                '#pragma once\n#include "bb_core.h"\n#include "mdns.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "espressif__mdns / mdns.h must NOT be flagged via the alias map")

    def test_genuinely_unused_platform_dep_still_fires_alongside_aliases(self):
        """A real, unreferenced platform dep must still fire even when an
        aliased dep is present and correctly suppressed."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_mixed",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core json esp_netif\n)\n')
            self._make_header(td, "bb_mixed", "bb_mixed.h",
                '#pragma once\n#include "bb_core.h"\n#include "cJSON.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertEqual(len(violations), 1,
                "esp_netif must still fire while json/cJSON.h is suppressed")
            self.assertIn("esp_netif", violations[0]["detail"])

    def test_line_attribution_for_multiline_requires(self):
        """The violation's line number must point at the actual line inside
        the idf_component_register(...) block where the dep token appears,
        not an unconditional fallback to line 1."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_multiline",
                'idf_component_register(\n'
                '    SRCS "fake.c"\n'
                '    REQUIRES\n'
                '        bb_core\n'
                '        esp_netif\n'
                ')\n')
            self._make_header(td, "bb_multiline", "bb_multiline.h",
                '#pragma once\n#include "bb_core.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertEqual(len(violations), 1)
            # esp_netif is on line 5 (1-indexed) of the CMakeLists.txt fixture.
            self.assertEqual(violations[0]["line"], 5,
                "line attribution must point at the esp_netif token's own line,"
                " not fall back to line 1")

    def test_conditional_set_requires_is_skipped_not_crashed(self):
        """A component whose REQUIRES comes from a conditional set() raises
        ConditionalSetError inside parse_requires; the rule must catch it and
        continue (out of scope), not propagate or crash the whole lint run."""
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_conditional",
                'if(CONFIG_BB_CONDITIONAL_VARIANT)\n'
                '    set(_reqs bb_core esp_netif)\n'
                'else()\n'
                '    set(_reqs bb_core)\n'
                'endif()\n'
                'idf_component_register(\n'
                '    SRCS "fake.c"\n'
                '    REQUIRES ${_reqs}\n'
                ')\n')
            self._make_header(td, "bb_conditional", "bb_conditional.h",
                '#pragma once\n#include "bb_core.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertEqual(violations, [],
                "a conditional-set REQUIRES component must be skipped, not raise")


class TestKconfigBridgeShadow(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_missing_bridge(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 24\n'
                '#endif\n')
            violations = _check_kconfig_bridge_shadow(make_ctx(td))
            self.assertTrue(violations,
                "bare #ifndef/#define with a matching Kconfig int and no CONFIG_ bridge must fire")

    def test_no_fire_with_bridge_present(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifdef ESP_PLATFORM\n'
                '#include "sdkconfig.h"\n'
                '#ifdef CONFIG_BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN CONFIG_BB_FAKE_LEN\n'
                '#endif\n'
                '#endif\n'
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 24\n'
                '#endif\n')
            violations = _check_kconfig_bridge_shadow(make_ctx(td))
            self.assertFalse(violations, "bridge present in the same file must NOT fire")

    def test_no_fire_on_unrelated_c_default(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_OTHER\n'
                '    int "Other"\n'
                '    default 1\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifndef BB_UNRELATED\n'
                '#define BB_UNRELATED 999\n'
                '#endif\n')
            violations = _check_kconfig_bridge_shadow(make_ctx(td))
            self.assertFalse(violations,
                "C default with no matching Kconfig int entry must NOT fire")

    def test_fires_despite_unrelated_longer_config_symbol_in_file(self):
        """A raw substring test for f"CONFIG_{name}" in content is defeated
        by CONFIG_BB_FAKE_LEN matching inside CONFIG_BB_FAKE_LEN_EXTRA — the
        match must be word-bounded so a genuinely-unbridged BB_FAKE_LEN still
        fires even when an unrelated CONFIG_BB_FAKE_LEN_EXTRA symbol is
        present in the same file."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifdef CONFIG_BB_FAKE_LEN_EXTRA\n'
                '#define BB_FAKE_LEN_EXTRA 1\n'
                '#endif\n'
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 24\n'
                '#endif\n')
            violations = _check_kconfig_bridge_shadow(make_ctx(td))
            self.assertTrue(violations,
                "a genuinely-unbridged BB_FAKE_LEN must still fire despite an"
                " unrelated CONFIG_BB_FAKE_LEN_EXTRA symbol in the same file")

    def test_no_fire_when_properly_bridged_alongside_longer_symbol(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_LEN\n'
                '    int "Fake length"\n'
                '    default 24\n')
            self._make_file(td, "platform/espidf/bb_fake/bb_fake.c",
                '#ifdef CONFIG_BB_FAKE_LEN_EXTRA\n'
                '#define BB_FAKE_LEN_EXTRA 1\n'
                '#endif\n'
                '#ifdef ESP_PLATFORM\n'
                '#include "sdkconfig.h"\n'
                '#ifdef CONFIG_BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN CONFIG_BB_FAKE_LEN\n'
                '#endif\n'
                '#endif\n'
                '#ifndef BB_FAKE_LEN\n'
                '#define BB_FAKE_LEN 24\n'
                '#endif\n')
            violations = _check_kconfig_bridge_shadow(make_ctx(td))
            self.assertFalse(violations,
                "a properly-bridged symbol must NOT fire")


class TestRawTimestampDivide(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def test_fires_on_esp_timer_get_time_divide_outside_bb_clock(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'uint64_t now_ms = esp_timer_get_time() / 1000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertTrue(violations,
                "raw esp_timer_get_time()/1000 outside bb_clock/bb_timer must fire")

    def test_fires_on_bb_timer_now_us_divide_outside_bb_timer(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'uint64_t now_ms = bb_timer_now_us() / 1000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertTrue(violations,
                "raw bb_timer_now_us()/1000 outside bb_clock/bb_timer must fire")

    def test_no_fire_inside_bb_clock(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_clock/bb_clock.c",
                'uint64_t now_ms = esp_timer_get_time() / 1000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside bb_clock/")

    def test_no_fire_inside_bb_timer(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_timer/bb_timer.c",
                'uint64_t now_ms = bb_timer_now_us() / 1000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertFalse(violations, "must NOT fire inside bb_timer/")

    def test_fires_on_integer_suffix_variants(self):
        """1000u/1000U/1000UL/1000ULL/1000LL must all fire — the trailing \\b
        after 1000 previously failed on any C integer suffix, making the
        rule nearly inert on the real tree."""
        for suffix in ("u", "U", "UL", "ULL", "LL"):
            with tempfile.TemporaryDirectory() as td:
                self._make_file(td, "components/bb_fake/src/fake.c",
                    f'uint64_t now_ms = esp_timer_get_time() / 1000{suffix};\n')
                violations = _check_raw_timestamp_divide(make_ctx(td))
                self.assertTrue(violations,
                    f"esp_timer_get_time()/1000{suffix} must fire")

    def test_no_fire_on_10000(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'uint64_t x = esp_timer_get_time() / 10000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertFalse(violations, "/10000 must NOT fire (not a ms conversion)")

    def test_no_fire_on_100000(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/src/fake.c",
                'uint64_t x = esp_timer_get_time() / 100000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertFalse(violations, "/100000 must NOT fire (not a ms conversion)")

    def test_no_fire_inside_real_bb_clock_layout(self):
        """The canonical clock impl lives at platform/espidf/bb_core/bb_clock.c
        (component bb_core, no bb_clock/ directory) — the exemption must key
        on the bb_clock.c/.h basename, not a path component literally named
        bb_clock."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_core/bb_clock.c",
                'uint64_t now_ms = esp_timer_get_time() / 1000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertFalse(violations,
                "must NOT fire inside the real platform/espidf/bb_core/bb_clock.c path")

    def test_fires_on_normal_file_same_component_dir(self):
        """A sibling file in the same bb_core component dir (not named
        bb_clock.c) must still fire — the exemption is basename-scoped."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "platform/espidf/bb_core/bb_other.c",
                'uint64_t now_ms = esp_timer_get_time() / 1000;\n')
            violations = _check_raw_timestamp_divide(make_ctx(td))
            self.assertTrue(violations,
                "a normal file in bb_core (not bb_clock.c) must fire")


# Real breadboard repo root (scripts/bbtool/tests/../../.. ), mirrors
# test_lint_integration.py's REPO_ROOT — used only by the integration case
# below (never for the synthetic-fixture cases in this class).
_REAL_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..")
)


class TestEmitSeamUnwiredSubscriber(unittest.TestCase):
    """B1-740: an app links an emit-seam publisher (BB_CALLBACK_SLOT_VOID
    over a bb_emit_fn slot) and a subscriber of its topic, but never wires
    the seam's setter -- generic over the emit-seam pattern; wifi.net/
    bb_wifi (real repo, tested here as the integration case) was the first
    live instance."""

    _SEAM_C = (
        'void bb_seam_init(void) {}\n'
        '\n'
        'BB_CALLBACK_SLOT_VOID(emit, bb_emit_fn, bb_seam_set_emit, bb_seam_emit_invoke,\n'
        '                      (const char *topic, int32_t id, const void *payload, size_t size),\n'
        '                      (topic, id, payload, size))\n'
        '\n'
        'void bb_seam_fire(void)\n'
        '{\n'
        '    bb_seam_emit_invoke(BB_SEAM_TOPIC, 0, NULL, 0);\n'
        '}\n'
    )

    _SUB_C = (
        'void bb_sub_init(void)\n'
        '{\n'
        '    bb_event_subscribe(BB_SEAM_TOPIC, handler, NULL, &sub);\n'
        '}\n'
    )

    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def _make_component(self, tmpdir: str, name: str, source: str, requires=None) -> None:
        """Flat components/<name>/ layout (CMakeLists.txt + <name>.c
        directly under the component dir) so boards.discover_components /
        derive_component resolve it like a real flat component (e.g.
        bb_mdns)."""
        body = f'idf_component_register(\n    SRCS "{name}.c"\n'
        if requires:
            body += f"    REQUIRES {' '.join(requires)}\n"
        body += ")\n"
        self._make_file(tmpdir, f"components/{name}/CMakeLists.txt", body)
        self._make_file(tmpdir, f"components/{name}/{name}.c", source)

    def _make_app(self, tmpdir: str, app_name: str, requires: list) -> None:
        self._make_file(
            tmpdir, f"examples/{app_name}/main/CMakeLists.txt",
            "idf_component_register(\n    SRCS \"main.c\"\n"
            f"    REQUIRES {' '.join(requires)}\n)\n")
        self._make_file(tmpdir, f"examples/{app_name}/main/main.c",
                         'void app_main(void) {}\n')

    def test_fires_when_seam_and_subscriber_in_closure_but_unwired(self):
        """FIRES: fake seam + fake subscriber both in a fake app's closure,
        no setter call anywhere under the app's main/ -> violation."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            self._make_app(td, "fakeapp", requires=["bb_seam", "bb_sub"])
            violations = _check_emit_seam_unwired_subscriber(make_ctx(td))
            self.assertTrue(violations,
                "unwired seam+subscriber co-present in an app's closure must fire")
            self.assertIn("bb_seam_set_emit", violations[0]["detail"])

    def test_no_fire_when_setter_is_wired(self):
        """PASSES: same closure, but the app's main/ calls the setter."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            self._make_app(td, "fakeapp", requires=["bb_seam", "bb_sub"])
            self._make_file(td, "examples/fakeapp/main/main.c",
                'void app_main(void) {\n'
                '    bb_seam_set_emit(my_sink);\n'
                '}\n')
            violations = _check_emit_seam_unwired_subscriber(make_ctx(td))
            self.assertFalse(violations, "a wired setter call must NOT fire")

    def test_no_fire_when_app_omits_subscriber(self):
        """PASSES: app links the seam but never the subscriber."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            self._make_app(td, "fakeapp", requires=["bb_seam"])
            violations = _check_emit_seam_unwired_subscriber(make_ctx(td))
            self.assertFalse(violations,
                "an app without the subscriber in its closure must NOT fire")

    def test_no_fire_when_app_omits_seam(self):
        """PASSES: app links the subscriber but never the seam owner."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            self._make_app(td, "fakeapp", requires=["bb_sub"])
            violations = _check_emit_seam_unwired_subscriber(make_ctx(td))
            self.assertFalse(violations,
                "an app without the seam owner in its closure must NOT fire")

    def test_suppressed_via_allowlist(self):
        """SUPPRESSED: an allowlisted app path must not fire even when
        unwired."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            self._make_app(td, "fakeapp", requires=["bb_seam", "bb_sub"])
            config = {"lint": {"rules": {"emit-seam-unwired-subscriber": {
                "allow": ["examples/fakeapp/main"]
            }}}}
            ctx = Context(root=td, config=config)
            violations = _check_emit_seam_unwired_subscriber(ctx)
            self.assertFalse(violations, "allowlisted app path must NOT fire")

    def test_graceful_skip_on_conditional_set_error(self):
        """A conditionally-set() REQUIRES var must be skipped (stderr note),
        not crash the whole rule -- other apps still get checked."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            self._make_file(
                td, "examples/conditionalapp/main/CMakeLists.txt",
                'if(SOME_FLAG)\n'
                '    set(APP_REQUIRES bb_seam bb_sub)\n'
                'endif()\n'
                'idf_component_register(\n    SRCS "main.c"\n'
                '    REQUIRES ${APP_REQUIRES}\n)\n')
            self._make_file(td, "examples/conditionalapp/main/main.c",
                             'void app_main(void) {}\n')
            self._make_app(td, "fakeapp", requires=["bb_seam", "bb_sub"])
            violations = _check_emit_seam_unwired_subscriber(make_ctx(td))
            # conditionalapp is skipped (ConditionalSetError); fakeapp still
            # gets checked and fires (it has no setter wire either).
            self.assertTrue(violations, "the conditional-set app must be skipped, not crash")
            paths = {v["path"] for v in violations}
            self.assertTrue(
                any("fakeapp" in p for p in paths),
                "fakeapp must still be checked after conditionalapp is skipped")

    def test_graceful_skip_on_dependency_conditional_set_error(self):
        """A DEPENDENCY component's (not the app's own CMakeLists.txt)
        conditionally-set() REQUIRES var raises ConditionalSetError deep
        inside resolve_composition -> boards.derive_component ->
        cmake_parse.parse_requires -- this must be caught the same way as
        the app's own file, not propagate uncaught out of the rule (which
        would abort the entire bbtool lint run, not just this rule)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(td, "bb_seam", self._SEAM_C)
            self._make_component(td, "bb_sub", self._SUB_C)
            # bb_dep's OWN CMakeLists.txt conditionally set()s its REQUIRES
            # -- the app requiring bb_dep parses fine at the top level; the
            # ConditionalSetError only surfaces once resolve_composition
            # walks into bb_dep's own derive_component() call.
            self._make_file(
                td, "components/bb_dep/CMakeLists.txt",
                'if(SOME_FLAG)\n'
                '    set(DEP_REQUIRES bb_seam bb_sub)\n'
                'endif()\n'
                'idf_component_register(\n    SRCS "bb_dep.c"\n'
                '    REQUIRES ${DEP_REQUIRES}\n)\n')
            self._make_file(td, "components/bb_dep/bb_dep.c",
                             'void bb_dep_init(void) {}\n')
            self._make_app(td, "depapp", requires=["bb_dep"])
            self._make_app(td, "fakeapp", requires=["bb_seam", "bb_sub"])
            # Must not raise ConditionalSetError out of the rule.
            violations = _check_emit_seam_unwired_subscriber(make_ctx(td))
            paths = {v["path"] for v in violations}
            self.assertTrue(
                any("fakeapp" in p for p in paths),
                "fakeapp must still be checked after depapp is skipped")
            self.assertFalse(
                any("depapp" in p for p in paths),
                "depapp must never appear as a violation -- it was skipped"
                " (dependency-level conditional REQUIRES), not evaluated")

    def test_real_repo_clean_smoke_and_floor(self):
        """INTEGRATION: the real repo's wifi.net/bb_wifi instance -- floor's
        closure never includes bb_wifi (out of scope). smoke's closure
        includes bb_wifi + bb_mdns/bb_mqtt_client; its wifi.net emit seam is
        UNWIRED since the bb_event provider dissolved (B1-1045) -- an
        unmatched `consumes=emit_sink` key is a soft no-op in codegen (not a
        hard error, see wire_parse.py), so this stays clean under this rule
        rather than firing an unwired-subscriber violation (smoke's rehab,
        repointing the seam onto bb_lifecycle, is B1-1051). Regenerated into
        examples/smoke/main/generated/bb_app_init.c by the Makefile's
        smoke-gen/floor-gen, a prerequisite of this test's own `test-py`
        target -- so the real repo must be clean under this rule."""
        violations = _check_emit_seam_unwired_subscriber(make_ctx(_REAL_REPO_ROOT))
        self.assertFalse(
            violations,
            f"real repo must be clean under emit-seam-unwired-subscriber: {violations}")


class TestProvDefaultFormInternalRef(unittest.TestCase):
    """prov-default-form-internal-ref (B1-1255) -- bb_wifi_prov must never
    reference the default-form getter/gz symbols outside the form's own TU
    (components/bb_wifi_prov/default_form/bb_wifi_prov_default_form.c +
    components/bb_wifi_prov/include/bb_wifi_prov_default_form.h)."""

    def _write(self, tmpdir: str, rel_path: str, content: str) -> None:
        path = os.path.join(tmpdir, *rel_path.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        Path(path).write_text(content)

    def test_fires_on_reference_elsewhere_in_bb_wifi_prov(self):
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "components/bb_wifi_prov/src/bb_wifi_prov_page.c",
                'const bb_http_asset_t *x(void) {\n'
                '    return bb_wifi_prov_default_form_get();\n'
                '}\n')
            violations = _check_prov_default_form_internal_ref(make_ctx(td))
            self.assertTrue(
                violations,
                "a reference from another bb_wifi_prov .c file must fire")

    def test_fires_on_reference_in_platform_layer(self):
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "platform/espidf/bb_wifi_prov/bb_wifi_prov.c",
                'extern const uint8_t bb_wifi_prov_default_form_gz[];\n')
            violations = _check_prov_default_form_internal_ref(make_ctx(td))
            self.assertTrue(
                violations,
                "a reference from platform/*/bb_wifi_prov/ must fire")

    def test_no_fire_in_forms_own_tu(self):
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td,
                "components/bb_wifi_prov/default_form/"
                "bb_wifi_prov_default_form.c",
                'extern const uint8_t bb_wifi_prov_default_form_gz[];\n'
                'const bb_http_asset_t *bb_wifi_prov_default_form_get(void)'
                ' { return 0; }\n')
            self._write(
                td, "components/bb_wifi_prov/include/"
                "bb_wifi_prov_default_form.h",
                'const bb_http_asset_t *bb_wifi_prov_default_form_get(void);\n')
            violations = _check_prov_default_form_internal_ref(make_ctx(td))
            self.assertFalse(
                violations,
                "the form's own TU (.c + .h) must never fire")

    def test_no_fire_on_doxygen_usage_example_comment(self):
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "components/bb_wifi_prov/include/bb_wifi_prov.h",
                '/**\n'
                ' * For bare-minimum bringup:\n'
                ' *   const bb_http_asset_t *a = bb_wifi_prov_default_form_get();\n'
                ' *   bb_wifi_prov_start(a, 1, NULL);\n'
                ' */\n'
                'bb_err_t bb_wifi_prov_start(void);\n')
            violations = _check_prov_default_form_internal_ref(make_ctx(td))
            self.assertFalse(
                violations,
                "a comment mentioning the symbol as a usage example must"
                " not fire -- it causes no linking (comment-stripping path)")

    def test_same_basename_decoy_in_different_directory_still_fires(self):
        """Regression test for the basename-only-exemption bypass: a file
        sharing the form's own basename (bb_wifi_prov_default_form.c) but
        living in a DIFFERENT directory (components/bb_wifi_prov/decoy/,
        not default_form/) still contains a genuine internal reference and
        MUST fire. A basename-only exemption set (matching on path.name
        instead of the full repo-relative path) silently exempts this decoy
        regardless of directory -- this test fails against that
        implementation and passes only once the exemption is anchored to
        the specific own-TU paths."""
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "components/bb_wifi_prov/decoy/"
                "bb_wifi_prov_default_form.c",
                'extern const uint8_t bb_wifi_prov_default_form_gz[];\n'
                'const uint8_t *decoy(void) { return bb_wifi_prov_default_form_gz; }\n')
            violations = _check_prov_default_form_internal_ref(make_ctx(td))
            self.assertTrue(
                violations,
                "a same-basename file outside the form's own directory"
                " must still fire -- basename-only exemption is a bypass")


class TestInitMarkerGatedSrcs(unittest.TestCase):
    """B1-1337: a `// bbtool:init fn=` marker naming a function whose ONLY
    definition lives in a `.c` file conditionally added to a component's
    SRCS behind a Kconfig gate (`if(CONFIG_X) list(APPEND SRCS ...)
    endif()`) must hard-error -- disabling that symbol would emit a call
    to an uncompiled symbol."""

    def _write(self, tmpdir: str, relpath: str, content: str) -> None:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def _make_gated_component(self, tmpdir: str, header_body: str) -> None:
        """components/bb_fake/ with an always-built bb_fake_always.c and a
        bb_fake_gated.c added to SRCS only inside if(CONFIG_BB_FAKE_ROUTES),
        both discovered via a var-indirected idf_component_register(SRCS
        ${_srcs} ...) call -- the exact shape B1-1337 targets."""
        self._write(
            tmpdir, "components/bb_fake/CMakeLists.txt",
            'set(_srcs "${CMAKE_CURRENT_LIST_DIR}/bb_fake_always.c")\n'
            "if(CONFIG_BB_FAKE_ROUTES)\n"
            '    list(APPEND _srcs "${CMAKE_CURRENT_LIST_DIR}/bb_fake_gated.c")\n'
            "endif()\n"
            "idf_component_register(\n"
            "    SRCS ${_srcs}\n"
            '    INCLUDE_DIRS "include"\n'
            ")\n")
        self._write(
            tmpdir, "components/bb_fake/bb_fake_always.c",
            'bb_err_t bb_fake_always_init(void) { return BB_OK; }\n')
        self._write(
            tmpdir, "components/bb_fake/bb_fake_gated.c",
            'bb_err_t bb_fake_gated_init(void) { return BB_OK; }\n')
        self._write(
            tmpdir, "components/bb_fake/include/bb_fake.h",
            "#pragma once\n" + header_body)

    def test_fires_on_gated_only_definition(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "bb_err_t bb_fake_gated_init(void);\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertTrue(
                violations,
                "marker fn defined only in a Kconfig-gated SRCS entry must fire")
            self.assertIn("bb_fake_gated_init", violations[0]["detail"])
            self.assertIn("CONFIG_BB_FAKE_ROUTES", violations[0]["detail"])

    def test_no_fire_with_header_else_stub(self):
        """The B1-1336 fix shape: the real declaration is #if-gated, but
        the #else branch supplies a static inline stub -- always reachable
        regardless of the SRCS gate, so this must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "#if CONFIG_BB_FAKE_ROUTES\n"
                "bb_err_t bb_fake_gated_init(void);\n"
                "#else\n"
                "static inline bb_err_t bb_fake_gated_init(void)"
                " { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "a header #else static-inline stub must exempt the marker")

    def test_fires_when_header_stub_is_primary_arm_only(self):
        """B1-1337 review HIGH: a `static inline` stub textually present
        but ONLY inside the PRIMARY #if arm (no #else/#elif) is NOT a real
        fallback -- when the gate is off, NEITHER the .c definition NOR
        this stub exists. The prior (preprocessor-blind) version of the
        header-stub exclusion treated ANY textual `fn(...) { ... }`
        occurrence as a trusted fallback and missed this exact shape."""
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "#if CONFIG_BB_FAKE_ROUTES\n"
                "static inline bb_err_t bb_fake_gated_init(void)"
                " { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertTrue(
                violations,
                "a stub confined to the #if primary arm (no #else/#elif)"
                " must NOT exempt the marker -- it's unreachable when the"
                " gate is off, same as having no stub at all")

    def test_fires_when_header_stub_is_elif_arm_only(self):
        """B1-1337 review HIGH round 2: an #elif arm is NOT a terminal
        catch-all -- its own condition can also be false. A stub confined
        to an #elif arm with no trailing #else is unreachable for the
        (valid, uncoupled) combination where every arm's condition is
        off -- codegen still emits the call unconditionally, and neither
        the .c definition (gated on CONFIG_BB_FAKE_ROUTES) nor this stub
        exists for that combination. Must fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "#if CONFIG_BB_FAKE_OTHER\n"
                "bb_err_t bb_fake_gated_init(void);\n"
                "#elif CONFIG_BB_FAKE_ROUTES\n"
                "static inline bb_err_t bb_fake_gated_init(void)"
                " { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertTrue(
                violations,
                "a stub confined to an #elif arm (no trailing #else) must"
                " NOT exempt the marker -- #elif is not a terminal"
                " catch-all, same reasoning as the primary-#if-only case")

    def test_no_fire_with_header_if_elif_else_stub(self):
        """The shape that actually matters: an #elif chain that ENDS in a
        genuine terminal #else stub is trusted -- reachable regardless of
        which of the two prior conditions is false."""
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "#if CONFIG_BB_FAKE_OTHER\n"
                "bb_err_t bb_fake_gated_init(void);\n"
                "#elif CONFIG_BB_FAKE_ROUTES\n"
                "bb_err_t bb_fake_gated_init(void);\n"
                "#else\n"
                "static inline bb_err_t bb_fake_gated_init(void)"
                " { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "a terminal #else stub after an #elif chain must exempt"
                " the marker -- it's reachable regardless of which"
                " earlier arm's condition is false")

    def test_no_fire_with_header_else_stub_nested_in_platform_guard(self):
        """The real bb_ota_boot.h shape (B1-1347 fix regression): a genuine
        Kconfig `#else` stub sits INSIDE an outer `#ifdef ESP_PLATFORM`
        that itself has no `#else`/terminal arm. The outer platform frame
        must NOT block the inner Kconfig `#else` stub from counting as
        trusted -- a non-Kconfig platform selector is structurally fixed
        for the backend TU the header is compiled into, not a knob a
        board's sdkconfig can flip."""
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "#ifdef ESP_PLATFORM\n"
                "#if CONFIG_BB_FAKE_ROUTES\n"
                "bb_err_t bb_fake_gated_init(void);\n"
                "#else\n"
                "static inline bb_err_t bb_fake_gated_init(void)"
                " { return BB_OK; }\n"
                "#endif\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "a Kconfig #else stub nested inside a no-#else platform"
                " guard (#ifdef ESP_PLATFORM) must still exempt the"
                " marker -- the platform frame is not a Kconfig knob")

    def test_no_fire_on_ungated_file(self):
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "components/bb_fake/CMakeLists.txt",
                "idf_component_register(\n"
                '    SRCS "${CMAKE_CURRENT_LIST_DIR}/bb_fake_ungated.c"\n'
                '    INCLUDE_DIRS "include"\n'
                ")\n")
            self._write(
                td, "components/bb_fake/bb_fake_ungated.c",
                'bb_err_t bb_fake_ungated_init(void) { return BB_OK; }\n')
            self._write(
                td, "components/bb_fake/include/bb_fake.h",
                "#pragma once\n"
                "// bbtool:init tier=early fn=bb_fake_ungated_init\n"
                "bb_err_t bb_fake_ungated_init(void);\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "an unconditionally-compiled defining file must not fire")

    def test_no_fire_on_multi_platform_definition(self):
        """A SECOND definition of the same fn exists in a file that is
        NEVER referenced as a SRCS token by any component's CMakeLists.txt
        at all (e.g. a host-backend file reached via
        boards.derive_component's directory convention rather than a
        literal idf_component_register(SRCS ...) token) -- can't prove
        it's gated, so this must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_gated_component(
                td,
                "// bbtool:init tier=early fn=bb_fake_gated_init\n"
                "bb_err_t bb_fake_gated_init(void);\n")
            self._write(
                td, "platform/host/bb_fake/bb_fake_gated.c",
                'bb_err_t bb_fake_gated_init(void) { return BB_OK; }\n')
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "a second, unreferenced-by-any-SRCS definition must exempt"
                " the marker (multi-platform false-positive guard)")

    def test_no_markers_no_violations(self):
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "components/bb_fake/CMakeLists.txt",
                "idf_component_register(\n"
                '    SRCS "${CMAKE_CURRENT_LIST_DIR}/bb_fake.c"\n'
                ")\n")
            self._write(td, "components/bb_fake/bb_fake.c", "void noop(void) {}\n")
            self.assertEqual(_check_init_marker_gated_srcs(make_ctx(td)), [])

    def _make_ungated_srcs_component(self, tmpdir: str, def_body: str) -> None:
        """components/bb_fake/ with a SINGLE .c file that is UNCONDITIONALLY
        added to SRCS -- the `_srcs_gate_map` unconditional-file exemption
        must not, by itself, prove `fn` is safe; the definition's own body
        may still be wrapped in an in-file preprocessor conditional. This
        is the real bb_mqtt_client_init_default shape (B1-1347): the
        DEFINING FILE was unconditional in SRCS, but the function body
        inside it was wrapped in `#if ... CONFIG_BB_MQTT_CLIENT_AUTOREGISTER`
        while the marker/declaration were ungated."""
        self._write(
            tmpdir, "components/bb_fake/CMakeLists.txt",
            "idf_component_register(\n"
            '    SRCS "${CMAKE_CURRENT_LIST_DIR}/bb_fake_unconditional.c"\n'
            '    INCLUDE_DIRS "include"\n'
            ")\n")
        self._write(
            tmpdir, "components/bb_fake/bb_fake_unconditional.c",
            def_body)
        self._write(
            tmpdir, "components/bb_fake/include/bb_fake.h",
            "#pragma once\n"
            "// bbtool:init tier=early fn=bb_fake_unconditional_init\n"
            "bb_err_t bb_fake_unconditional_init(void);\n")

    def test_fires_on_infile_kconfig_gated_definition(self):
        """The bb_mqtt_client_init_default shape: the SRCS entry is
        unconditional, but the definition's own BODY sits inside an
        in-file `#if CONFIG_X` with no unconditional/terminal-#else
        fallback -- disabling CONFIG_X leaves the (unconditionally
        compiled) file with no definition of fn at all, exactly the same
        link/compile failure as the SRCS-gated case."""
        with tempfile.TemporaryDirectory() as td:
            self._make_ungated_srcs_component(
                td,
                "#if CONFIG_BB_FAKE_AUTOREGISTER\n"
                "bb_err_t bb_fake_unconditional_init(void) { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertTrue(
                violations,
                "an in-file #if CONFIG_X gate around the definition body,"
                " with no unconditional/terminal-#else fallback, must fire"
                " even though the defining .c file is unconditional in SRCS")
            self.assertIn("bb_fake_unconditional_init", violations[0]["detail"])
            self.assertIn("CONFIG_BB_FAKE_AUTOREGISTER", violations[0]["detail"])
            self.assertIn(
                "disabling CONFIG_BB_FAKE_AUTOREGISTER", violations[0]["detail"],
                "the real-Kconfig-symbol message must keep the"
                " 'disabling X' framing -- only the opaque-condition"
                " case drops it (B1-1347 review LOW)")

    def test_no_fire_on_infile_esp_platform_guard(self):
        """A definition body wrapped in `#ifdef ESP_PLATFORM` is a platform
        selector, not a Kconfig knob a board can flip -- it's structurally
        always-true or always-false for a given backend TU (this file
        lives under an espidf-only component and is only ever compiled
        into an ESP-IDF build). Must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_ungated_srcs_component(
                td,
                "#ifdef ESP_PLATFORM\n"
                "bb_err_t bb_fake_unconditional_init(void) { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "#ifdef ESP_PLATFORM is a platform selector, not a"
                " board-flippable Kconfig knob -- must not fire")

    def test_no_fire_on_infile_testing_guard(self):
        """A definition body wrapped in an in-tree `*_TESTING` compile-mode
        macro (e.g. BB_LIFECYCLE_TESTING/BB_DATA_TESTING-style host-test
        gating) is a build-mode selector set by the test harness, never a
        Kconfig option a board's sdkconfig can turn off -- must NOT fire."""
        with tempfile.TemporaryDirectory() as td:
            self._make_ungated_srcs_component(
                td,
                "#ifdef BB_FAKE_TESTING\n"
                "bb_err_t bb_fake_unconditional_init(void) { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertFalse(
                violations,
                "*_TESTING is a build-mode selector, not a Kconfig knob --"
                " must not fire")

    def test_fires_on_infile_if_zero_guard(self):
        """B1-1347 review MEDIUM: `#if 0` is neither a CONFIG_* Kconfig
        token nor an allowlisted platform/build-mode selector -- it's an
        OPAQUE condition this text-only checker cannot prove is safe, so
        it must default to blocking, exactly like the old fully-generic
        mask did for every unrecognized condition. An earlier cut of this
        rule treated "not CONFIG_*" as "safe", which silently exempted
        this -- the single most obviously unreachable construct there
        is."""
        with tempfile.TemporaryDirectory() as td:
            self._make_ungated_srcs_component(
                td,
                "#if 0\n"
                "bb_err_t bb_fake_unconditional_init(void) { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertTrue(
                violations,
                "#if 0 around the only definition must fire -- it is"
                " never reachable, and being unrecognized (not CONFIG_*,"
                " not an allowlisted platform guard) must mean dangerous,"
                " never safe")
            detail = violations[0]["detail"]
            self.assertIn(
                "cannot prove is always true", detail,
                "the opaque-condition message must explain the checker"
                " can't prove the guard is always true (B1-1347 review LOW)")
            self.assertNotIn(
                "disabling", detail,
                "the opaque-condition message must NOT use the"
                " 'disabling X' framing -- that only makes sense for a"
                " real CONFIG_* symbol with an off state (B1-1347 review"
                " LOW: 'disabling #if 0' / 'when #if 0 is off' is"
                " nonsense)")
            self.assertNotIn(
                "is off", detail,
                "the opaque-condition message must NOT use the"
                " 'when X is off' framing (same B1-1347 review LOW nit)")

    def test_fires_on_infile_typoed_config_symbol(self):
        """B1-1347 review MEDIUM: a one-character typo in the CONFIG_
        symbol name (CONFGI_ instead of CONFIG_) makes `_CONFIG_TOKEN_RE`
        miss it -- but it's still not an allowlisted platform/build-mode
        selector, so it must be treated as OPAQUE (blocking), not safe.
        Same defect class as #if 0: unrecognized must mean dangerous."""
        with tempfile.TemporaryDirectory() as td:
            self._make_ungated_srcs_component(
                td,
                "#if CONFGI_BB_FAKE_AUTOREGISTER\n"
                "bb_err_t bb_fake_unconditional_init(void) { return BB_OK; }\n"
                "#endif\n")
            violations = _check_init_marker_gated_srcs(make_ctx(td))
            self.assertTrue(
                violations,
                "a typo'd/unrecognized macro gating the only definition"
                " must fire -- this checker cannot prove it's safe, so it"
                " must not be silently trusted")


class TestJoinPreprocContinuations(unittest.TestCase):
    """Direct unit coverage for `_join_preproc_continuations` -- previously
    exercised only indirectly through init-marker-gated-srcs/kconfig-inert
    rule-level tests. Every case asserts BOTH the joined content and
    `len(out) == len(input.splitlines())`, since line-count preservation
    is what keeps callers' physical-line indexing (and any chain-depth
    bookkeeping built on top of it) aligned with the original source."""

    def test_two_line_continuation(self):
        src = "#if CONFIG_A && \\\nCONFIG_B\n"
        out = _join_preproc_continuations(src)
        self.assertEqual(len(out), len(src.splitlines()))
        self.assertEqual(out, ["#if CONFIG_A &&  CONFIG_B", ""])

    def test_three_line_chain(self):
        src = ("#if CONFIG_A && \\\n"
               "    CONFIG_B && \\\n"
               "    CONFIG_C\n")
        out = _join_preproc_continuations(src)
        self.assertEqual(len(out), len(src.splitlines()))
        self.assertEqual(
            out,
            ["#if CONFIG_A &&  CONFIG_B &&  CONFIG_C", "", ""])

    def test_trailing_whitespace_after_backslash_still_joins(self):
        """`.rstrip()` runs before the `endswith('\\\\')` check, so a
        backslash followed by trailing whitespace must still be treated
        as a continuation marker, not a literal end-of-line backslash."""
        src = "#if CONFIG_A \\   \nCONFIG_B\n"
        out = _join_preproc_continuations(src)
        self.assertEqual(len(out), len(src.splitlines()))
        self.assertEqual(out, ["#if CONFIG_A  CONFIG_B", ""])

    def test_dangling_backslash_on_final_line_is_left_alone(self):
        """A trailing backslash on the LAST physical line has no
        successor to join onto -- must not crash, and the dangling text
        (backslash included) is left untouched."""
        src = "#define X \\\n"
        out = _join_preproc_continuations(src)
        self.assertEqual(len(out), len(src.splitlines()))
        self.assertEqual(out, ["#define X \\"])

    def test_continuation_followed_by_independent_if_not_swallowed(self):
        """A continuation chain must not absorb the NEXT physical line's
        `#if` -- once its own chain ends, a later `#if` stays its own
        entry, indexed on its own physical line."""
        src = "#if CONFIG_A \\\nCONFIG_B\n#if CONFIG_C\n"
        out = _join_preproc_continuations(src)
        self.assertEqual(len(out), len(src.splitlines()))
        self.assertEqual(
            out,
            ["#if CONFIG_A  CONFIG_B", "", "#if CONFIG_C"])

    def test_no_continuations_is_pure_passthrough(self):
        src = "#if CONFIG_A\nfoo\nbar\n"
        out = _join_preproc_continuations(src)
        self.assertEqual(len(out), len(src.splitlines()))
        self.assertEqual(out, ["#if CONFIG_A", "foo", "bar"])


class TestBindsDataMismatch(unittest.TestCase):
    """B1-1376: a `// bbtool:init binds_data=` marker's declared key list
    must exactly match its owning component's real bb_data_bind() call
    sites, in either direction."""

    def _write(self, tmpdir: str, relpath: str, content: str) -> None:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def _make_component(self, tmpdir: str, name: str, header_marker: str,
                         c_body: str) -> None:
        self._write(
            tmpdir, f"components/{name}/CMakeLists.txt",
            f'idf_component_register(SRCS "{name}.c" INCLUDE_DIRS "include")\n')
        self._write(
            tmpdir, f"components/{name}/include/{name}.h",
            "#pragma once\n" + header_marker + f"bb_err_t {name}_init(void);\n")
        self._write(tmpdir, f"components/{name}/{name}.c", c_body)

    def test_exact_match_passes(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init binds_data=fan,power\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    bb_data_bind(&fan_binding);\n"
                "    bb_data_binding_t power_binding = {\n"
                "        .key = \"power\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    bb_data_bind(&power_binding);\n"
                "    return BB_OK;\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_declared_but_not_bound_errors(self):
        """The marker claims 'thermal' but no bb_data_bind() call in the
        component actually binds it -- inflates check_binds_data_cap's
        distinct-key count for no real capacity use."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init"
                " binds_data=fan,power,thermal\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    bb_data_bind(&fan_binding);\n"
                "    bb_data_binding_t power_binding = {\n"
                "        .key = \"power\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    bb_data_bind(&power_binding);\n"
                "    return BB_OK;\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertTrue(violations, "a declared-but-unbound key must fire")
            detail = violations[-1]["detail"]
            self.assertIn("declared but not bound", detail)
            self.assertIn("thermal", detail)

    def test_bound_but_not_declared_errors(self):
        """The component's bb_data_bind() calls bind 'power' too, but the
        marker only claims 'fan' -- undercounts check_binds_data_cap's
        distinct-key count, the exact defect class the cap check exists to
        catch, one level up."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init binds_data=fan\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    bb_data_bind(&fan_binding);\n"
                "    bb_data_binding_t power_binding = {\n"
                "        .key = \"power\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    bb_data_bind(&power_binding);\n"
                "    return BB_OK;\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertTrue(violations, "a bound-but-undeclared key must fire")
            detail = violations[-1]["detail"]
            self.assertIn("bound but not declared", detail)
            self.assertIn("power", detail)

    def test_macro_constant_key_resolves(self):
        """The BB_DISPLAY_INFO_TOPIC/BB_DIAG_BOOT_TOPIC/BB_OTA_CHECK_TOPIC
        shape: a `.key=` identifier resolved via a `#define IDENT "value"`
        grepped from the same component's own tree, never an exemption
        list -- must match cleanly when the marker's declared key equals
        the RESOLVED macro value, not the macro's bare identifier text."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init"
                " binds_data=fake.topic\n",
                "#define BB_FAKE_TOPIC \"fake.topic\"\n"
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t binding = {\n"
                "        .key = BB_FAKE_TOPIC, .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&binding);\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_unresolvable_macro_key_reports_loudly(self):
        """A `.key=` identifier with no matching #define anywhere in the
        component's tree must be reported as its own violation -- never
        silently treated as 'not bound' (which would misreport a real
        binding as declared-but-not-bound)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init"
                " binds_data=fake.topic\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t binding = {\n"
                "        .key = BB_UNDEFINED_TOPIC, .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&binding);\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertTrue(violations, "an unresolvable .key= macro must fire")
            details = " ".join(v["detail"] for v in violations)
            self.assertIn("BB_UNDEFINED_TOPIC", details)
            self.assertIn("no resolvable #define", details)

    def test_platform_only_binds_resolve(self):
        """A component whose bb_data_bind() calls live entirely under
        platform/<plat>/<name>/, not components/<name>/ -- checking
        components/ alone would be blind to them (bb_mdns_cache's real
        shape)."""
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "components/bb_fake/CMakeLists.txt",
                'idf_component_register(INCLUDE_DIRS "include")\n')
            self._write(
                td, "components/bb_fake/include/bb_fake.h",
                "#pragma once\n"
                "// bbtool:init tier=regular fn=bb_fake_init binds_data=fan\n"
                "bb_err_t bb_fake_init(void);\n")
            self._write(
                td, "platform/espidf/bb_fake/bb_fake_espidf.c",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&fan_binding);\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_no_marker_no_binds_declaration_never_fires(self):
        """A component with real bb_data_bind() calls but NO
        `binds_data=` marker at all is out of scope -- this rule only
        verifies an EXISTING claim, it never invents one."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&fan_binding);\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertFalse(
                violations,
                "no binds_data= marker at all must never fire, regardless"
                " of real bb_data_bind() calls present")

    def test_zero_call_sites_reports_actionable_message(self):
        """A component with NO bb_data_bind() call site anywhere in its own
        tree (e.g. bb_log_event's real shape -- its bind is made by the
        CONSUMING app at its composition root, per
        components/bb_log_event/README.md) must not be reported the same
        way as a real per-key mismatch: that message's suggested fix ("bind
        the key") is impossible to apply on this marker, since this checker
        cannot follow a bind into an arbitrary consuming app. It must
        instead name the actual situation with an applicable fix."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init"
                " binds_data=log\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    return BB_OK;\n"
                "}\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertTrue(
                violations,
                "declaring binds_data= on a component with zero"
                " bb_data_bind() call sites must fire")
            detail = violations[-1]["detail"]
            self.assertIn("NO bb_data_bind() call site anywhere", detail)
            self.assertNotIn("declared but not bound anywhere in component",
                              detail,
                              "must not reuse the per-key mismatch message"
                              " -- its suggested fix is impossible to apply"
                              " when the component has zero call sites")

    def test_test_only_bind_helper_excluded(self):
        """A `*_for_test` helper (the in-tree convention --
        bb_system_reboot_bind_for_test(),
        bb_storage_http_factory_reset_bind_for_test()) binding a DIFFERENT
        key than production must be excluded from the real bind set --
        otherwise a future test-only helper binding a synthetic key would
        force a developer to add a test-only key to the real declaration,
        the exact phantom-binding inflation this rule exists to prevent."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init"
                " binds_data=fan\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&fan_binding);\n"
                "}\n"
                "\n"
                "#ifdef BB_FAKE_TESTING\n"
                "bb_err_t bb_fake_thermal_bind_for_test(void)\n"
                "{\n"
                "    bb_data_binding_t thermal_binding = {\n"
                "        .key = \"thermal\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&thermal_binding);\n"
                "}\n"
                "#endif\n")
            violations = _check_binds_data_mismatch(make_ctx(td))
            self.assertFalse(
                violations,
                "a test-only helper's bind of a different key must not be"
                " counted as a production bind: " + str(violations))

    def test_wired_into_lint_run_entry_point(self):
        """Proof this rule actually runs through the real `bbtool lint`
        entry point (`run()`), not merely reachable if called directly
        (PR #1189's failure mode: a check whose only test called the
        function directly, leaving the registration call site itself
        deletable with everything green). Filters to just this rule id and
        asserts a non-zero exit over a tree with a genuine mismatch."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake",
                "// bbtool:init tier=regular fn=bb_fake_init"
                " binds_data=fan,power\n",
                "bb_err_t bb_fake_init(void) {\n"
                "    bb_data_binding_t fan_binding = {\n"
                "        .key = \"fan\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&fan_binding);\n"
                "}\n")
            args = argparse.Namespace(
                root=td,
                profile=None,
                rules=["binds-data-mismatch"],
                list=False,
                _config_dict={},
                _root_abs=td,
            )
            rc = lint_run(args)
            self.assertEqual(
                rc, 1,
                "bbtool lint's real run() entry point must surface a"
                " binds-data-mismatch violation as a non-zero exit")


class TestBindsDataHiddenBind(unittest.TestCase):
    """B1-1428: a real bb_data_bind() call reached only through a same-tree
    helper in a DIFFERENT component than the composing marker must be named
    in wire_parse.INDIRECT_BB_DATA_BINDS so check_binds_data_cap counts it."""

    def _write(self, tmpdir: str, relpath: str, content: str) -> None:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def _make_component(self, tmpdir: str, name: str, header_marker: str,
                         c_body: str) -> None:
        self._write(
            tmpdir, f"components/{name}/CMakeLists.txt",
            f'idf_component_register(SRCS "{name}.c" INCLUDE_DIRS "include")\n')
        self._write(
            tmpdir, f"components/{name}/include/{name}.h",
            "#pragma once\n" + header_marker + f"bb_err_t {name}_init(void);\n")
        self._write(tmpdir, f"components/{name}/{name}.c", c_body)

    def _make_indirect_fixture(self, td: str) -> None:
        """Two components mirroring the real diag.boot shape: bb_fake_a's
        marker fn calls bb_fake_b_wrapper(), defined in a DIFFERENT
        component (bb_fake_b), which itself calls bb_data_bind() for key
        'hidden.key' -- invisible to binds_data=/binds-data-mismatch since
        neither marker names bb_fake_b_wrapper directly."""
        self._make_component(
            td, "bb_fake_a",
            "// bbtool:init tier=regular fn=bb_fake_a_init\n",
            "bb_err_t bb_fake_a_init(void) {\n"
            "    return bb_fake_b_wrapper();\n"
            "}\n")
        self._make_component(
            td, "bb_fake_b",
            "",
            "bb_err_t bb_fake_b_wrapper(void) {\n"
            "    bb_data_binding_t hidden_binding = {\n"
            "        .key = \"hidden.key\", .desc = &d, .gather = g,\n"
            "    };\n"
            "    return bb_data_bind(&hidden_binding);\n"
            "}\n")

    def test_reachable_undocumented_indirect_bind_fires(self):
        """No manifest entry at all for bb_fake_b_wrapper -- the exact
        B1-1427 shape (reachable from a marker, no way for
        check_binds_data_cap to ever count it) -- must be a hard
        violation naming the function, key, and file:line."""
        with tempfile.TemporaryDirectory() as td:
            self._make_indirect_fixture(td)
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertTrue(violations, "an undocumented reachable indirect bind must fire")
            detail = violations[-1]["detail"]
            self.assertIn("bb_fake_b_wrapper", detail)
            self.assertIn("hidden.key", detail)

    def test_reachable_accounted_indirect_bind_passes(self):
        """The manifest names bb_fake_b_wrapper/hidden.key, triggered by
        bb_fake_a_init -- check_binds_data_cap can now count it, so this
        rule must stay silent."""
        from wire_parse import IndirectBind
        manifest = (
            IndirectBind(trigger_fn="bb_fake_a_init",
                         wrapper_fn="bb_fake_b_wrapper", key="hidden.key"),
        )
        with tempfile.TemporaryDirectory() as td:
            self._make_indirect_fixture(td)
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", manifest):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_manifest_wrapper_present_but_key_mismatch_fires(self):
        """The manifest names bb_fake_b_wrapper but for a DIFFERENT key than
        the one it actually binds -- a stale/wrong manifest key must still
        surface as an undocumented bind for the REAL key (never silently
        treated as covered just because the wrapper name matches)."""
        from wire_parse import IndirectBind
        manifest = (
            IndirectBind(trigger_fn="bb_fake_a_init",
                         wrapper_fn="bb_fake_b_wrapper", key="wrong.key"),
        )
        with tempfile.TemporaryDirectory() as td:
            self._make_indirect_fixture(td)
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", manifest):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertTrue(violations, "a wrapper-matched but key-mismatched manifest entry must still fire")
            hidden_bind_violations = [v for v in violations if "hidden.key" in v["detail"]]
            self.assertTrue(hidden_bind_violations, violations)

    def test_unreachable_indirect_bind_not_flagged(self):
        """bb_fake_b_wrapper() is never called by anything reachable from a
        marker OR from a hand-wired `app_main()` (it's only invoked by a
        plain, non-marker, non-entry-point helper, and this synthetic
        fixture defines no `examples/*/main/*.c` at all, so the handwire
        seed has nothing to reach through either) -- the same 'invisible to
        codegen' scope limit real OUT-OF-TREE-only code has today (e.g.
        `bb_ota_check_register_init()`, called only by an out-of-tree
        consumer's own `app_main`, never by anything in THIS repo -- see
        `wire_parse.INDIRECT_BB_DATA_BINDS`'s `bb_ota_check_config_bind`
        entry). Must NOT be flagged, since neither check_binds_data_cap nor
        binds-data-mismatch could ever see it either.

        NOTE (B1-1428 review): this must NOT be read as claiming
        `bb_display_info_bind()` is an example of unreachable code --
        that WAS true before `_build_call_graph_edges` started scanning
        examples/*/main/*.c and seeding BFS from `app_main()`
        (`_HANDWIRE_ENTRY_FNS`); it is real, in-tree-reachable, hand-wired
        code today (`examples/smoke/main/entry_espidf.c`'s `app_main()` ->
        `bb_display_register_info()` -> `bb_display_info_bind()`), covered
        by its own `IndirectBind(trigger_component='bb_display', ...)`
        entry and exercised by `test_reachable_undocumented_indirect_bind_
        fires`-style coverage against the real tree (see also
        `TestBindsDataCapIndirect` in test_wire.py)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake_a",
                "// bbtool:init tier=regular fn=bb_fake_a_init\n",
                "bb_err_t bb_fake_a_init(void) {\n"
                "    return BB_OK;\n"
                "}\n"
                "bb_err_t bb_fake_a_unwired(void) {\n"
                "    return bb_fake_b_wrapper();\n"
                "}\n")
            self._make_component(
                td, "bb_fake_b",
                "",
                "bb_err_t bb_fake_b_wrapper(void) {\n"
                "    bb_data_binding_t hidden_binding = {\n"
                "        .key = \"hidden.key\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&hidden_binding);\n"
                "}\n")
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertFalse(
                violations,
                "an indirect bind unreachable from any marker OR any"
                " in-tree app_main() must not fire: " + str(violations))

    def test_handwire_only_reachable_indirect_bind_fires(self):
        """The B1-1428 review finding, reproduced directly: a bind reached
        ONLY through a hand-wired `app_main()` (never a marker) must still
        fire -- `_reachable_from_composition_roots`' handwire seed
        (`_HANDWIRE_ENTRY_FNS`) closes exactly the blind spot
        `test_unreachable_indirect_bind_not_flagged` used to (incorrectly)
        codify as permanent. Mirrors the real `bb_display_info_bind()`
        shape: `app_main()` (examples/<x>/main/entry.c) calls a plain
        registration helper (never a marker), which calls the real
        bb_data_bind()-calling wrapper, in a DIFFERENT component."""
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "examples/fake/main/entry.c",
                "void app_main(void) {\n"
                "    bb_fake_a_register();\n"
                "}\n")
            self._make_component(
                td, "bb_fake_a",
                "",
                "void bb_fake_a_register(void) {\n"
                "    bb_fake_b_wrapper();\n"
                "}\n")
            self._make_component(
                td, "bb_fake_b",
                "",
                "bb_err_t bb_fake_b_wrapper(void) {\n"
                "    bb_data_binding_t hidden_binding = {\n"
                "        .key = \"hidden.key\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&hidden_binding);\n"
                "}\n")
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertTrue(
                violations,
                "a bind reachable only through a hand-wired app_main() must"
                " still fire")
            detail = violations[-1]["detail"]
            self.assertIn("bb_fake_b_wrapper", detail)
            self.assertIn("hidden.key", detail)
            self.assertIn("trigger_component", detail)

    def test_unrecognized_entry_point_name_hard_fails(self):
        """B1-1428 review LOW 1: an examples/*/main/ directory that defines
        .c files but NO function matching _HANDWIRE_ENTRY_FNS at all (e.g.
        a hypothetical future example whose entry point is named something
        other than app_main/main) must hard-fail LOUDLY, naming the
        directory -- silently scanning it and finding nothing would
        reproduce this rule's own blind spot for that example."""
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "examples/fake/main/entry.c",
                "void bb_fake_custom_entry(void) {\n"
                "    bb_fake_a_register();\n"
                "}\n")
            self._make_component(
                td, "bb_fake_a",
                "// bbtool:init tier=regular fn=bb_fake_a_init\n",
                "bb_err_t bb_fake_a_init(void) { return BB_OK; }\n")
            violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertTrue(
                violations,
                "an examples/*/main/ dir with no recognized entry point"
                " must fire")
            details = " ".join(v["detail"] for v in violations)
            self.assertIn("examples", details)
            self.assertIn("fake", details)
            self.assertIn("main", details)
            self.assertIn("_HANDWIRE_ENTRY_FNS", details)

    def test_recognized_entry_point_name_not_flagged(self):
        """The normal case (app_main present) must NOT trip the
        unrecognized-entry-point guard -- a negative-path sibling to
        test_unrecognized_entry_point_name_hard_fails."""
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "examples/fake/main/entry.c",
                "void app_main(void) {\n"
                "    bb_fake_a_init();\n"
                "}\n")
            self._make_component(
                td, "bb_fake_a",
                "// bbtool:init tier=regular fn=bb_fake_a_init\n",
                "bb_err_t bb_fake_a_init(void) { return BB_OK; }\n")
            violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_host_main_entry_point_recognized(self):
        """A host-platform example's C entry point (`main`, the POSIX/libc
        convention) must ALSO be recognized, not just `app_main` -- proves
        FIX 1's addition to _HANDWIRE_ENTRY_FNS, and that a bind reachable
        only through it is caught."""
        with tempfile.TemporaryDirectory() as td:
            self._write(
                td, "examples/fake/main/entry.c",
                "int main(void) {\n"
                "    bb_fake_a_register();\n"
                "    return 0;\n"
                "}\n")
            self._make_component(
                td, "bb_fake_a",
                "",
                "void bb_fake_a_register(void) {\n"
                "    bb_fake_b_wrapper();\n"
                "}\n")
            self._make_component(
                td, "bb_fake_b",
                "",
                "bb_err_t bb_fake_b_wrapper(void) {\n"
                "    bb_data_binding_t hidden_binding = {\n"
                "        .key = \"hidden.key\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&hidden_binding);\n"
                "}\n")
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            detail = " ".join(v["detail"] for v in violations)
            self.assertIn("bb_fake_b_wrapper", detail)
            self.assertIn("hidden.key", detail)

    def test_component_wide_declared_key_exempts_indirect_helper(self):
        """bb_fake_a's marker declares binds_data=hidden.key even though
        the real bb_data_bind() call lives inside a SIBLING helper
        (bb_fake_a_bind_helper), not the marker's own fn -- mirrors the
        real bb_sensor_http shape (bb_sensor_http_init declares
        fan/power/thermal, bb_sensor_http_bind_and_register() is the real
        call site). Already covered component-wide by binds_data=/
        binds-data-mismatch; must not ALSO be flagged here as an
        undocumented indirect bind."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake_a",
                "// bbtool:init tier=regular fn=bb_fake_a_init"
                " binds_data=hidden.key\n",
                "bb_err_t bb_fake_a_init(void) {\n"
                "    return bb_fake_a_bind_helper();\n"
                "}\n"
                "bb_err_t bb_fake_a_bind_helper(void) {\n"
                "    bb_data_binding_t hidden_binding = {\n"
                "        .key = \"hidden.key\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&hidden_binding);\n"
                "}\n")
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_direct_marker_fn_bind_not_flagged(self):
        """The bb_data_bind() call's enclosing function IS itself a
        marker's fn= (a DIRECT bind) -- binds-data-mismatch's job, not this
        rule's, even when the component carries no declaring marker at
        all (a pre-existing, documented scope limit this ticket does not
        extend)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake_a",
                "// bbtool:init tier=regular fn=bb_fake_a_init\n",
                "bb_err_t bb_fake_a_init(void) {\n"
                "    bb_data_binding_t hidden_binding = {\n"
                "        .key = \"hidden.key\", .desc = &d, .gather = g,\n"
                "    };\n"
                "    return bb_data_bind(&hidden_binding);\n"
                "}\n")
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertFalse(violations, violations)

    def test_stale_manifest_entry_fires(self):
        """The manifest names bb_fake_b_wrapper/stale.key, but this tree's
        bb_fake_b_wrapper() (a REAL, defined function in this root) binds a
        different key -- the stale manifest entry inflates
        check_binds_data_cap's count for no real capacity use and must be
        flagged."""
        from wire_parse import IndirectBind
        manifest = (
            IndirectBind(trigger_fn="bb_fake_a_init",
                         wrapper_fn="bb_fake_b_wrapper", key="stale.key"),
        )
        with tempfile.TemporaryDirectory() as td:
            self._make_indirect_fixture(td)
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", manifest):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            stale = [v for v in violations if "stale entry" in v["detail"]]
            self.assertTrue(stale, violations)
            self.assertIn("bb_fake_b_wrapper", stale[0]["detail"])
            self.assertIn("stale.key", stale[0]["detail"])

    def test_manifest_entry_for_absent_component_not_flagged_stale(self):
        """The manifest names a wrapper_fn this root's tree never defines
        at all (its owning component simply isn't present here, e.g. a
        synthetic single-component test fixture) -- must NOT be reported as
        stale, since an absent component says nothing about whether the
        entry is stale in the real tree it actually describes."""
        from wire_parse import IndirectBind
        manifest = (
            IndirectBind(trigger_fn="some_other_marker_fn",
                         wrapper_fn="bb_totally_unrelated_wrapper",
                         key="unrelated.key"),
        )
        with tempfile.TemporaryDirectory() as td:
            self._make_component(
                td, "bb_fake_a",
                "// bbtool:init tier=regular fn=bb_fake_a_init\n",
                "bb_err_t bb_fake_a_init(void) {\n"
                "    return BB_OK;\n"
                "}\n")
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", manifest):
                violations = _check_binds_data_hidden_bind(make_ctx(td))
            self.assertFalse(
                violations,
                "a manifest entry for a component absent from this root"
                " must not be flagged stale: " + str(violations))

    def test_wired_into_lint_run_entry_point(self):
        """Proof this rule actually runs through the real `bbtool lint`
        entry point (`run()`), not merely reachable if called directly.
        Filters to just this rule id and asserts a non-zero exit over a
        tree with a genuine undocumented reachable indirect bind."""
        with tempfile.TemporaryDirectory() as td:
            self._make_indirect_fixture(td)
            with mock.patch("commands.lint.INDIRECT_BB_DATA_BINDS", ()):
                args = argparse.Namespace(
                    root=td,
                    profile=None,
                    rules=["binds-data-hidden-bind"],
                    list=False,
                    _config_dict={},
                    _root_abs=td,
                )
                rc = lint_run(args)
            self.assertEqual(
                rc, 1,
                "bbtool lint's real run() entry point must surface a"
                " binds-data-hidden-bind violation as a non-zero exit")


class TestKconfigInertSymbol(unittest.TestCase):
    def _make_file(self, tmpdir: str, relpath: str, content: str) -> str:
        path = Path(tmpdir) / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return tmpdir

    def _names(self, violations) -> set:
        return {v["detail"].split()[1] for v in violations}

    def test_fires_on_genuinely_inert_symbol(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_INERT\n'
                '    bool "Inert fake symbol"\n'
                '    default n\n'
                '    help\n'
                '        Nothing anywhere consults this knob.\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertIn("BB_FAKE_INERT", self._names(violations))

    def test_comment_only_reference_still_fires(self):
        """A CONFIG_<SYM> token sitting only inside a C comment must NOT
        exempt the symbol -- comments are stripped before the scan, so a
        stale/aspirational comment describing a consumer that was never
        actually built (the real in-tree BB_STORAGE_ENTRY_LIST_CAP case)
        still gets flagged."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_INERT\n'
                '    int "Inert fake symbol"\n'
                '    default 32\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                '// downstream code reads CONFIG_BB_FAKE_INERT out of'
                ' sdkconfig.h\n'
                'int x;\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertIn("BB_FAKE_INERT", self._names(violations))

    def test_no_fire_on_ifdef_usage(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_USED\n'
                '    bool "Used fake symbol"\n'
                '    default n\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                '#ifdef CONFIG_BB_FAKE_USED\n'
                'int x;\n'
                '#endif\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_USED", self._names(violations))

    def test_no_fire_on_if_defined_usage(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_USED\n'
                '    bool "Used fake symbol"\n'
                '    default n\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                '#if defined(CONFIG_BB_FAKE_USED) && CONFIG_BB_FAKE_USED\n'
                'int x;\n'
                '#endif\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_USED", self._names(violations))

    def test_no_fire_on_is_enabled_usage(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_USED\n'
                '    bool "Used fake symbol"\n'
                '    default n\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '    if (IS_ENABLED(CONFIG_BB_FAKE_USED)) { return; }\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_USED", self._names(violations))

    def test_no_fire_on_cmake_if_usage(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_USED\n'
                '    bool "Used fake symbol"\n'
                '    default n\n')
            self._make_file(td, "components/bb_fake/CMakeLists.txt",
                'if(CONFIG_BB_FAKE_USED)\n'
                '    list(APPEND SRCS "extra.c")\n'
                'endif()\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_USED", self._names(violations))

    def test_no_fire_on_python_tooling_usage(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_USED\n'
                '    int "Used fake symbol"\n'
                '    default 4\n')
            self._make_file(td, "scripts/bbtool/commands/fake_tool.py",
                'def f():\n'
                '    return CONFIG_BB_FAKE_USED\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_USED", self._names(violations))

    def test_python_comment_only_usage_still_fires(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_INERT\n'
                '    int "Inert fake symbol"\n'
                '    default 4\n')
            self._make_file(td, "scripts/bbtool/commands/fake_tool.py",
                '# reads CONFIG_BB_FAKE_INERT eventually\n'
                'def f():\n'
                '    return 1\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertIn("BB_FAKE_INERT", self._names(violations))

    def test_no_fire_on_kconfig_depends_on_reference(self):
        """A symbol named only in ANOTHER Kconfig entry's `depends on` is a
        real usage -- it changes that entry's own visibility/gating, a
        deliberate scope decision (mirrors the real
        BB_HTTP_TLS_ENABLE/bb_tls_creds `depends on` case)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_GATE\n'
                '    bool "Gate"\n'
                '    default n\n'
                '\n'
                'config BB_FAKE_GATED\n'
                '    bool "Gated"\n'
                '    default n\n'
                '    depends on BB_FAKE_GATE\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_GATE", self._names(violations))

    def test_no_fire_on_kconfig_select_reference(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_TARGET\n'
                '    bool "Target"\n'
                '    default n\n'
                '\n'
                'config BB_FAKE_SELECTOR\n'
                '    bool "Selector"\n'
                '    default y\n'
                '    select BB_FAKE_TARGET\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_TARGET", self._names(violations))

    def test_no_fire_on_choice_default_reference(self):
        """Mirrors the real BB_WIFI_PS_MIN_MODEM case: a choice's own
        `default <member>` line naming one of its members bare is a real
        `default` directive reference, not prose."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    default BB_FAKE_MODE_A\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                'endchoice\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_MODE_A", self._names(violations))

    def test_no_fire_on_else_arm_choice_member_usage(self):
        """Reproduces the reviewer's minimal repro (mirrors the real
        BB_WIFI_PS_MIN_MODEM/bb_wifi.c:838-843 case, which escapes only
        because that choice happens to carry a `default` naming it): a
        `choice` with members A/B and NO `default`, where a C `#if`/
        `#else` chain names only member A explicitly and consults member
        B purely via the `#else` fallback -- no literal
        CONFIG_BB_FAKE_MODE_B token ever appears anywhere. The `#else`
        arm IS a real consultation of B (choice members are mutually
        exclusive, so `#else` here means "B is selected"); B must NOT be
        flagged as inert."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#if CONFIG_BB_FAKE_MODE_A\n'
                '    mode_a();\n'
                '#else\n'
                '    mode_b();  // consults B via the sole remaining choice member\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_MODE_B", self._names(violations))

    def test_ifndef_else_arm_does_not_rescue_choice_member(self):
        """Polarity inversion guard: `#ifndef CONFIG_BB_FAKE_MODE_A` /
        `#else` fires when A IS selected, not when it isn't -- it never
        consults B, unlike the positive `#if CONFIG_BB_FAKE_MODE_A` /
        `#else` case in test_no_fire_on_else_arm_choice_member_usage. B
        must still be reported inert."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#ifndef CONFIG_BB_FAKE_MODE_A\n'
                '    mode_a_absent();\n'
                '#else\n'
                '    mode_a_present();\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertIn("BB_FAKE_MODE_B", self._names(violations))

    def test_negated_if_else_arm_does_not_rescue_choice_member(self):
        """Same polarity guard, `#if !CONFIG_X` form -- `#else` fires when
        A IS selected, so B is never actually consulted."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#if !CONFIG_BB_FAKE_MODE_A\n'
                '    mode_a_absent();\n'
                '#else\n'
                '    mode_a_present();\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertIn("BB_FAKE_MODE_B", self._names(violations))

    def test_positive_if_else_arm_still_rescues_choice_member(self):
        """Regression guard for the polarity fix: a genuinely positive
        `#if CONFIG_X` / `#else` chain must keep rescuing its sibling
        (duplicate of test_no_fire_on_else_arm_choice_member_usage, named
        explicitly here alongside its ifndef/negated-if counterparts so
        the three polarity cases read together)."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#if CONFIG_BB_FAKE_MODE_A\n'
                '    mode_a();\n'
                '#else\n'
                '    mode_b();\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_MODE_B", self._names(violations))

    def test_backslash_continued_negation_defeats_rescue(self):
        """A `#if`/`#elif` condition split across physical lines via a
        trailing `\\` must be joined into ONE logical line before polarity
        classification -- a `!CONFIG_X` negation sitting only on the
        continuation line must NOT be invisible to `_preproc_arm_is_positive`.

        3-member choice: MODE_A is named positively on the opener, MODE_B
        is negated on the continuation, MODE_C is never named anywhere.
        MODE_B itself is a real reference either way (its raw `CONFIG_`
        token is textually present in the file, so `_collect_c_config_refs`
        -- a separate, whole-text scan unaffected by this bug -- always
        marks it live regardless of the frame's polarity); the bug's
        actual observable effect on this rule's OUTPUT is entirely on
        MODE_C, the sibling that is never named anywhere and can ONLY be
        marked live via `_collect_else_arm_choice_refs`'s (possibly
        mis-polarized) rescue. Before the continuation-join fix, only the
        opener line `#if CONFIG_BB_FAKE_MODE_A && \\` was scanned; its lone
        token (MODE_A) was positive, so the frame was wrongly treated as
        all-positive and the `#else` arm rescued MODE_C even though the
        joined condition `CONFIG_BB_FAKE_MODE_A && !CONFIG_BB_FAKE_MODE_B`
        is actually negative (MODE_B's negation, invisible pre-fix). After
        the fix, the rescue never fires and MODE_C must still be reported
        inert."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                '    config BB_FAKE_MODE_C\n'
                '        bool "C"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#if CONFIG_BB_FAKE_MODE_A && \\\n'
                '    !CONFIG_BB_FAKE_MODE_B\n'
                '    do_a();\n'
                '#else\n'
                '    do_other();\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            names = self._names(violations)
            self.assertIn(
                "BB_FAKE_MODE_C", names,
                "the never-named third choice member must still be"
                " reported inert -- the #else rescue must not fire for a"
                " frame whose polarity is negative only on its"
                " backslash-continuation line")

    def test_backslash_continued_positive_condition_still_rescues(self):
        """Regression guard for the join itself: a POSITIVE condition split
        across physical lines via `\\` must still rescue the remaining
        choice member -- proves the continuation-join didn't break the
        positive path it's meant to preserve."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_A\n'
                '        bool "A"\n'
                '    config BB_FAKE_MODE_B\n'
                '        bool "B"\n'
                '    config BB_FAKE_MODE_C\n'
                '        bool "C"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#if CONFIG_BB_FAKE_MODE_A || \\\n'
                '    CONFIG_BB_FAKE_MODE_B\n'
                '    do_ab();\n'
                '#else\n'
                '    do_other();  // consults C via the sole remaining choice member\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertNotIn("BB_FAKE_MODE_C", self._names(violations))

    def test_three_member_choice_else_arm_rescues_all_remaining(self):
        """Locks down the deliberate over-broadening for a 3+-member
        choice: naming only ONE member before a positive `#else` rescues
        ALL remaining members (no attempt to disambiguate WHICH one) --
        see `_collect_else_arm_choice_refs` docstring. Both C and D must
        be rescued here, not just one."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'choice BB_FAKE_MODE\n'
                '    prompt "Mode"\n'
                '    config BB_FAKE_MODE_C\n'
                '        bool "C"\n'
                '    config BB_FAKE_MODE_D\n'
                '        bool "D"\n'
                '    config BB_FAKE_MODE_E\n'
                '        bool "E"\n'
                'endchoice\n')
            self._make_file(td, "components/bb_fake/bb_fake.c",
                'void f(void) {\n'
                '#if CONFIG_BB_FAKE_MODE_C\n'
                '    mode_c();\n'
                '#else\n'
                '    mode_other();\n'
                '#endif\n'
                '}\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            names = self._names(violations)
            self.assertNotIn("BB_FAKE_MODE_D", names)
            self.assertNotIn("BB_FAKE_MODE_E", names)

    def test_help_prose_reference_does_not_count_as_real_usage(self):
        """A bare symbol name mentioned only in ANOTHER entry's help-text
        PROSE (not a real depends on/select/default directive) must NOT
        exempt it -- the help-block stripper must blank that prose before
        the depends-on/select/default scan runs over it."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_INERT\n'
                '    bool "Inert fake symbol"\n'
                '    default n\n'
                '\n'
                'config BB_FAKE_OTHER\n'
                '    bool "Other"\n'
                '    default n\n'
                '    help\n'
                '        Mentions BB_FAKE_INERT here by name, in prose only,\n'
                '\n'
                '        across a blank line, never as a real directive.\n')
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertIn("BB_FAKE_INERT", self._names(violations))

    def test_allowlist_suppresses_violation(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_INERT\n'
                '    bool "Inert fake symbol"\n'
                '    default n\n')
            ctx = Context(root=td, config={
                "lint": {"rules": {"kconfig-inert-symbol": {
                    "allow": ["BB_FAKE_INERT"],
                }}},
            })
            violations = _check_kconfig_inert_symbol(ctx)
            self.assertFalse(violations,
                "allowlisted symbol must not fire despite being inert")

    def test_no_kconfig_files_short_circuits(self):
        with tempfile.TemporaryDirectory() as td:
            violations = _check_kconfig_inert_symbol(make_ctx(td))
            self.assertFalse(violations)

    def test_wired_into_lint_run_entry_point(self):
        """Proof this rule actually runs through the real `bbtool lint`
        entry point (`run()`), not merely reachable if called directly.
        Severity is overridden to 'error' for this test only -- the rule's
        real shipped default is 'warn' (never fails `make check` on its
        own), so proving the wiring needs a config override to observe a
        non-zero exit."""
        with tempfile.TemporaryDirectory() as td:
            self._make_file(td, "components/bb_fake/Kconfig",
                'config BB_FAKE_INERT\n'
                '    bool "Inert fake symbol"\n'
                '    default n\n')
            args = argparse.Namespace(
                root=td,
                profile=None,
                rules=["kconfig-inert-symbol"],
                list=False,
                _config_dict={
                    "lint": {"rules": {"kconfig-inert-symbol": {
                        "severity": "error",
                    }}},
                },
                _root_abs=td,
            )
            rc = lint_run(args)
            self.assertEqual(
                rc, 1,
                "bbtool lint's real run() entry point must surface a"
                " kconfig-inert-symbol violation as a non-zero exit"
                " when the rule's severity is escalated to error")


if __name__ == "__main__":
    unittest.main()
