"""fence.variant_ladder family tests (B1-1461): suffixed variant-ladder
scanning (fires + does not fire), the bb_http_server_get_handle-shaped
acceptance case, typed-accessor exclusion, public-vs-internal scope, and
the shrink-only baseline semantics (via the generic `fence` CLI) applied
to this concrete family."""
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
from fence.variant_ladder import scan_all, counts_by_bucket  # noqa: E402
from fence_test_support import run_fence_cli  # noqa: E402


def _write(root: Path, rel: str, content: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class TestFamilyDiscovery(unittest.TestCase):
    def test_variant_ladder_is_discovered(self):
        self.assertIn("variant_ladder", fence_pkg.FAMILIES)

    def test_baseline_path_is_per_family_convention(self):
        path = fence_pkg.baseline_path("/repo", "variant_ladder")
        self.assertEqual(path, Path("/repo/.baseline/bbtool/fence/variant_ladder.json"))


class TestScanFiresOnTrueLadder(unittest.TestCase):
    def test_base_plus_ex_sibling_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_register(const bb_fake_cfg_t *cfg);\n"
                "bb_err_t bb_fake_register_ex(const bb_fake_cfg_t *cfg, int extra);\n"
            ))
            found = scan_all(str(root))
            self.assertIn(
                Marker(
                    "variant_ladder_suffix",
                    "components/bb_fake/include/bb_fake.h",
                    "bb_fake:bb_fake_register->bb_fake_register_ex",
                ),
                found,
            )

    def test_base_plus_with_suffix_sibling_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_send(const char *msg);\n"
                "bb_err_t bb_fake_send_with_timeout(const char *msg, int timeout_ms);\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "variant_ladder_suffix" and "bb_fake_send_with_timeout" in m.id
                    for m in found),
                found,
            )

    def test_base_plus_named_sibling_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_lookup(int id);\n"
                "bb_err_t bb_fake_lookup_named(int id, const char *name);\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "variant_ladder_suffix" and "bb_fake_lookup_named" in m.id
                    for m in found),
                found,
            )

    def test_ex2_suffix_fires(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_open(int fd);\n"
                "bb_err_t bb_fake_open_ex2(int fd, int flags);\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "variant_ladder_suffix" and "bb_fake_open_ex2" in m.id
                    for m in found),
                found,
            )


class TestPointerReturnLadderFires(unittest.TestCase):
    """B1-1461 review finding: `_FN_DECL_RE` must see pointer-returning
    declarations, the dominant style for this class of accessor
    (`bb_task.h`, `bb_mem.h`, `bb_pool.h`, `bb_registry.h`, ...), where the
    `*` is attached directly to the function NAME rather than floating
    after the first return-type word."""

    def _assert_fires(self, decl_line: str, base_decl: str, expected_ex_name: str):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                f"{base_decl}\n"
                f"{decl_line}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "variant_ladder_suffix" and expected_ex_name in m.id
                    for m in found),
                (decl_line, found),
            )

    def test_void_star_attached_to_name_fires(self):
        self._assert_fires(
            "void *bb_fake_get_ex(void);",
            "void *bb_fake_get(void);",
            "bb_fake_get_ex",
        )

    def test_typed_star_attached_to_name_fires(self):
        self._assert_fires(
            "bb_fake_t *bb_fake_get_ex(void);",
            "bb_fake_t *bb_fake_get(void);",
            "bb_fake_get_ex",
        )

    def test_const_typed_star_attached_to_name_fires(self):
        self._assert_fires(
            "const bb_fake_t *bb_fake_get_ex(void);",
            "const bb_fake_t *bb_fake_get(void);",
            "bb_fake_get_ex",
        )

    def test_multiword_type_star_attached_to_name_fires(self):
        self._assert_fires(
            "unsigned char *bb_fake_get_ex(void);",
            "unsigned char *bb_fake_get(void);",
            "bb_fake_get_ex",
        )

    def test_star_with_space_before_name_fires(self):
        self._assert_fires(
            "bb_fake_t * bb_fake_get_ex(void);",
            "bb_fake_t * bb_fake_get(void);",
            "bb_fake_get_ex",
        )

    def test_non_pointer_return_still_fires(self):
        # Existing behaviour must be unaffected by the widened regex.
        self._assert_fires(
            "bb_err_t bb_fake_get_ex(void);",
            "bb_err_t bb_fake_get(void);",
            "bb_fake_get_ex",
        )


class TestPointerReturnWithNoBaseSiblingNotFlagged(unittest.TestCase):
    def test_pointer_return_ex_with_no_base_sibling_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "void *bb_fake_get_ex(void);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)


class TestAcceptanceHandleWithNoBaseSibling(unittest.TestCase):
    """The design's acceptance test: a `_handle` suffix alone does not
    decide it. bb_http_server_get_handle() is legitimate because there is
    no bb_http_server_get() sibling."""

    def test_get_handle_with_no_get_sibling_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_http_server/include/bb_http_server.h", (
                "#pragma once\n"
                "bb_http_handle_t bb_http_server_get_handle(void);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)

    def test_synthetic_suffix_with_no_base_sibling_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_lookup_ref(int id);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)


class TestTypedAccessorFamilyNeverFlagged(unittest.TestCase):
    def test_u8_u16_u32_family_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_set_u8(uint8_t v);\n"
                "bb_err_t bb_fake_set_u16(uint16_t v);\n"
                "bb_err_t bb_fake_set_u32(uint32_t v);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)

    def test_f32_i64_pair_does_not_fire(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "bb_err_t bb_fake_get_f32(float *out);\n"
                "bb_err_t bb_fake_get_i64(int64_t *out);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)


class TestStructEnumUnionReturnQualifierFires(unittest.TestCase):
    """B1-1461 review finding 1 (third round): `struct`/`enum`/`union` must
    disqualify a TYPE DEFINITION line, never a real declaration merely
    using one of those keywords as a return-type qualifier."""

    def _assert_fires(self, decl_line: str, base_decl: str, expected_ex_name: str):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                f"{base_decl}\n"
                f"{decl_line}\n"
            ))
            found = scan_all(str(root))
            self.assertTrue(
                any(m.type == "variant_ladder_suffix" and expected_ex_name in m.id
                    for m in found),
                (decl_line, found),
            )

    def test_struct_pointer_return_fires(self):
        self._assert_fires(
            "struct bb_fake *bb_fake_get_ex(void);",
            "struct bb_fake *bb_fake_get(void);",
            "bb_fake_get_ex",
        )

    def test_const_struct_pointer_return_fires(self):
        self._assert_fires(
            "const struct bb_fake *bb_fake_peek_ex(void);",
            "const struct bb_fake *bb_fake_peek(void);",
            "bb_fake_peek_ex",
        )

    def test_enum_value_return_fires(self):
        self._assert_fires(
            "enum bb_fake_state bb_fake_state_get_ex(void);",
            "enum bb_fake_state bb_fake_state_get(void);",
            "bb_fake_state_get_ex",
        )

    def test_union_pointer_return_fires(self):
        self._assert_fires(
            "union bb_fake_value *bb_fake_value_get_ex(void);",
            "union bb_fake_value *bb_fake_value_get(void);",
            "bb_fake_value_get_ex",
        )


class TestStructEnumUnionTypeDefinitionStillSkipped(unittest.TestCase):
    """The disambiguation must still skip genuine type definitions/forward
    declarations -- these must never be mistaken for a public function
    declaration, ladder or not."""

    def test_struct_definition_body_not_a_declaration(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "struct bb_fake {\n"
                "    int x;\n"
                "};\n"
                "struct bb_fake *bb_fake_get_ex(void);\n"
            ))
            found = scan_all(str(root))
            # No base sibling declared (only the struct body + one suffixed
            # decl), so nothing should fire -- proves the struct-body line
            # was never mistaken for `bb_fake_get`.
            self.assertEqual(found, set(), found)

    def test_struct_forward_decl_not_a_declaration(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "struct bb_fake;\n"
                "struct bb_fake *bb_fake_get_ex(void);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)

    def test_typedef_struct_not_a_declaration(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "typedef struct bb_fake bb_fake_t;\n"
                "bb_fake_t *bb_fake_get_ex(void);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)

    def test_enum_definition_body_not_a_declaration(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "enum bb_fake_state { BB_FAKE_A, BB_FAKE_B };\n"
                "enum bb_fake_state bb_fake_state_get_ex(void);\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)


class TestInternalStaticNeverFlagged(unittest.TestCase):
    def test_static_suffixed_sibling_in_c_file_does_not_fire(self):
        # Not scanned at all: components/*/src is outside the scan roots
        # (public API only is components/*/include/*.h).
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/src/bb_fake.c", (
                "static bb_err_t bb_fake_do(int x) { return BB_OK; }\n"
                "static bb_err_t bb_fake_do_ex(int x, int y) { return BB_OK; }\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)

    def test_static_declaration_in_public_header_does_not_fire(self):
        # A static inline pair declared right in the public header must
        # not count either -- `static`/`inline` are not real public API.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", (
                "#pragma once\n"
                "static inline bb_err_t bb_fake_do(int x) { return 0; }\n"
                "static inline bb_err_t bb_fake_do_ex(int x, int y) { return 0; }\n"
            ))
            found = scan_all(str(root))
            self.assertEqual(found, set(), found)


class TestCountsByBucket(unittest.TestCase):
    def test_bucket_labels(self):
        markers = {Marker("variant_ladder_suffix", "a.h", "bb_fake:bb_fake_x->bb_fake_x_ex")}
        counts = counts_by_bucket(markers)
        self.assertEqual(counts, {"suffixed variant-ladder sibling": 1})


class TestFenceCliVariantLadder(unittest.TestCase):
    """Exercises the generic `fence` CLI's shrink-only / net-new semantics
    against the real variant_ladder family scanner, on a synthetic tree."""

    def _ladder_src(self, suffix="ex"):
        return (
            "#pragma once\n"
            "bb_err_t bb_fake_register(const bb_fake_cfg_t *cfg);\n"
            f"bb_err_t bb_fake_register_{suffix}(const bb_fake_cfg_t *cfg, int extra);\n"
        )

    def test_seed_then_clean_run_passes(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", self._ladder_src())

            rc, out, _ = run_fence_cli(str(root), seed="variant_ladder")
            self.assertEqual(rc, 0)
            self.assertIn("baseline seeded", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["variant_ladder"])
            self.assertEqual(rc2, 0)
            self.assertIn("PASS", out2)

    def test_new_synthetic_ladder_fails(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write(root, "components/bb_fake/include/bb_fake.h", "#pragma once\n")
            run_fence_cli(str(root), seed="variant_ladder")

            _write(root, "components/bb_other/include/bb_other.h", self._ladder_src())

            rc, out, err = run_fence_cli(str(root), family=["variant_ladder"])
            self.assertEqual(rc, 1)
            self.assertIn("new marker added", err)
            self.assertIn("bb_other.h", err)

    def test_update_baseline_does_not_bless_new_ladder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/include/bb_fake.h", self._ladder_src("ex"))
            run_fence_cli(str(root), seed="variant_ladder")

            # Simultaneously: remove the seeded ladder AND add a new one
            # (different suffix) under the same identity family.
            src.write_text(self._ladder_src("named"), encoding="utf-8")

            rc, out, _ = run_fence_cli(str(root), family=["variant_ladder"], update_baseline=True)
            self.assertEqual(rc, 0)
            self.assertIn("baseline pruned", out)
            self.assertIn("NOT added to the", out)

            rc2, _, err2 = run_fence_cli(str(root), family=["variant_ladder"])
            self.assertEqual(rc2, 1)
            self.assertIn("bb_fake.h", err2)

    def test_removed_ladder_prunes_cleanly(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = _write(root, "components/bb_fake/include/bb_fake.h", self._ladder_src())
            run_fence_cli(str(root), seed="variant_ladder")

            # Collapse the ladder into a single cfg-struct entry point --
            # the sanctioned remedy.
            src.write_text(
                "#pragma once\n"
                "bb_err_t bb_fake_register(const bb_fake_cfg_t *cfg);\n",
                encoding="utf-8",
            )

            rc, out, _ = run_fence_cli(str(root), family=["variant_ladder"])
            self.assertEqual(rc, 0, "removing a ladder site must never fail the fence")
            self.assertIn("PASS", out)
            self.assertIn("candidate to prune from baseline", out)

            rc2, out2, _ = run_fence_cli(str(root), family=["variant_ladder"], update_baseline=True)
            self.assertEqual(rc2, 0)
            baseline = fence_pkg.load_baseline(str(root), "variant_ladder")
            self.assertEqual(baseline, set())


if __name__ == "__main__":
    unittest.main()
