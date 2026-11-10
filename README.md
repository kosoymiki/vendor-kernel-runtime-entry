# H2 Runtime Entry — run new kernel functionality on a vendor-frozen kernel

**Status:** reference implementation, proven in QEMU on the Realme GT3 (RMX3709) vendor kernel `5.10.236-android12-9-o-g27a48ef6c082`. Not a universal binary. Read [SECURITY.md](docs/SECURITY.md) before loading anything.

## What problem does this solve?

Vendor kernels are frozen at an old version. New kernel functionality and fixes normally require `CONFIG_*=y` at build time, a rebuilt `Image`, and a flashable bootloader. When the bootloader is locked and the vendor does not ship a newer kernel, that path is closed.

This project demonstrates a different entry point:

1. build a **vendor-ABI-compatible loadable kernel module** (LKM) for the exact running kernel;
2. make it pass the vendor kernel's **module loader**, **CFI**, `__versions`, vermagic and `struct module` ABI;
3. inside the module, resolve non-exported symbols through **kprobe**;
4. allocate executable kernel memory (`module_alloc` + `set_memory_x`), run trusted payloads, and expose a `/proc` control/status surface;
5. package it as a **KernelSU/ReSukiSU module** so it loads at boot.

The entry does **not** depend on `KEXEC`, `KEXEC_FILE`, `DEVMEM`, `MULTIKERNEL`, `LIVEPATCH`, or `FUNCTION_TRACER`. It depends on `CONFIG_MODULES=y`, `CONFIG_KPROBES=y`, and unsigned module loading being allowed. If those are unavailable, this method does not apply.

## Why “=y” used to be the wall

A kernel feature is either compiled in (`=y`), built as a module (`=m`), or absent (`n`). If a vendor ships `CONFIG_KEXEC=n`, `CONFIG_DEVMEM=n`, `CONFIG_MULTIKERNEL=n`, or no `FUNCTION_TRACER`, you cannot enable it at runtime by editing a file. Rebuilding the kernel is not possible with a locked bootloader.

This project does not try to turn those symbols on. It uses the one door that is frequently left open — **module loading** — and solves the real blockers around it: CFI, symbol versions, vermagic, `struct module` layout, ELF symbol-table corruption, and executable-memory permissions.

## What is proven (QEMU, exact stock Image)

**Panic probe** — `kbootstrap.probe.ko` loaded and `init_module` executed:

```
cfi: not using shadow for module kbootstrap
Kernel panic - not syncing: kbootstrap entered
  panic+0x188
  init_module+0x1c/0xfe8 [kbootstrap]
  do_one_initcall+0x114/0x458
  do_init_module+0x64/0x8cc
  load_module+0x3650/0x3f44
```

**Payload bus** — `kbootstrap.ko` loaded, executed a raw AArch64 payload and exposed `/proc/kbootstrap`:

```
insmod_rc=0
kbootstrap: ready kln=... alloc=... flush=... sct=... payload=42
/proc/kbootstrap:
  kallsyms_lookup_name=...
  module_alloc=...
  flush_icache_range=...
  set_memory_x=...
  sys_call_table=...
  payload_result=42
  proc_entry=created
```

See [docs/EVIDENCE.md](docs/EVIDENCE.md) for the exact logs and hashes.

## Repository map

```
kernel/                 module source (core + CFI bridge + Makefile)
module/                 KernelSU/ReSukiSU package (scripts, module.prop)
tools/                  per-target builder, ELF/ABI/CRC gates
profiles/               target profiles (reference: RMX3709 5.10.236)
tests/                  static checks + QEMU payload gate
docs/                   methodology, build, compatibility, security, evidence
```

## Quick start (reference profile)

Prerequisites: matching kernel source/build tree, uncompressed `Image`, `/proc/kallsyms`, a known-good vendor `.ko` (ABI oracle), `clang`/`lld`, `zip`, `qemu-system-aarch64` for the gate.

```sh
# 1. collect target facts (root)
tools/collect_target.sh target/

# 2. edit profiles/rmx3709-5.10.236/profile.env (paths, image, kallsyms)

# 3. build + normalize + gate
tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env --qemu

# 4. install the produced module.zip in KernelSU/ReSukiSU
ksud module install dist/*/module.zip
# reboot; then:
cat /proc/kbootstrap
```

For another kernel, create a new profile from [profiles/README.md](profiles/README.md) and run the same builder. The builder refuses unsafe configurations (`MODULES=n`, `MODULE_SIG_FORCE=y`, missing kprobes, missing ABI evidence) instead of guessing.

## What “arbitrary modules” means here

The base module is a **runtime entry and payload bus**. It can:
- resolve kernel symbols that are not exported;
- allocate executable memory and run trusted AArch64/C payloads;
- expose status/control through `/proc`;
- be packaged as a KernelSU module.

It does **not** make a foreign binary portable. Every payload/module must be built for the target kernel's ABI, CFI mode, vermagic and symbol versions. There is no universal `.ko`.

## Limitations (honest)

- `CONFIG_MODULES=n` → no LKM entry; this method does not apply.
- `CONFIG_MODULE_SIG_FORCE=y` → unsigned modules blocked; no bypass is provided.
- `CONFIG_KPROBES=n` → the symbol-resolution path used here is unavailable.
- `CONFIG_MODVERSIONS=y` requires exact symbol CRCs; the builder reconstructs them from the running `Image` or uses an exact `Module.symvers`.
- CFI kernels: the module-local CFI bridge makes the vendor loader accept the module, but it **disables CFI checks for this module**. This is a deliberate ABI tradeoff, not a kernel-wide CFI bypass. Only load trusted code.
- `rmmod` can panic if the unload/CFI path was not proven. Do not unload on a device; reboot is the rollback.
- Vendor kernels differ. ABI offsets and symbol names must be derived per target; the builder does not invent them.

## Security

This is arbitrary kernel code execution. A bad payload can panic, corrupt data, or brick the device. Use a spare/test device, back up boot/vendor_boot, and validate in QEMU first. Do not use it to bypass third-party authentication, DRM, CAPTCHA, paywalls, or to attack systems you do not own. See [SECURITY.md](docs/SECURITY.md).

## Backport program

The same entry is the base for runtime backports of newer-kernel functionality. A full file-to-file delta of Linux 7.4-rc2 vs this vendor 5.10.236, plus a subsystem/feature backport map, is documented in [docs/BACKPORT-7.4.md](docs/BACKPORT-7.4.md). CVE point-fixes are out of scope.

## License

GPL-2.0-only. Kernel modules must be GPL-compatible; see [LICENSE](LICENSE).
