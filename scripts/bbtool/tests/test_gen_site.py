"""gen-site command tests."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "commands"))

from commands.gen_site import (
    generate, _mime, _sanitize, _symbol, _url, _collect_files,
)


def _make_fixture(tmp_path: str) -> str:
    """Create a minimal SPA fixture under tmp_path/dist; return dist_dir."""
    dist = os.path.join(tmp_path, "dist")
    assets = os.path.join(dist, "assets")
    os.makedirs(assets)
    with open(os.path.join(dist, "index.html"), "w", encoding="utf-8") as f:
        f.write("<!doctype html><html><body>hello</body></html>\n")
    with open(os.path.join(assets, "app.js"), "w", encoding="utf-8") as f:
        f.write("console.log('ok');\n")
    with open(os.path.join(assets, "style.css"), "w", encoding="utf-8") as f:
        f.write("body{}\n")
    return dist


class TestMime(unittest.TestCase):
    def test_html(self):  self.assertEqual(_mime("index.html"), "text/html")
    def test_js(self):    self.assertEqual(_mime("app.js"), "application/javascript")
    def test_mjs(self):   self.assertEqual(_mime("mod.mjs"), "application/javascript")
    def test_css(self):   self.assertEqual(_mime("style.css"), "text/css")
    def test_svg(self):   self.assertEqual(_mime("logo.svg"), "image/svg+xml")
    def test_json(self):  self.assertEqual(_mime("manifest.json"), "application/json")
    def test_ico(self):   self.assertEqual(_mime("favicon.ico"), "image/x-icon")
    def test_png(self):   self.assertEqual(_mime("img.png"), "image/png")
    def test_woff2(self): self.assertEqual(_mime("font.woff2"), "font/woff2")
    def test_txt(self):   self.assertEqual(_mime("robots.txt"), "text/plain")
    def test_unknown(self): self.assertEqual(_mime("blob.bin"), "application/octet-stream")


class TestUrl(unittest.TestCase):
    def test_index_html_maps_to_root(self):
        self.assertEqual(_url("index.html", "/"), "/")

    def test_asset_js_maps_to_slash_relpath(self):
        self.assertEqual(_url("assets/app.js", "/"), "/assets/app.js")

    def test_url_prefix_prepended(self):
        self.assertEqual(_url("assets/app.js", "/ui"), "/ui/assets/app.js")

    def test_url_prefix_trailing_slash_stripped(self):
        self.assertEqual(_url("assets/app.js", "/ui/"), "/ui/assets/app.js")


class TestSymbol(unittest.TestCase):
    def test_sanitises_slash_and_dot(self):
        self.assertEqual(_symbol("my_site", "assets/app.js"), "my_site__assets_app_js")

    def test_all_alnum_unchanged(self):
        self.assertEqual(_symbol("s", "appjs"), "s__appjs")

    def test_valid_c_identifier(self):
        import re
        sym = _symbol("demo_site", "assets/index2.js")
        self.assertRegex(sym, r"[A-Za-z_][A-Za-z0-9_]*")


class TestGenerate(unittest.TestCase):

    def test_returns_sorted_paths(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            paths = generate(dist, out, "demo_site")
            self.assertEqual(paths, sorted(paths))

    def test_creates_correct_file_count(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            paths = generate(dist, out, "demo_site")
            # 3 fixture files → 3 blob .c + 1 table .c = 4
            self.assertEqual(len(paths), 4)

    def test_all_files_exist(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            for p in generate(dist, out, "demo_site"):
                self.assertTrue(os.path.isfile(p), f"missing: {p}")

    def test_blob_contains_symbol_decls(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            paths = generate(dist, out, "demo_site")
            blobs = [p for p in paths if not p.endswith("_table.c")]
            for p in blobs:
                content = open(p).read()
                self.assertIn("const uint8_t demo_site__", content)
                self.assertIn("const size_t demo_site__", content)

    def test_table_has_root_url_for_index(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn('.path     = "/"', content)

    def test_table_has_js_mime(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn("application/javascript", content)

    def test_table_has_css_mime(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn("text/css", content)

    def test_table_encoding_gzip(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn('"gzip"', content)

    def test_table_count_matches_file_count(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn("const size_t demo_site_count = 3;", content)

    def test_table_has_accessor(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn("demo_site_get", content)

    def test_table_has_extern_decls(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn("extern const uint8_t demo_site__", content)
            self.assertIn("extern const size_t  demo_site__", content)

    def test_url_prefix_applied(self):
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            generate(dist, out, "demo_site", url_prefix="/app")
            content = open(os.path.join(out, "demo_site_table.c")).read()
            self.assertIn("/app/assets/app.js", content)

    def test_missing_dist_exits(self):
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "out")
            os.makedirs(out)
            with self.assertRaises(SystemExit) as cm:
                generate(os.path.join(td, "nonexistent"), out, "sym")
            self.assertNotEqual(cm.exception.code, 0)

    def test_blob_filenames_are_valid_c_identifiers(self):
        import re
        with tempfile.TemporaryDirectory() as td:
            dist = _make_fixture(td)
            out = os.path.join(td, "out")
            os.makedirs(out)
            paths = generate(dist, out, "demo_site")
            for p in paths:
                name = os.path.basename(p)[:-2]  # strip .c
                self.assertRegex(name, r"[A-Za-z_][A-Za-z0-9_]*",
                                 f"not a valid C identifier: {name}")

    def test_blob_byte_parity_gzip(self):
        """Blob .c hex bytes must decode to canonical gzip of the source file."""
        import gzip as gz_mod
        import io
        raw = b"hello breadboard site"
        with tempfile.TemporaryDirectory() as td:
            dist = os.path.join(td, "dist")
            os.makedirs(dist)
            with open(os.path.join(dist, "index.html"), "wb") as f:
                f.write(raw)
            out = os.path.join(td, "out")
            os.makedirs(out)
            paths = generate(dist, out, "site")
            blob = [p for p in paths if not p.endswith("_table.c")][0]
            content = open(blob).read()
            hex_bytes = []
            for line in content.splitlines():
                line = line.strip()
                if line.startswith("0x") or ", 0x" in line:
                    for token in line.split(","):
                        token = token.strip().rstrip(",")
                        if token.startswith("0x"):
                            hex_bytes.append(int(token, 16))
            actual_gz = bytes(hex_bytes)
            bio = io.BytesIO()
            with gz_mod.GzipFile(fileobj=bio, mode="wb", mtime=0) as g:
                g.write(raw)
            expected_gz = bio.getvalue()
            self.assertEqual(actual_gz, expected_gz)


if __name__ == "__main__":
    unittest.main()
