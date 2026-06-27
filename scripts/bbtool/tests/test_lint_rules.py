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
    _check_public_requires_watchlist,
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


class TestPublicRequiresWatchlist(unittest.TestCase):
    def _make_cmake(self, tmpdir: str, comp: str, content: str) -> str:
        d = os.path.join(tmpdir, "components", comp)
        os.makedirs(d, exist_ok=True)
        Path(os.path.join(d, "CMakeLists.txt")).write_text(content)
        return tmpdir

    def test_fires_on_non_allowlisted_watchlist_dep(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_fake",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_lcd\n)\n')
            violations = _check_public_requires_watchlist(make_ctx(td))
            self.assertTrue(violations, "expected violation on watchlist dep in REQUIRES")

    def test_no_fire_on_priv_requires(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_fake",
                'idf_component_register(\n    SRCS "fake.c"\n    PRIV_REQUIRES bb_core esp_lcd\n)\n')
            violations = _check_public_requires_watchlist(make_ctx(td))
            self.assertFalse(violations, "PRIV_REQUIRES must NOT fire")

    def test_allowlist_bb_display_ssd1306_esp_driver_i2c(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_display_ssd1306",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core bb_display esp_driver_i2c\n)\n')
            violations = _check_public_requires_watchlist(make_ctx(td))
            self.assertFalse(violations, "bb_display_ssd1306 / esp_driver_i2c must be allowlisted")

    def test_fires_bb_fake_i2c_esp_driver_i2c(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_fake_i2c",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core esp_driver_i2c\n)\n')
            violations = _check_public_requires_watchlist(make_ctx(td))
            self.assertTrue(violations, "non-allowlisted component with esp_driver_i2c must fire")

    def test_allowlist_bb_display_ek79007_esp_lvgl_port(self):
        with tempfile.TemporaryDirectory() as td:
            self._make_cmake(td, "bb_display_ek79007",
                'idf_component_register(\n    SRCS "fake.c"\n    REQUIRES bb_core bb_display esp_lvgl_port\n)\n')
            violations = _check_public_requires_watchlist(make_ctx(td))
            self.assertFalse(violations, "bb_display_ek79007 / esp_lvgl_port must be allowlisted")


if __name__ == "__main__":
    unittest.main()
