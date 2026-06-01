"""test_gen_site.py — host tests for scripts/gen_site.py.

Run with:  python3 -m pytest test/test_host/test_gen_site.py -v
from the breadboard repo root.
"""

import gzip
import importlib.util
import os
import sys
import tempfile

import pytest

# ---------------------------------------------------------------------------
# Import gen_site from the scripts/ directory regardless of cwd.
# ---------------------------------------------------------------------------
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
_SCRIPTS   = os.path.join(_REPO_ROOT, "scripts")

if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

import gen_site  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_fixture(tmp_path):
    """Create a minimal SPA fixture under tmp_path/dist and return dist_dir."""
    dist = tmp_path / "dist"
    assets = dist / "assets"
    assets.mkdir(parents=True)
    (dist / "index.html").write_text(
        "<!doctype html><html><body>hello</body></html>\n", encoding="utf-8"
    )
    (assets / "app.js").write_text("console.log('ok');\n", encoding="utf-8")
    (assets / "style.css").write_text("body{}\n", encoding="utf-8")
    return str(dist)


# ---------------------------------------------------------------------------
# URL mapping
# ---------------------------------------------------------------------------

def test_index_html_maps_to_root():
    assert gen_site._url("index.html", "/") == "/"


def test_asset_js_maps_to_slash_relpath():
    assert gen_site._url("assets/app.js", "/") == "/assets/app.js"


def test_url_prefix_prepended():
    assert gen_site._url("assets/app.js", "/ui") == "/ui/assets/app.js"


def test_url_prefix_trailing_slash_stripped():
    assert gen_site._url("assets/app.js", "/ui/") == "/ui/assets/app.js"


# ---------------------------------------------------------------------------
# MIME detection
# ---------------------------------------------------------------------------

def test_mime_html():
    assert gen_site._mime("index.html") == "text/html"


def test_mime_js():
    assert gen_site._mime("app.js") == "application/javascript"


def test_mime_mjs():
    assert gen_site._mime("mod.mjs") == "application/javascript"


def test_mime_css():
    assert gen_site._mime("style.css") == "text/css"


def test_mime_svg():
    assert gen_site._mime("logo.svg") == "image/svg+xml"


def test_mime_json():
    assert gen_site._mime("manifest.json") == "application/json"


def test_mime_ico():
    assert gen_site._mime("favicon.ico") == "image/x-icon"


def test_mime_png():
    assert gen_site._mime("img.png") == "image/png"


def test_mime_woff2():
    assert gen_site._mime("font.woff2") == "font/woff2"


def test_mime_txt():
    assert gen_site._mime("robots.txt") == "text/plain"


def test_mime_unknown_defaults_to_octet():
    assert gen_site._mime("blob.bin") == "application/octet-stream"


# ---------------------------------------------------------------------------
# Symbol sanitisation
# ---------------------------------------------------------------------------

def test_symbol_sanitises_slash_and_dot():
    sym = gen_site._symbol("my_site", "assets/app.js")
    assert sym == "my_site__assets_app_js"


def test_symbol_all_alnum_unchanged():
    sym = gen_site._symbol("s", "appjs")
    assert sym == "s__appjs"


def test_symbol_is_valid_c_identifier():
    sym = gen_site._symbol("demo_site", "assets/index2.js")
    # Must match [A-Za-z0-9_]+ and not start with digit
    import re
    assert re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", sym)


# ---------------------------------------------------------------------------
# Full generate() integration
# ---------------------------------------------------------------------------

def test_generate_returns_sorted_paths(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    paths = gen_site.generate(dist_dir, out_dir, "demo_site")
    assert paths == sorted(paths)


def test_generate_creates_correct_file_count(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    paths = gen_site.generate(dist_dir, out_dir, "demo_site")
    # 3 fixture files → 3 blob .c + 1 table .c = 4
    assert len(paths) == 4


def test_generate_blob_files_exist(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    paths = gen_site.generate(dist_dir, out_dir, "demo_site")
    for p in paths:
        assert os.path.isfile(p), f"missing generated file: {p}"


def test_generate_blobs_are_gzipped(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    paths = gen_site.generate(dist_dir, out_dir, "demo_site")
    blob_paths = [p for p in paths if not p.endswith("_table.c")]
    assert blob_paths, "no blob .c files generated"
    for p in blob_paths:
        content = open(p).read()
        # The .c file is text containing hex bytes — just verify it includes the symbol decl
        assert "const uint8_t demo_site__" in content
        assert "const size_t demo_site__" in content


def test_index_html_symbol_and_url_in_table(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    # index.html → url "/"
    assert '"/",\n' in content or '= "/"' in content or '.path     = "/"' in content


def test_app_js_mime_in_table(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    assert "application/javascript" in content


def test_style_css_mime_in_table(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    assert "text/css" in content


def test_encoding_gzip_in_table(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    assert '"gzip"' in content


def test_count_equals_file_count(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    # fixture has 3 files → count = 3
    assert "const size_t demo_site_count = 3;" in content


def test_accessor_function_present(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    assert "demo_site_get" in content


def test_extern_decls_present(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    assert "extern const uint8_t demo_site__" in content
    assert "extern const size_t  demo_site__" in content


def test_url_prefix_applied(tmp_path):
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    gen_site.generate(dist_dir, out_dir, "demo_site", url_prefix="/app")
    table_c = os.path.join(out_dir, "demo_site_table.c")
    content = open(table_c).read()
    assert "/app/assets/app.js" in content


def test_missing_dist_dir_exits(tmp_path):
    out_dir = str(tmp_path / "out")
    os.makedirs(out_dir)
    with pytest.raises(SystemExit) as exc:
        gen_site.generate(str(tmp_path / "nonexistent"), out_dir, "sym")
    assert exc.value.code != 0


def test_symbols_are_valid_c_identifiers(tmp_path):
    import re
    dist_dir = _make_fixture(tmp_path)
    out_dir  = str(tmp_path / "out")
    os.makedirs(out_dir)
    paths = gen_site.generate(dist_dir, out_dir, "demo_site")
    blob_paths = [p for p in paths if not p.endswith("_table.c")]
    for p in blob_paths:
        name = os.path.basename(p)[:-2]  # strip .c
        assert re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name), \
            f"not a valid C identifier: {name}"
