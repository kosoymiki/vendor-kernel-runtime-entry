# Release procedure (and why it is mechanical)

A version bump in this repository is not one edit. The release number, every
module hash and every measured counter appear in:

- `module/module.prop` (package metadata),
- `README.md` and `README.ru.md` (version history table, module table, evidence
  table),
- `docs/ENTRYPOINTS.md` / `.ru.md` (mechanism evidence),
- `docs/MGLRU.md` / `.ru.md` (probe counters),
- `docs/VMSSCAN-HOOK.md` / `.ru.md` (hook mechanism and counters),
- each `modules/<name>/README.md` (artifact hash and measured numbers),
- **ten generated SVGs** — the banner carries the release chip, the advantages
  and pipeline panels restate the guarantees, the library cards carry
  per-module versions, the package and hook panels restate the install and
  reclaim paths, and the evidence card carries the measured counters.

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
  "release": { "version": "1.5.0", "versionCode": 10, "date": "2026-11-11" },
  "modules": [
    { "name": "vmscan_hook", "version": "1.2.0", "status": "experimental",
      "sha256": "a366be997e7a5e7d7c76b6c45f2452cd5fc12eebd1ac84cea4a7635e56eb33e8",
      "gate": "tests/qemu_vmscan_hook_gate.sh",
      "dir": "modules/vmscan_hook" }
  ],
  "evidence": { "vmscan_hook": { "calls": 114, "scans": 20589, "moved": 8323 } }
}
```

Counters in `evidence` are copied from the gate log of the artifact whose
`sha256` sits in the same file. Rebuild without a fresh log means the counters
are not evidence and must not be pasted.

## Checklist

Run it top to bottom; nothing here is optional.

1. **Build and gate the module.** A bump without a fresh QEMU log is not a
   release. Build twice when the artifact is pinned by hash; both builds must
   produce the same byte string.
2. **`versions.json`** — release version/versionCode/date, module `version`,
   `sha256`, `status`, and the `evidence` counters from the fresh log.
3. **`module/module.prop`** — same `version` and `versionCode`.
4. **`README.md` and `README.ru.md`** — add the version row to *Versions*, update
   the module table, the evidence table, and the roadmap if a stage moved.
   Both languages, in the same commit.
5. **`docs/ENTRYPOINTS.md` / `.ru.md`** — new mechanism row or new evidence.
6. **`docs/MGLRU.md` / `.ru.md`** — counters and stage status, if they moved.
7. **`docs/VMSSCAN-HOOK.md` / `.ru.md`** — hook counters, `moved`/`ttl_ms`, and
   the stack-gate numbers.
8. **`modules/<name>/README.md`** — hash, counters, limits.
9. **Regenerate the graphics:** `python3 tools/make_assets.py`. Never edit an
   SVG by hand; the gate re-renders all ten from `versions.json` and compares
   the bytes.
10. **Gate:** `python3 tools/check_versions.py && bash tests/static_checks.sh`.
11. **Rebuild `module.zip`** and copy the new `module.prop` into
    `dist/out-kbootstrap/module/` before zipping. Stale `dist/out-*` trees are
    moved to `dist/attic-<date>/` first — the gate checks every packaged
    `module.prop` under `dist/` and fails on a stale one.
12. **Write `SHA256SUMS`** over the release artifacts in `dist/release-v<ver>/`.
13. **Commit, tag, release** (`gh release create …`), then verify **CI success
    and a fresh clone passing the static gate**.

## What the gate actually checks

`tools/check_versions.py` fails when:

1. `module.prop` disagrees with `versions.json` (version or versionCode);
2. a module `sha256` is missing from either root README;
3. the current release version is not mentioned in either root README;
4. a module has no directory, `README.md` or `Makefile`;
5. a `docs/*.ru.md` has no English counterpart, or a paired document is missing;
6. regenerating the ten SVGs from `versions.json` does not reproduce the files
   byte-for-byte (i.e. someone hand-edited a graphic);
7. a stale artifact hash appears in a current-state document — every 64-hex
   string and every shortened hash followed by `…` must belong to a module in
   `versions.json`. Timestamped stage records (`docs/BACKPORT-7.4.*`,
   `docs/MGLRU-STAGE-*`) are excluded by name on purpose: they record what was
   measured on that date, and rewriting them would falsify the record;
8. either root README's module table row does not show the current version;
9. a module README does not carry its current artifact hash;
10. a copied `module.prop` under `dist/out-*/module/` disagrees with the release
    block (that copy is what ends up inside `module.zip`).

`tests/static_checks.sh` additionally fails when any README loses its
active-development notice, and runs the asset consistency check itself.

## Reflection: why a human checklist is not enough

- **The failure is silent.** A stale hash still renders; a stale banner still
  looks right. Only a diff against the source of truth catches it.
- **Two languages drift independently.** The Russian README is the one that
  rots when a bump is done at 2 a.m. after an English edit.
- **Graphics are data.** The banner, library cards and evidence card are
  *rendered facts*, not decoration. Treating them as art is exactly how they end
  up contradicting the tables below them.
- **Counters are evidence only next to their artifact.** A number from an older
  `.ko` pasted under a newer hash is the same lie as a stale hash, so the
  numbers move together with the pin, never separately.
- **The package is part of the surface.** `dist/out-*/module/module.prop` is the
  file users install; leaving an old one there ships the previous release under
  the new tag.
- **CI must be able to fail.** A check that only exists in a document is a
  suggestion. `check_versions.py` runs in `static_checks.sh`, which runs in CI on
  every push and pull request.
