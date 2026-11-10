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

if errors:
    print("FAIL version/documentation consistency:")
    for e in errors:
        print("  - " + e)
    sys.exit(1)

print(f"PASS version consistency (release {rel['version']}, {len(META['modules'])} modules, "
      f"{len(list((ROOT / 'assets').glob('*.svg')))} assets)")
