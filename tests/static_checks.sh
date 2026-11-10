#!/usr/bin/env bash
set -euo pipefail
R=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for f in "$R"/module/*.sh "$R"/tools/*.sh "$R"/tests/*.sh; do [ -f "$f" ] && bash -n "$f"; done
for f in "$R"/tools/*.py; do python3 -m py_compile "$f"; done
! grep -RIn --exclude-dir=.git --exclude=static_checks.sh -E "TODO|FIXME|placeholder" "$R/kernel" "$R/module" "$R/tools" "$R/tests" || { echo "FAIL TODO/FIXME"; exit 1; }
! grep -RIn --exclude-dir=.git --exclude=static_checks.sh -E "8843388838|TELEGRAM_BOT_TOKEN|ghp_[A-Za-z0-9]{20,}" "$R" || { echo "FAIL secret-like token"; exit 1; }
echo "PASS static checks"
