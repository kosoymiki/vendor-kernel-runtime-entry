# Release procedure (and why it is mechanical)

A version bump in this repository is not one edit. The release number and every
module hash appear in:

- `module/module.prop` (package metadata),
- `README.md` and `README.ru.md` (version history table, module table, evidence
  table),
- `docs/ENTRYPOINTS.md` / `.ru.md` (mechanism evidence),
- `docs/MGLRU.md` / `.ru.md` (probe counters),
- each `modules/<name>/README.md` (artifact hash and measured numbers),
- **six generated SVGs** — the banner carries the release chip, the library
  cards carry per-module versions, the evidence card carries the measured
  counters.

Missing one of those produces documentation that lies about a binary. That has
already happened here: the banner chip said `v1.0.3` while `module.prop` still
said `1.0.2`, and the library card versions were hardcoded in the generator.
Both are now impossible by construction.

## Single source of truth

[`versions.json`](../versions.json) holds the release block, the module list
(name, version, status, sha256, gate, directory) and the measured evidence
block. `tools/make_assets.py` reads it; `tools/check_versions.py` enforces it.

```json
{
  "release": { "version": "1.0.3", "versionCode": 4, "date": "2026-11-10" },
  "modules": [
    { "name": "ktramp", "version": "1.0.3", "status": "probe",
      "sha256": "bbfb599a…", "gate": "tests/qemu_ktramp_gate.sh",
      "dir": "modules/ktramp" }
  ],
  "evidence": { "ktramp": { "calls": 113, "pgscan_file": 6084 } }
}
```

## Checklist

Run it top to bottom; nothing here is optional.

1. **Build and gate the module.** A bump without a fresh QEMU log is not a
   release.
2. **`versions.json`** — release version/versionCode/date, module `version`,
   `sha256`, `status`, and the `evidence` counters.
3. **`module/module.prop`** — same `version` and `versionCode`.
4. **`README.md` and `README.ru.md`** — add the version row to *Versions*, update
   the module table, the evidence table, and the roadmap if a stage moved.
   Both languages, in the same commit.
5. **`docs/ENTRYPOINTS.md` / `.ru.md`** — new mechanism row or new evidence.
6. **`docs/MGLRU.md` / `.ru.md`** — counters and stage status, if they moved.
7. **`modules/<name>/README.md`** — hash, counters, limits.
8. **Regenerate the graphics:** `python3 tools/make_assets.py`. Never edit an
   SVG by hand.
9. **Gate:** `python3 tools/check_versions.py && bash tests/static_checks.sh`.
10. **Rebuild `module.zip`** and copy the new `module.prop` into
    `dist/out-kbootstrap/module/` before zipping.
11. **Commit, tag, release** (`gh release create …`), then verify **CI success
    and a fresh clone passing the static gate**.

## What the gate actually checks

`tools/check_versions.py` fails when:

- `module.prop` disagrees with `versions.json` (version or versionCode);
- a module `sha256` is missing from either root README;
- the current release version is not mentioned in either root README;
- a module has no directory, `README.md` or `Makefile`;
- a `docs/*.ru.md` has no English counterpart, or a paired document is missing;
- regenerating the six SVGs from `versions.json` does not reproduce the files
  byte-for-byte (i.e. someone hand-edited a graphic).

`tests/static_checks.sh` additionally fails when any README loses its
active-development notice, and runs the asset check itself.

## Reflection: why a human checklist is not enough

- **The failure is silent.** A stale hash still renders; a stale banner still
  looks right. Only a diff against the source of truth catches it.
- **Two languages drift independently.** The Russian README is the one that
  rots when a bump is done at 2 a.m. after an English edit.
- **Graphics are data.** The banner, library cards and evidence card are
  *rendered facts*, not decoration. Treating them as art is exactly how they end
  up contradicting the tables below them.
- **CI must be able to fail.** A check that only exists in a document is a
  suggestion. `check_versions.py` runs in `static_checks.sh`, which runs in CI on
  every push and pull request.
