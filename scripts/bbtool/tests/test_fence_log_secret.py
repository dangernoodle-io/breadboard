"""fence.log_secret family tests: repo-wide secret-shaped-log-call scanning
(fires + does not fire), family auto-discovery, and the shrink-only
baseline semantics (via the generic `fence` CLI) applied to this concrete
family. This is the real guard behind the per-file smoke test in
test/test_host/test_bb_log_secret.c -- the reviewer-proved bypass (a
second, differently-worded raw log line) must fire here."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

import fence as fence_pkg  # noqa: E402
from fence import Marker  # noqa: E402
from fence.log_secret import scan_all, counts_by_bucket  # noqa: E402
from fence_test_support import run_fence_cli  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    _ensure_component_cmakelists(root, rel)
    return path


def _ensure_component_cmakelists(root: Path, rel: str) -> None:
    """See test_fence_sat_sub.py's twin helper for the full rationale."""
    parts = Path(rel).parts
    if len(parts) < 3 or parts[0] != "components":
        return
    cmake = root / "components" / parts[1] / "CMakeLists.txt"
    if not cmake.is_file():
        cmake.parent.mkdir(parents=True, exist_ok=True)
        cmake.write_text("idf_component_register()\n", encoding="utf-8")


class TestFamilyDiscovery(unittest.TestCase):
    def test_log_secret_is_discovered(self):
        self.assertIn("log_secret", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "log_secret")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/log_secret.json"))


class TestScanFiresOnLabelToken(unittest.TestCase):
    def test_password_equals_label_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *p)\n"
                "{\n"
                "    bb_log_i(TAG, \"password=%s\", p);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("log_secret_leak", "platform/host/bb_fake/bb_fake.c", "bb_fake:f:password"),
                found,
            )

    def test_reviewer_mutant_differently_worded_label_fires(self):
        # The exact bypass the reviewer proved live against the old
        # per-file smoke test: a differently-worded raw password log line.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/espidf/bb_wifi_ap/bb_wifi_ap.c", (
                "static char s_ap_password[64] = \"breadboard\";\n"
                "static void bb_wifi_ap_start(void)\n"
                "{\n"
                "    bb_log_i(TAG, \"AP pass: %s\", s_ap_password);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "log_secret_leak" and "bb_wifi_ap_start" in m.id for m in found),
                found,
            )

    def test_esp_log_e_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *psk)\n"
                "{\n"
                "    ESP_LOGE(TAG, \"psk: %s\", psk);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(any(m.type == "log_secret_leak" for m in found))

    def test_printf_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *token)\n"
                "{\n"
                "    printf(\"token=%s\\n\", token);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(any(m.type == "log_secret_leak" for m in found))

    def test_fprintf_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *bearer)\n"
                "{\n"
                "    fprintf(stderr, \"bearer=%s\\n\", bearer);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(any(m.type == "log_secret_leak" for m in found))


class TestScanFiresOnModifiedConversion(unittest.TestCase):
    """The scan must match the whole `%[flags][width][.precision]s`
    conversion family, not only a literal `"%s"` substring -- a
    width/precision-modified conversion is exactly as much of a leak."""

    def test_star_precision_conversion_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(int n, const char *pw)\n"
                "{\n"
                "    bb_log_i(TAG, \"password=%.*s\", n, pw);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "log_secret_leak" for m in found),
                found,
            )

    def test_fixed_precision_conversion_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *pw)\n"
                "{\n"
                "    bb_log_i(TAG, \"password=%.8s\", pw);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "log_secret_leak" for m in found),
                found,
            )

    def test_width_flag_conversion_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *pw)\n"
                "{\n"
                "    bb_log_i(TAG, \"password=%-10s\", pw);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "log_secret_leak" for m in found),
                found,
            )

    def test_escaped_percent_s_does_not_fire(self):
        # "%%s" is a literal "%s" (escaped percent + literal char 's'), not
        # a conversion at all -- must not be mistaken for a real `%s`.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *pw)\n"
                "{\n"
                "    bb_log_i(TAG, \"password=%%s literal\", pw);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestScanFiresOnArgumentIdentifier(unittest.TestCase):
    def test_secret_shaped_arg_name_fires_even_with_generic_label(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *s_ap_password)\n"
                "{\n"
                "    bb_log_i(TAG, \"value: %s\", s_ap_password);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(any(m.type == "log_secret_leak" for m in found))

    def test_credential_arg_name_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *api_key)\n"
                "{\n"
                "    bb_log_w(TAG, \"got: %s\", api_key);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(any(m.type == "log_secret_leak" for m in found))


class TestScanDoesNotFire(unittest.TestCase):
    def test_bb_log_secret_call_itself_never_fires(self):
        # The sanctioned helper's own implementation: label/value never
        # match the secret-name pattern.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_log/src/bb_log_secret.c", (
                "void bb_log_secret(int level, const char *tag, const char *label, const char *secret)\n"
                "{\n"
                "    const char *value = secret ? \"***\" : \"(unset)\";\n"
                "    bb_log_i(tag, \"%s=%s\", label, value);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_ssid_label_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *ssid)\n"
                "{\n"
                "    bb_log_i(TAG, \"AP started: SSID=%s\", ssid);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_no_percent_s_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(int password_len)\n"
                "{\n"
                "    bb_log_i(TAG, \"password length: %d\", password_len);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_nested_call_arg_does_not_confuse_paren_balance(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(int err)\n"
                "{\n"
                "    bb_log_e(TAG, \"failed: %s\", esp_err_to_name(err));\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_multiline_call_with_label_not_before_percent_s_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(int err)\n"
                "{\n"
                "    bb_log_w(TAG, \"scan still running; sent stop() (%s)\",\n"
                "             esp_err_to_name(err));\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_commented_out_reference_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *p)\n"
                "{\n"
                "    // bb_log_i(TAG, \"password=%s\", p);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_block_commented_reference_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *p)\n"
                "{\n"
                "    /* bb_log_i(TAG, \"password=%s\", p); */\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_unrelated_identifier_containing_no_word_boundary_does_not_fire(self):
        # "passed" has no underscore boundary around "pass" -- must not
        # be mistaken for the secret-shaped subword.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "static void f(const char *passed)\n"
                "{\n"
                "    bb_log_i(TAG, \"state: %s\", passed);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_excluded_test_dir_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/test/test_fake.c", (
                "static void f(const char *p)\n"
                "{\n"
                "    bb_log_i(TAG, \"password=%s\", p);\n"
                "}\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("log_secret_leak", "a.c", "bb_fake:f:password")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"unmasked secret-shaped log call": 1})


class TestFenceCliLogSecret(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real log_secret family scanner, on a synthetic tree."""

    def _leak_src(self, var="password"):
        return (
            f"static void f(const char *{var})\n"
            "{\n"
            f"    bb_log_i(TAG, \"{var}=%s\", {var});\n"
            "}\n"
        )

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", self._leak_src())

            rc, out, _ = run_fence_cli(str(root), seed="log_secret")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["log_secret"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_synthetic_site_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", "// nothing here\n")
            run_fence_cli(str(root), seed="log_secret")

            _write(root, "platform/host/bb_fake/bb_fake_new.c", self._leak_src())

            rc, out, err = run_fence_cli(str(root), family=["log_secret"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("bb_fake_new.c", err)

    def test_update_baseline_does_not_bless_new_site(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", self._leak_src())
            run_fence_cli(str(root), seed="log_secret")

            # Simultaneously: remove the seeded site AND add a new one.
            src.write_text(self._leak_src("token"), encoding="utf-8")

            rc, out, _ = run_fence_cli(str(root), family=["log_secret"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            rc2, _, err2 = run_fence_cli(str(root), family=["log_secret"])
            self.assertEqual(rc2, 1)
            self.assertIn("bb_fake.c", err2)

    def test_removed_site_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "platform/host/bb_fake/bb_fake.c", self._leak_src())
            run_fence_cli(str(root), seed="log_secret")

            src.write_text(
                "static void f(const char *label, const char *value)\n"
                "{\n"
                "    bb_log_secret(BB_LOG_LEVEL_INFO, TAG, label, value);\n"
                "}\n",
                encoding="utf-8",
            )

            rc, out, _ = run_fence_cli(str(root), family=["log_secret"])
            self.assertEqual(rc, 0, "removing a leak site must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["log_secret"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "log_secret")
            self.assertEqual(baseline, set())


if __name__ == "__main__":
    unittest.main()
