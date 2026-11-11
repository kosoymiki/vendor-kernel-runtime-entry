# MGLRU parity report — runtime backport vs 7.4

Scope: what of 7.4 MGLRU is reproduced by the runtime modules (`ktramp`,
`mglru_probe`, `kpatch`, `kbootstrap`) on the vendor 5.10.236 kernel, what is
approximate, and what cannot be reproduced without rebuilding vmscan.

Method: each row compares the 7.4 mechanism (source: `mm/vmscan.c`,
`mm/workingset.c`, `Documentation/admin-guide/mm/multigen_lru.rst`) with the
code in `modules/ktramp/` and the QEMU gate output. Statuses are evidence-based,
not claims.

Status legend: **exact** — same algorithm/data; **approximate** — same decision
with a runtime substitute; **partial** — only part of the mechanism; **absent
as structure** — the data structure is not replaced (side table instead).

| mechanism | 7.4 | runtime implementation | status |
|---|---|---|---|
| per-memcg generation state | `lru_gen_folio` with `min_seq/max_seq` | `mgseq/mgwin` per memcg from `android_vh_shrink_node_memcgs` | approximate |
| generation clock | advances when a generation is created | advances per memcg scan epoch; `window=min(seq,2)` | approximate |
| page generation storage | folio flags / generation lists | side table `page -> seq` (8192 entries, hash) | approximate |
| young/cold classification | membership in generation lists | page `seq` equals current epoch → young, else cold | approximate |
| eviction age histogram | generation stats | 4-bucket `ev_age` (0,1,2,3+) per memcg | approximate |
| refault distance | shadow timestamp vs `nonresident_age` | identical: `shadow >> 19`, `lruvec+96`, 46-bit mask | **exact** |
| refault threshold | compared to `max_seq - min_seq` | `32 * window`, window scaled by adaptive ttl | approximate |
| adaptive `min_ttl` | ms-based `min_ttl_ms` | ttl 2..4 from refault/eviction thrash | approximate |
| per-generation LRU lists | `lru_gen` lists | module sorts the kernel's inactive lists into 4 age buckets, oldest-first, under `lru_lock`; no `lru_gen` structures | approximate (real list order) |
| folio paths | multi-order folios | head-page normalization in the module tables; QEMU produces no compound LRU pages (r2-proven vendor THP/CHP init gate) | partial (unit-tested) |
| text patch / SMP safety | n/a | `aarch64_insn_patch_text` (stop_machine), restore | **exact** |
| custom `shrink_lruvec` (vmscan entry) | source rebuild | `vmscan_hook`: module function replaces `shrink_lruvec`, calls kernel `shrink_active_list`/`shrink_inactive_list` | **exact entry point**, approximate loop |
| observability | tracepoints | kprobes + vendor tracepoint; counters in `/proc` | **exact** |

## What the gates prove

- **Stage A/B:** reclaim-path kprobes register 6/6 (`reg=1 err=0`) and fire
  under real reclaim; mode 5 rewrites `scan_control.priority` (`changed=55/84`,
  `gt0=55`, `zero=84`).
- **Stage E/F:** per-memcg generation and scan-epoch clock; cold cache takes
  the full-scan branch; `y80` protects the working set (~50.6%); `t5` zeroes
  file scans (100%); real refault halves them (`tdist=847`); restore and
  unload are clean.
- **Stage G:** adaptive ttl 2..4 from thrash, 4-bucket eviction age, branches
  8/9 (old evictions → full scan, young evictions → protect).

## Gap list (explicit)

1. **No `lru_gen` lists.** The list *structures* are not replaced; the runtime
   uses side tables plus a custom `shrink_lruvec` (`vmscan_hook`) that sorts the
   kernel's own inactive lists into age buckets (oldest-first) and calls the
   kernel scanners. The ordering is MGLRU-like; the storage is still the
   kernel's lists, not `lru_gen`.
2. **Clock semantics.** The epoch advances per memcg scan (vendor tracepoint),
   not at the exact MGLRU point where a generation is created. This changes
   generation boundaries, not the decision direction.
3. **Refault threshold scaling.** MGLRU compares generation sequences; the
   runtime compares page counts and scales by a window. The distance itself is
   exact; the threshold is approximate.
4. **`min_ttl` is not milliseconds.** The runtime ttl is a 2..4 window driven by
   the refault/eviction ratio, not a wall-clock TTL.
5. **Folio tails.** `isolate_lru_pages` rejects tail pages
   (`mm/vmscan.c:1953`), so eviction/refault probes always receive head pages;
   the tail→head normalization is unit-tested, not exercised by QEMU.
6. **No source rebuild.** The modules hook/replace functions (`kpatch`,
   `ktramp`, kprobes) and never modify the kernel image; a full MGLRU source
   backport remains a kernel-build task, not an LKM one.

## Evidence map

| stage | document | gate |
|---|---|---|
| A/B | `docs/MGLRU.md` | `tests/qemu_mglru_gate.sh` |
| C/D | `docs/MGLRU-STAGE-E.md` | `tests/qemu_ktramp_gate.sh` |
| E | `docs/MGLRU-STAGE-E.md` | `tests/qemu_ktramp_gate.sh` |
| F | `docs/MGLRU-STAGE-F.md` | `tests/qemu_ktramp_gate.sh` |
| G | `docs/MGLRU-STAGE-G.md` | `tests/qemu_ktramp_gate.sh` |
| vmscan entry | `docs/VMSSCAN-HOOK.md` | `tests/qemu_vmscan_hook_gate.sh` |

The 5.10 → 7.4 file/LOC delta and the feature matrix are in
[`BACKPORT-7.4.md`](BACKPORT-7.4.md).
