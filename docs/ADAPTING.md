# Adapting to another vendor kernel — detailed guide

This is the long version. Read it before building. The short version is in the
root `README.md`.

The builder is **not** a universal binary generator. It takes facts from the
target kernel and builds/normalizes a module for that exact target. If a fact
is missing, it stops.

---

## 0. What you need

| item | why |
|---|---|
| matching kernel source tree | kbuild, headers, config |
| build output tree with `.config` | compile flags/headers |
| running kernel config `/proc/config.gz` | feature gates (CFI, MODVERSIONS, KPROBES, …) |
| uncompressed `Image` | reconstruct symbol CRCs / verify ABI |
| `/proc/kallsyms` dump | symbol addresses/names, CRC tables |
| a known-good vendor `.ko` | ABI oracle: vermagic, init/exit offsets, CFI mode |
| `clang`, `ld.lld`, `llvm-objcopy`, `llvm-readelf`, `make`, `python3`, `zip` | toolchain |
| `qemu-system-aarch64` + static busybox | safe gate before device |

If you do not have a vendor `.ko`, you can derive ABI offsets with QEMU+gdb
(see §4), but the builder needs explicit `--init-off/--exit-off`.

---

## 1. Collect target facts

```sh
tools/collect_target.sh target/
# target/config, target/kallsyms, target/version, target/uname
```

Root is required for `/proc/config.gz` and `/proc/kallsyms` on Android.

**Image:** extract the uncompressed kernel from `boot.img`/`vendor_boot.img`
with your device's tools (`magiskboot`, `unpack_bootimg`, vendor dumper, etc.).
Keep the exact `Image` that is running. This extraction is device-specific and
intentionally not automated. Do not use an Image from a different build.

**Vendor `.ko` oracle:** take a module that provably loads on the device, e.g.
from `/vendor/lib/modules` or a vendor/KernelSU module directory. Check:

```sh
readelf -p .modinfo vendor.ko | grep -E 'vermagic|name='
readelf -SW vendor.ko | grep .gnu.linkonce.this_module
readelf -rW vendor.ko | sed -n '/this_module/,/^$/p'
readelf -sW vendor.ko | grep -E '__cfi|cfi_jt'
```

The `init_module`/`cleanup_module` relocation offsets are your ABI ground
truth for `struct module`. On the reference kernel they are `0x190`/`0x3c0`,
size `0x400`.

---

## 2. Build tree

You need a tree that matches the running kernel closely enough for headers and
kbuild. It does not have to be the vendor's full build, but it must be the same
kernel version/config family.

```sh
# example: target source and output tree
export KERNEL_SRC=$HOME/my-kernel
export KERNEL_OUT=$HOME/my-kernel-out
mkdir -p "$KERNEL_OUT"

# exact running config (from target/config)
cp target/config "$KERNEL_OUT/.config"

make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error modules_prepare
```

`olddefconfig` may drop vendor options because the local tree/compiler differs.
That is why the profile separates **`KB_CONFIG`** (running config, used for
feature decisions) from the build tree `.config` (used to compile). The builder
reads `KB_CONFIG` for `CFI_CLANG`, `MODVERSIONS`, `KPROBES`, `MODULES`.

If `modules_prepare` fails on a warning-as-error, use `KCFLAGS=-Wno-error` (and
optionally a narrower `-Wno-error=<warning>`); do not disable CFI/LTO just to
make it compile.

---

## 3. Vermagic

Take the exact string from the vendor `.ko` `.modinfo` or from
`/proc/version` + the target build. It includes `SMP`, `preempt`, `mod_unload`,
`modversions` and the architecture suffix.

Wrong vermagic → loader rejects with “no version magic”. The builder normalizes
`.modinfo` but does not invent the string.

---

## 4. `struct module` ABI (init/exit offsets)

The local tree may lay out `struct module` differently (CFI, LTO, config
options). The running kernel reads `mod->init`/`mod->exit` at fixed offsets.

**With a vendor `.ko`:**

```sh
readelf -rW vendor.ko | sed -n '/this_module/,/^$/p'
# init_module + 0 -> offset
# cleanup_module + 0 -> offset
```

**Without a vendor `.ko`:** QEMU + gdb on the exact Image:

```
break do_init_module
# inspect mod and mod->init; disassemble do_init_module to see the offset
```

Then pass `--init-off`/`--exit-off` (or set them in the profile). The builder
patches relocations **by symbol name** (`fix_this_module_relocs.py`), not by a
raw hardcoded offset.

---

## 5. `__versions` / CRCs

If `CONFIG_MODVERSIONS=y`, a missing/empty `__versions` can fail silently with
`-ENOEXEC` because `CONFIG_MODULE_FORCE_LOAD` is not set. You have two options:

### A. Exact `Module.symvers`

Use the `Module.symvers` from the exact kernel build. Put it in
`$KERNEL_OUT/Module.symvers`. Best case; no reconstruction needed.

### B. Reconstruct from `Image` + kallsyms

The builder can run `tools/symvers_from_image.py` when
`KB_REGENERATE_SYMVERS=1` and `KB_LINK_BASE` is set. It decodes
`__ksymtab`/`__kcrctab` from the Image.

`KB_LINK_BASE` is the link-time address of `_head`/`_text` for the Image. It is
device/build-specific. Get it from a matching `vmlinux` (`nm vmlinux | grep
' _head\| _text'`) or from your vendor build metadata. If you cannot establish
it, use an exact `Module.symvers`; do not guess.

The builder gates the final module with `tools/verify_versions.py`: every
import must be present, exported, and have the exact CRC. A wrong table is a
hard failure.

---

## 6. CFI

Check the running config:

```sh
grep -E 'CONFIG_(CFI_CLANG|LTO_CLANG|SHADOW_CALL_STACK|ARM64_BTI|ARM64_PTR_AUTH)' target/config
```

If `CONFIG_CFI_CLANG=y`, build with `KB_CFI=1` (the profile can set
`KB_FORCE_CFI=1` when the local build tree config is not the running config).
The bridge provides:

- `init_module.cfi_jt` / `cleanup_module.cfi_jt` → `bti c; b <target>`
- `__cfi_jt_init_module` / `__cfi_jt_cleanup_module` → pointers to those
- `__cfi_check` → `ret`
- `__ubsan_handle_cfi_check_fail_abort` → panic stub

The vendor loader's `cfi_init()` finds these and rewrites `mod->init/exit`.
This accepts the module but **disables CFI checks for this module**. That is a
module-local tradeoff. Load only trusted code.

If the target has no CFI, `KB_CFI=0`; the bridge is not linked.

---

## 7. Profile

Copy the reference profile and edit every value:

```sh
cp profiles/rmx3709-5.10.236/profile.env profiles/my-device/profile.env
```

Typical values:

```sh
KB_PROFILE_NAME="my-device"
KB_KERNEL_SRC="$HOME/my-kernel"
KB_KERNEL_OUT="$HOME/my-kernel-out"
KB_CONFIG="$HOME/target/config"       # running config
KB_IMAGE="$HOME/target/Image"         # exact Image
KB_KALLSYMS="$HOME/target/kallsyms"
KB_VENDOR_KO="$HOME/target/vendor.ko" # ABI oracle (or leave empty + offsets)
KB_VERMAGIC="<exact>"
KB_INIT_OFF=0x...
KB_EXIT_OFF=0x...
KB_LINK_BASE=0x...                    # only if reconstructing symvers
KB_MODULE_CFLAGS=""                   # reference audit off for portability
```

The builder reads `KB_CONFIG` for feature checks, `KERNEL_OUT/.config` for
compilation.

---

## 8. Build + normalize + gate

```sh
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

The builder:

1. validates `MODULES=y`, `MODULE_SIG_FORCE != y`, `KPROBES=y`;
2. detects CFI/MODVERSIONS from `KB_CONFIG`;
3. reads vermagic/offsets from `KB_VENDOR_KO` (or explicit args);
4. uses exact `Module.symvers` or reconstructs from Image/kallsyms;
5. builds with the target kbuild tree (`KB_CFI` passed as a make variable);
6. runs `fix_symtab_shndx.py` (ld.lld SHN_XINDEX fix);
7. runs `postprocess_common.sh` (versions gate, modinfo, relocations, final
   checks);
8. writes `dist/.../{kbootstrap.ko,module.zip,manifest.json}`;
9. optionally runs the QEMU payload gate.

If it stops, read the exact failure; do not “fix” by removing a gate.

---

## 9. QEMU gate (do this before the device)

```sh
KB_IMAGE=$HOME/target/Image \
KB_KO=$PWD/kernel/kbootstrap.ko \
BUSYBOX=$HOME/ae-qemu-gate/bin/busybox \
tests/qemu_payload_gate.sh
```

It boots the exact Image with a minimal initramfs and asserts:

- `insmod_rc=0`;
- module live;
- `/proc/kbootstrap` exists;
- `payload_result=42`;
- no CFI failure at init.

Expected harmless warning: `cfi: not using shadow for module kbootstrap`.
The gate does not prove unload/`rmmod` or hardware-specific behavior.

---

## 10. Install on the device

```sh
ksud module install dist/<profile>/module.zip
# reboot
cat /proc/kbootstrap
```

`post-fs-data.sh` loads the module; `service.sh` records status. **Do not
`rmmod`**: an unproven unload/CFI path can panic. Reboot is the rollback.

Before installing: back up boot/vendor_boot and know how to restore them. Use a
test device if possible.

---

## Troubleshooting

| symptom | cause | action |
|---|---|---|
| `no symbol version` / silent `-ENOEXEC` | missing/empty `__versions` | exact `Module.symvers` or reconstruct from Image; run `verify_versions.py` |
| `disagrees about version of symbol` | wrong CRCs | use the target Image/kallsyms; do not reuse another kernel's table |
| `no version magic` | wrong vermagic | read from target/vendor `.ko`; normalize `.modinfo` |
| CFI failure at load/rmmod | bridge missing/wrong | `KB_CFI=1`, include `kbootstrap_cfi*`, verify `__cfi_jt_*` |
| Oops `load_module+0x20d4` | `SHN_XINDEX` without `SYMTAB_SHNDX` | `fix_symtab_shndx.py` before postprocess |
| init not run | wrong `struct module` offset | derive from vendor `.ko`/gdb; patch by symbol |
| instruction abort at payload PC (`ESR 8600000f`) | NX page | `set_memory_x(page,1)` then flush again |
| `register_kprobe` unresolved | no `CONFIG_KPROBES`/kallsyms | this method does not apply |
| module signature error | `MODULE_SIG_FORCE=y` | no bypass; use a signed/vendor path |
| `CONFIG_MODULES=n` | no LKM entry | this method does not apply |

---

## Adaptation checklist

- [ ] exact running config collected (`KB_CONFIG`)
- [ ] exact Image + kallsyms collected
- [ ] vendor `.ko` oracle or QEMU/gdb offsets
- [ ] vermagic exact
- [ ] `Module.symvers` exact or Image+link base
- [ ] CFI mode known; `KB_CFI` correct
- [ ] `MODVERSIONS` mode known
- [ ] profile paths/offsets filled
- [ ] builder PASS (final gate)
- [ ] QEMU payload gate PASS
- [ ] boot backup + rollback plan
- [ ] device install only after QEMU; no `rmmod`

## Do not

- Do not ship a foreign `.ko`.
- Do not remove `__versions` to “make it load”.
- Do not reuse another kernel's CRC table or offsets.
- Do not disable CFI globally or patch the kernel just to load a module.
- Do not use this to bypass third-party authentication/DRM/CAPTCHA/paywalls.
