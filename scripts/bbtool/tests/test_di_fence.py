"""di-fence command tests: marker scanning + baseline ratchet diff logic."""
import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.di_fence import (
    Marker,
    scan_all,
    baseline_path,
    load_baseline,
    write_baseline,
    diff,
    counts_by_bucket,
    add_arguments,
    run,
)


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class TestScanInitRegister(unittest.TestCase):
    def test_finds_regular_and_variant_invocations(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER_EARLY(bb_fake_early, bb_fake_early_init);\n"
                "BB_INIT_REGISTER_N(bb_fake_ordered, bb_fake_init, 4);\n"
            ))
            found = scan_all(str(root))
            self.assertIn(Marker("BB_INIT_REGISTER", "components/bb_fake/src/bb_fake.c", "bb_fake"), found)
            self.assertIn(
                Marker("BB_INIT_REGISTER_EARLY", "components/bb_fake/src/bb_fake.c", "bb_fake_early"),
                found,
            )
            self.assertIn(
                Marker("BB_INIT_REGISTER_N", "components/bb_fake/src/bb_fake.c", "bb_fake_ordered"),
                found,
            )

    def test_macro_definition_file_not_scanned_as_invocation(self):
        # Mirrors components/bb_init/include/bb_init.h: the #define line
        # itself must not be mistaken for a call site.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_init/include/bb_init.h", (
                "#define BB_INIT_REGISTER_N(name_, fn_, n_) \\\n"
                "    void bb_init_register__##name_(void) {}\n"
                "#define BB_INIT_REGISTER(name_, fn_) BB_INIT_REGISTER_N(name_, fn_, 0)\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())

    def test_comment_reference_not_counted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", (
                "// see BB_INIT_REGISTER(bb_fake, bb_fake_init) for details\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestScanAutoregisterAndAutoAttach(unittest.TestCase):
    def test_kconfig_def_and_usage(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/Kconfig", (
                "config BB_FAKE_AUTOREGISTER\n"
                "    bool \"enable\"\n"
                "    default y\n"
            ))
            _write(root, "platform/espidf/bb_fake/bb_fake.c", (
                "#if CONFIG_BB_FAKE_AUTOREGISTER\n"
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "#endif\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("autoregister_kconfig", "components/bb_fake/Kconfig", "BB_FAKE_AUTOREGISTER"),
                found,
            )
            self.assertIn(
                Marker(
                    "autoregister_usage",
                    "platform/espidf/bb_fake/bb_fake.c",
                    "BB_FAKE_AUTOREGISTER",
                ),
                found,
            )

    def test_auto_attach_kconfig_and_usage(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/Kconfig", "config BB_FAKE_AUTO_ATTACH\n    bool\n")
            _write(root, "platform/espidf/bb_fake/bb_fake.c", "#if CONFIG_BB_FAKE_AUTO_ATTACH\n#endif\n")
            found = scan_all(str(root))
            self.assertIn(
                Marker("auto_attach_kconfig", "components/bb_fake/Kconfig", "BB_FAKE_AUTO_ATTACH"),
                found,
            )
            self.assertIn(
                Marker("auto_attach_usage", "platform/espidf/bb_fake/bb_fake.c", "BB_FAKE_AUTO_ATTACH"),
                found,
            )


class TestScanPubSink(unittest.TestCase):
    def test_finds_type_and_add_sink(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/host/bb_fake/bb_fake.c", (
                "bb_pub_sink_t s;\n"
                "bb_pub_add_sink(&s);\n"
            ))
            found = scan_all(str(root))
            self.assertIn(Marker("pub_sink", "platform/host/bb_fake/bb_fake.c", "bb_pub_sink_t"), found)
            self.assertIn(Marker("pub_sink", "platform/host/bb_fake/bb_fake.c", "bb_pub_add_sink"), found)


class TestScanDisplayForceKeep(unittest.TestCase):
    def test_macro_and_linker_flag(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "platform/espidf/bb_display_fake/bb_display_fake.c", (
                'BB_DISPLAY_AUTOREGISTER(fake, CONFIG_BB_DISPLAY_FAKE_AUTOREGISTER, &s_backend)\n'
            ))
            _write(root, "components/bb_display_fake/CMakeLists.txt", (
                'target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bb_display_register__fake")\n'
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker(
                    "display_force_keep",
                    "platform/espidf/bb_display_fake/bb_display_fake.c",
                    "macro:fake",
                ),
                found,
            )
            self.assertIn(
                Marker(
                    "display_force_keep",
                    "components/bb_display_fake/CMakeLists.txt",
                    "linker:fake",
                ),
                found,
            )


class TestScanLinkerForceRegister(unittest.TestCase):
    def test_helper_calls_all_tiers(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/CMakeLists.txt", (
                "bb_init_force_register(${COMPONENT_LIB} bb_fake)\n"
                "bb_init_force_register_early(${COMPONENT_LIB} bb_fake_early)\n"
                "bb_init_force_register_pre_http(${COMPONENT_LIB} bb_fake_pre)\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("linker_force_register", "components/bb_fake/CMakeLists.txt", "regular:bb_fake"),
                found,
            )
            self.assertIn(
                Marker("linker_force_register", "components/bb_fake/CMakeLists.txt", "early:bb_fake_early"),
                found,
            )
            self.assertIn(
                Marker(
                    "linker_force_register",
                    "components/bb_fake/CMakeLists.txt",
                    "pre_http:bb_fake_pre",
                ),
                found,
            )


class TestExcludedDirs(unittest.TestCase):
    def test_build_and_test_dirs_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/build/generated.c", "BB_INIT_REGISTER(x, y);\n")
            _write(root, "components/bb_fake/test/test_fake.c", "BB_INIT_REGISTER(x, y);\n")
            found = scan_all(str(root))
            self.assertEqual(found, set())


class TestBaselineRoundtrip(unittest.TestCase):
    def test_write_then_load_roundtrips(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            markers = {
                Marker("BB_INIT_REGISTER", "components/bb_fake/src/bb_fake.c", "bb_fake"),
                Marker("pub_sink", "platform/host/bb_fake/bb_fake.c", "bb_pub_sink_t"),
            }
            path = write_baseline(str(root), markers)
            self.assertTrue(path.is_file())
            self.assertEqual(path, baseline_path(str(root)))
            loaded = load_baseline(str(root))
            self.assertEqual(loaded, markers)

    def test_missing_baseline_loads_empty(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(load_baseline(td), set())

    def test_write_is_deterministic(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            markers = {
                Marker("pub_sink", "b.c", "bb_pub_sink_t"),
                Marker("pub_sink", "a.c", "bb_pub_sink_t"),
            }
            path = write_baseline(str(root), markers)
            snapshot = path.read_text(encoding="utf-8")
            write_baseline(str(root), markers)
            self.assertEqual(path.read_text(encoding="utf-8"), snapshot)


class TestDiff(unittest.TestCase):
    def test_no_change_yields_empty_diff(self):
        markers = {Marker("pub_sink", "a.c", "bb_pub_sink_t")}
        new, removed = diff(markers, markers)
        self.assertEqual(new, [])
        self.assertEqual(removed, [])

    def test_new_marker_detected(self):
        baseline = {Marker("pub_sink", "a.c", "bb_pub_sink_t")}
        current = baseline | {Marker("pub_sink", "b.c", "bb_pub_add_sink")}
        new, removed = diff(current, baseline)
        self.assertEqual(new, [Marker("pub_sink", "b.c", "bb_pub_add_sink")])
        self.assertEqual(removed, [])

    def test_removed_marker_reported_not_failed(self):
        baseline = {
            Marker("pub_sink", "a.c", "bb_pub_sink_t"),
            Marker("pub_sink", "b.c", "bb_pub_add_sink"),
        }
        current = {Marker("pub_sink", "a.c", "bb_pub_sink_t")}
        new, removed = diff(current, baseline)
        self.assertEqual(new, [])
        self.assertEqual(removed, [Marker("pub_sink", "b.c", "bb_pub_add_sink")])


class TestCountsByBucket(unittest.TestCase):
    def test_init_register_variants_collapse_to_one_bucket(self):
        markers = {
            Marker("BB_INIT_REGISTER", "a.c", "x"),
            Marker("BB_INIT_REGISTER_EARLY", "b.c", "y"),
        }
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"BB_INIT_REGISTER family": 2})


def _run_cli(root: str, update_baseline: bool = False) -> tuple:
    args = argparse.Namespace(root=root, update_baseline=update_baseline)
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = run(args)
    return rc, stdout.getvalue(), stderr.getvalue()


class TestRunCli(unittest.TestCase):
    def test_update_baseline_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")

            rc, out, _ = _run_cli(str(root), update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline updated", out)
            self.assertTrue(baseline_path(str(root)).is_file())

            rc2, out2, _ = _run_cli(str(root))
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_marker_fails_with_actionable_message(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n")
            _run_cli(str(root), update_baseline=True)

            # Simulate a new call site being added after the baseline was cut.
            _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_new, bb_fake_new_init);\n"
            ))

            rc, out, err = _run_cli(str(root))
            self.assertEqual(rc, 1)
            self.assertIn("new legacy DI marker added", err)
            self.assertIn("bb_fake_new", err)
            self.assertIn("--update-baseline", err)

    def test_rename_of_marker_file_does_not_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            old_path = _write(root, "components/bb_fake/src/bb_fake_old.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
            ))
            _run_cli(str(root), update_baseline=True)

            # Simulate a pure file rename: same symbol, new filename.
            old_path.unlink()
            _write(root, "components/bb_fake/src/bb_fake_renamed.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
            ))

            rc, out, _ = _run_cli(str(root))
            self.assertEqual(rc, 0, "a pure file rename must never fail the fence")
            self.assertIn("PASS", out)
            self.assertNotIn("candidate to prune", out)

    def test_removed_marker_passes_and_reports_prune_candidate(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/src/bb_fake.c", (
                "BB_INIT_REGISTER(bb_fake, bb_fake_init);\n"
                "BB_INIT_REGISTER(bb_fake_gone, bb_fake_gone_init);\n"
            ))
            _run_cli(str(root), update_baseline=True)

            # Simulate removing one of the two call sites (a legitimate shrink).
            src.write_text("BB_INIT_REGISTER(bb_fake, bb_fake_init);\n", encoding="utf-8")

            rc, out, _ = _run_cli(str(root))
            self.assertEqual(rc, 0, "removals must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)
            self.assertIn("bb_fake_gone", out)


class TestAddArguments(unittest.TestCase):
    def test_parses_update_baseline_flag(self):
        parser = argparse.ArgumentParser()
        add_arguments(parser)
        ns = parser.parse_args(["--update-baseline"])
        self.assertTrue(ns.update_baseline)
        ns2 = parser.parse_args([])
        self.assertFalse(ns2.update_baseline)


if __name__ == "__main__":
    unittest.main()
