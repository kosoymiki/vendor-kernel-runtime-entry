# Entry-point matrix (evidence-backed)

This matrix is the result of the runtime audit on the reference vendor kernel
(`5.10.236-android12-9-o-g27a48ef6c082`). “Open” means we proved it in QEMU on
the exact stock Image; it is not a claim for every kernel.

| mechanism | needs | state | what it gives | evidence |
|---|---|---|---|---|
| LKM + CFI bridge | `MODULES=y`, unsigned allowed | **proven** | module code executes; init/exit and module callbacks | panic probe + payload `/proc/kbootstrap`, payload_result=42 |
| kprobe / kretprobe | `KPROBES=y` | **proven** | resolve non-exported symbols; observe/modify `get_scan_count` args | `mglru_probe` registered 6/6, hits 426; kretprobe changed=305 |
| text patch | `aarch64_insn_patch_text_nosync` + W^X | **proven in QEMU (single CPU)** | replace a live function and restore | `kpatch`: getpid 114 → patched 0 → restored 118 |
| trampoline | patcher + `module_alloc`/`set_memory_x` | **proven in QEMU (single CPU)** | replace a whole function body and still call the original | `ktramp`: `calls=113`, `pgscan_file=6084`, restore `patched=0` |
| raw payload | `module_alloc` + `set_memory_x` + flush | **proven** | execute trusted AArch64 code | `payload_result=42` |
| kallsyms via kprobe | `KPROBES`, kallsyms | **proven** | resolve `kallsyms_lookup_name`, module_alloc, set_memory_x, sys_call_table | `/proc/kbootstrap` addresses |
| procfs/sysfs control | `proc_create`/module params | **proven** | status/control surface | `/proc/kbootstrap`, `/proc/kpatch`, `/proc/mglru_probe` |
| BPF | `BPF_SYSCALL/EVENTS/JIT` | **observe only** | tracing/observability; verifier limits | config only; no arbitrary-code claim |
| ftrace function tracer | `FUNCTION_TRACER/DYNAMIC_FTRACE` | **closed** | — | config absent |
| livepatch | `LIVEPATCH` | **closed** | — | config absent |
| kexec / devmem / multikernel | their configs | **closed** | — | config `n` |

## What the CFI bridge actually solves

Vendor kernel: clang 12.0.5 + LTO + `CONFIG_CFI_CLANG=y`. The module is built
with clang 21 without LTO. It still loads and runs because:

1. `__cfi_check: ret` + `__cfi_jt_init_module`/`__cfi_jt_cleanup_module` +
   `bti c; b init_module` match what `cfi_init()` looks for
   (`kernel/module.c:4530-4545`);
2. `__versions` carries real CRCs from the running `Image`;
3. `struct module` init/exit relocations are patched to the vendor offsets;
4. `SHN_XINDEX` is fixed before load.

CFI no longer blocks “not in 5.10” at the loading layer: module code, kprobe
callbacks and patched functions are callable. **But** the bridge disables CFI
checks **for this module**; it is not a kernel-wide CFI disable. Treat module
code as trusted.

## Text patch: what is proven and what is not

`kpatch` (QEMU, exact Image) saved the first two instructions of
`__arm64_sys_getpid`, wrote `mov w0,#0; ret` with
`aarch64_insn_patch_text_nosync`, verified `getpid()==0`, then restored the
original instructions and got `118`.

This proves arbitrary function replacement + rollback is possible. It does
**not** prove SMP-safe patching:

- `aarch64_insn_patch_text_nosync` does not stop other CPUs;
- the probe used single-CPU QEMU;
- on a live SMP device use `aarch64_insn_patch_text`/stop_machine and verify
  under load;
- a wrong patch can panic/corrupt; keep the restore path; never rmmod before
  restore.

## Trampoline: what is proven and what is not

`ktramp` (QEMU, exact Image) copies the first **7 instructions** of
`get_scan_count` into an executable page allocated with `module_alloc`, appends
a branch back to `target + 28`, patches the function entry with `b wrap`, and
lets the wrapper call the trampoline — so the original code still runs while the
module observes and can adjust `nr[]`.

Measured under **real reclaim** (ext4 file through `loop0` + 345 MiB anonymous
pressure, see `docs/MEASURING-RECLAIM.md`): `calls=113`, `pgscan_file=6084`,
`pgscan_kswapd=5917`, `pgscan_direct=167`, then `patched=0` after restore.

Limits:

- single-CPU QEMU only — the patcher is `..._nosync`;
- the branch must be within ±128 MiB (checked; `-ERANGE` otherwise);
- the copied prologue must not contain PC-relative instructions that break when
  relocated — checked per target, not in general;
- a policy on `nr[]` is an experiment, not an MGLRU implementation.

## MGLRU: probe, not parity

`mglru_probe` is Stage A/B: reclaim-path kprobes + a `get_scan_count`
kretprobe with policy modes; a page-generation side table feeds mode 4.
Observed QEMU: Stage A 426 hook hits; mode 2 `changed=305`; mode 4
`acc=10786`, `trk=250`, `yng=250`, `ratio=100`, `before=0,0,2,3` →
`after=0,0,1,1`. This is experimental, not full 7.4 MGLRU. 5.10 has no
`gen_lru` fields; see `docs/MGLRU.md`.

## How this maps to backports

- replace a function body: text patch/trampoline (proven in QEMU);
- observe/patch arguments: kprobe/kretprobe (proven);
- run new code: raw payload (proven);
- resolve symbols: kprobe (proven);
- control: procfs/module params (proven).

The remaining work for 7.4 functionality is the **actual implementation** and
ABI mapping, not the loading mechanism.
