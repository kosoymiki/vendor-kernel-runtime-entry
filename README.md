# Vendor Kernel Runtime Entry

**English | [Русский](README.ru.md)**

Load trusted code into a **vendor-frozen Linux kernel** at runtime — without
recompiling the kernel, without `KEXEC`, `DEVMEM`, `MULTIKERNEL`, `LIVEPATCH`
or `FUNCTION_TRACER`, and without an unlocked bootloader.

This repository is a **reference implementation + per-target builder**, not a
universal kernel module. It was proven in QEMU on the Realme GT3 (RMX3709)
vendor kernel `5.10.236-android12-9-o-g27a48ef6c082`.

> **Security:** this is arbitrary kernel code execution. Read
> [`docs/SECURITY.md`](docs/SECURITY.md) first. A bad module can panic or brick
> the device. Test in QEMU; never `rmmod` on a device until the unload path is
> proven.

## The wall, and the door

Kernel features are compile-time switches (`=y` / `=m` / `=n`). If a vendor
ships `CONFIG_KEXEC=n`, `CONFIG_DEVMEM=n` or no livepatch/ftrace, you cannot
turn them on at runtime, and a locked bootloader prevents flashing a new
kernel.

This project uses the door vendors usually leave open — **loadable modules** —
and solves the real blockers around it:

1. exact **vermagic** and `.modinfo`;
2. **`__versions` / CRC** table from the running `Image` + `/proc/kallsyms`;
3. **`struct module`** init/exit offsets, patched by symbol name;
4. **CFI** `cfi_init()` bridge (`__cfi_check`, `__cfi_jt_*`, jump tables);
5. **`SHN_XINDEX`** linker bug (`ld.lld` without `SYMTAB_SHNDX`);
6. executable memory for payloads (`module_alloc` + `set_memory_x` + flush);
7. symbol resolution through **kprobe** (`kallsyms_lookup_name` is not exported);
8. a **KernelSU/ReSukiSU** boot package.

## Proof (QEMU, exact stock Image)

Panic probe — `init_module` executes:

```
cfi: not using shadow for module kbootstrap
Kernel panic - not syncing: kbootstrap entered
  init_module+0x1c/0xfe8 [kbootstrap]
  do_one_initcall+0x114/0x458
  do_init_module+0x64/0x8cc
  load_module+0x3650/0x3f44
```

Payload bus — raw AArch64 code runs and a side effect is exposed:

```
insmod_rc=0
kbootstrap: ready kln=... alloc=... flush=... sct=... payload=42
/proc/kbootstrap:
  kallsyms_lookup_name=...
  module_alloc=...
  set_memory_x=...
  sys_call_table=...
  payload_result=42
  proc_entry=created
```

Reference artifact: `kbootstrap.ko` sha256
`065f520b1b562f55eef48a138a12d4df3c18d51357bcbacb39b46d1e9a33c593`.
QEMU gate: `tests/qemu_payload_gate.sh` → `PASS qemu payload gate`.

## Quick start — reference target only

```sh
# release asset: module.zip (RMX3709 5.10.236 only)
ksud module install module.zip
# reboot
cat /proc/kbootstrap
```

**Do not install this `.ko` on another kernel.** It is tied to the reference
vermagic, CRCs, struct offsets and CFI mode.

## Adapt to another kernel

The builder derives config/ABI/vermagic from the target and refuses to guess:

```sh
git clone https://github.com/kosoymiki/vendor-kernel-runtime-entry.git
cd vendor-kernel-runtime-entry
tools/collect_target.sh target/
# fill profiles/my-device/profile.env (Image, kallsyms, vendor .ko, offsets)
tools/adapt_kernel.sh --profile profiles/my-device/profile.env --qemu
```

Full step-by-step: [`docs/ADAPTING.md`](docs/ADAPTING.md) (detailed, with
troubleshooting). Russian: [`docs/ADAPTING.ru.md`](docs/ADAPTING.ru.md).

## What works / what does not

**Works when:** `CONFIG_MODULES=y`, unsigned modules are allowed
(`MODULE_SIG_FORCE` not `y`), `CONFIG_KPROBES=y`, the target `Image` and
`kallsyms` are available (or exact `Module.symvers`), and ABI offsets can be
derived.

**Does not work when:** `CONFIG_MODULES=n` (no LKM entry);
`CONFIG_MODULE_SIG_FORCE=y` (no bypass); `CONFIG_KPROBES=n`; no target ABI
evidence. The CFI bridge disables CFI checks **for this module** — a deliberate
module-local ABI tradeoff, not a kernel-wide bypass.

## Repository map

```
kernel/      module source + CFI bridge
module/      KernelSU/ReSukiSU package
tools/       per-target builder, ELF/ABI/CRC gates, delta tooling
profiles/    target profiles (reference: RMX3709 5.10.236)
tests/       static checks + QEMU payload gate
docs/        adaptation, methodology, security, backport map
```

## Patching a CVE at runtime

A defensive template shows how to turn an upstream fix into a runtime
mitigation module (kprobe argument guard; trampoline only for narrow cases)
and load it through the same entry. No exploit code.

- [English](docs/CVE-PATCHING.md)
- [Русский](docs/CVE-PATCHING.ru.md)

## Backporting 7.4

Measured 5.10.236 → 7.4-rc2 delta: **+13.6M LOC** (vendor 30.96M vs latest
44.56M), union **111,989** paths (43,291 added / 14,639 removed / 40,388
modified), line diff **+20.76M / −7.40M**, Kconfig **+6,004/−3,084**, 31 new
syscalls. MGLRU is the first feature/benchmark; CVE point-fixes are outside
the 7.4 feature-backport roadmap (a separate defensive patching template is
linked above). See [`docs/BACKPORT-7.4.md`](docs/BACKPORT-7.4.md).

## Release

- Repository: <https://github.com/kosoymiki/vendor-kernel-runtime-entry>
- v1.0.1: <https://github.com/kosoymiki/vendor-kernel-runtime-entry/releases/tag/v1.0.1>

## License

GPL-2.0-only. Kernel modules must be GPL-compatible.
