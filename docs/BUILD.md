# Build and install

## Prerequisites

- Target kernel source tree matching the running kernel (`KERNEL_SRC`).
- Build output tree (`KERNEL_OUT`) with `.config` and generated headers.
- Uncompressed `Image` from the device (`KERNEL_IMAGE`).
- `/proc/kallsyms` dump from the running kernel (`KB_KALLSYMS`).
- A known-good vendor `.ko` that loads on the target (ABI oracle), or explicit
  `--init-off`/`--exit-off`/`--vermagic` values verified with QEMU+gdb.
- `clang`, `ld.lld`, `llvm-objcopy`, `llvm-readelf`, `make`, `python3`, `zip`.
- For the QEMU gate: `qemu-system-aarch64`, a statically linked busybox.

## 1. Collect target facts

```sh
tools/collect_target.sh target/
# produces target/config, target/kallsyms, target/version, target/uname
```

Extract `Image` from the device's `boot.img` with your vendor tools; this is
device-specific and intentionally not automated. Keep the exact file used for
the running kernel.

## 2. Make a profile

Copy `profiles/rmx3709-5.10.236/profile.env` and change every value:

```sh
KB_PROFILE_NAME="my-device"
KB_KERNEL_SRC="$HOME/my-kernel"
KB_KERNEL_OUT="$HOME/my-kernel-out"
KB_IMAGE="$HOME/target/Image"
KB_KALLSYMS="$HOME/target/kallsyms"
KB_VENDOR_KO="$HOME/target/vendor-module.ko"   # ABI oracle
KB_VERMAGIC="..."                               # exact
KB_INIT_OFF=0x...                               # from vendor ko / gdb
KB_EXIT_OFF=0x...
KB_LINK_BASE=0x...                              # only if reconstructing symvers
KB_MODULE_CFLAGS=""                             # do not enable reference audit
```

If you do not have a vendor `.ko`, derive offsets with QEMU+gdb and the exact
Image (break `do_init_module`, inspect `mod->init`) and pass them explicitly.

## 3. Build, normalize, gate

```sh
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

The builder:
1. checks `CONFIG_MODULES`, `CONFIG_MODULE_SIG_FORCE`, `CONFIG_KPROBES`,
   `CONFIG_CFI_CLANG`, `CONFIG_MODVERSIONS`;
2. takes vermagic and init/exit offsets from the vendor `.ko` (or explicit
   args);
3. uses exact `Module.symvers` or reconstructs CRCs from `Image`+kallsyms;
4. builds with the target kbuild tree and `KB_CFI=1/0`;
5. runs `fix_symtab_shndx.py`, `postprocess_common.sh`, `verify_versions.py`;
6. packages `dist/<out>-kbootstrap/{kbootstrap.ko,module.zip,manifest.json}`;
7. optionally runs the QEMU payload gate.

It refuses to guess. A missing Image, kallsyms, link base, vermagic or ABI
offset is a hard error with a concrete requirement.

## 4. Install on KernelSU/ReSukiSU

```sh
ksud module install dist/<out>-kbootstrap/module.zip
# reboot
cat /proc/kbootstrap
```

`post-fs-data.sh` loads the module; `service.sh` records status. Do **not**
`rmmod`: the unload/CFI path is not part of the proven gate; reboot is the safe
rollback.

## 5. Manual build (what the script does)

```sh
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 \
  CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" HOSTCFLAGS=-Wno-error \
  KBUILD_MODPOST_WARN=1 KB_CFI=1 -j1 M="$PWD/kernel" modules

python3 tools/fix_symtab_shndx.py kernel/kbootstrap.ko
KB_IMAGE=... KB_KALLSYMS=... KB_VERMAGIC=... KB_INIT_OFF=0x... KB_EXIT_OFF=0x... \
KB_MODVERSIONS=1 sh tools/postprocess_common.sh kernel/kbootstrap.ko kbootstrap
```

## Troubleshooting

| symptom | cause | fix |
|---|---|---|
| `no symbol version` / silent `-ENOEXEC` | missing/empty `__versions` | exact `Module.symvers` or reconstruct from Image; gate with `verify_versions.py` |
| `disagrees about version of symbol` | wrong CRCs | use the target Image/kallsyms; do not reuse another kernel's table |
| `no version magic` | wrong/missing vermagic | read from target/vendor ko; normalize `.modinfo` |
| CFI failure on load/rmmod | no CFI bridge / wrong jump table | `KB_CFI=1`, include `kbootstrap_cfi*`, verify `__cfi_jt_*` |
| Oops `load_module+0x20d4` | `SHN_XINDEX` without `SYMTAB_SHNDX` | run `fix_symtab_shndx.py` before postprocess |
| init not run | wrong `struct module` offset | derive init offset from vendor ko/gdb; patch by symbol |
| instruction abort at payload PC (`ESR 8600000f`) | NX page | `set_memory_x(page,1)` then flush again |
| `register_kprobe` unresolved | no `CONFIG_KPROBES`/kallsyms | this method does not apply; use another entry |
| module signature error | `MODULE_SIG_FORCE=y` | no bypass; use a signed build/vendor path |
