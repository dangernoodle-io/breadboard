"""docs command — regenerate marker-delimited generated regions in component READMEs.

Subcommand:
    bbtool docs gen    Regenerate + write the GENERATED marker regions
                       (deps, platform) in every components/<name>/README.md
                       that already contains them.

There is no --check mode: the drift gate is simply `bbtool docs gen` followed
by `git diff --exit-code` (wired into `make check`).

Marker convention (terraform-docs style):

    <!-- BEGIN bbtool:deps -->
    ...generated content...
    <!-- END bbtool:deps -->

Only the text strictly between a BEGIN/END pair is rewritten; everything else
in the file (hand-authored prose, other sections) is left byte-for-byte
untouched. A README with no markers is never modified and never created by
this command — component READMEs are hand-authored from birth; `docs gen`
only fills in the generated regions of READMEs that already opt in.

Generated regions today:
  - deps     — REQUIRES / PRIV_REQUIRES parsed from components/<name>/CMakeLists.txt
  - platform — presence matrix over platform/{host,espidf,arduino}/<name>/

Determinism is load-bearing: sorted lists, no dict/set iteration-order leaks,
no timestamps, no absolute paths, normalized trailing newline. A second `gen`
run on unchanged inputs MUST produce zero diff.

See scripts/bbtool/README.md for the full per-component README template.
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path
from typing import Callable, Dict, List, Tuple

from core import Context
from registry import Rule

NAME = "docs"
HELP = "Regenerate generated marker regions in component READMEs"

# ---------------------------------------------------------------------------
# CMakeLists.txt REQUIRES / PRIV_REQUIRES parsing
# ---------------------------------------------------------------------------

_CMAKE_ARG_KEYWORDS = frozenset({
    "SRCS", "SRC_DIRS", "EXCLUDE_SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS",
    "REQUIRES", "PRIV_REQUIRES", "LDFRAGMENTS", "EMBED_FILES", "EMBED_TXTFILES",
    "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE",
})


def _strip_cmake_line_comment(line: str) -> str:
    """Strip a CMake `#`-to-end-of-line comment from a single physical line.
    Defensive against `#` inside a quoted string (not observed in practice,
    but tracked so a stray `#` in a quoted arg isn't mistaken for a comment)."""
    out = []
    in_quotes = False
    for c in line:
        if c == '"':
            in_quotes = not in_quotes
            out.append(c)
        elif c == '#' and not in_quotes:
            break
        else:
            out.append(c)
    return "".join(out)


def _strip_cmake_comments(cmake_text: str) -> str:
    """Strip `#`-to-end-of-line comments from every physical line."""
    return "\n".join(_strip_cmake_line_comment(line) for line in cmake_text.splitlines())


def _parse_requires(cmake_text: str) -> Tuple[List[str], List[str]]:
    """Parse REQUIRES / PRIV_REQUIRES args out of an idf_component_register(...)
    call. Returns (requires, priv_requires) — sorted, de-duplicated lists."""
    cmake_text = _strip_cmake_comments(cmake_text)
    m = re.search(r'idf_component_register\s*\(', cmake_text)
    if not m:
        return [], []
    start = cmake_text.index('(', m.start())
    depth = 0
    end = -1
    for i in range(start, len(cmake_text)):
        c = cmake_text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                end = i
                break
    if end == -1:
        return [], []

    block = cmake_text[start + 1:end]
    tokens = block.split()

    requires: set = set()
    priv_requires: set = set()
    current = None
    for tok in tokens:
        if tok in _CMAKE_ARG_KEYWORDS:
            current = tok
            continue
        clean = tok.strip('"')
        if current == "REQUIRES":
            requires.add(clean)
        elif current == "PRIV_REQUIRES":
            priv_requires.add(clean)

    return sorted(requires), sorted(priv_requires)


def _render_deps(requires: List[str], priv_requires: List[str]) -> str:
    req_txt = ", ".join(f"`{r}`" for r in requires) if requires else "_(none)_"
    priv_txt = ", ".join(f"`{r}`" for r in priv_requires) if priv_requires else "_(none)_"
    return f"**REQUIRES:** {req_txt}\n\n**PRIV_REQUIRES:** {priv_txt}"


# ---------------------------------------------------------------------------
# Platform support matrix
# ---------------------------------------------------------------------------

_PLATFORMS = ("host", "espidf", "arduino")


def _platform_matrix(root: Path, name: str) -> Dict[str, bool]:
    return {p: (root / "platform" / p / name).is_dir() for p in _PLATFORMS}


def _render_platform(matrix: Dict[str, bool]) -> str:
    lines = ["| host | espidf | arduino |", "|------|--------|---------|"]
    row = " | ".join("yes" if matrix[p] else "no" for p in _PLATFORMS)
    lines.append(f"| {row} |")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Marker rewriting — only touches text strictly between BEGIN/END pairs
# ---------------------------------------------------------------------------

_MARKER_RE = re.compile(
    r'<!-- BEGIN bbtool:(?P<key>[A-Za-z0-9_-]+) -->\r?\n'
    r'(?P<body>.*?)\r?\n'
    r'<!-- END bbtool:(?P=key) -->',
    re.DOTALL,
)

_BEGIN_MARKER_RE = re.compile(r'<!-- BEGIN bbtool:(?P<key>[A-Za-z0-9_-]+) -->')
_NESTED_BEGIN_RE = re.compile(r'<!-- BEGIN bbtool:')


class NestedMarkerError(Exception):
    """Raised when a marker region's body itself contains a nested BEGIN
    marker — rewriting it would silently swallow or corrupt the inner
    region, so this is a fatal error rather than a silent no-op."""


def _rewrite_markers(content: str, generators: Dict[str, Callable[[], str]],
                      source: str = "<content>") -> str:
    def _sub(m: re.Match) -> str:
        key = m.group("key")
        body = m.group("body")
        if _NESTED_BEGIN_RE.search(body):
            print(
                f"bbtool docs: error: {source}: marker region 'bbtool:{key}' "
                "contains a nested BEGIN marker; refusing to rewrite it "
                "(fix the nesting by hand)",
                file=sys.stderr,
            )
            raise NestedMarkerError(source, key)
        gen = generators.get(key)
        if gen is None:
            return m.group(0)  # unknown marker key — leave untouched
        rendered = gen().rstrip("\n")
        return f"<!-- BEGIN bbtool:{key} -->\n{rendered}\n<!-- END bbtool:{key} -->"

    new_content = _MARKER_RE.sub(_sub, content)

    # Warn (non-fatal) about any BEGIN marker with no matching END — it is
    # left untouched, but that should never happen silently.
    matched_begin_starts = {m.start() for m in _MARKER_RE.finditer(content)}
    for m in _BEGIN_MARKER_RE.finditer(content):
        if m.start() not in matched_begin_starts:
            print(
                f"bbtool docs: warning: {source}: marker 'bbtool:{m.group('key')}' "
                "has no matching END marker; left untouched",
                file=sys.stderr,
            )

    return new_content


def _gen_component_readme(root: Path, name: str) -> Tuple[Path, bool]:
    """Regenerate marker regions in components/<name>/README.md in place.
    Returns (path, changed)."""
    readme_path = root / "components" / name / "README.md"
    content = readme_path.read_text(encoding="utf-8")

    cmake_path = root / "components" / name / "CMakeLists.txt"
    cmake_text = cmake_path.read_text(encoding="utf-8") if cmake_path.is_file() else ""
    requires, priv_requires = _parse_requires(cmake_text)
    matrix = _platform_matrix(root, name)

    generators: Dict[str, Callable[[], str]] = {
        "deps": lambda: _render_deps(requires, priv_requires),
        "platform": lambda: _render_platform(matrix),
    }

    new_content = _rewrite_markers(content, generators, source=str(readme_path))
    changed = new_content != content
    if changed:
        readme_path.write_text(new_content, encoding="utf-8")
    return readme_path, changed


def gen_all(root: str) -> List[Tuple[Path, bool]]:
    """Regenerate marker regions in every components/*/README.md that has one.
    Components without a README.md are untouched — this command never creates
    or scaffolds a README. Returns [(path, changed), ...], sorted by name."""
    root_p = Path(root)
    comp_root = root_p / "components"
    results: List[Tuple[Path, bool]] = []
    if not comp_root.is_dir():
        return results
    for child in sorted(comp_root.iterdir()):
        if not child.is_dir():
            continue
        readme = child / "README.md"
        if not readme.is_file():
            continue
        results.append(_gen_component_readme(root_p, child.name))
    return results


def _cmd_gen(root: str) -> int:
    results = gen_all(root)
    for path, changed in results:
        status = "updated" if changed else "unchanged"
        print(f"bbtool docs gen: {path} ({status})")
    return 0


# ---------------------------------------------------------------------------
# Lint rule: component-readme (doc-completeness)
# ---------------------------------------------------------------------------

def _check_component_readme(ctx: Context) -> list:
    """Rule: component-readme — flags components/<name>/ directories with no
    README.md. Fires broadly on the undocumented components today by
    design (severity stays "warn" until the fill lands — B1-646)."""
    violations = []
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.is_dir():
        return violations
    for child in sorted(comp_root.iterdir()):
        if not child.is_dir():
            continue
        readme = child / "README.md"
        if not readme.is_file():
            violations.append(
                ctx.violation(child, 1, f"components/{child.name}/ has no README.md")
            )
    return violations


_DOCS_RULES = [
    Rule(
        id="component-readme",
        default_severity="warn",
        profiles={"library"},
        check=_check_component_readme,
        hint="every components/<name>/ directory should have a README.md"
             " (see scripts/bbtool/README.md for the template)",
    ),
]


# ---------------------------------------------------------------------------
# Plugin bus registration
# ---------------------------------------------------------------------------

def register(api) -> None:
    api.add_command(NAME, sys.modules[__name__])
    for rule in _DOCS_RULES:
        api.add_rule(rule)


# ---------------------------------------------------------------------------
# Command interface
# ---------------------------------------------------------------------------

def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--root",
        default=os.getcwd(),
        help="repository root (default: from top-level --root or cwd)",
    )
    parser.add_argument(
        "action",
        choices=["gen"],
        help="docs subcommand (only 'gen' today)",
    )


def run(args: argparse.Namespace) -> int:
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    root = os.path.abspath(root)

    if args.action == "gen":
        return _cmd_gen(root)
    return 1
