#!/usr/bin/env bash
# Build dist/release-v<version>/ from versions.json and the built artifacts.
#
# Steps (docs/RELEASE.md, steps 11-12):
#   1. verify every module .ko hash equals versions.json (never copy a stale one);
#   2. copy the five .ko files into the release directory;
#   3. copy the current module/module.prop into every dist/out-*/module/ tree;
#   4. stage module/bin/kbootstrap.ko and zip module/ into module.zip;
#   5. write manifest.json (release + modules + evidence) and SHA256SUMS.
#
# Usage: tools/package_release.sh [version]
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

VER=${1:-$(python3 -c 'import json;print(json.load(open("versions.json"))["release"]["version"])')}
OUT="dist/release-v$VER"
PROFILE=${PROFILE:-rmx3709-5.10.236}

if [ -d "$OUT" ]; then
  echo "FAIL $OUT already exists (move it to dist/attic-<date>/ first)"
  exit 1
fi

mkdir -p "$OUT"
python3 - "$VER" "$OUT" "$PROFILE" <<'PY'
import hashlib, json, pathlib, shutil, sys
ver, out, profile = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3]
d = json.load(open("versions.json"))
assert d["release"]["version"] == ver, f"versions.json release {d['release']['version']} != {ver}"
src = {
  "kbootstrap": "kernel/kbootstrap.ko",
  "kpatch": "modules/kpatch/kpatch.ko",
  "ktramp": "modules/ktramp/ktramp.ko",
  "mglru_probe": "modules/mglru/mglru_probe.ko",
  "vmscan_hook": "modules/vmscan_hook/vmscan_hook.ko",
}
mods = []
for m in d["modules"]:
    p = pathlib.Path(src[m["name"]])
    got = hashlib.sha256(p.read_bytes()).hexdigest()
    assert got == m["sha256"], f"{m['name']}: built {got} != versions.json {m['sha256']}"
    shutil.copy2(p, out / p.name)
    mods.append({k: m[k] for k in ("name", "version", "sha256", "status", "gate") if k in m})
    if m.get("gate_stagek"):
        mods[-1]["gate_stagek"] = m["gate_stagek"]
manifest = {
  "release": d["release"],
  "profile": profile,
  "modules": mods,
  "evidence": d["evidence"],
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
print(f"staged {len(mods)} modules + manifest.json")
PY

# package metadata must match the release block everywhere it is shipped
for dir in dist/out-*/module; do
  [ -d "$dir" ] && cp -f module/module.prop "$dir/module.prop"
done
mkdir -p module/bin
cp -f kernel/kbootstrap.ko module/bin/kbootstrap.ko
rm -f "$OUT/module.zip"
( cd module && zip -qr "../$OUT/module.zip" . -x '*.log' )
rm -f module/bin/kbootstrap.ko

( cd "$OUT" && sha256sum *.ko module.zip manifest.json > SHA256SUMS )

echo "PASS release $VER staged in $OUT"
ls -l "$OUT"
