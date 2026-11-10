# MGLRU probe: Stage A/B status (experimental)

`modules/mglru` is a **probe**, not a full 7.4 MGLRU backport. It exists to
measure the 5.10 reclaim path and to test generation-aware policies through the
proven entry.

## What it does

- **Stage A (read-only):** kprobes on `shrink_node`, `shrink_node_memcgs`,
  `get_scan_count`, `page_referenced`, `shrink_slab`,
  `do_try_to_free_pages`; counters in `/proc/mglru_probe`.
- **Stage B (policy), selected by writing a digit to `/proc/mglru_probe`:**
  - `1` anon-light, `2` file-light, `3` aggressive — `nr[]` heuristics;
  - `4` generation-aware — `shrink_node` entry decides from the page-generation
    side table;
  - `5` priority policy — `shrink_node` entry sets `scan_control.priority` to 0
    (aggressive reclaim).
- `mark_page_accessed` kprobe feeds a 1024-entry page-generation side table.

Mode 5 reads `scan_control` from `regs[1]` and writes the `s8 priority` field at
offset `0x2c` (verified against the target ABI, see the neighbouring session's
`docs/OFFSETS.md`). It counts every entry (`phits`) and every write that
actually changed the value (`changed`).

## A defect worth remembering: kprobe + kretprobe on one symbol

An earlier revision registered a kprobe **and** a kretprobe on
`get_scan_count`. The return handler never fired (`modified=0`, `nmissed=0`):
the kprobe entry on the same address suppresses the kretprobe path. The
kretprobe was removed; policy decisions now happen at `shrink_node` entry, where
`scan_control` is reachable anyway.

## Observed in QEMU (exact stock Image)

Workload: ext4 file through `loop0` + 345 MiB anonymous pressure, polling
`pgscan_file` — see [`MEASURING-RECLAIM.md`](MEASURING-RECLAIM.md). Gate:
`tests/qemu_mglru_gate.sh`.

| metric | mode 0 (baseline) | mode 5 (priority policy) |
|---|---|---|
| `shrink_node` hits | 101 | 102 |
| `get_scan_count` hits | 101 | 102 |
| `page_referenced` hits | 5266 | 4436 |
| `pgscan_file` | 5934 | 4880 |
| `pgscan_kswapd` | 5768 | 4655 |
| `pgscan_direct` | 166 | 225 |
| policy writes | — | `changed=77` / `phits=102` |

**What is proven:** the hook fires and rewrites `scan_control.priority` in 77 of
102 `shrink_node` calls. That is a deterministic, in-kernel fact.

**What is *not* proven:** that the policy changes the *amount* of scanning.
`pgscan_file` is a running total and the workload is terminated by the OOM
killer at a different point in each run — an independent pair of the same gate
produced `4178` (mode 0) vs `6834` (mode 5), the opposite direction. Totals are
not comparable across runs; only a fixed-endpoint measurement can settle it.

## Why it is not "MGLRU backported"

- 5.10 has no `gen_lru` fields; this uses a small side table (hash by page), not
  the 7.4 `lru_gen` data structures.
- A handful of functions are hooked; 7.4 MGLRU spans `mm/vmscan.c`,
  `mm/workingset.c`, page flags, lruvec and more.
- Writing `sc->priority` is a blunt instrument, not generation-based reclaim.

## Next stages

- **Fixed-endpoint measurement:** sum `nr[]` per `get_scan_count` call (the
  `ktramp` trampoline already sees it) and compare medians over repeated runs,
  without letting the workload reach OOM.
- **Stage C:** replace selected `vmscan` functions with 7.4-inspired
  implementations via trampoline. On SMP use `aarch64_insn_patch_text` +
  `stop_machine`, not `_nosync`.
- **Stage D:** parity subset + benchmarks (pgscan, refault, latency, working
  set) with an explicit gap list.

## Measuring reclaim correctly

`tmpfs` never triggers the scanner on a swapless VM, `brd` reads do not populate
the file LRU here, and normal allocations are served from cache silently. The
working recipe — ext4 file through `loop0` plus anonymous pressure, polling
`pgscan_file` — is in [`MEASURING-RECLAIM.md`](MEASURING-RECLAIM.md).

## Build and gate

```sh
KB_MODULE_SRC=$PWD/modules/mglru KB_MODNAME=mglru_probe \
  tools/adapt_kernel.sh --profile profiles/rmx3709-5.10.236/profile.env

MODE=0 KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/mglru/mglru_probe.ko bash tests/qemu_mglru_gate.sh
MODE=5 KB_IMAGE=$HOME/tmp/kernel_Image.bin \
  KB_KO=$PWD/modules/mglru/mglru_probe.ko bash tests/qemu_mglru_gate.sh
```

Experimental: QEMU first; disable policy (`write 0`) before unload; no `rmmod` on
a device until the unload path is proven.
