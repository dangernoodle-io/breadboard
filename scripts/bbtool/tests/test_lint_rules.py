"""Per-rule fixture tests ported from check_lint_test.sh."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

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
    _strip_noise,
    _parse_kconfig_int_defaults,
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
        inc = os.path.join(tmpdir, "components", comp, "include")
        os.makedirs(inc, exist_ok=True)
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
        inc = os.path.join(tmpdir, "components", comp, "include")
        os.makedirs(inc, exist_ok=True)
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
        inc = os.path.join(tmpdir, "components", comp, "include")
        os.makedirs(inc, exist_ok=True)
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


class TestPragmaOnce(unittest.TestCase):
    def _make_header(self, tmpdir: str, comp: str, filename: str, content: str) -> str:
        inc = os.path.join(tmpdir, "components", comp, "include")
        os.makedirs(inc, exist_ok=True)
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
        inc = os.path.join(tmpdir, "components", comp, "include")
        os.makedirs(inc, exist_ok=True)
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

    def test_default_allowlist_bb_display_ssd1306_esp_driver_i2c(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_display_ssd1306",
                'idf_component_register(\n    SRCS "fake.c"\n'
                '    REQUIRES bb_core bb_display esp_driver_i2c\n)\n')
            self._make_header(td, "bb_display_ssd1306", "bb_display_ssd1306.h",
                '#pragma once\n#include "bb_core.h"\n#include "bb_display.h"\n')
            violations = _check_public_requires_unused(make_ctx(td))
            self.assertFalse(violations,
                "bb_display_ssd1306 / esp_driver_i2c must be allowlisted by default"
                " (carried over from the retired public-requires-watchlist rule)")

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


if __name__ == "__main__":
    unittest.main()
