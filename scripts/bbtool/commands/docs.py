"""docs command — regenerate marker-delimited generated regions in component READMEs.

Subcommands:
    bbtool docs gen               Regenerate + write the GENERATED marker
                                  regions (deps, platform) in every
                                  components/<name>/README.md that already
                                  contains them.
    bbtool docs scaffold <name>   Stamp scripts/bbtool/templates/component-readme.md
                                  into components/<name>/README.md (error, never
                                  overwrite, if it already exists), substitute
                                  tokens, then run the same marker-region
                                  generation as `gen` over the fresh file.

There is no --check mode: the drift gate is simply `bbtool docs gen` followed
by `git diff --exit-code` (wired into `make check`).

Marker convention (terraform-docs style):

    <!-- BEGIN bbtool:deps -->
    ...generated content...
    <!-- END bbtool:deps -->

Only the text strictly between a BEGIN/END pair is rewritten; everything else
in the file (hand-authored prose, other sections) is left byte-for-byte
untouched. A README with no markers is never modified by `gen`. `docs gen`
itself never creates or scaffolds a README for a component that doesn't have
one — component READMEs are hand-authored from birth; `docs gen` only fills
in the generated regions of READMEs that already opt in. `docs scaffold` is
the separate, explicit opt-in path for creating a new one.

Generated regions today:
  - deps     — REQUIRES / PRIV_REQUIRES parsed from components/<name>/CMakeLists.txt
  - platform — presence matrix over platform/{host,espidf,arduino}/<name>/

Determinism is load-bearing: sorted lists, no dict/set iteration-order leaks,
no timestamps, no absolute paths, normalized trailing newline. A second `gen`
run on unchanged inputs MUST produce zero diff.

See scripts/bbtool/templates/component-readme.md for the stampable README
template (the single source of truth for its shape) and
scripts/bbtool/README.md for the `[docs]` config schema and `docs scaffold`
usage.
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from core import Context
from registry import Rule
from templating import render, find_dangling_tokens

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
# docs scaffold — stamp templates/component-readme.md into a new component
# ---------------------------------------------------------------------------

_TEMPLATE_PATH = Path(__file__).resolve().parent.parent / "templates" / "component-readme.md"


def _load_docs_config(config: dict) -> Optional[dict]:
    """Parse the optional `[docs]` bbtool.toml block. Returns None when
    absent — callers must treat that as "omit every optional doc region",
    never as empty-string tokens."""
    docs_cfg = config.get("docs") if config else None
    if not docs_cfg:
        return None
    return docs_cfg


def _component_prefix(component: str) -> str:
    """Derive the symbol-prefix token from a component directory name, e.g.
    'bb_foo' -> 'bb'. Leading underscores are stripped first so a name like
    '_bb_widget' still yields 'bb' instead of an empty prefix. Falls back to
    the (stripped) full name if there is no underscore left to split on."""
    stripped = component.lstrip("_")
    return stripped.split("_", 1)[0] if "_" in stripped else stripped


def _render_badges_row(repo_url: str, badges: Dict[str, str]) -> str:
    """Render a markdown badge row from a name->image-url map, sorted by name
    for determinism. Each badge links to repo_url."""
    if not badges:
        return ""
    return " ".join(
        f"[![{name}]({url})]({repo_url})" for name, url in sorted(badges.items())
    )


def scaffold_component(root: str, component: str, config: dict) -> Path:
    """Stamp templates/component-readme.md into components/<component>/README.md,
    substituting tokens, then run marker-region generation over the fresh file
    so it lands fully populated in one shot.

    Raises FileExistsError if the README already exists — this function must
    NEVER overwrite an existing README.
    Raises RuntimeError if the rendered output still has dangling {{tokens}}.
    """
    root_p = Path(root)
    readme_path = root_p / "components" / component / "README.md"
    if readme_path.exists():
        raise FileExistsError(f"{readme_path} already exists; docs scaffold refuses to overwrite it")

    template_text = _TEMPLATE_PATH.read_text(encoding="utf-8")

    vars = {
        "component": component,
        "prefix": _component_prefix(component),
    }

    optional_vars: Dict[str, Optional[Dict[str, str]]] = {"badges": None}
    docs_cfg = _load_docs_config(config)
    repo_url = ""
    wiki_base = ""
    if docs_cfg:
        repo_url = docs_cfg.get("repo_url", "")
        wiki_base = docs_cfg.get("wiki_base", "")
        badges = docs_cfg.get("badges", {}) or {}
        optional_vars["badges"] = {
            "repo_url": repo_url,
            "wiki_base": wiki_base,
            "badges_row": _render_badges_row(repo_url, badges),
        }

    rendered = render(template_text, vars, optional_vars)

    if docs_cfg:
        # Each link line is independently conditional on its own token being
        # present — a partial [docs] config (only one of repo_url/wiki_base
        # set) must omit the missing line entirely rather than emit a broken
        # `[]()` link.
        if not repo_url:
            rendered = re.sub(r'^- Repository:.*\n?', "", rendered, flags=re.MULTILINE)
        if not wiki_base:
            rendered = re.sub(r'^- Wiki:.*\n?', "", rendered, flags=re.MULTILINE)
        # An empty badges row (or a dropped link line above) can leave a
        # doubled blank line under the "Links" heading — collapse it.
        # NOTE: this collapse is global over the rendered template and is safe
        # only because the template contains no intentional triple-newline and
        # this runs before marker population — a future multi-paragraph
        # template with deliberate blank-line spacing would need a Links-scoped
        # collapse instead.
        rendered = re.sub(r'\n{3,}', "\n\n", rendered)

    dangling = find_dangling_tokens(rendered)
    if dangling:
        raise RuntimeError(f"docs scaffold: unresolved template tokens {dangling} in {readme_path}")

    readme_path.parent.mkdir(parents=True, exist_ok=True)
    readme_path.write_text(rendered, encoding="utf-8")

    # Populate the freshly-stamped marker regions in the same pass.
    _gen_component_readme(root_p, component)
    return readme_path


def _cmd_scaffold(root: str, component: str, config: dict) -> int:
    try:
        path = scaffold_component(root, component, config)
    except FileExistsError as e:
        print(f"bbtool docs scaffold: error: {e}", file=sys.stderr)
        return 1
    except RuntimeError as e:
        print(f"bbtool docs scaffold: error: {e}", file=sys.stderr)
        return 1
    print(f"bbtool docs scaffold: {path} (created)")
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
             " (run `bbtool docs scaffold <name>`, or see"
             " scripts/bbtool/templates/component-readme.md)",
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
        choices=["gen", "scaffold"],
        help="docs subcommand: 'gen' regenerates marker regions; "
             "'scaffold' stamps a new component README",
    )
    parser.add_argument(
        "component",
        nargs="?",
        default=None,
        help="component name, e.g. bb_foo (required for 'scaffold')",
    )


def run(args: argparse.Namespace) -> int:
    root = getattr(args, "root", None) or getattr(args, "_root_abs", None) or os.getcwd()
    root = os.path.abspath(root)

    if args.action == "gen":
        return _cmd_gen(root)
    if args.action == "scaffold":
        if not args.component:
            print("bbtool docs scaffold: error: component name required", file=sys.stderr)
            return 1
        config = getattr(args, "_config_dict", None) or {}
        return _cmd_scaffold(root, args.component, config)
    return 1
