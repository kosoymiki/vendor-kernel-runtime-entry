# Kernel module sources

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

The source is intentionally a reference, not a universal binary. The builder
adapts config, vermagic, symbol versions and `struct module` offsets.
