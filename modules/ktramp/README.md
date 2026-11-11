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
ktramp ... patched=1 policy=1 nr_calls=105 seq=105 win=2 min=105
res=30587,0,0,0   branch=0,0,0,2,0,0,103,0,0   yc=0/331   pre20=371 delta20=2
PASS cold: policy=1 nr_calls>0 branch6/7 (0/103)
PASS y80: branch6=91 delta20=252 pre20=487 (>=40%)
PASS t5: branch1=25 delta20=5117 pre20=5117 (100%)
PASS a80: branch8=109 (old evictions -> full scan)
PASS a10: branch9=63 delta20=376 pre20=737 (>=40%)
PASS ref: branch2=74 tdist=848
PASS restore: patched=0 rmmod_rc=0 proc_gone=1 module_gone=1
PASS ktramp Stage E gate
```

Artifact: `ktramp.ko`, sha256
`edf1d550520bf540d71ae4271dce051a109789d2f6d00ebcc7e4d7841c82b698`.

The wrapper is entered by genuine reclaim; the Stage E policy then makes a
generation-tier decision: cold pages get the full scan (branch 7), a young
working set is protected (branch 6, 51.7% suppression under `y80`), a small
refault distance zeroes file scans (branch 1, 100% under `t5`), and a real
refault halves them (branch 2, `tdist=848`). Stage G adds adaptive ttl
(`ttl=2..4`) and the eviction-age branches 8/9 (`a80`/`a10`). Restore and
unload are proven in the same gate.

## Stage K — per-memcg resident generation histogram

`mark_page_accessed` / `workingset_eviction` maintain a per-memcg 4-bucket
resident histogram (`mgres`). Each scan epoch shifts bucket `i` to `i+1`
(bucket 3 saturates), so a bucket index is the page's age in generations;
`min_seq` is derived from the oldest non-empty bucket and
`window = max_seq - min_seq` feeds the refault threshold together with the
adaptive ttl. Readback adds `res=`, `min=`, `win=`. Without the shift the
histogram is degenerate (accesses only ever land in bucket 0, `min_seq == seq`).

Gate: `tests/qemu_ktramp_stagek_gate.sh` (cold + refault boots) enforces
`min_seq in [seq-3, seq]`, a non-empty histogram, `window >= seq - min_seq`
and, on the cold boot, `min_seq < seq` (the aging proof).

## Stage L/M — bloom hot-page filter, tiered generations, min_ttl

Mode 3 adds the three MGLRU mechanisms that were still missing:

- **Hot-page bloom filter.** Every memcg keeps four 64-bit filters, one per
  generation bucket. `mark_page_accessed` sets the page bit in bucket 0 and the
  filters rotate with the histogram, so a hit at eviction time means "this page
  was hot in that generation" (`bloom=set,hit,miss,rot`).
- **Tiered generation scanning.** The decision is driven by the oldest resident
  generation: hot evictions (`hit >= 60%`) protect the working set (branch 10),
  cold evictions (`hit < 40%`) take the full scan of the oldest generation
  (branch 11).
- **`min_ttl` floor.** A generation younger than `min_ttl_ms` (default 1000 ms,
  settable with `m<ms>` on `/proc/ktramp`) is not evicted yet; pressure inside
  that window is deferred (branch 12, `ttlfloor` counter).

Gate: `tests/qemu_ktramp_stageL_gate.sh` proves, in one boot, the bloom
population/rotation and the tiered branch with the floor disabled (`m0`), then
the 60 s floor deferral (`m60000`, branch 12 and `ttlfloor` growth), then
restore/unload.

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
