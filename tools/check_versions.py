#!/usr/bin/env python3
"""Version and documentation consistency gate.

Why this exists: the release number and the module hashes appear in two
languages, in several documents *and* inside generated graphics. Bumping a
version by hand and forgetting one of those places produces docs that lie.
This gate makes the bump mechanical:

  versions.json  ->  module.prop        (package version)
                 ->  README.md / .ru.md (tables, hashes, version history)
                 ->  assets/*.svg       (banner chip, library cards, evidence)

Checks:
  1. module.prop matches versions.json (version + versionCode).
  2. Every module's sha256 is present in both root READMEs.
  3. The current release version is present in both root READMEs.
  4. Every module has a directory/README (except the base kernel module).
  5. Every docs/*.ru.md has its English counterpart, and vice versa where the
     pair is declared by the README documentation table.
  6. Regenerating assets from versions.json produces byte-identical files.
  7. No stale artifact hash appears in a current-state document: every 64-hex
     string and every shortened hash followed by an ellipsis must belong to a
     module in versions.json. Timestamped stage records (docs/BACKPORT-7.4.*,
     docs/MGLRU-STAGE-*) are excluded on purpose: they quote the binaries that
     were measured on that date, and rewriting them would falsify the record.
  8. Both root READMEs show the *current* version in each module table row.
  9. Each module README carries its current artifact hash.
 10. Every copied `module.prop` under `dist/` matches the release block.

Checks 7-9 exist because a release is not one edit: an old hash next to old
counters reads as a fact and silently lies about the shipped binary. The gate
must fail on that, not a reviewer's memory.

Run: python3 tools/check_versions.py
"""
from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
META = json.loads((ROOT / "versions.json").read_text(encoding="utf-8"))
errors: list[str] = []


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def need(cond: bool, msg: str) -> None:
    if not cond:
        errors.append(msg)


# 1. package metadata -------------------------------------------------------
prop = read("module/module.prop")
rel = META["release"]
m = re.search(r"^version=(.+)$", prop, re.M)
need(bool(m) and m.group(1).strip() == rel["version"],
     f"module.prop version != versions.json release.version ({rel['version']})")
m = re.search(r"^versionCode=(\d+)$", prop, re.M)
need(bool(m) and int(m.group(1)) == int(rel["versionCode"]),
     f"module.prop versionCode != versions.json release.versionCode ({rel['versionCode']})")

# 2/3. root READMEs, both languages -----------------------------------------
for name in ("README.md", "README.ru.md"):
    body = read(name)
    need(f"v{rel['version']}" in body, f"{name}: current release v{rel['version']} not mentioned")
    for mod in META["modules"]:
        need(mod["sha256"] in body, f"{name}: sha256 of {mod['name']} missing")
        need(mod["name"] in body, f"{name}: module {mod['name']} not listed")

# 4. module directories ------------------------------------------------------
for mod in META["modules"]:
    d = ROOT / mod["dir"]
    need((d / "README.md").exists(), f"{mod['dir']}/README.md missing")
    need((d / "Makefile").exists(), f"{mod['dir']}/Makefile missing")

# 5. documentation language pairs -------------------------------------------
docs = ROOT / "docs"
for ru in sorted(docs.glob("*.ru.md")):
    en = ru.with_name(ru.name.replace(".ru.md", ".md"))
    need(en.exists(), f"{ru.name} has no English counterpart {en.name}")

# documents that must exist in both languages (declared in the README table)
paired = ["ADAPTING", "ENTRYPOINTS", "MGLRU", "CVE-PATCHING"]
for stem in paired:
    for suf in (".md", ".ru.md"):
        need((docs / (stem + suf)).exists(), f"docs/{stem}{suf} missing")

# 6. assets are reproducible -------------------------------------------------
with tempfile.TemporaryDirectory() as tmp:
    # render into a scratch copy of the tree so the repo is untouched
    work = pathlib.Path(tmp) / "repo"
    (work / "assets").mkdir(parents=True)
    (work / "tools").mkdir()
    (work / "versions.json").write_text((ROOT / "versions.json").read_text(encoding="utf-8"))
    (work / "tools" / "make_assets.py").write_text(
        (ROOT / "tools" / "make_assets.py").read_text(encoding="utf-8"))
    r = subprocess.run([sys.executable, str(work / "tools" / "make_assets.py")],
                       capture_output=True, text=True)
    need(r.returncode == 0, f"asset regeneration failed: {r.stdout}{r.stderr}")
    if r.returncode == 0:
        for svg in sorted((ROOT / "assets").glob("*.svg")):
            produced = work / "assets" / svg.name
            need(produced.exists(), f"generator did not produce {svg.name}")
            if produced.exists():
                need(produced.read_bytes() == svg.read_bytes(),
                     f"assets/{svg.name} is stale — run tools/make_assets.py")

# 7-9. no stale facts in the current-state documentation surface ------------
# Timestamped stage/backport records are evidence of what was measured *then*.
# Restating them with today's hash would be a lie in the opposite direction,
# so they are excluded by name and only these files are excluded.
HISTORICAL = {
    "docs/BACKPORT-7.4.md", "docs/BACKPORT-7.4.ru.md",
    "docs/MGLRU-STAGE-E.md", "docs/MGLRU-STAGE-E.ru.md",
    "docs/MGLRU-STAGE-F.md", "docs/MGLRU-STAGE-F.ru.md",
    "docs/MGLRU-STAGE-G.md", "docs/MGLRU-STAGE-G.ru.md",
}
SCAN_SUFFIX = {".md", ".prop"}
SKIP_DIRS = {".git", "dist", "target", "node_modules", "__pycache__", "build"}
doc_files = [
    p for p in ROOT.rglob("*")
    if p.is_file()
    and p.suffix in SCAN_SUFFIX
    and str(p.relative_to(ROOT)) not in HISTORICAL
    and not any(part in SKIP_DIRS for part in p.relative_to(ROOT).parts)
]
KNOWN = {mod["sha256"] for mod in META["modules"]}
for p in doc_files:
    text = p.read_text(encoding="utf-8", errors="ignore")
    rel_name = str(p.relative_to(ROOT))
    for full in re.finditer(r"\b[0-9a-f]{64}\b", text):
        need(full.group(0) in KNOWN,
             f"{rel_name}: stale sha256 {full.group(0)[:12]}… is not a current module hash")
    for short in re.finditer(r"\b([0-9a-f]{8,16})\s*…", text):
        pref = short.group(1)
        need(any(k.startswith(pref) for k in KNOWN),
             f"{rel_name}: stale short hash {pref}… is not a prefix of a current module hash")

# 8. module table rows carry the current version, in both languages
for name in ("README.md", "README.ru.md"):
    body = read(name)
    for mod in META["modules"]:
        row = re.compile(r"\[`?" + re.escape(mod["name"]) + r"`?\]\([^)]*\)\s*\|\s*"
                         + re.escape(mod["version"]) + r"\s*\|")
        need(bool(row.search(body)),
             f"{name}: {mod['name']} row does not show current version {mod['version']}")

# 9. each module README carries the current artifact hash
for mod in META["modules"]:
    body = (ROOT / mod["dir"] / "README.md").read_text(encoding="utf-8")
    need(mod["sha256"] in body or mod["sha256"][:8] in body,
         f"{mod['dir']}/README.md: current sha256 of {mod['name']} missing")

# 10. the packaged copies under dist/ are not stale (when a build exists)
# dist/ is gitignored and regenerated by the release build, so a fresh clone
# skips this. Locally it must never contain a module.prop that disagrees with
# the release block: that copy is what ends up inside module.zip.
dist = ROOT / "dist"
if dist.is_dir():
    for prop_path in sorted(dist.glob("out-*/module/module.prop")):
        body = prop_path.read_text(encoding="utf-8")
        m = re.search(r"^version=(.+)$", body, re.M)
        need(bool(m) and m.group(1).strip() == rel["version"],
             f"{prop_path.relative_to(ROOT)}: version != {rel['version']} (stale build dir?)")
        m = re.search(r"^versionCode=(\d+)$", body, re.M)
        need(bool(m) and int(m.group(1)) == int(rel["versionCode"]),
             f"{prop_path.relative_to(ROOT)}: versionCode != {rel['versionCode']} (stale build dir?)")

if errors:
    print("FAIL version/documentation consistency:")
    for e in errors:
        print("  - " + e)
    sys.exit(1)

print(f"PASS version consistency (release {rel['version']}, {len(META['modules'])} modules, "
      f"{len(list((ROOT / 'assets').glob('*.svg')))} assets)")
