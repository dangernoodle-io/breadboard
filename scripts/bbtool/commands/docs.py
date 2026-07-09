"""docs command — regenerate marker-delimited generated regions in component READMEs.

Subcommands:
    bbtool docs gen               Regenerate + write the GENERATED marker
                                  regions (deps, platform, links, wiring) in
                                  every components/<name>/README.md that
                                  already contains them.
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
  - brief    — the component's one-line purpose, sourced from the FIRST
               Doxygen `@brief` in its primary public header
               (components/<name>/include/<name>.h) via header_annot.extract_brief.
               This is the ONE header-annotation channel this generator reads
               for prose; there is no `@wiki` tag. A README carrying a
               `bbtool:brief` marker whose header has no `@brief` is a hard
               error (fail loud, never silently blank).
  - api      — a linked list of every public header under
               components/<name>/include/*.h, plus a line naming the
               component's `<prefix>_` symbol prefix.
  - deps     — a `Component | Kind | Role | Docs` table over the DIRECT
               (non-transitive) REQUIRES + PRIV_REQUIRES parsed from
               components/<name>/CMakeLists.txt via cmake_parse.parse_requires.
               "Kind" is "public" (from REQUIRES) or "private" (from
               PRIV_REQUIRES only); a dep in both is "public" (public wins).
               "Docs" links to the dep's own README.md when
               components/<dep>/ exists in this repo; external SDK deps
               (esp_timer, freertos, ...) render as plain text with role "—"
               and no link. "Role" is the first sentence of the dep's own
               README brief when it has one, else "—" — marker-comment lines
               are skipped when hunting for that first prose line, so a
               converted dep's `bbtool:brief` marker text never leaks in.
  - platform — presence matrix over platform/{host,espidf,arduino}/<name>/
  - links    — merged Links section: the component's own wiki page (derived
               from `[docs].wiki_base` + the wiki `components/` subdir
               convention — see rules/docs.md) + `[docs].links` (global,
               applied to every component) + `[docs.component_links]`
               (per-component, keyed by component name), deduplicated by URL,
               order preserved.
  - wiring   — a single pointer line to the wiki's "use in your project"
               guide for this component, as an ABSOLUTE `[docs].wiki_base`
               URL (repo-relative `../../wiki/...` 404s cross-repo); degrades
               to plain text (no link) when wiki_base is unset. The wiki page
               itself is a separate, non-generated deliverable.

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
from cmake_parse import (
    parse_requires as _parse_requires,
    strip_cmake_comments as _strip_cmake_comments,
)
from header_annot import extract_brief as _extract_brief, primary_header as _primary_header

NAME = "docs"
HELP = "Regenerate generated marker regions in component READMEs"

# ---------------------------------------------------------------------------
# CMakeLists.txt REQUIRES / PRIV_REQUIRES parsing — lifted to cmake_parse.py
# (shared with boards.py's build-graph derivation); imported above.
# ---------------------------------------------------------------------------


_COMMENT_LINE_RE = re.compile(r'^\s*<!--.*-->\s*$')


def _extract_first_sentence(readme_path: Path) -> str:
    """Pull the first sentence of a component README's one-line brief (the
    first non-blank, non-marker prose line after the title). Mirrors
    scripts/gen_components_readme.py's `extract_purpose` line-finding, then
    additionally trims to the first `.`/`!`/`?`-terminated sentence. Blank
    lines and bare `<!-- ... -->` marker lines (e.g. `bbtool:brief`
    BEGIN/END lines) are skipped, so a converted dep's marker delimiters
    never leak into a dependent's Role cell — after conversion, the brief
    region's BODY (the first line following the marker) is the correct
    prose to use. Returns "—" (em dash) if the README has no such line."""
    lines = readme_path.read_text(encoding="utf-8").splitlines()
    idx = 0
    n = len(lines)

    def _skip_noise(i: int) -> int:
        while i < n and (not lines[i].strip() or _COMMENT_LINE_RE.match(lines[i])):
            i += 1
        return i

    idx = _skip_noise(idx)
    if idx < n and lines[idx].lstrip().startswith("#"):
        idx += 1
    idx = _skip_noise(idx)
    if idx >= n or not lines[idx].strip():
        return "—"
    brief = lines[idx].strip()
    m = _SENTENCE_RE.match(brief)
    return m.group(1) if m else brief


_SENTENCE_RE = re.compile(r'(.+?[.!?])(?=\s+[A-Z]|\s*$)')


def _dep_role_and_link(root: Path, dep: str) -> Tuple[str, Optional[str]]:
    """Return (role, docs_link) for one direct dependency `dep` of the
    component being rendered. `role` is the dep's own README first sentence,
    or "—" when the dep has no README. `docs_link` points at the dep's own
    README.md when `components/<dep>/` exists in this repo; falls back to
    the generated components/ index (components/README.md) for a local
    component that has no README yet; is None for an external SDK
    dependency (e.g. esp_timer, freertos) that has no components/<dep>/ dir
    at all — the caller renders that as plain text, no link."""
    dep_dir = root / "components" / dep
    if not dep_dir.is_dir():
        return "—", None
    dep_readme = dep_dir / "README.md"
    if not dep_readme.is_file():
        return "—", "../README.md"
    return _extract_first_sentence(dep_readme), f"../{dep}/README.md"


def _render_deps(root: Path, name: str, requires: List[str], priv_requires: List[str]) -> str:
    """Render the Dependencies table: one row per DIRECT dep in this
    component's REQUIRES + PRIV_REQUIRES (scoped to direct deps only — no
    transitive walk), sorted, deduplicated. "Kind" is "public" when the dep
    is in REQUIRES (a dep in both REQUIRES and PRIV_REQUIRES is "public" —
    public wins), else "private" (PRIV_REQUIRES only)."""
    requires_set = set(requires)
    deps = sorted(requires_set | set(priv_requires))
    if not deps:
        return "_(none)_"
    lines = ["| Component | Kind | Role | Docs |", "|-----------|------|------|------|"]
    for dep in deps:
        kind = "public" if dep in requires_set else "private"
        role, link = _dep_role_and_link(root, dep)
        role = role.replace("|", "\\|")
        docs_cell = f"[{dep}]({link})" if link else dep
        lines.append(f"| `{dep}` | {kind} | {role} | {docs_cell} |")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Public API region — linked list of headers under components/<name>/include/
# ---------------------------------------------------------------------------

def _render_api(root: Path, name: str) -> str:
    """Render the Public API region: a linked list of every public header
    under components/<name>/include/*.h, sorted, followed by a line naming
    the component's symbol prefix."""
    include_dir = root / "components" / name / "include"
    headers = sorted(p.name for p in include_dir.glob("*.h")) if include_dir.is_dir() else []
    prefix = _component_prefix(name)
    lines = [f"- [`{h}`](include/{h})" for h in headers]
    if lines:
        lines.append("")
    lines.append(f"Public symbols use the `{prefix}_` prefix.")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Brief region — sourced from the primary public header's Doxygen @brief
# ---------------------------------------------------------------------------

class DocsGenError(Exception):
    """Raised when a README carries a `bbtool:brief` marker but its primary
    public header has no `@brief` tag — fail loud rather than emit a blank
    or stale brief region."""


def _render_brief(root: Path, name: str) -> str:
    header = _primary_header(root, name)
    brief = _extract_brief(header)
    if brief is None:
        raise DocsGenError(
            f"components/{name}/README.md has a bbtool:brief marker but "
            f"{header} has no @brief tag"
        )
    return brief


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
# Links — self wiki link + two-tier [docs] extra links, merged + deduplicated
# ---------------------------------------------------------------------------

def _self_wiki_link(wiki_base: str, component: str) -> Optional[str]:
    """Derive a component's own primary wiki link. The only header
    annotation channel this generator reads is Doxygen `@brief` (prose,
    via header_annot.extract_brief) — there is no per-component wiki-link
    header annotation. Instead this deterministically derives the link from
    `[docs].wiki_base` + the wiki's `components/<name>` subdir convention
    (see rules/docs.md's "conceptual/architectural docs -> wiki
    components/ subdir" routing). Returns None when wiki_base is unset."""
    if not wiki_base:
        return None
    return f"{wiki_base.rstrip('/')}/components/{component}"


def _dedupe_links(*groups: List[str]) -> List[str]:
    """Merge link lists in the given order, deduplicated by URL, first
    occurrence wins (order preserved) — determinism per module docstring."""
    seen: set = set()
    out: List[str] = []
    for group in groups:
        for url in group:
            if url and url not in seen:
                seen.add(url)
                out.append(url)
    return out


def _render_links(config: dict, component: str) -> str:
    """Render the merged Links section: self wiki link + `[docs].links`
    (global) + `[docs.component_links]` (per-component), deduplicated."""
    docs_cfg = _load_docs_config(config) or {}
    repo_url = docs_cfg.get("repo_url", "")
    wiki_base = docs_cfg.get("wiki_base", "")
    global_links = list(docs_cfg.get("links", []) or [])
    component_links = list((docs_cfg.get("component_links", {}) or {}).get(component, []))

    self_link = _self_wiki_link(wiki_base, component)
    merged = _dedupe_links([self_link] if self_link else [], global_links, component_links)

    lines: List[str] = []
    if repo_url:
        lines.append(f"- Repository: [{repo_url}]({repo_url})")
    for url in merged:
        lines.append(f"- [{url}]({url})")
    return "\n".join(lines) if lines else "_(no links configured)_"


# ---------------------------------------------------------------------------
# Use-in-your-project pointer — wiring guidance lives in the wiki, not here
# ---------------------------------------------------------------------------

def _render_wiring(config: dict, component: str) -> str:
    """Render an ABSOLUTE `[docs].wiki_base`-derived pointer to the wiki's
    "use in your project" guide for this component (a repo-relative
    `../../wiki/...` link 404s when the README is viewed cross-repo).
    Degrades to plain text (no link) when wiki_base is unset."""
    docs_cfg = _load_docs_config(config) or {}
    wiki_base = docs_cfg.get("wiki_base", "")
    link = _self_wiki_link(wiki_base, component)
    if link is None:
        return "Use in your project → wiring guide (no `[docs].wiki_base` configured)."
    return f"Use in your project → [wiring guide]({link}#use)."


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


def _gen_component_readme(root: Path, name: str, config: Optional[dict] = None) -> Tuple[Path, bool]:
    """Regenerate marker regions in components/<name>/README.md in place.
    `config` is the parsed bbtool.toml dict (only its `[docs]` block matters
    here, for the links region); defaults to {} when omitted. Returns
    (path, changed)."""
    config = config or {}
    readme_path = root / "components" / name / "README.md"
    content = readme_path.read_text(encoding="utf-8")

    cmake_path = root / "components" / name / "CMakeLists.txt"
    cmake_text = cmake_path.read_text(encoding="utf-8") if cmake_path.is_file() else ""
    requires, priv_requires = _parse_requires(cmake_text, component=name)
    matrix = _platform_matrix(root, name)

    generators: Dict[str, Callable[[], str]] = {
        "brief": lambda: _render_brief(root, name),
        "api": lambda: _render_api(root, name),
        "deps": lambda: _render_deps(root, name, requires, priv_requires),
        "platform": lambda: _render_platform(matrix),
        "links": lambda: _render_links(config, name),
        "wiring": lambda: _render_wiring(config, name),
    }

    new_content = _rewrite_markers(content, generators, source=str(readme_path))
    changed = new_content != content
    if changed:
        readme_path.write_text(new_content, encoding="utf-8")
    return readme_path, changed


def gen_all(root: str, config: Optional[dict] = None) -> List[Tuple[Path, bool]]:
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
        results.append(_gen_component_readme(root_p, child.name, config))
    return results


def _cmd_gen(root: str, config: Optional[dict] = None) -> int:
    try:
        results = gen_all(root, config)
    except DocsGenError as e:
        print(f"bbtool docs gen: error: {e}", file=sys.stderr)
        return 1
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


def scaffold_component(root: str, component: str, config: dict) -> Path:
    """Stamp templates/component-readme.md into components/<component>/README.md,
    substituting tokens, then run marker-region generation over the fresh file
    so it lands fully populated in one shot.

    Raises FileExistsError if the README already exists — this function must
    NEVER overwrite an existing README.
    Raises RuntimeError if the rendered output still has dangling {{tokens}}.
    Raises DocsGenError if the component's primary public header has no
    `@brief` tag (the template's brief region is a bbtool:brief marker).
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

    # No optional regions left in the template — repo_url/wiki_base/badges
    # all moved into the marker-generated "links" region (below), populated
    # by _gen_component_readme from `config` regardless of whether [docs] is
    # set (an absent [docs] block just renders "_(no links configured)_").
    rendered = render(template_text, vars)

    dangling = find_dangling_tokens(rendered)
    if dangling:
        raise RuntimeError(f"docs scaffold: unresolved template tokens {dangling} in {readme_path}")

    readme_path.parent.mkdir(parents=True, exist_ok=True)
    readme_path.write_text(rendered, encoding="utf-8")

    # Populate the freshly-stamped marker regions in the same pass.
    _gen_component_readme(root_p, component, config)
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
    except DocsGenError as e:
        print(f"bbtool docs scaffold: error: {e}", file=sys.stderr)
        return 1
    print(f"bbtool docs scaffold: {path} (created)")
    return 0


# ---------------------------------------------------------------------------
# Lint rule: component-readme (doc-completeness)
# ---------------------------------------------------------------------------

_BRIEF_MARKER_RE = re.compile(r'<!-- BEGIN bbtool:brief -->')


def _check_component_readme(ctx: Context) -> list:
    """Rule: component-readme — flags components/<name>/ directories with no
    README.md (fires broadly on the undocumented components today by
    design; severity stays "warn" until the fill lands — B1-646), and ALSO
    flags a README that carries a `bbtool:brief` marker whose primary
    public header has no `@brief` tag (same "warn" severity — `docs gen`
    itself fails loud/hard on this case; the lint surfaces it ahead of a
    `make check` run)."""
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
            continue
        content = ctx.read(readme)
        if _BRIEF_MARKER_RE.search(content):
            header = _primary_header(root, child.name)
            if _extract_brief(header) is None:
                violations.append(
                    ctx.violation(
                        readme, 1,
                        f"components/{child.name}/README.md has a bbtool:brief"
                        f" marker but {header} has no @brief tag",
                    )
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
        config = getattr(args, "_config_dict", None) or {}
        return _cmd_gen(root, config)
    if args.action == "scaffold":
        if not args.component:
            print("bbtool docs scaffold: error: component name required", file=sys.stderr)
            return 1
        config = getattr(args, "_config_dict", None) or {}
        return _cmd_scaffold(root, args.component, config)
    return 1
