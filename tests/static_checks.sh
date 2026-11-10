#!/usr/bin/env bash
# Repository static gate: syntax, no deferred-work markers, no secret-like
# strings, generated assets are reproducible, and every README carries the
# active-development notice (the library grows; readers must know).
set -euo pipefail
R=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for f in "$R"/module/*.sh "$R"/tools/*.sh "$R"/tests/*.sh; do [ -f "$f" ] && bash -n "$f"; done
for f in "$R"/tools/*.py "$R"/tests/*.py; do [ -f "$f" ] && python3 -m py_compile "$f"; done

! grep -RIn --exclude-dir=.git --exclude=static_checks.sh -E "TODO|FIXME|placeholder" \
    "$R/kernel" "$R/modules" "$R/module" "$R/tools" "$R/tests" || { echo "FAIL TODO/FIXME"; exit 1; }
! grep -RIn --exclude-dir=.git --exclude=static_checks.sh -E "8843388838|TELEGRAM_BOT_TOKEN|ghp_[A-Za-z0-9]{20,}" \
    "$R" || { echo "FAIL secret-like token"; exit 1; }

# generated infographics must be valid and reproducible from tools/make_assets.py
python3 "$R/tools/make_assets.py" --check >/dev/null || { echo "FAIL assets"; exit 1; }

# every README states that the library is under active development
missing=0
while IFS= read -r f; do
  if ! grep -qiE "active development|активная разработка" "$f"; then
    echo "FAIL active-development notice missing: ${f#$R/}"
    missing=1
  fi
done < <(find "$R" -name "README*.md" -not -path "*/.git/*" -not -path "*/dist/*")
[ "$missing" = 0 ] || exit 1

echo "PASS static checks"
