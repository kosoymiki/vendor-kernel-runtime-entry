# Methodology: finding and using a runtime entry point

This is the engineering explanation behind the reference implementation. It is
deliberately mechanism-first: every step has an observable failure mode and a
gate.

## 1. Entry-point matrix

Enumerate every mechanism that can execute code in kernel context against the
actual `/proc/config.gz`; do not assume one path.

| mechanism | needs | typical vendor state | use |
|---|---|---|---|
| loadable module (LKM) | `CONFIG_MODULES=y`, signature policy allows it | often `=y`; `MODULE_SIG_FORCE=n` | arbitrary kernel code, hooks, payload bus |
| kprobe/kretprobe | `CONFIG_KPROBES=y` | often `=y` | resolve non-exported symbols, observe/patch behavior |
| BPF | `CONFIG_BPF_SYSCALL=y`, `BPF_EVENTS`, `BPF_JIT`, caps | varies | tracing/observability; verifier limits |
| ftrace | `CONFIG_FUNCTION_TRACER`, `DYNAMIC_FTRACE` | often absent on vendor kernels | tracing/hooking |
| livepatch | `CONFIG_LIVEPATCH` | often absent | function replacement |
| kexec / devmem / multikernel | their `CONFIG_*` | often `n` | not usable without rebuild |
| vendor driver primitive | runtime audit | device-specific | research/last resort |

The reference chooses **LKM + kprobe**. It does not require kexec, devmem,
livepatch, ftrace-function-tracer or multikernel. If `CONFIG_MODULES=n`, the
method stops: there is no LKM entry. If `CONFIG_MODULE_SIG_FORCE=y`, unsigned
modules are blocked; no bypass is included.

## 2. The vendor module loader is the real gate

A `.ko` that builds is not a `.ko` that runs. On a vendor arm64 kernel the
loader checks, in order:

1. ELF shape and `.gnu.linkonce.this_module` size;
2. vermagic;
3. symbol versions (`__versions`) when `CONFIG_MODVERSIONS=y`;
4. `struct module` field layout when it copies/initializes the module;
5. CFI metadata / jump tables when `CONFIG_CFI_CLANG=y`;
6. module init/exit pointers and their call sites.

The reference implementation addresses each:

### 2.1 Vermagic and `.modinfo`
The target's exact vermagic is mandatory. The builder reads it from a known-good
vendor `.ko` (or `--vermagic`) and `tools/modinfo_fix.py` normalizes prefixes,
name, depends and license, and drops `srcversion` (build-path leak). The final
gate compares the exact string.

### 2.2 `__versions` / CRCs
With `CONFIG_MODVERSIONS=y`, the vendor kernel may reject a module with no
`__versions` section (`try_to_force_load()` returns `-ENOEXEC` when
`CONFIG_MODULE_FORCE_LOAD` is not set). The builder:
- uses an exact `Module.symvers` if present;
- otherwise reconstructs CRCs from the running `Image` + `/proc/kallsyms`
  (`tools/symvers_from_image.py`) and lets modpost emit the table;
- gates the final artifact with `tools/verify_versions.py`.

A CRC is not a guess: it is derived from the target's own `__ksymtab` /
`__kcrctab` pair. If the target does not expose enough symbols/Image data, the
builder fails rather than shipping a module with wrong versions.

### 2.3 `struct module` layout
The local build tree may have a different `struct module` layout than the
running vendor kernel (CFI, LTO, config options). The module's
`.rela.gnu.linkonce.this_module` init/exit relocations must be at the offsets
the running kernel expects. The builder derives them from a known-good vendor
`.ko` (or explicit `--init-off/--exit-off`) and patches by **symbol name**, not
by a raw hardcoded offset (`tools/fix_this_module_relocs.py`). On the reference
kernel: init `0x190`, exit `0x3c0`, size `0x400`.

### 2.4 CFI / CFI jump tables
A `CONFIG_CFI_CLANG=y` kernel's `cfi_init()` looks for:
- `__cfi_check`
- `__cfi_jt_init_module`
- `__cfi_jt_cleanup_module`

and rewrites `mod->init/exit` to the jump-table entries. It also needs a
module-local `cfi_check` for indirect calls targeting module code. The CFI
bridge (`kernel/kbootstrap_cfi.c`, `kernel/kbootstrap_cfi_jt.S`) provides:
- `init_module.cfi_jt` / `cleanup_module.cfi_jt`: `bti c; b <target>`
- `__cfi_jt_init_module` / `__cfi_jt_cleanup_module`: pointers to those thunks
- `__cfi_check`: `ret`
- `__ubsan_handle_cfi_check_fail_abort`: panic stub

This is a **module-local ABI bridge**. It lets the vendor loader accept the
module, but it disables CFI checks for that module. It is not a kernel-wide CFI
disable. Only load trusted code; see `docs/SECURITY.md`.

### 2.5 ELF `SHN_XINDEX` corruption
`ld.lld` can emit `st_shndx = 0xffff` (`SHN_XINDEX`) for `__UNIQUE_ID_*` /
`.modinfo` symbols without a `SHT_SYMTAB_SHNDX` table. The vendor
`simplify_symbols()` indexes `sechdrs[st_shndx]` and can Oops in
`load_module+0x20d4`. `tools/fix_symtab_shndx.py` maps those symbols to the
`.modinfo` section before the ABI gate. This is a linker/toolchain artifact,
not a CFI failure.

## 3. Inside the module: symbol resolution and executable memory

`kbootstrap_core.c` uses a classic chain:

1. `register_kprobe({.symbol_name = "kallsyms_lookup_name"})` → resolve the
   non-exported `kallsyms_lookup_name` (kprobe resolves symtab entries).
2. Use it to resolve `module_alloc`, `__flush_icache_range`,
   `set_memory_x`, `proc_create`, `simple_read_from_buffer`, `sys_call_table`.
3. Allocate one page with `module_alloc`.
4. Write a tiny AArch64 payload (`bti c; mov w0,#42; ret`).
5. `__flush_icache_range` → `set_memory_x(page, 1)` → flush again.
6. Call the payload; expose results through `/proc/kbootstrap`.

Step 5 is essential: `module_alloc` memory is not executable for ad-hoc
payloads. Writing + flush alone produces an instruction abort
(`ESR 8600000f`, `pc` = payload address, “bad PC value”). `set_memory_x` makes
the page executable; flush after the permission change.

## 4. Evidence gates (no claim without a side effect)

- **Build gate:** exact vermagic, `__versions` verification, size/offset
  checks, no `SHN_XINDEX` leftovers.
- **QEMU gate:** boot the exact stock `Image` with `nokaslr`; assert
  `insmod_rc=0`, module live, `/proc/kbootstrap` present, `payload_result=42`.
- **Device gate:** separate, reversible-by-reboot step; back up boot; never
  `rmmod` until unload/CFI is proven; keep the module loaded.

A module that loads is not a module that runs: `insmod rc=0` alone is not
evidence. The panic probe exists to prove `init_module` execution; the payload
probe proves raw code execution and a side effect.

## 5. Adapting to another kernel

`tools/adapt_kernel.sh` is the adaptation layer, not a universal binary:

1. Read the target `.config`; refuse `MODULES=n`, `MODULE_SIG_FORCE=y`,
   missing kprobes.
2. Take vermagic and init/exit offsets from a known-good vendor `.ko` (ABI
   oracle) or explicit values verified with QEMU+gdb.
3. If `MODVERSIONS=y`, use exact `Module.symvers` or reconstruct CRCs from the
   target `Image`/kallsyms with the target link base.
4. Build with the target kernel tree and kbuild; pass `KB_CFI=1` when CFI is
   enabled.
5. Normalize the symtab, postprocess `.modinfo`/relocations, gate the artifact.
6. Package a KernelSU zip and run the QEMU gate.

If a step cannot be evidenced (no Image, no kallsyms, no link base, no vendor
oracle), the builder stops with a concrete requirement. It does not guess.

## 6. From entry to backports

The entry is the base; the backport layer is separate:
- **ABI/symbol registry** per target build;
- **hook engine**: kprobe/kretprobe, text patch/trampoline
  (`aarch64_insn_patch_text`, `set_memory_rw/x`), function-pointer replacement;
- **feature payloads**: one module per feature with init/exit, feature flag,
  benchmark and rollback;
- **control plane**: `/proc` + KernelSU scripts;
- **evidence**: QEMU metrics/behavior before claiming parity.

See `docs/BACKPORT-7.4.md` for the measured 5.10 → 7.4-rc2 delta and the
subsystem/feature map.
