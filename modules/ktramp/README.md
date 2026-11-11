# ktramp — full-function trampoline probe

> Part of a library under **active development** — new modules and stages land as
> they pass their QEMU gates.

`kpatch` proves that a function can be *replaced*; `ktramp` proves that a whole
function **body** can be replaced and still call the original.

## Mechanism

```
get_scan_count (kernel)                module wrapper
+--------------------------+          +---------------------------+
| b  wrap            <-- patched       | calls++                   |
| ... 7 insns copied ...   |          | lv_touch(lruvec)          |
+--------------------------+          | tramp_fn(...)  ---------->+
                                      | if (policy) nr[2]=nr[3]=0 |
                                      +---------------------------+
trampoline page (module_alloc + set_memory_x):
  <7 original instructions> ; b get_scan_count+28
```

1. Resolve `get_scan_count`, `kallsyms_lookup_name`,
   `aarch64_insn_patch_text_nosync`, `module_alloc`, `set_memory_x`,
   `__flush_icache_range` through kprobe.
2. Allocate one executable page, copy the first **7 instructions** of the target
   into it, append an unconditional branch back to `target + 28`, flush the
   icache and mark the page executable.
3. Patch the target's first instruction with `b wrap` (26-bit immediate, ±128 MiB).
4. The wrapper counts calls, records the `lruvec` in a small side table, calls
   the trampoline (so the original code still runs), optionally applies a policy
   to `nr[]`, and returns.
5. `echo 0 > /proc/ktramp` restores the original first instruction.

## Observed (QEMU, exact stock Image)

Gate: [`tests/qemu_ktramp_gate.sh`](../../tests/qemu_ktramp_gate.sh) —
ext4 file through `loop0`, 345 MiB anonymous pressure, poll `pgscan_file`.

```
=== KTRAMP cold ===
ktramp ... patched=1 policy=1 nr_calls=123 seq=123 ttl=2
branch=0,0,1,1,0,0,121,0,0   yc=0/415   pre20=610 delta20=1
PASS cold: policy=1 nr_calls>0 branch6/7 (0/106)
PASS y80: branch6=138 delta20=376 pre20=737 (>=40%)
PASS t5: branch1=38 delta20=3070 pre20=3070 (100%)
PASS a80: branch8=59 (old evictions -> full scan)
PASS a10: branch9=136 delta20=375 pre20=734 (>=40%)
PASS ref: branch2=118 tdist=848
PASS restore: patched=0 rmmod_rc=0 proc_gone=1 module_gone=1
PASS ktramp Stage E gate
```

Artifact: `ktramp.ko`, sha256
`e688ddc30758321a8bc6fb5110529c13af8ee8adc363d70829cef064007fddc2`.

The wrapper is entered by genuine reclaim; the Stage E policy then makes a
generation-tier decision: cold pages get the full scan (branch 7), a young
working set is protected (branch 6, 51.1% suppression under `y80`), a small
refault distance zeroes file scans (branch 1, 100% under `t5`), and a real
refault halves them (branch 2, `tdist=848`). Stage G adds adaptive ttl
(`ttl=2..4`) and the eviction-age branches 8/9 (`a80`/`a10`). Restore and
unload are proven in the same gate.

## What this proves, and what it does not

**Proves:** a whole kernel function can be redirected to module code that still
calls the original implementation, then restored. That is the mechanism a 7.4
`vmscan` backport needs.

**Does not prove:**

- SMP safety on a live device — the module prefers `aarch64_insn_patch_text`
  (stop_machine) when the symbol resolves and falls back to `_nosync`; the QEMU
  gate runs `-smp 1`, so live SMP load testing is still required.
- branch reachability beyond ±128 MiB (checked, `-ERANGE` otherwise);
- full MGLRU parity: the policy runs at `get_scan_count`; a custom reclaim path
  is composed from `kpatch`/`ktramp`/kprobe hooks and side tables, not from a
  vmscan rebuild. The exact `min_seq/max_seq` clock and per-generation LRU lists
  remain M4 fidelity work (see `docs/BACKPORT-7.4.md`).

## Safety

QEMU only. Never `rmmod` while `patched=1`; disable policy (`echo 0`) first and
confirm the restore in `/proc/ktramp`. A wrong patch or a bad trampoline copy
panics the kernel.

## Build

```sh
KB_MODULE_SRC=$PWD/modules/ktramp KB_MODNAME=ktramp \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

KB_IMAGE=$HOME/tmp/kernel_Image.bin KB_KO=$PWD/modules/ktramp/ktramp.ko \
  bash tests/qemu_ktramp_gate.sh
```
