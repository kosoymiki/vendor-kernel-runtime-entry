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
ktramp target=0xffffffc00843c0c0 tramp=0xffffffc001a36000 patched=1 calls=0
pgscan_file 0
...
ktramp ... patched=1 calls=113
pgscan_kswapd 5917  pgscan_direct 167  pgscan_file 6084
ktramp ... patched=0 calls=113      # restored
PASS ktramp gate (calls=113, patched, restored)
```

Artifact: `ktramp.ko`, sha256
`bbfb599a17db566dae1eb664dbafba5de1f5568bd08ac7363ca1ceefc35b55e3`.

`calls=113` with `pgscan_file=6084` is the important pair: the wrapper was
entered by genuine reclaim, not by a synthetic call.

## What this proves, and what it does not

**Proves:** a whole kernel function can be redirected to module code that still
calls the original implementation, then restored. That is the mechanism a 7.4
`vmscan` backport needs.

**Does not prove:**

- SMP safety — `aarch64_insn_patch_text_nosync` does not stop other CPUs; the
  gate runs `-smp 1`. On a live device use `aarch64_insn_patch_text` /
  `stop_machine`, then verify under load.
- branch reachability beyond ±128 MiB (checked, `-ERANGE` otherwise);
- that the policy is *good*: `nr[2]=nr[3]=0` is a blunt "protect page cache"
  experiment, not an MGLRU implementation.

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
