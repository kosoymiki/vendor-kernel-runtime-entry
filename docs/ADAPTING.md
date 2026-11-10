# Adapting to another vendor kernel — full guide with reflection

This is the long version. The root `README.md` is the entry point. This guide
walks the **whole user chain**: from “does my device qualify?” to
`cat /proc/kbootstrap` after install. Each step has goal, commands, expected
result, failure handling and evidence.

The builder does **not** make a universal binary. It takes facts from the target
and stops when a fact is missing.

---

# Part I. User-chain map

```
[0] Does the method apply?
      CONFIG_MODULES=y? MODULE_SIG_FORCE!=y? KPROBES=y?
      no -> stop; this method does not apply
[1] Collect target facts
      config, kallsyms, Image, vendor .ko, version
[2] Prepare build tree
      source + O-tree + KB_CONFIG (running config)
[3] ABI oracle
      vermagic, init/exit offsets, CFI mode
[4] __versions / CRCs
      exact Module.symvers or Image + kallsyms + link base
[5] CFI / MODVERSIONS
[6] Fill profile
[7] Run builder
      build -> symtab fix -> postprocess -> gates -> dist
[8] QEMU gate
      insmod_rc=0, payload_result=42
[9] Device install
      KernelSU zip -> reboot -> /proc/kbootstrap
[10] Rollback/maintenance
      reboot, no rmmod; rebuild after kernel update
```

Three user scenarios:

- **A. Same reference device** (RMX3709 5.10.236): install `module.zip`, reboot,
  `cat /proc/kbootstrap`; no build.
- **B. Another device with a vendor `.ko`**: clone -> collect -> profile ->
  build -> QEMU -> install.
- **C. Another device without a vendor `.ko`**: same, but derive offsets with
  QEMU+gdb; needs the exact Image.

---

# Part II. Step by step, with reflection

## Step 0. Decide whether the method applies

### Why
Do not build for hours if the kernel has no LKM entry.

```sh
mkdir -p target
su -c 'zcat /proc/config.gz' > target/config
grep -E '^CONFIG_(MODULES|MODULE_SIG_FORCE|KPROBES|KALLSYMS|KALLSYMS_ALL|MODVERSIONS|CFI_CLANG|LTO_CLANG|SHADOW_CALL_STACK|ARM64_BTI|ARM64_PTR_AUTH|STRICT_KERNEL_RWX|STRICT_MODULE_RWX|MODULE_UNLOAD|MODULE_SCMVERSION)=' target/config
```

| config | required | if absent |
|---|---|---|
| `MODULES=y` | yes | no LKM entry; method does not apply |
| `MODULE_SIG_FORCE` | not `y` | unsigned module blocked; no bypass |
| `KPROBES=y` | yes | symbol-resolution path unavailable |
| `KALLSYMS_ALL=y` | recommended | non-exported symbols may fail |
| `MODVERSIONS=y` | determines CRC path | exact symvers/Image required |
| `CFI_CLANG=y` | determines bridge | `KB_CFI=1` |
| `MODULE_UNLOAD=y` | optional | module permanent; still do not rmmod |

**Reflection**
- Why: this is a gate, not a preference.
- If skipped: “module loading disabled” or silent `-ENOEXEC`.
- Observe: exact config values.
- If not: record the symbol and stop; do not bypass signature/policy.
- Evidence: `target/config`, `uname -a`.

---

## Step 1. Collect target facts

### Why
The builder needs exact vermagic, ABI and CRCs; approximations do not load.

```sh
tools/collect_target.sh target/
# or manually:
su -c 'cat /proc/kallsyms' > target/kallsyms
su -c 'cat /proc/version' > target/version
uname -a > target/uname
sha256sum target/config target/kallsyms target/Image 2>/dev/null
```

Extract the uncompressed `Image` from `boot.img`/`vendor_boot.img` with your
device tools (`magiskboot`, `unpack_bootimg`, vendor dumper). This is
device-specific and intentionally not automated.

Copy a known-good vendor `.ko` that loads on the device (from
`/vendor/lib/modules` or a vendor/KernelSU module directory) into `target/`.

```sh
readelf -p .modinfo target/vendor.ko | grep -E 'vermagic|name='
readelf -SW target/vendor.ko | grep .gnu.linkonce.this_module
readelf -rW target/vendor.ko | sed -n '/this_module/,/^$/p'
readelf -sW target/vendor.ko | grep -E '__cfi|cfi_jt'
```

**Reflection**
- Why: Image/kallsyms are CRC ground truth; vendor `.ko` is the ABI oracle.
- If skipped: `verify_versions` fails; init offset is wrong.
- Observe: vermagic, init/exit offsets, `__cfi_*` presence.
- If not: no vendor `.ko` -> QEMU+gdb (step 3B); no Image -> exact `Module.symvers` (step 4A).
- Evidence: hashes, readelf output, vendor `.ko` provenance.

---

## Step 2. Prepare the build tree

### Why
You need headers/kbuild from the same kernel family.

```sh
export KERNEL_SRC=$HOME/my-kernel
export KERNEL_OUT=$HOME/my-kernel-out
mkdir -p "$KERNEL_OUT"
cp target/config "$KERNEL_OUT/.config"

make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error modules_prepare
```

### Why `KB_CONFIG` is separate
`olddefconfig` can drop vendor options because the local tree/compiler differ.
- `KB_CONFIG` = running config, used for CFI/MODVERSIONS/KPROBES decisions.
- `KERNEL_OUT/.config` = build config, used only to compile.

**Reflection**
- Why: build/running config mismatch is the usual “CFI is in the kernel but the
  module was built without it” bug.
- If skipped: module lacks bridge/CRCs; loader rejects it.
- Observe: `modules_prepare` rc=0; generated headers.
- If not: `KCFLAGS=-Wno-error`; do not disable CFI/LTO globally.
- Evidence: prepare log, both configs.

---

## Step 3. ABI oracle: vermagic and `struct module`

### Why
The vendor kernel reads `mod->init`/`mod->exit` at fixed offsets; the local
tree may lay out `struct module` differently.

### 3A. With a vendor `.ko`

```sh
readelf -rW target/vendor.ko | sed -n '/this_module/,/^$/p'
# init_module + 0 -> 0x190 (reference)
# cleanup_module + 0 -> 0x3c0 (reference)
```

### 3B. Without a vendor `.ko` — QEMU+gdb

```
break do_init_module
# inspect mod->init; disassemble do_init_module for the offset
```

Pass `--init-off`/`--exit-off` or put them in the profile. Relocations are
patched **by symbol name**, not by raw offset.

**Reflection**
- Why: a wrong offset makes the kernel read NULL/garbage; init is not called.
- If skipped: `insmod rc=0` but no code, or panic.
- Observe: relocation offsets and `this_module` size.
- If not: go back to vendor `.ko`/gdb; do not use a “similar” offset.
- Evidence: readelf output, QEMU gdb log, profile.

---

## Step 4. `__versions` / CRCs

### Why
With `MODVERSIONS=y` and `MODULE_FORCE_LOAD` unset, a missing/empty
`__versions` can silently fail with `-ENOEXEC`.

### 4A. Exact `Module.symvers`
Put the `Module.symvers` from the exact build into
`$KERNEL_OUT/Module.symvers`. Best path.

### 4B. Reconstruct from Image

```sh
KB_REGENERATE_SYMVERS=1 KB_LINK_BASE=0x... \
  tools/adapt_kernel.sh --profile ...
```

`symvers_from_image.py` decodes `__ksymtab/__kcrctab`. `KB_LINK_BASE` is the
link-time `_head`/`_text`; take it from a matching `vmlinux`
(`nm vmlinux | grep ' _head\| _text'`) or vendor metadata. Do not guess.

**Reflection**
- Why: CRC is an ABI hash; a wrong table is rejected.
- If skipped: `no symbol version` / `disagrees about version`.
- Observe: `verify_versions.py` PASS, imports count, `crc_bad=0`.
- If not: do not delete `__versions`; use exact symvers/Image.
- Evidence: symvers provenance/hash, verify output.

---

## Step 5. CFI / MODVERSIONS

### Why
A CFI kernel's `cfi_init()` looks for `__cfi_check` and `__cfi_jt_*`; without the
bridge the module init may not run and rmmod can panic.

```sh
grep -E '^CONFIG_(CFI_CLANG|LTO_CLANG|SHADOW_CALL_STACK|ARM64_BTI|ARM64_PTR_AUTH|MODVERSIONS)=' target/config
```

Set `KB_CFI=1` (or `KB_FORCE_CFI=1`) and `KB_MODVERSIONS=1` (or
`KB_FORCE_MODVERSIONS=1`) in the profile as appropriate.

**Reflection**
- Why: the bridge is a module-local ABI bridge; it disables CFI **for this
  module**, not for the kernel.
- If skipped: CFI failure or init not called.
- Observe: `cfi: not using shadow for module ...` (expected), then init.
- If not: verify `__cfi_jt_*` in the `.ko`; do not disable CFI globally.
- Evidence: config, readelf symbols, QEMU log.

---

## Step 6. Profile

### Why
The profile records every target fact so the build is reproducible.

```sh
cp profiles/rmx3709-5.10.236/profile.env profiles/my-device/profile.env
```

```sh
KB_PROFILE_NAME="my-device"
KB_KERNEL_SRC="$HOME/my-kernel"
KB_KERNEL_OUT="$HOME/my-kernel-out"
KB_CONFIG="$HOME/target/config"
KB_IMAGE="$HOME/target/Image"
KB_KALLSYMS="$HOME/target/kallsyms"
KB_VENDOR_KO="$HOME/target/vendor.ko"   # empty if using offsets
KB_VERMAGIC="<exact>"
KB_INIT_OFF=0x...
KB_EXIT_OFF=0x...
KB_LINK_BASE=0x...                      # only for symvers reconstruction
KB_MODULE_CFLAGS=""                     # reference audit off
```

**Reflection**
- Why: one file = reproducibility; no remembered flags.
- If skipped: builder uses build config and may miss CFI.
- Observe: no empty required fields.
- If not: `tools/adapt_kernel.sh --help`; check each path.
- Evidence: profile (no secrets), Image/kallsyms hashes.

---

## Step 7. Build

```sh
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

Inside:
1. config gates;
2. CFI/MODVERSIONS choice;
3. vermagic/offsets;
4. symvers (exact or Image);
5. kbuild;
6. `fix_symtab_shndx.py`;
7. `postprocess_common.sh` (versions/modinfo/relocs/checks);
8. `dist/...`;
9. QEMU gate if requested.

**Reflection**
- Why: the builder is the single entry; do not hand-roll a module.
- If skipped: missing symtab fix/CRC/modinfo.
- Observe: `PASS`, `OK kbootstrap: this_module=0x400 init=0x190 exit=0x3c0`, sha256.
- If not: read the exact error; do not remove a gate.
- Evidence: build log, `manifest.json`, sha256.

---

## Step 8. QEMU gate

```sh
KB_IMAGE=$HOME/target/Image \
KB_KO=$PWD/kernel/kbootstrap.ko \
BUSYBOX=$HOME/ae-qemu-gate/bin/busybox \
tests/qemu_payload_gate.sh
```

Asserts: `insmod_rc=0`, module live, `/proc/kbootstrap`, `payload_result=42`,
no CFI failure at init.

**Reflection**
- Why: device load is irreversible risk; QEMU catches ABI/CFI errors.
- If skipped: panic/brick on device.
- Observe: `PASS qemu payload gate`.
- If not: `cfi: not using shadow` is normal; CFI failure is not; inspect serial.
- Evidence: QEMU log, KO sha.

---

## Step 9. Device install

```sh
# backup boot/vendor_boot first
ksud module install dist/<profile>/module.zip
# reboot
cat /proc/kbootstrap
```

**Reflection**
- Why: KernelSU package delivers; reboot activates.
- If skipped backup: no rollback.
- Observe: `/proc/kbootstrap` with `payload_result=42`.
- If not: reboot; do not rmmod; inspect `load.log`/dmesg; return to QEMU.
- Evidence: dmesg, proc output, module zip sha.

---

## Step 10. Rollback / maintenance

- Rollback = reboot without the module or with restored boot.
- **Do not `rmmod`** until unload/CFI is proven.
- After a kernel update, rebuild: vermagic/CRC/offsets change.
- Never reuse a `.ko` across kernels.

---

# Part III. User scenarios

## A. Reference device
1. Download release `module.zip`.
2. `ksud module install module.zip`.
3. Reboot.
4. `cat /proc/kbootstrap`.
5. If missing: `load.log`, dmesg; verify the kernel is the reference one.

## B. Another device + vendor `.ko`
1. `collect_target`.
2. Build tree + `KB_CONFIG`.
3. `readelf` vendor `.ko` -> vermagic/offsets.
4. Exact `Module.symvers` or Image+link base.
5. Profile.
6. `adapt_kernel.sh --qemu`.
7. Install.

## C. Another device without vendor `.ko`
1–2 as B.
3. QEMU+gdb: `do_init_module`, `mod->init` offset.
4. `--init-off/--exit-off`.
5. Continue as B.

## D. CVE patch
1. Upstream fix commit -> function/condition.
2. Map to vendor symbol.
3. Kprobe guard or trampoline.
4. Build.
5. QEMU with a reproducer.
6. Install.
Details: `docs/CVE-PATCHING.md`.

---

# Part IV. Troubleshooting with reflection

| symptom | why | action | verify |
|---|---|---|---|
| `no symbol version` / silent `-ENOEXEC` | missing/empty `__versions`; force-load off | exact symvers/Image | `verify_versions.py` PASS |
| `disagrees about version` | wrong CRCs | target Image/kallsyms | `crc_bad=0` |
| `no version magic` | wrong vermagic | vendor `.ko`/`/proc/version` | `readelf -p .modinfo` |
| CFI failure | missing/wrong bridge | `KB_CFI=1`, bridge objects | `__cfi_jt_*` |
| Oops `load_module+0x20d4` | `SHN_XINDEX` without table | `fix_symtab_shndx.py` | symtab fixed/N |
| init not called | wrong `struct module` offset | vendor `.ko`/gdb | gdb `mod->init` |
| IABT payload (`8600000f`) | NX page | `set_memory_x` + flush | `set_memory_x_rc=0` |
| `register_kprobe` unresolved | no KPROBES/kallsyms | method does not apply | config |
| signature error | `SIG_FORCE` | no bypass | config |

---

# Part V. Checklist and prohibitions

- [ ] `MODULES=y`, `SIG_FORCE!=y`, `KPROBES=y`
- [ ] config/kallsyms/Image/vendor `.ko`
- [ ] build tree + `KB_CONFIG`
- [ ] vermagic/offsets
- [ ] exact symvers or Image+link base
- [ ] `KB_CFI`/`KB_MODVERSIONS`
- [ ] profile filled
- [ ] builder PASS
- [ ] QEMU PASS
- [ ] backup/rollback
- [ ] install; no rmmod

**Do not:** ship a foreign `.ko`; remove `__versions`; reuse CRCs/offsets;
disable CFI globally; bypass authentication/DRM/CAPTCHA/paywalls.

---

# Part VI. Probe modules: kpatch and mglru

The same builder can build the probe modules:

```sh
KB_MODULE_SRC=$PWD/modules/kpatch KB_MODNAME=kpatch \
  tools/adapt_kernel.sh --profile profiles/my-device/profile.env

KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/my-device/profile.env
```

`kpatch` proves save/patch/restore of a live function. It uses
`aarch64_insn_patch_text_nosync`; this is **not SMP-safe** (single-CPU QEMU
only). For SMP, use `aarch64_insn_patch_text`/stop_machine, verify under load,
and keep a restore path. `mglru` is a Stage A/B probe, not full MGLRU; see
`docs/MGLRU.md`. QEMU first; no device load before the gate; no `rmmod` until
restore/unload is proven.
