# MGLRU probe: Stage A/B status (experimental)

`modules/mglru` is a **probe**, not a full 7.4 MGLRU backport. It exists to
measure the 5.10 reclaim path and to test generation-aware policies through the
proven entry.

## What it does

- **Stage A (read-only):** kprobes on `shrink_node`, `shrink_node_memcgs`,
  `get_scan_count`, `page_referenced`, `shrink_slab`,
  `do_try_to_free_pages`; counters in `/proc/mglru_probe`.
- **Stage B (policy):** kretprobe on `get_scan_count`; modes:
  `1` anon-light, `2` file-light, `3` aggressive, `4` generation-aware.
- `mark_page_accessed` kprobe feeds a 1024-entry page-generation side table.
- Mode 4 computes `tracked/young/ratio` and halves scans when mostly young,
  otherwise doubles file scans.

## Observed in QEMU (exact Image)

- Stage A: reclaim hooks 426 hits; `page_referenced` 0 in that workload.
- Mode 2: `modified=796`, `changed=305`, `before=0,0,1,1` → `after=0,0,0,0`.
- Mode 4: `acc=10786`, `trk=250`, `yng=250`, `ratio=100`,
  `changed=368`, `before=0,0,2,3` → `after=0,0,1,1`.
- `gen` remained 1 in the captured run while `modified` was high; generation
  advance semantics need validation. Do not use this as parity evidence.

Logs: `$PREFIX/tmp/mglru-*` (QEMU). These are probe results, not benchmarks.

## Why it is not “MGLRU backported”

- 5.10 has no `gen_lru` fields; this uses a small side table (hash by page),
  not the 7.4 `lru_gen` data structures.
- Only a handful of functions are hooked; 7.4 MGLRU spans `mm/vmscan.c`,
  `mm/workingset.c`, page flags, lruvec, and more.
- The policy is heuristic; no parity metrics/reclaim quality proof.

## Next stages

- **Stage C:** replace selected `vmscan` functions with 7.4-inspired
  implementations via text patch/trampoline. Use `aarch64_insn_patch_text` +
  stop_machine on SMP (not `nosync`), W^X/CFI-safe trampolines, rollback.
- **Stage D:** parity subset + benchmark (pgscan, refault, latency, working
  set); document gaps.

## Build

```sh
KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env
```

Experimental: QEMU first; disable policy (`write 0`) before unload; no `rmmod`
on device until the unload path is proven.
