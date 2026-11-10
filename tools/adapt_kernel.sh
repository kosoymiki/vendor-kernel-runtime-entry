#!/usr/bin/env bash
# Adapt the H2 runtime-entry module to a different vendor kernel.
# NOT a universal binary. Derives config/ABI/vermagic from the target, builds
# with the target kbuild tree, normalizes the module and gates the artifact.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODULE_SRC=${KB_MODULE_SRC:-$ROOT/kernel}; PROFILE=${KB_PROFILE:-}
KERNEL_SRC=${KB_KERNEL_SRC:-}; KERNEL_OUT=${KB_KERNEL_OUT:-}
KERNEL_IMAGE=${KB_IMAGE:-}; KALLSYMS=${KB_KALLSYMS:-}
VENDOR_KO=${KB_VENDOR_KO:-}; VERMAGIC=${KB_VERMAGIC:-}
INIT_OFF=${KB_INIT_OFF:-}; EXIT_OFF=${KB_EXIT_OFF:-}; LINK_BASE=${KB_LINK_BASE:-}
MODNAME=${KB_MODNAME:-kbootstrap}; JOBS=${JOBS:-1}; QEMU=${KB_QEMU:-0}
for ((i=1;i<=$#;i++)); do if [ "${!i}" = "--profile" ]; then j=$((i+1)); PROFILE=${!j}; fi; done
if [ -n "$PROFILE" ]; then [ -f "$PROFILE" ] || { echo "no profile $PROFILE" >&2; exit 2; }; . "$PROFILE"; fi
KERNEL_SRC=${KB_KERNEL_SRC:-$KERNEL_SRC}; KERNEL_OUT=${KB_KERNEL_OUT:-${KERNEL_OUT:-$KERNEL_SRC}}
KERNEL_IMAGE=${KB_IMAGE:-$KERNEL_IMAGE}; KALLSYMS=${KB_KALLSYMS:-$KALLSYMS}
VENDOR_KO=${KB_VENDOR_KO:-$VENDOR_KO}; VERMAGIC=${KB_VERMAGIC:-$VERMAGIC}
INIT_OFF=${KB_INIT_OFF:-$INIT_OFF}; EXIT_OFF=${KB_EXIT_OFF:-$EXIT_OFF}; LINK_BASE=${KB_LINK_BASE:-$LINK_BASE}
while [ $# -gt 0 ]; do case "$1" in
  --profile) PROFILE=$2; shift 2;; --kernel-src) KERNEL_SRC=$2; shift 2;;
  --kernel-out) KERNEL_OUT=$2; shift 2;; --image) KERNEL_IMAGE=$2; shift 2;;
  --kallsyms) KALLSYMS=$2; shift 2;; --vendor-ko) VENDOR_KO=$2; shift 2;;
  --vermagic) VERMAGIC=$2; shift 2;; --init-off) INIT_OFF=$2; shift 2;;
  --exit-off) EXIT_OFF=$2; shift 2;; --link-base) LINK_BASE=$2; shift 2;;
  --module-src) MODULE_SRC=$2; shift 2;; --qemu) QEMU=1; shift;;
  -h|--help) sed -n '2,7p' "$0"; exit 0;; *) echo "unknown argument: $1" >&2; exit 2;;
esac; done
MODULE_CFLAGS=${KB_MODULE_CFLAGS:-}
die(){ echo "FAIL: $*" >&2; exit 1; }; cfg(){ awk -F= -v k="$1" '$1==k{print $2;exit}' "$CFG"; }
[ -n "$KERNEL_SRC" ] && [ -d "$KERNEL_SRC" ] || die "KB_KERNEL_SRC missing"
[ -d "$KERNEL_OUT" ] || die "KB_KERNEL_OUT missing"
CFG=${KB_CONFIG:-$KERNEL_OUT/.config}; [ -f "$CFG" ] || CFG=$KERNEL_SRC/.config
[ -f "$CFG" ] || die "no .config (set KB_CONFIG to the running kernel config)"
MODULES=$(cfg CONFIG_MODULES); SIGF=$(cfg CONFIG_MODULE_SIG_FORCE); KPROBES=$(cfg CONFIG_KPROBES)
KALL=$(cfg CONFIG_KALLSYMS_ALL); CFI=$(cfg CONFIG_CFI_CLANG); MV=$(cfg CONFIG_MODVERSIONS)
[ "$MODULES" = y ] || die "CONFIG_MODULES != y: no LKM entry"
[ "$SIGF" != y ] || die "CONFIG_MODULE_SIG_FORCE=y: unsigned load blocked (no bypass)"
[ "$KPROBES" = y ] || die "CONFIG_KPROBES != y: symbol resolution unavailable"
[ "$KALL" = y ] || echo "WARN: CONFIG_KALLSYMS_ALL != y; non-exported symbols may not resolve" >&2
if [ "${KB_FORCE_CFI:-}" = 1 ] || [ "$CFI" = y ]; then KB_CFI=1; else KB_CFI=0; fi
if [ "${KB_FORCE_MODVERSIONS:-}" = 1 ] || [ "$MV" = y ]; then KB_MODVERSIONS=1; else KB_MODVERSIONS=0; echo "WARN: CONFIG_MODVERSIONS != y; versions skipped" >&2; fi
if [ -n "$VENDOR_KO" ] && [ -f "$VENDOR_KO" ]; then
  VMI=$(readelf -p .modinfo "$VENDOR_KO" 2>/dev/null | sed -n 's/.*vermagic=//p' | head -1)
  [ -n "$VERMAGIC" ] || VERMAGIC=$VMI
  REL=$(readelf -rW "$VENDOR_KO" 2>/dev/null | awk '/rela.gnu.linkonce.this_module/{f=1;next} f&&/^$/{exit} f&&/init_module \+ 0$/{print $1;exit}')
  REX=$(readelf -rW "$VENDOR_KO" 2>/dev/null | awk '/rela.gnu.linkonce.this_module/{f=1;next} f&&/^$/{exit} f&&/cleanup_module \+ 0$/{print $1;exit}')
  [ -n "$INIT_OFF" ] || INIT_OFF=$REL; [ -n "$EXIT_OFF" ] || EXIT_OFF=$REX
  SZ=$(readelf -SW "$VENDOR_KO" 2>/dev/null | awk '/\.gnu\.linkonce\.this_module /{print $6;exit}')
  [ -n "$SZ" ] && KB_THIS_MODULE_SIZE=$SZ
fi
[ -n "$VERMAGIC" ] || die "KB_VERMAGIC required"
[ -n "$INIT_OFF" ] || die "KB_INIT_OFF required"
[ -n "$EXIT_OFF" ] || die "KB_EXIT_OFF required"
KB_THIS_MODULE_SIZE=${KB_THIS_MODULE_SIZE:-0x400}
if [ "$KB_MODVERSIONS" = 1 ]; then
  [ -n "$KERNEL_IMAGE" ] && [ -f "$KERNEL_IMAGE" ] || die "KB_IMAGE required for MODVERSIONS"
  [ -n "$KALLSYMS" ] && [ -f "$KALLSYMS" ] || die "KB_KALLSYMS required for MODVERSIONS"
  if [ ! -s "$KERNEL_OUT/Module.symvers" ] || [ "${KB_REGENERATE_SYMVERS:-0}" = 1 ]; then
    [ -n "$LINK_BASE" ] || die "KB_LINK_BASE required to reconstruct Module.symvers"
    T=$(mktemp -d "${KB_TMP:-${TMPDIR:-/tmp}}/kb-adapt.XXXXXX")
    awk '/__ksymtab_/ {n=$3; sub("__ksymtab_","",n); print n}' "$KALLSYMS" | sort -u > "$T/symbols.txt"
    python3 "$ROOT/tools/symvers_from_image.py" --image "$KERNEL_IMAGE" --kallsyms "$KALLSYMS" \
      --symbols "$T/symbols.txt" --link-base "$LINK_BASE" --out "$KERNEL_OUT/Module.symvers"
  fi
fi
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}" \
  KCFLAGS="-Wno-error $MODULE_CFLAGS" HOSTCFLAGS=-Wno-error \
  KBUILD_MODPOST_WARN=1 KB_CFI="$KB_CFI" -j"$JOBS" M="$MODULE_SRC" modules
KO=$MODULE_SRC/$MODNAME.ko; [ -f "$KO" ] || die "expected $KO"
python3 "$ROOT/tools/fix_symtab_shndx.py" "$KO"
KB_IMAGE="$KERNEL_IMAGE" KB_KALLSYMS="$KALLSYMS" KB_VERMAGIC="$VERMAGIC" \
KB_INIT_OFF="$INIT_OFF" KB_EXIT_OFF="$EXIT_OFF" KB_MODVERSIONS="$KB_MODVERSIONS" \
KB_THIS_MODULE_SIZE="$KB_THIS_MODULE_SIZE" \
sh "$ROOT/tools/postprocess_common.sh" "$KO" "$MODNAME" "$VERMAGIC"
DIST=${KB_DIST:-$ROOT/dist}/$(basename "$KERNEL_OUT")-$MODNAME; mkdir -p "$DIST"
cp -f "$KO" "$DIST/$MODNAME.ko"; cp -a "$ROOT/module/." "$DIST/module/"; mkdir -p "$DIST/module/bin"
cp -f "$KO" "$DIST/module/bin/$MODNAME.ko"; ( cd "$DIST/module" && zip -qr ../module.zip . )
SHA=$(sha256sum "$KO" | awk '{print $1}')
cat > "$DIST/manifest.json" <<JSON
{"profile":"${KB_PROFILE_NAME:-custom}","kernel_src":"$KERNEL_SRC","kernel_out":"$KERNEL_OUT",
"config":{"MODULES":"$MODULES","KPROBES":"$KPROBES","CFI_CLANG":"$CFI","MODVERSIONS":"$MV","MODULE_SIG_FORCE":"$SIGF"},
"vermagic":"$VERMAGIC","abi":{"init_off":"$INIT_OFF","exit_off":"$EXIT_OFF","this_module_size":"$KB_THIS_MODULE_SIZE"},
"sha256":"$SHA","artifacts":{"ko":"$MODNAME.ko","module_zip":"module.zip"}}
JSON
echo "DONE $DIST"; echo "sha256 $SHA"
if [ "$QEMU" = 1 ]; then KB_IMAGE="$KERNEL_IMAGE" KB_KO="$KO" bash "$ROOT/tests/qemu_payload_gate.sh"; fi
