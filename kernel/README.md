# Kernel module sources

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

- `kbootstrap_core.c` — resolves symbols through `register_kprobe`, allocates
  executable kernel memory, runs a raw AArch64 payload, exposes
  `/proc/kbootstrap`. `KB_PROBE_PANIC` builds a panic probe; `KB_AUDIT_SCT`
  enables the reference syscall-table audit (device/profile-specific).
- `kbootstrap_cfi.c` + `kbootstrap_cfi_jt.S` — module-local CFI/ABI bridge
  expected by a `CONFIG_CFI_CLANG=y` vendor kernel (`cfi_init()` looks for
  `__cfi_check`, `__cfi_jt_init_module`, `__cfi_jt_cleanup_module`). The
  bridge is a security tradeoff: it disables CFI checks for this module; load
  only trusted code.
- `Makefile` — pass `KB_CFI=1` for CFI kernels, `KB_CFI=0` otherwise.

Artifact: `kbootstrap.ko` 1.0.0, sha256
`065f520b1b562f55eef48a138a12d4df3c18d51357bcbacb39b46d1e9a33c593`, gated by
`tests/qemu_payload_gate.sh` (CFI bridge, kallsyms resolution, raw payload,
`/proc` surface).

The source is intentionally a reference, not a universal binary. The builder
adapts config, vermagic, symbol versions and `struct module` offsets.
