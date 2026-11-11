# MGLRU Stage E — generation-tier policy (`ktramp` 1.1.0)

Stage E turns the trampoline from a counter into an MGLRU-style decision:
protect a young working set, but let cold pages be scanned and evicted
("evict oldest first"). It is a runtime policy at the `get_scan_count`
boundary. No vmscan rebuild is needed: `kpatch` replaces a function body,
`ktramp` trampolines a whole function while still calling the original, and
kprobes hook the reclaim path. Together they are the compatibility layer
(CFI bridge, MODVERSIONS, vermagic, init/exit relocations, SMP-safe patching)
that makes a custom reclaim policy possible without touching the kernel image.

## Mechanism

- `mark_page_accessed` kprobe: every access stores `page -> generation` in the
  folio shadow table and bumps the lruvec access counter.
- `workingset_eviction` kprobe: if `cur_gen - page_gen <= 1` the page was
  accessed in the current generation and is counted **young**, otherwise
  **cold**.
- `workingset_refault` kprobe: unpacks the kernel shadow timestamp
  (`shadow >> 19`) and computes the refault distance against
  `lruvec->nonresident_age` (offset 96).
- `get_scan_count` trampoline policy, in order:
  1. refault distance `d != 0`: `d <= 32` zeroes file scans, else halves them;
  2. `young/(young+cold) > 60%`: halve file scans (protect the working set);
  3. `young/(young+cold) < 20%`: keep the full scan (cold pages dominate);
  4. otherwise per-memcg generation parity;
  5. `can_swap` last: only zero anon counts (the kernel already does this
     without swap, `mm/vmscan.c:2522`).
- Fault injection for otherwise-unreachable inputs: `echo t5` forces `d=5`;
  `echo y80` forces young/cold 80/20. Both are debug-only and labeled in the
  readback (`fd=`, `fyc=`).

## Observed in QEMU (stock `Image`, 512 MiB, swapless)

`tests/qemu_ktramp_gate.sh` (module sha `85d01060…`):

| regime | counters | branch | suppression |
|---|---|---|---|
| cold ext4 cache | `yc=0/398`, `nr_calls=108` | 7 (full scan) | `delta20=2 / pre20=610` |
| forced young `y80` | `fyc=80/20` | **6** | `delta20=376 / pre20=736` = **51.1%** |
| forced distance `t5` | `fd=5` | **1** | `delta20=8703 / pre20=8703` = **100%** |
| real refault (`MADV_PAGEOUT` 128 KiB) | `refaults>0` | 2 | `tdist=847` |
| restore + unload | — | — | `patched 1→0`, `rmmod_rc=0`, `proc_gone`, `module_gone` |

Interpretation: both real workloads evict only cold pages, so full scan is the
correct MGLRU behaviour; the young-protection branch is reached
deterministically through `y80` and suppresses 51.1% of requested file scans.

## Tests

- `modules/ktramp/tests/test_policy.c`: 29/29 PASS (branches 1–7, no-swap
  ordering, shadow unpack, distance mask, folio head, eviction table).
- QEMU gate above: cold, `y80`, `t5`, refault, restore, unload.

## Limits

- Generation advance is scan-quantum based (64 scans), not the exact MGLRU
  `min_seq/max_seq` reclaim clock.
- Real young evictions need a working set larger than free memory; the 512 MiB
  gate evicts cold pages only, hence the labeled injection.
- The policy changes `nr[]` only; kernel data structures are not modified.
