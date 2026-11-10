#!/usr/bin/env sh
# Normalize an out-of-tree module for a vendor kernel and verify the ABI.
#
# Required env:
#   KB_VERMAGIC       exact vermagic from the target kernel / vendor module
#   KB_INIT_OFF       struct module init relocation offset (from vendor module)
#   KB_EXIT_OFF       struct module exit relocation offset (from vendor module)
#   KB_MODVERSIONS    1 if CONFIG_MODVERSIONS=y, else 0
#   KB_IMAGE          uncompressed Image (required when KB_MODVERSIONS=1)
#   KB_KALLSYMS       /proc/kallsyms dump (required when KB_MODVERSIONS=1)
# Optional:
#   KB_THIS_MODULE_SIZE default 0x400
#   OBJCOPY/READELF/PYTHON, KB_TMP
#
# Usage: postprocess_common.sh <module.ko> <modname> [vermagic]
set -eu

KO=$1
MOD=$2
VERMAGIC=${3:-${KB_VERMAGIC:-}}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OBJCOPY=${OBJCOPY:-llvm-objcopy}
READELF=${READELF:-llvm-readelf}
PY=${PYTHON:-python3}
IMG=${KB_IMAGE:-}
KS=${KB_KALLSYMS:-}
INIT_OFF=${KB_INIT_OFF:-0x190}
EXIT_OFF=${KB_EXIT_OFF:-0x3c0}
SIZE=${KB_THIS_MODULE_SIZE:-0x400}
MODVERSIONS=${KB_MODVERSIONS:-1}
T=$(mktemp -d "${KB_TMP:-${TMPDIR:-/tmp}}/kb-post.XXXXXX")
trap 'rm -rf "$T"' EXIT

[ -f "$KO" ] || { echo "FAIL missing $KO" >&2; exit 1; }
[ -n "$VERMAGIC" ] || { echo "FAIL KB_VERMAGIC not set" >&2; exit 1; }

# 1. Strip debug; keep .symtab for the __versions gate.
$OBJCOPY --strip-debug "$KO" "$KO.strip"
mv -f "$KO.strip" "$KO"

# 2. Verify/normalize the module symbol-version table when the target uses
#    CONFIG_MODVERSIONS=y. Empty/missing __versions is fatal on a kernel with
#    "# CONFIG_MODULE_FORCE_LOAD is not set" (silent -ENOEXEC).
if [ "$MODVERSIONS" = "1" ]; then
    [ -n "$IMG" ] && [ -f "$IMG" ] || { echo "FAIL KB_IMAGE required for MODVERSIONS" >&2; exit 1; }
    [ -n "$KS" ] && [ -f "$KS" ] || { echo "FAIL KB_KALLSYMS required for MODVERSIONS" >&2; exit 1; }
    $READELF -S "$KO" | grep -qE '^ *\[[ 0-9]+\] +__versions[[:space:]]' \
        || { echo "FAIL $MOD: __versions missing (silent -ENOEXEC)" >&2; exit 1; }
    "$PY" "$ROOT/tools/verify_versions.py" --image "$IMG" --kallsyms "$KS" "$KO" \
        || { echo "FAIL $MOD: __versions gate rejected the artifact" >&2; exit 1; }
fi

# 3. Normalize .modinfo (prefixes, vermagic/name/depends/license, srcversion).
$OBJCOPY --dump-section .modinfo="$T/$MOD.modinfo" "$KO"
"$PY" "$ROOT/tools/modinfo_fix.py" "$T/$MOD.modinfo" "$MOD" "$VERMAGIC"
$OBJCOPY --update-section .modinfo="$T/$MOD.modinfo" "$KO"

# 4. Patch struct module relocations by symbol name.
"$PY" "$ROOT/tools/fix_this_module_relocs.py" --init-off "$INIT_OFF" --exit-off "$EXIT_OFF" "$KO"

# 5. Hard checks.
SZ=$($READELF -S "$KO" | awk '/\.gnu\.linkonce\.this_module /{print $6; exit}')
[ "$SZ" = "$SIZE" ] || { echo "FAIL this_module size=$SZ expected $SIZE" >&2; exit 1; }
INIT=$($READELF -r "$KO" | awk '/rela.gnu.linkonce.this_module/{f=1; next} f && /^$/{exit} f && /init_module \+ 0$/{print $1; exit}')
EXIT=$($READELF -r "$KO" | awk '/rela.gnu.linkonce.this_module/{f=1; next} f && /^$/{exit} f && /cleanup_module \+ 0$/{print $1; exit}')
[ "$INIT" = "$(printf '%016x' "$((INIT_OFF))")" ] || { echo "FAIL init reloc=$INIT expected $INIT_OFF" >&2; exit 1; }
[ "$EXIT" = "$(printf '%016x' "$((EXIT_OFF))")" ] || { echo "FAIL exit reloc=$EXIT expected $EXIT_OFF" >&2; exit 1; }

MI=$($READELF -p .modinfo "$KO" | sed -n 's/^ *\[[^]]*\] *//p')
printf '%s\n' "$MI" | grep -qx "vermagic=$VERMAGIC" || { echo "FAIL vermagic" >&2; exit 1; }
printf '%s\n' "$MI" | grep -qx "name=$MOD" || { echo "FAIL name=$MOD" >&2; exit 1; }
printf '%s\n' "$MI" | grep -qx 'depends=' || { echo "FAIL depends=" >&2; exit 1; }
printf '%s\n' "$MI" | grep -q '^license=.*GPL' || { echo "FAIL license" >&2; exit 1; }

if [ "$MODVERSIONS" = "1" ]; then
    "$PY" "$ROOT/tools/verify_versions.py" --image "$IMG" --kallsyms "$KS" "$KO" \
        || { echo "FAIL $MOD: final __versions gate rejected the artifact" >&2; exit 1; }
fi

echo "OK $MOD: this_module=$SZ init=$INIT exit=$EXIT"
sha256sum "$KO"
