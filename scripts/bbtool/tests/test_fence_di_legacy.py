"""fence.di_legacy family tests: marker scanning + identity override.

Mirrors the pre-generalization di-fence scanner tests — same markers, same
regexes — proving the migration into the fence/ package preserved detection
behavior exactly.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from fence import Marker  # noqa: E402
from fence.di_legacy import scan_all, counts_by_bucket, identity  # noqa: E402


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

    def test_kconfig_def_detected_when_indented_under_menu(self):
        # Regression: the scanner regex was previously column-0 anchored, so
        # a `config BB_*_AUTOREGISTER` indented under a `menu` block escaped
        # detection entirely (B1-724). Must still be found regardless of
        # leading whitespace.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/Kconfig", (
                'menu "BB Fake"\n'
                "\n"
                "    config BB_FAKE_AUTOREGISTER\n"
                "        bool \"enable\"\n"
                "        default y\n"
                "\n"
                "endmenu\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker("autoregister_kconfig", "components/bb_fake/Kconfig", "BB_FAKE_AUTOREGISTER"),
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

    def test_identity_is_path_insensitive_by_component(self):
        # Same literal id ("bb_pub_sink_t") in two different components must
        # not collide under identity — each component's occurrence is its
        # own ratchet entry.
        m1 = Marker("pub_sink", "components/bb_sink_a/bb_sink_a.c", "bb_pub_sink_t")
        m2 = Marker("pub_sink", "components/bb_sink_b/bb_sink_b.c", "bb_pub_sink_t")
        self.assertNotEqual(identity(m1), identity(m2))
        m3 = Marker("pub_sink", "platform/host/bb_sink_a/other_file.c", "bb_pub_sink_t")
        # Different filename, same owning directory name -> same identity
        # (rename-stable within a component).
        self.assertEqual(
            identity(Marker("pub_sink", "components/bb_sink_a/x.c", "bb_pub_sink_t")),
            identity(Marker("pub_sink", "components/bb_sink_a/y.c", "bb_pub_sink_t")),
        )
        self.assertIsNotNone(m3)

    def test_non_pub_sink_identity_is_type_and_id(self):
        m = Marker("BB_INIT_REGISTER", "components/bb_fake/bb_fake.c", "bb_fake")
        self.assertEqual(identity(m), ("BB_INIT_REGISTER", "bb_fake"))


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


class TestCountsByBucket(unittest.TestCase):
    def test_init_register_variants_collapse_to_one_bucket(self):
        markers = {
            Marker("BB_INIT_REGISTER", "a.c", "x"),
            Marker("BB_INIT_REGISTER_EARLY", "b.c", "y"),
        }
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"BB_INIT_REGISTER family": 2})


if __name__ == "__main__":
    unittest.main()
