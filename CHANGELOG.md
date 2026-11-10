# Changelog

## 1.0.0 — 2026-11-10

- Reference runtime-entry module for RMX3709 vendor 5.10.236.
- Vendor-ABI module build: vermagic, `__versions` from stock Image, `struct
  module` relocation patch, CFI/ABI bridge, `SHN_XINDEX` symtab fix,
  `set_memory_x` executable payload.
- QEMU proof: panic probe reaches `init_module`; payload bus exposes
  `/proc/kbootstrap` and returns `payload_result=42`.
- Per-target builder `tools/adapt_kernel.sh` with config/ABI gates.
- KernelSU/ReSukiSU module package.
- Measured 5.10.236 → 7.4-rc2 delta methodology (file-to-file manifest,
  per-file line delta, Kconfig/syscall/feature map).
- CVE point-fixes are out of scope.
