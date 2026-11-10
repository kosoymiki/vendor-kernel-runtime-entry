# Compatibility matrix

This project is a **method + per-target builder**, not a universal module.

## Reference target (observed)

- Realme GT3 / RMX3709, Android 16, arm64.
- `Linux 5.10.236-android12-9-o-g27a48ef6c082`.
- `CONFIG_MODULES=y`, `CONFIG_MODULE_SIG` not set, `CONFIG_MODVERSIONS=y`,
  `CONFIG_MODULE_SCMVERSION=y`, `CONFIG_KALLSYMS=y/ALL`, `CONFIG_KPROBES=y`.
- `CONFIG_CFI_CLANG=y`, `CONFIG_LTO_CLANG=y`, `CONFIG_SHADOW_CALL_STACK=y`,
  `CONFIG_ARM64_BTI=y`, `CONFIG_ARM64_PTR_AUTH=y`,
  `CONFIG_STRICT_KERNEL_RWX=y`, `CONFIG_STRICT_MODULE_RWX=y`.
- `CONFIG_KEXEC/KEXEC_FILE/CRASH_DUMP/DEVMEM/MULTIKERNEL/LIVEPATCH/
  FUNCTION_TRACER` not set.
- ABI: `struct module` init `0x190`, exit `0x3c0`, size `0x400`; vendor
  modules use CFI jump tables; module loader accepts modules built with the
  local clang + CFI bridge after symtab/versions normalization.
- QEMU: exact Image + `nokaslr`; panic probe and payload bus pass.

## What must be true on another target

| requirement | if false |
|---|---|
| `CONFIG_MODULES=y` | no LKM entry; this method does not apply |
| unsigned modules allowed (`MODULE_SIG_FORCE` not `y`) | no bypass; use signed modules/vendor path |
| `CONFIG_KPROBES=y` | `kallsyms_lookup_name` resolution path unavailable |
| `CONFIG_KALLSYMS` adequate | non-exported symbols may not resolve; validate target symbols |
| exact `Module.symvers` or Image+kallsyms+link base | cannot build a loadable `__versions` table |
| vendor ABI oracle or QEMU/gdb offsets | cannot safely patch `struct module` |
| CFI mode known | wrong bridge can panic; set `KB_CFI` correctly |
| not a hardened/secure-boot device that blocks unsigned modules | no entry without a signed/vendor path |

## Configs that do **not** block this entry

`KEXEC`, `KEXEC_FILE`, `DEVMEM`, `MULTIKERNEL`, `LIVEPATCH`,
`FUNCTION_TRACER`, `BPF_SYSCALL` may be `n`; the reference entry does not use
them. (BPF may still be a separate observability path when present.)

## Architectures

The reference payload is AArch64 (`bti c`, `set_memory_x`, `module_alloc`).
x86_64/other architectures require:
- architecture-specific executable-memory API and instruction cache flush;
- different CFI/IBT/endbr semantics;
- different `struct module` offsets and relocation handling;
- a new payload and QEMU target.
The methodology ports; the binary does not.

## Kernel versions

The reference is 5.10.236. 5.15/6.x/7.x targets can use the same approach when
their module loader/symbol/CFI behavior is characterized, but every offset,
symbol and version table must be re-derived. The `7.4-rc2` delta in
`docs/BACKPORT-7.4.md` is a map, not a claim that the reference binary runs on
7.4.
