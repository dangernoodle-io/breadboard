"""docs command — regenerate marker-delimited generated regions in component READMEs.

Subcommands:
    bbtool docs gen               Regenerate + write the GENERATED marker
                                  regions (deps, platform, links, wiring) in
                                  every components/<name>/README.md that
                                  already contains them.
    bbtool docs scaffold <name>   Stamp scripts/bbtool/templates/component-readme.md
                                  into <name>'s real on-disk directory (resolved
                                  via the discovery SSOT; components/<name>/ when
                                  not yet discoverable) — README.md (error, never
                                  overwrite, if it already exists), substitute
                                  tokens, then run the same marker-region
                                  generation as `gen` over the fresh file.
    bbtool docs scaffold --group <name>
                                  Stamp a new components/<name>/README.md GROUP
                                  stub (hand-authored prose placeholder above an
                                  empty bbtool:group-index marker region) — see
                                  scripts/gen_components_readme.py, which owns
                                  populating that marker region.

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
  - platform — presence matrix over platform/{host,espidf,arduino}/<name>/,
               PLUS (host column only) whether test/test_host/ has a
               test_<name>[.c|_*.c] file — a component can be genuinely
               host-supported (host-tested, Kconfig/define-gated single
               source) without shipping a platform/host/<name>/
               backend-dispatch implementation at all (B1-1197); see
               `_has_host_tests`.
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
  - budget   — a `| Target | flash | Δ vs baseline |` table (B1-719 phase A)
               sourced from every committed `.baseline/bbtool/metrics/*.json`
               that has this component in its `flash.components` map. Δ is
               always "—" — this renders a committed static value, not a
               live compare (that's `bbtool size --check`'s job). When any
               baseline's `heap` block is populated (phase B), min_free/
               high_water columns are added. FAIL-SOFT: a component with no
               matching baseline renders `_(no baseline)_` rather than
               raising — unlike `brief`, footprint docs are advisory, not a
               hard doc-completeness gate.

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
import json
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
from header_annot import (
    extract_brief as _extract_brief,
    primary_header as _primary_header,
    first_sentence as _first_sentence,
    escape_table_cell as _escape_table_cell,
)
from discovery import build_index, normalize_roots

NAME = "docs"
HELP = "Regenerate generated marker regions in component READMEs"


def _resolve_component_dir(root: Path, name: str) -> Path:
    """Resolve `name`'s real on-disk directory via the discovery SSOT
    (`discovery.build_index`) — works at any nesting depth for a component
    with a `CMakeLists.txt`. Falls back to the flat `components/<name>/`
    convention when the component isn't (yet) discoverable — e.g. before
    its `CMakeLists.txt` exists (a fresh `docs scaffold` target), or a unit
    fixture exercising a single render function against a bare tree with no
    `CMakeLists.txt` at all. Never raises — callers that need "not a real
    component" to be a hard error (the brief region) check the RESULT
    (`primary_header`/`extract_brief` returning `None`), not this
    resolver."""
    comp_dir = build_index(normalize_roots(root)).component_dir(name)
    return comp_dir if comp_dir is not None else root / "components" / name


def _header_for_dir(comp_dir: Path, name: str) -> Optional[Path]:
    """Primary public header path for a component whose real on-disk
    directory (`comp_dir`) is ALREADY KNOWN — computed directly
    (`comp_dir/include/<name>.h`), never re-resolved through
    `discovery.build_index` by bare name. This is what keeps the two-pass
    `gen_all`/`_check_component_readme` walk PATH-aware rather than
    NAME-aware: once a caller already has a specific directory in hand
    (pass 1's own `iterdir()` walk, or pass 2's discovery-indexed leaf),
    re-deriving the header from the bare name alone would let an unrelated
    directory elsewhere in the tree that happens to share the same
    component name silently supply THIS directory's header — discovery's
    collision guard only fires between two INDEXED leaves (each with its
    own `CMakeLists.txt`); a directory with no `CMakeLists.txt` of its own
    is invisible to it and can share a name with a real leaf elsewhere with
    zero error.

    Returns `None` when `comp_dir` has no `CMakeLists.txt` of its own —
    mirrors `header_annot.primary_header`'s "not a discovered component"
    contract, but keyed on the actual directory already resolved by the
    caller, never a fresh name lookup."""
    if not (comp_dir / "CMakeLists.txt").is_file():
        return None
    return comp_dir / "include" / f"{name}.h"


# ---------------------------------------------------------------------------
# CMakeLists.txt REQUIRES / PRIV_REQUIRES parsing — lifted to cmake_parse.py
# (shared with boards.py's build-graph derivation); imported above.
# ---------------------------------------------------------------------------


_COMMENT_LINE_RE = re.compile(r'^\s*<!--.*-->\s*$')


def _extract_first_sentence(readme_path: Path) -> str:
    """Pull the first sentence of a component README's one-line brief (the
    first non-blank, non-marker prose line after the title). Mirrors
    scripts/gen_components_readme.py's `extract_purpose` line-finding, then
    delegates to the shared `header_annot.first_sentence` extractor (the
    single home for `.`/`!`/`?`-terminated sentence-splitting — see that
    module for why the candidate-assembly step stays per-caller while the
    extraction itself is shared). Blank lines and bare `<!-- ... -->`
    marker lines (e.g. `bbtool:brief` BEGIN/END lines) are skipped, so a
    converted dep's marker delimiters never leak into a dependent's Role
    cell — after conversion, the brief region's BODY (the first line
    following the marker) is the correct prose to use. Returns "—" (em
    dash) if the README has no such line."""
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
    return _first_sentence(lines[idx].strip())


def _dep_role_and_link(root: Path, comp_dir: Path, dep: str) -> Tuple[str, Optional[str]]:
    """Return (role, docs_link) for one direct dependency `dep` of the
    component rendered from `comp_dir`. `role` is the dep's own README first
    sentence, or "—" when the dep has no README. `docs_link` is relative to
    `comp_dir` and points at the dep's own README.md when `components/<dep>/`
    exists in this repo (looked up via the discovery index, so a dep grouped
    under components/<group>/<dep>/ still resolves at any nesting depth);
    falls back to the generated components/ index (components/README.md)
    for a local component that has no README yet; is None for an external
    SDK dependency (e.g. esp_timer, freertos) that has no components/<dep>/
    dir at all — the caller renders that as plain text, no link."""
    # `dep_dir` (when found) comes back realpath-canonicalized (build_index
    # resolves every root/dir via os.path.realpath), so `comp_dir` must be
    # canonicalized the same way before any relpath call below — otherwise
    # a symlinked tmp/root spelling (e.g. macOS's /var -> /private/var)
    # makes relpath see no common prefix and emit a long, wrong `../../..`
    # chain. This applies equally to the has-README branch and the
    # falls-back-to-the-top-level-index branch below — a hardcoded
    # "../README.md" is only correct for a consumer one level under
    # components/; a consumer nested under components/<group>/ needs
    # "../../README.md" instead, so both branches compute the link the
    # same relpath-against-real_comp_dir way.
    real_comp_dir = Path(os.path.realpath(str(comp_dir)))
    dep_dir = build_index(normalize_roots(root)).component_dir(dep)
    if dep_dir is None:
        return "—", None
    dep_readme = dep_dir / "README.md"
    if not dep_readme.is_file():
        index_readme = Path(os.path.realpath(str(root))) / "components" / "README.md"
        rel_link = os.path.relpath(index_readme, real_comp_dir).replace(os.sep, "/")
        return "—", rel_link
    rel_link = os.path.relpath(dep_readme, real_comp_dir).replace(os.sep, "/")
    return _extract_first_sentence(dep_readme), rel_link


def _render_deps(root: Path, comp_dir: Path, requires: List[str], priv_requires: List[str]) -> str:
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
        role, link = _dep_role_and_link(root, comp_dir, dep)
        role = _escape_table_cell(role)
        docs_cell = f"[{dep}]({link})" if link else dep
        lines.append(f"| `{dep}` | {kind} | {role} | {docs_cell} |")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Public API region — linked list of headers under components/<name>/include/
# ---------------------------------------------------------------------------

def _render_api(root: Path, name: str, comp_dir: Optional[Path] = None) -> str:
    """Render the Public API region: a linked list of every public header
    under components/<name>/include/*.h, sorted, followed by a line naming
    the component's symbol prefix. `comp_dir` lets a caller that already
    resolved the component's real on-disk directory (e.g.
    `_gen_component_readme`) pass it straight through instead of paying a
    second `_resolve_component_dir` lookup; defaults to resolving it here
    for a standalone caller (e.g. tests exercising this function in
    isolation)."""
    if comp_dir is None:
        comp_dir = _resolve_component_dir(root, name)
    include_dir = comp_dir / "include"
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


def _render_brief(root: Path, name: str, comp_dir: Optional[Path] = None) -> str:
    """`comp_dir`, when given, is the ALREADY-RESOLVED directory this
    component's README lives in (threaded through by `_gen_component_readme`
    so the two-pass `gen_all` walk stays path-aware — see `_header_for_dir`).
    Defaults to a fresh discovery-by-name lookup (`header_annot.primary_header`)
    for a standalone caller, e.g. tests exercising this function in
    isolation."""
    header = _header_for_dir(comp_dir, name) if comp_dir is not None else _primary_header(root, name)
    if header is None:
        raise DocsGenError(
            f"components/{name}/README.md has a bbtool:brief marker but "
            f"'{name}' is not a discovered component (no CMakeLists.txt"
            f" found under components/{name}/)"
        )
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


def _owning_component(stem: str, known_names: "frozenset[str]") -> Optional[str]:
    """Given a test_host/*.c filename stem (e.g. 'test_bb_serialize_meta_validate'),
    return the LONGEST `known_names` entry that is a genuine, underscore-bounded
    prefix of its body (the part after 'test_') — or None if none matches.

    The longest-match rule is what keeps this attribution correct in a repo
    where a shorter component name is itself a prefix of a real, differently
    -named sibling (e.g. `bb_serialize` vs `bb_serialize_meta`, `bb_wifi` vs
    `bb_wifi_http`, `bb_led` vs `bb_led_anim`, ... — dozens of these pairs
    exist): a naive per-name boundary check alone would credit BOTH the
    short and the long component from the long component's own test file,
    wrongly flipping the short one to host: yes."""
    if not stem.startswith("test_"):
        return None
    body = stem[len("test_"):]
    best: Optional[str] = None
    for candidate in known_names:
        if body == candidate or body.startswith(candidate + "_"):
            if best is None or len(candidate) > len(best):
                best = candidate
    return best


def _has_host_tests(root: Path, name: str, known_names: Optional["frozenset[str]"] = None) -> bool:
    """True when `name` has at least one PlatformIO native host test file
    under test/test_host/ genuinely attributed to it (see `_owning_component`)
    — the SSOT signal that a component is actually exercised (and thus
    genuinely supported) on the host platform, whether or not it also ships
    a platform/host/<name>/ backend-dispatch implementation (the OTHER,
    narrower host signal — see `_platform_matrix`). Without this, a
    component whose host presence is a Kconfig/define-gated single-source
    build (e.g. `BB_SERIALIZE_META_HOST` unconditionally shipping
    bb_serialize_meta in the native env) or any other host-tested-but-no
    -backend-split component reads `host: no` even at 100% host coverage —
    this was B1-1197, and it affects every similarly-shaped component, not
    just one.

    `known_names` is the full discovery-SSOT component name set, used for
    longest-match disambiguation (see `_owning_component`); defaults to a
    fresh `discovery.build_index(...).names()` lookup for a standalone
    caller (e.g. a test exercising this function in isolation). `name` is
    always unioned in regardless of which branch supplies `known_names` —
    `gen_all`'s pass 1 walks bare/malformed component directories that may
    not be discovery-indexed at all (see its docstring), so `name` must
    remain disambiguation-eligible even when absent from the passed-in
    set."""
    test_host_dir = root / "test" / "test_host"
    if not test_host_dir.is_dir():
        return False
    if known_names is None:
        known_names = frozenset(build_index(normalize_roots(root)).names())
    known_names = known_names | {name}
    for path in test_host_dir.glob("test_*.c"):
        if _owning_component(path.stem, known_names) == name:
            return True
    return False


def _platform_matrix(root: Path, name: str,
                      known_names: Optional["frozenset[str]"] = None) -> Dict[str, bool]:
    """`known_names`, when given, is the caller's already-built discovery-SSOT
    component name set — threaded straight through to `_has_host_tests` so it
    skips its own fresh `discovery.build_index` tree-walk (see `gen_all`,
    which builds this set ONCE up front rather than once per component)."""
    matrix = {p: (root / "platform" / p / name).is_dir() for p in _PLATFORMS}
    if _has_host_tests(root, name, known_names):
        matrix["host"] = True
    return matrix


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
# Footprint budget region — sourced from committed .baseline/bbtool/metrics/
# JSON files (B1-719 phase A: `bbtool size --update-baseline`). FAIL-SOFT by
# design (see module docstring's "budget" entry): a component with no
# matching baseline is advisory-missing, never a `docs gen` hard error.
# ---------------------------------------------------------------------------

def _load_metrics_baselines(root: Path) -> List[dict]:
    """Load every `.baseline/bbtool/metrics/*.json` file, sorted by
    filename for determinism. Skips (never raises on) unreadable/malformed
    files — a corrupt baseline is a `bbtool size` concern, not a `docs gen`
    one."""
    metrics_dir = root / ".baseline" / "bbtool" / "metrics"
    if not metrics_dir.is_dir():
        return []
    baselines: List[dict] = []
    for path in sorted(metrics_dir.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict):
            baselines.append(data)
    return baselines


def _render_budget(root: Path, name: str) -> str:
    """Render the Footprint region: one row per target baseline that has
    `name` in its `flash.components` map. Δ is always "—" (a committed
    static value, not a live compare). Adds min_free/high_water columns
    when any baseline's `heap` block is populated (phase B; null in phase
    A, so those columns are omitted today)."""
    matches = []
    for baseline in _load_metrics_baselines(root):
        flash = baseline.get("flash") or {}
        components = flash.get("components") or {}
        if name not in components:
            continue
        matches.append((baseline.get("target", "?"), components[name], baseline.get("heap") or {}))

    if not matches:
        return "_(no baseline)_"

    matches.sort(key=lambda m: m[0])
    heap_present = any(
        heap.get("min_free") is not None or heap.get("high_water") is not None
        for _, _, heap in matches
    )

    if heap_present:
        lines = [
            "| Target | flash | Δ vs baseline | min_free | high_water |",
            "|--------|-------|----------------|----------|------------|",
        ]
    else:
        lines = [
            "| Target | flash | Δ vs baseline |",
            "|--------|-------|----------------|",
        ]
    for target, flash_bytes, heap in matches:
        row = f"| `{target}` | {flash_bytes} | — |"
        if heap_present:
            min_free = heap.get("min_free")
            high_water = heap.get("high_water")
            row += (
                f" {min_free if min_free is not None else '—'} |"
                f" {high_water if high_water is not None else '—'} |"
            )
        lines.append(row)
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


def _gen_component_readme(root: Path, name: str, config: Optional[dict] = None,
                           comp_dir: Optional[Path] = None,
                           known_names: Optional["frozenset[str]"] = None) -> Tuple[Path, bool]:
    """Regenerate marker regions in components/<name>/README.md in place.
    `config` is the parsed bbtool.toml dict (only its `[docs]` block matters
    here, for the links region); defaults to {} when omitted. `comp_dir`,
    when given, is the caller's ALREADY-RESOLVED real directory for `name`
    (threaded straight through to `_render_brief`/`_render_api` so a bare
    NAME is never re-resolved through discovery mid-pipeline — see
    `_header_for_dir`'s docstring for why that matters for `gen_all`'s
    two-pass walk); defaults to `_resolve_component_dir(root, name)` for a
    standalone caller (e.g. `scaffold_component`). `known_names`, when
    given, is `gen_all`'s ONE-TIME discovery-SSOT name set, threaded through
    to `_platform_matrix`/`_has_host_tests` to avoid a fresh full-tree
    `discovery.build_index` walk per component (O(N^2) otherwise); defaults
    to None so `_has_host_tests` falls back to its own single lookup for a
    standalone caller. Returns (path, changed)."""
    config = config or {}
    if comp_dir is None:
        comp_dir = _resolve_component_dir(root, name)
    readme_path = comp_dir / "README.md"
    content = readme_path.read_text(encoding="utf-8")

    cmake_path = comp_dir / "CMakeLists.txt"
    cmake_text = cmake_path.read_text(encoding="utf-8") if cmake_path.is_file() else ""
    requires, priv_requires = _parse_requires(cmake_text, component=name)
    matrix = _platform_matrix(root, name, known_names)

    generators: Dict[str, Callable[[], str]] = {
        "brief": lambda: _render_brief(root, name, comp_dir),
        "api": lambda: _render_api(root, name, comp_dir),
        "deps": lambda: _render_deps(root, comp_dir, requires, priv_requires),
        "platform": lambda: _render_platform(matrix),
        "links": lambda: _render_links(config, name),
        "wiring": lambda: _render_wiring(config, name),
        "budget": lambda: _render_budget(root, name),
    }

    new_content = _rewrite_markers(content, generators, source=str(readme_path))
    changed = new_content != content
    if changed:
        readme_path.write_text(new_content, encoding="utf-8")
    return readme_path, changed


def gen_all(root: str, config: Optional[dict] = None) -> List[Tuple[Path, bool]]:
    """Regenerate marker regions in every components/*/README.md that has
    one — a TWO-PASS walk, not a discovery-only one:

    1. The depth-1 `iterdir()` walk, UNCHANGED from the pre-hierarchy
       behaviour: every direct `components/<name>/` directory with a
       README.md gets regenerated, regardless of whether discovery.py's
       leaf rule recognizes it as a real component. This is deliberate — a
       bare/malformed directory (no `CMakeLists.txt` anywhere under it, so
       invisible to the discovery SSOT) carrying marker regions must still
       be regenerated, never silently skipped just because it isn't a real
       component. Dropping this pass would silently stop regenerating such
       a directory with zero signal (the exact defect class PR2's
       `ComponentIndexError` was introduced to make loud, not the one to
       reintroduce here — see B1-1128 for the sibling gap in the fence's
       identity fallback).
    2. A supplemental discovery-SSOT pass (`discovery.build_index`) for any
       component DIRECTORY not already covered by pass 1 — i.e. a component
       nested under `components/<group>/<name>/`, unreachable by a depth-1
       walk.

    PATH-aware dedup, not name-aware: pass 2 skips a discovery-indexed leaf
    only when its resolved directory is IDENTICAL to one pass 1 already
    walked — never merely because its NAME matches a pass-1 directory.
    Discovery's own collision guard only fires between two INDEXED leaves
    (each with a `CMakeLists.txt`); a pass-1 directory with none is
    invisible to it and can legitimately share a name with an unrelated
    indexed leaf elsewhere in the tree (e.g. a flat, undiscovered
    `components/bb_foo/` alongside a real nested
    `components/bb_group/bb_foo/`). Deduping on name alone would either
    (a) skip the real nested leaf entirely (its own missing README/marker
    regen going unnoticed), or (b) feed pass 1's directory-scoped call a
    bare name that `_resolve_component_dir`/`_render_brief` would silently
    re-resolve to the UNRELATED indexed directory instead — both are the
    exact defect this two-pass split exists to avoid. Threading each pass's
    already-known `comp_dir` straight into `_gen_component_readme` (never
    re-derived from the bare name mid-pipeline) is what makes this safe.

    Components without a README.md are untouched — this command never
    creates or scaffolds a README. Returns [(path, changed), ...], sorted by
    name within each pass (pass 1 first, by directory name; pass 2 after,
    by component name)."""
    root_p = Path(root)
    comp_root = root_p / "components"
    results: List[Tuple[Path, bool]] = []
    if not comp_root.is_dir():
        return results

    # Built ONCE up front (not per-component) and threaded through every
    # `_gen_component_readme` call below — avoids an O(N^2) re-walk of the
    # whole tree for each component's `_platform_matrix`/`_has_host_tests`
    # host-detection check (see those functions' docstrings).
    index = build_index(normalize_roots(root_p))
    known_names = frozenset(index.names())

    processed_dirs: set = set()
    for child in sorted(comp_root.iterdir()):
        if not child.is_dir():
            continue
        readme = child / "README.md"
        if readme.is_file():
            results.append(_gen_component_readme(root_p, child.name, config, comp_dir=child,
                                                   known_names=known_names))
        processed_dirs.add(Path(os.path.realpath(str(child))))

    for name in sorted(index.names()):
        comp_dir = index.component_dir(name)
        if comp_dir is None or comp_dir in processed_dirs:
            continue
        readme = comp_dir / "README.md"
        if not readme.is_file():
            continue
        results.append(_gen_component_readme(root_p, name, config, comp_dir=comp_dir,
                                               known_names=known_names))
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
    comp_dir = _resolve_component_dir(root_p, component)
    readme_path = comp_dir / "README.md"
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

    # Populate the freshly-stamped marker regions in the same pass -- pass
    # the already-resolved comp_dir through rather than a bare-name lookup.
    _gen_component_readme(root_p, component, config, comp_dir=comp_dir)
    return readme_path


# ---------------------------------------------------------------------------
# docs scaffold --group — stamp a hand-authored components/<group>/README.md
# stub for a NEW group directory (see scripts/gen_components_readme.py's
# two-level index, which owns the `bbtool:group-index` marker region's
# actual content generation — this only creates the file with a prose
# placeholder above an empty marker region for that generator to fill in).
# ---------------------------------------------------------------------------

GROUP_INDEX_BEGIN = "<!-- BEGIN bbtool:group-index -->"
GROUP_INDEX_END = "<!-- END bbtool:group-index -->"

_GROUP_README_TEMPLATE = (
    "# {group}\n\n"
    "TODO: describe this component group (one prose paragraph — this text\n"
    "is what `scripts/gen_components_readme.py` surfaces as the group's\n"
    "description in the top-level components/README.md index).\n\n"
    f"{GROUP_INDEX_BEGIN}\n"
    "_(placeholder — run `python3 scripts/gen_components_readme.py` to populate)_\n"
    f"{GROUP_INDEX_END}\n"
)


def scaffold_group(root: str, group: str) -> Path:
    """Stamp a new components/<group>/README.md stub: a hand-authored
    prose placeholder (edit this) above an empty `bbtool:group-index`
    marker region (populated by `scripts/gen_components_readme.py`, not by
    this function). Raises FileExistsError if the README already exists —
    this function must NEVER overwrite one."""
    root_p = Path(root)
    readme_path = root_p / "components" / group / "README.md"
    if readme_path.exists():
        raise FileExistsError(f"{readme_path} already exists; docs scaffold refuses to overwrite it")
    readme_path.parent.mkdir(parents=True, exist_ok=True)
    readme_path.write_text(_GROUP_README_TEMPLATE.format(group=group), encoding="utf-8")
    return readme_path


def _cmd_scaffold_group(root: str, group: str) -> int:
    try:
        path = scaffold_group(root, group)
    except FileExistsError as e:
        print(f"bbtool docs scaffold: error: {e}", file=sys.stderr)
        return 1
    print(f"bbtool docs scaffold: {path} (created)")
    return 0


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
    """Rule: component-readme — flags a `components/<name>/` directory with
    no README.md (fires broadly on the undocumented components today by
    design; severity stays "warn" until the fill lands — B1-646), and ALSO
    flags a README that carries a `bbtool:brief` marker whose primary
    public header has no `@brief` tag (same "warn" severity — `docs gen`
    itself fails loud/hard on this case; the lint surfaces it ahead of a
    `make check` run).

    TWO-PASS, not discovery-only (mirrors `gen_all` — see its docstring for
    the full PATH-vs-NAME-aware rationale):

    1. The depth-1 `iterdir()` walk, UNCHANGED from the pre-hierarchy
       behaviour: every direct `components/<name>/` directory is checked,
       regardless of whether discovery.py's leaf rule recognizes it as a
       real component. Deliberate — a bare/malformed directory (no
       `CMakeLists.txt` anywhere under it) must still be a visible lint
       finding (missing-README, or "not a discovered component" on its
       brief marker), never silently dropped from this rule's coverage just
       because it isn't a real component. See B1-1128 for the sibling gap
       in the fence's identity fallback.
    2. A supplemental discovery-SSOT pass (`discovery.build_index`) for any
       component DIRECTORY not already covered by pass 1 — i.e. a component
       nested under `components/<group>/<name>/`, unreachable by a depth-1
       walk.

    PATH-aware dedup, not name-aware: pass 2 skips a discovery-indexed leaf
    only when its resolved directory is IDENTICAL to one pass 1 already
    walked. The per-directory header lookup (`_header_for_dir`) is likewise
    keyed on the ALREADY-RESOLVED `comp_dir`, never re-derived from the bare
    name — a flat, undiscovered `components/bb_foo/` sharing a name with an
    unrelated real `components/bb_group/bb_foo/` must never have its brief
    marker satisfied by borrowing the unrelated component's `@brief` (a
    name-keyed `header_annot.primary_header` lookup would do exactly that,
    since discovery's collision guard only fires between two INDEXED
    leaves — a directory with no `CMakeLists.txt` is invisible to it)."""
    violations = []
    root = Path(ctx.root)
    comp_root = root / "components"
    if not comp_root.is_dir():
        return violations

    def _check_dir(comp_dir: Path, name: str, rel_label: str) -> None:
        readme = comp_dir / "README.md"
        if not readme.is_file():
            violations.append(ctx.violation(comp_dir, 1, f"{rel_label}/ has no README.md"))
            return
        content = ctx.read(readme)
        if not _BRIEF_MARKER_RE.search(content):
            return
        header = _header_for_dir(comp_dir, name)
        if header is None:
            violations.append(
                ctx.violation(
                    readme, 1,
                    f"{rel_label}/README.md has a bbtool:brief"
                    f" marker but '{name}' is not a discovered"
                    f" component (no CMakeLists.txt found under"
                    f" {rel_label}/)",
                )
            )
        elif _extract_brief(header) is None:
            violations.append(
                ctx.violation(
                    readme, 1,
                    f"{rel_label}/README.md has a bbtool:brief"
                    f" marker but {header} has no @brief tag",
                )
            )

    processed_dirs: set = set()
    for child in sorted(comp_root.iterdir()):
        if not child.is_dir():
            continue
        _check_dir(child, child.name, f"components/{child.name}")
        processed_dirs.add(Path(os.path.realpath(str(child))))

    # `discovery.build_index` canonicalizes roots via `os.path.realpath`, so
    # every returned `comp_dir` is realpath-rooted -- compute `rel_label`
    # against the SAME canonicalized root rather than `root` verbatim, or a
    # symlinked tmp/root spelling (e.g. macOS's /var -> /private/var) makes
    # `relative_to` raise ValueError.
    real_root = Path(os.path.realpath(str(root)))
    index = build_index(normalize_roots(root))
    for name in sorted(index.names()):
        comp_dir = index.component_dir(name)
        if comp_dir is None or comp_dir in processed_dirs:
            continue
        rel_label = comp_dir.relative_to(real_root).as_posix()
        _check_dir(comp_dir, name, rel_label)

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
        help="component name, e.g. bb_foo (required for 'scaffold'; a group"
             " name, e.g. bb_display, when --group is also given)",
    )
    parser.add_argument(
        "--group",
        action="store_true",
        help="'scaffold' only: stamp a new components/<name>/README.md"
             " GROUP stub (hand-authored prose + an empty bbtool:group-index"
             " marker region) instead of a component README",
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
        if getattr(args, "group", False):
            return _cmd_scaffold_group(root, args.component)
        config = getattr(args, "_config_dict", None) or {}
        return _cmd_scaffold(root, args.component, config)
    return 1
