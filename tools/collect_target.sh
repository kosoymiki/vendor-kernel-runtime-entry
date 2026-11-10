#!/usr/bin/env bash
# Collect target facts with root. Output directory: $1 (default ./target).
set -euo pipefail
OUT=${1:-target}
mkdir -p "$OUT"
su -c 'zcat /proc/config.gz' > "$OUT/config" 2>/dev/null || true
[ -s "$OUT/config" ] || echo "WARN: /proc/config.gz unavailable; provide config manually" >&2
su -c 'cat /proc/kallsyms' > "$OUT/kallsyms"
su -c 'cat /proc/version' > "$OUT/version"
su -c 'uname -a' > "$OUT/uname"
echo "wrote $OUT/config $OUT/kallsyms $OUT/version"
echo "Next: provide an uncompressed Image (boot.img -> Image) and a known-good vendor .ko."
